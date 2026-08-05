//
// See aot_compile.h.
//

#include "aot/aot_compile.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <elfio/elfio.hpp>
#include "fmt/format.h"

#include "aot/aot_guest.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/code_serial.h"
#include "runtime/backend/module.h"
#include "runtime/ir/function.h"
#include "translator/x86/translator.h"

namespace swift::aot {

namespace backend = swift::runtime::backend;
namespace ir = swift::runtime::ir;

// --------------------------------------------------------------------------
// Discovery
// --------------------------------------------------------------------------
bool DiscoverFunctions(const std::string& guest_elf_path,
                       std::vector<FuncCandidate>& out,
                       u32& zero_size_count,
                       std::string& error) {
    using namespace ELFIO;
    elfio reader;
    if (!reader.load(guest_elf_path)) {
        error = "cannot open guest ELF " + guest_elf_path;
        return false;
    }
    const section* symtab = nullptr;
    for (Elf_Half i = 0; i < reader.sections.size(); ++i) {
        if (reader.sections[i]->get_type() == SHT_SYMTAB) {
            symtab = reader.sections[i];
            break;
        }
    }
    if (!symtab) {
        // docs/aot-design.md §3/§8: stripped guests are an acknowledged
        // capability gap, not something to paper over with CFG discovery
        // that would silently cover a fraction of the binary.
        error = fmt::format(
                "{} has no .symtab; AOT discovery needs one (stripped guests are an "
                "acknowledged gap, see docs/aot-design.md §8)",
                guest_elf_path);
        return false;
    }

    const_symbol_section_accessor syms{reader, const_cast<section*>(symtab)};
    struct Raw {
        u64 addr;
        u64 size;
        u16 shndx;
        std::string name;
    };
    std::vector<Raw> raw;
    for (Elf_Xword i = 0; i < syms.get_symbols_num(); ++i) {
        std::string name;
        Elf64_Addr value{};
        Elf_Xword size{};
        unsigned char bind{};
        unsigned char type{};
        Elf_Half shndx{};
        unsigned char other{};
        if (!syms.get_symbol(i, name, value, size, bind, type, shndx, other)) {
            continue;
        }
        if (!IsCompilableFuncType(type) || shndx == SHN_UNDEF || shndx >= SHN_LORESERVE) {
            continue;
        }
        raw.push_back({value, size, shndx, name});
    }

    // Boundary table for the zero-size policy: every function entry, plus the
    // end of each section, sorted.
    std::set<u64> boundaries;
    for (const auto& r : raw) {
        boundaries.insert(r.addr);
    }
    std::map<u16, std::pair<u64, u64>> section_span;
    for (Elf_Half i = 0; i < reader.sections.size(); ++i) {
        const section* s = reader.sections[i];
        if (s->get_flags() & SHF_ALLOC) {
            section_span[i] = {s->get_address(), s->get_address() + s->get_size()};
            boundaries.insert(s->get_address() + s->get_size());
        }
    }

    std::map<u64, FuncCandidate> by_addr;
    zero_size_count = 0;
    for (const auto& r : raw) {
        FuncCandidate c{};
        c.addr = r.addr;
        c.declared_size = r.size;
        c.name = r.name;
        if (r.size != 0) {
            c.inferred_end = r.addr + r.size;
        } else {
            zero_size_count++;
            c.size_inferred = true;
            // Next boundary strictly above `addr`, clamped to the section end.
            u64 end = r.addr;
            auto it = boundaries.upper_bound(r.addr);
            if (it != boundaries.end()) {
                end = *it;
            }
            auto sit = section_span.find(r.shndx);
            if (sit != section_span.end()) {
                end = std::min(end, sit->second.second);
                if (end <= r.addr) {
                    end = sit->second.second;
                }
            }
            c.inferred_end = end;
        }
        // Aliases (173 addresses in func_tests_x86_64 carry two or more
        // names) collapse to one candidate; the artifact writer points every
        // alias at the same unit.
        auto [it, inserted] = by_addr.emplace(r.addr, c);
        if (!inserted && it->second.size_inferred && !c.size_inferred) {
            it->second = c;  // prefer a symbol that states its size
        }
    }
    out.clear();
    out.reserve(by_addr.size());
    for (auto& [addr, c] : by_addr) {
        out.push_back(c);
    }
    return true;
}

// --------------------------------------------------------------------------
// Compilation
// --------------------------------------------------------------------------
namespace {

// Guest byte range hash, byte-identical to what the loader recomputes.
bool HashGuestRange(GuestImage& guest, u64 start, u64 end, u64& out) {
    if (end <= start || end - start > (1u << 20)) {
        return false;
    }
    const auto len = static_cast<std::size_t>(end - start);
    if (!guest.memory.RangeIsMapped(start, len)) {
        return false;
    }
    out = backend::HashBytes(guest.memory.ToHostConst(start), len, 0xCBF29CE484222325ull);
    return true;
}

}  // namespace

bool CompileArtifact(const CompileOptions& options, AotStats& stats, std::string& error) {
    std::vector<FuncCandidate> candidates;
    u32 zero_size = 0;
    if (!DiscoverFunctions(options.guest_elf, candidates, zero_size, error)) {
        return false;
    }
    stats = AotStats{};
    stats.symbols_zero_size = zero_size;

    GuestImage guest;
    if (!guest.Load(options.guest_elf, error)) {
        return false;
    }

    AotImage image{};
    image.guest.entry = guest.image.entry;
    image.guest.phdr = guest.image.phdr;
    image.guest.phentsize = guest.image.phentsize;
    image.guest.phnum = guest.image.phnum;
    image.guest.brk_start = guest.image.brk_start;
    // ElfLoader realpath()s it; the guest sees this string as AT_EXECFN and as
    // readlink("/proc/self/exe"), so it is initial state and has to be
    // reproduced at run time rather than re-derived from the artifact's own
    // location.
    image.guest.path = guest.image.path;
    image.segments = guest.segments;

    auto* instance = translator::x86::X86Instance::Make(
            reinterpret_cast<void*>(guest.memory.GetBias()),
            guest.memory.Windowed() ? guest.memory.Mask() : 0);
    if (options.decode_budget != 0) {
        instance->SetFunctionDecodeBudget(options.decode_budget);
    }
    auto* address_space = instance->GetAddressSpace();
    address_space->LoadJitCache();
    const auto& config = address_space->GetConfig();
    const auto& host_image = backend::GetHostImage();
    if (host_image.size == 0) {
        error = "cannot determine the SwiftVM host image span";
        translator::x86::X86Instance::Destroy(instance);
        return false;
    }
    const u64 window = config.guest_addr_mask ? config.guest_addr_mask + 1 : 0;

    // Symbol census for the record: every STT_FUNC, aliases included.
    stats.symbols_seen = 0;
    {
        using namespace ELFIO;
        elfio reader;
        if (reader.load(options.guest_elf)) {
            for (Elf_Half i = 0; i < reader.sections.size(); ++i) {
                if (reader.sections[i]->get_type() != SHT_SYMTAB) continue;
                const_symbol_section_accessor syms{reader, reader.sections[i]};
                for (Elf_Xword k = 0; k < syms.get_symbols_num(); ++k) {
                    std::string name;
                    Elf64_Addr value{};
                    Elf_Xword size{};
                    unsigned char bind{}, type{}, other{};
                    Elf_Half shndx{};
                    if (syms.get_symbol(k, name, value, size, bind, type, shndx, other) &&
                        IsCompilableFuncType(type) && shndx != SHN_UNDEF &&
                        shndx < SHN_LORESERVE) {
                        stats.symbols_seen++;
                    }
                }
                break;
            }
        }
    }

    // Units keyed by guest start so an address reached twice (an alias, or a
    // function whose entry another unit already covers) records once.
    std::map<u64, AotUnit> units;
    std::vector<std::vector<u8>> unit_bytes;  // parallel to `units` insertion order
    std::map<u64, std::size_t> unit_bytes_index;

    // Host ranges already claimed by an emitted unit. A later symbol whose
    // entry falls inside one of them (an alias, a shared tail, an interior
    // block of a function unit) is *covered*, not failed.
    std::vector<std::pair<const u8*, const u8*>> claimed;
    auto already_claimed = [&](const u8* p) {
        return std::any_of(claimed.begin(), claimed.end(), [p](const auto& r) {
            return p >= r.first && p < r.second;
        });
    };

    // Successor addresses the decoder published but no unit covers. Filled
    // while harvesting each unit's blocks; drained by the --sweep rounds.
    std::set<u64> pending_successors;

    u32 attempted = 0;
    // One entry address through the ordinary function-mode path. Returns true
    // when a unit was added.
    auto compile_one = [&](u64 addr, const std::string& name) -> bool {
        const FuncCandidate c{addr, 0, 0, false, name};
        if (units.count(c.addr)) {
            return false;
        }
        void* entry = nullptr;
        try {
            // See X86Instance::ResetFunctionModeLatch: without this, the first
            // guest function above the 128-block cap forces every *later*
            // symbol onto the block compiler, and the artifact ends up holding
            // one entry block per function instead of whole functions.
            instance->ResetFunctionModeLatch();
            entry = instance->CompileAt(c.addr);
        } catch (const std::exception& e) {
            entry = nullptr;
        }
        if (!entry) {
            stats.fail_translate++;
            if (options.verbose) {
                fmt::print(stderr, "[aot] {:#x} {}: translate failed\n", c.addr, c.name);
            }
            return false;
        }
        if (already_claimed(static_cast<const u8*>(entry))) {
            return false;
        }

        auto module = address_space->GetModule(c.addr);
        // The unit's extent comes from the JIT fault table, which
        // TranslateIR fills for *both* function and block units
        // (Module::AddFaultEntry). JitCache::cache_size would have worked for
        // functions only -- the block path never writes it -- and guessing a
        // length for a block unit is exactly the kind of silent error this
        // pipeline must not make.
        backend::FaultEntry fault{};
        if (!module->LookupFault(static_cast<const u8*>(entry), fault) ||
            fault.host_start != static_cast<const u8*>(entry)) {
            stats.fail_block_mode++;
            return false;
        }
        const auto* code = static_cast<const u8*>(fault.host_start);
        const auto code_size = static_cast<u32>(fault.host_end - fault.host_start);
        if (code_size == 0 || code_size % 4 != 0) {
            stats.fail_block_mode++;
            return false;
        }

        auto scan = backend::ScanCodeUnit({code, code_size}, host_image, window);
        if (!scan.ok) {
            stats.fail_scan++;
            if (options.verbose) {
                fmt::print(stderr, "[aot] {:#x} {}: scan refused ({})\n", c.addr, c.name,
                           scan.reject_reason);
            }
            return false;
        }

        AotUnit unit{};
        unit.guest_start = c.addr;
        unit.code_size = code_size;
        unit.relocs = std::move(scan.relocs);

        // Collect the unit's guest blocks. A function unit owns every block
        // the whole-function decode published inside its buffer; a block-mode
        // fallback owns exactly one.
        bool blocks_ok = true;
        auto add_block = [&](u64 bs, u64 be) {
            auto* block_entry =
                    static_cast<const u8*>(address_space->GetCodeCache(ir::Location{bs}));
            if (!block_entry) {
                // A successor the function-mode decode named but did not
                // decode (the lazy budget stopped short, or the block cap did).
                // It is a *statically known* direct edge, not a guess, so it is
                // a legitimate sweep candidate; an indirect target still never
                // appears here and still lands in the dispatcher.
                if (ir::Location{bs}.Valid() && guest.memory.RangeIsMapped(bs, 1)) {
                    pending_successors.insert(bs);
                }
                return;
            }
            if (be <= bs) {
                return;
            }
            if (block_entry < code || block_entry >= code + code_size) {
                return;  // published by another unit (shared tail, alias)
            }
            SerialBlock sb{};
            sb.guest_start = bs;
            sb.guest_end = be;
            sb.code_offset = static_cast<u32>(block_entry - code);
            if (!HashGuestRange(guest, bs, be, sb.guest_bytes_hash)) {
                blocks_ok = false;
                return;
            }
            unit.blocks.push_back(sb);
        };

        auto node = module->GetNode(ir::Location{c.addr});
        if (backend::IsFunction(node)) {
            unit.is_function = 1;
            auto function = backend::GetFunction(node);
            auto guard = function->LockRead();
            for (auto& block : function->GetBlocks()) {
                add_block(block.GetStartLocation().Value(), block.GetEndLocation().Value());
            }
        } else if (backend::IsBlock(node)) {
            unit.is_function = 0;
            auto block = backend::GetBlock(node);
            add_block(block->GetStartLocation().Value(), block->GetEndLocation().Value());
        } else {
            stats.fail_block_mode++;
            return false;
        }

        if (!blocks_ok || unit.blocks.empty()) {
            stats.fail_guest_hash++;
            return false;
        }
        std::sort(unit.blocks.begin(), unit.blocks.end(), [](const auto& a, const auto& b) {
            return a.guest_start < b.guest_start;
        });

        claimed.emplace_back(code, code + code_size);
        unit_bytes_index[c.addr] = unit_bytes.size();
        unit_bytes.emplace_back(code, code + code_size);
        units.emplace(c.addr, std::move(unit));
        return true;
    };

    for (const auto& c : candidates) {
        if (options.max_functions != 0 && attempted >= options.max_functions) {
            break;
        }
        ++attempted;
        compile_one(c.addr, c.name);
    }

    // --- successor sweep --------------------------------------------------
    // Everything here was published as a direct successor by the *same*
    // decoder the JIT uses. The sweep never enumerates an indirect jump's
    // targets -- docs/aot-design.md §3 forbids that, and the decoder does not
    // even offer them -- so an address it cannot reach still goes through the
    // L2 dispatcher exactly as before. The only thing at stake is how much
    // work is left for run time.
    if (options.sweep && options.max_functions == 0) {
        // A round can uncover new successors; stop when a round adds nothing.
        for (u32 round = 0; round < 32; ++round) {
            // Second candidate source: the L2 dispatch table. A slot exists
            // only because the *emitter* reserved one, and it reserves one for
            // exactly two things: a block the decoder published, and the
            // return address of a `call` (JitTranslator::EmitPushRSB, which
            // takes the slot only for a Lambda(Imm) target and skips a dynamic
            // one). Return sites are the dominant gap -- I_CALL lowers to
            // PushRSB + an unconditional jump to the callee, so the block
            // *after* a call is not a CFG successor of the caller and whole-
            // function decoding never reaches it. Both sources are addresses
            // the decoder named from an immediate; an indirect jump's target
            // set appears in neither, so it still goes to the dispatcher.
            address_space->GetCodeCacheTable().ForEachEntry(
                    [&](u32, std::size_t key, std::size_t value) {
                        if (value == 0) {
                            pending_successors.insert(static_cast<u64>(key));
                        }
                    });
            std::set<u64> todo;
            todo.swap(pending_successors);
            for (const auto& u : units) {
                todo.erase(u.first);
            }
            if (todo.empty()) {
                break;
            }
            stats.sweep_rounds = round + 1;
            u32 added = 0;
            for (u64 addr : todo) {
                stats.sweep_addrs++;
                if (address_space->GetCodeCache(ir::Location{addr})) {
                    continue;  // an earlier unit in this round already covers it
                }
                added += compile_one(addr, "") ? 1 : 0;
            }
            stats.sweep_units += added;
            if (added == 0) {
                break;
            }
        }
    }
    stats.addrs_attempted = attempted;

    // --- lay out .svmaot.text --------------------------------------------
    for (auto& [addr, unit] : units) {
        const auto& bytes = unit_bytes[unit_bytes_index[addr]];
        unit.code_offset = static_cast<u32>(image.code.size());
        image.code.insert(image.code.end(), bytes.begin(), bytes.end());
        image.code.resize((image.code.size() + 15) & ~std::size_t{15}, 0);
        image.units.push_back(unit);
    }
    image.stub_offset = static_cast<u32>(image.code.size());
    {
        const u32 stub[2] = {kAotStubInsn, 0u};  // brk #0xA07 ; udf #0
        const auto* p = reinterpret_cast<const u8*>(stub);
        image.code.insert(image.code.end(), p, p + sizeof(stub));
    }
    image.code_hash = backend::HashBytes(image.code.data(), image.code.size(),
                                         0xCBF29CE484222325ull);
    stats.units_emitted = static_cast<u32>(image.units.size());

    // --- L2 dispatch-slot assignment -------------------------------------
    // Recorded in full, including slots owned by units we did *not* keep: the
    // indices baked into the units we did keep are only valid if the whole
    // assignment is reproduced, and a slot whose unit is missing simply reads
    // back as a miss and is filled by the JIT.
    address_space->GetCodeCacheTable().ForEachEntry(
            [&](u32 index, std::size_t key, std::size_t) {
                image.dispatch_slots.emplace_back(static_cast<u64>(key), index);
            });

    // --- validity key -----------------------------------------------------
    image.key.format_version = backend::kCacheFormatVersion;
    image.key.build_id = backend::ComputeBuildId();
    image.key.config_hash = backend::ComputeConfigHash(config);
    image.key.env_hash = backend::ComputeEnvHash();
    image.key.guest_id = HashGuestImage(image.segments, image.guest.entry);

    // Symbols that will point at the stub.
    {
        // Same rule the artifact writer uses: a symbol is covered when its
        // address is a unit entry *or* an entry block inside a unit.
        std::set<u64> covered;
        for (const auto& u : image.units) {
            covered.insert(u.guest_start);
            for (const auto& b : u.blocks) {
                covered.insert(b.guest_start);
            }
        }
        u32 stubbed = 0;
        for (const auto& c : candidates) {
            if (!covered.count(c.addr)) {
                stubbed++;
            }
        }
        stats.symbols_stubbed = stubbed;
    }
    image.stats = stats;

    translator::x86::X86Instance::Destroy(instance);

    return WriteArtifact(options.guest_elf, image, options.out_path, error);
}

}  // namespace swift::aot
