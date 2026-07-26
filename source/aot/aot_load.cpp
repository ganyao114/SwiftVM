//
// See aot_load.h.
//

#include "aot/aot_load.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <elfio/elfio.hpp>
#include "fmt/format.h"

#include "runtime/backend/address_space.h"
#include "runtime/backend/code_serial.h"
#include "runtime/backend/module.h"
#include "runtime/ir/function.h"

namespace swift::aot {

namespace backend = swift::runtime::backend;
namespace ir = swift::runtime::ir;

namespace {

// artifact code offset -> installed host address, sorted by offset. Written
// once by InstallArtifact before any guest code runs, read-only afterwards.
std::map<u64, std::pair<u64, u8*>>& CodeMap() {
    static std::map<u64, std::pair<u64, u8*>> map;  // offset -> (size, host)
    return map;
}

bool HashGuestRange(void* base, u64 mask, u64 start, u64 end, u64& out) {
    if (end <= start || end - start > (1u << 20)) {
        return false;
    }
    const u64 m = mask ? mask : UINT64_MAX;
    if (mask) {
        const u64 lo = start & m;
        if (end - start > m + 1 - lo) {
            return false;
        }
        if ((start & ~m) != ((end - 1) & ~m)) {
            return false;
        }
    }
    const auto* host = static_cast<const u8*>(base) + (start & m);
    out = backend::HashBytes(host, static_cast<std::size_t>(end - start), 0xCBF29CE484222325ull);
    return true;
}

}  // namespace

void* ResolveCodeOffset(u64 offset) {
    auto& map = CodeMap();
    auto it = map.upper_bound(offset);
    if (it == map.begin()) {
        return nullptr;
    }
    --it;
    const auto unit_offset = it->first;
    const auto [size, host] = it->second;
    if (offset >= unit_offset && offset < unit_offset + size) {
        return host + (offset - unit_offset);
    }
    return nullptr;
}

int InstallArtifact(backend::AddressSpace& address_space,
                    const AotImage& artifact,
                    void* guest_memory_base,
                    u64 guest_addr_mask,
                    LoadReport& report,
                    std::string& error) {
    const auto& config = address_space.GetConfig();
    const auto& host_image = backend::GetHostImage();
    if (host_image.size == 0) {
        error = "cannot determine the SwiftVM host image span";
        return kAotBadArgument;
    }
    if (!guest_memory_base) {
        error = "AOT requires a biased guest window (Config::memory_base)";
        return kAotBadArgument;
    }

    // --- validity key -----------------------------------------------------
    // Every field must match; docs/aot-design.md §4 is explicit that a miss
    // here is the most dangerous silent-error class this project has.
    const backend::ValidityKey live{backend::kCacheFormatVersion,
                                    backend::ComputeBuildId(),
                                    backend::ComputeConfigHash(config),
                                    backend::ComputeEnvHash(),
                                    artifact.key.guest_id};
    if (artifact.key.format_version != live.format_version) {
        error = fmt::format("format version {} != {}", artifact.key.format_version,
                            live.format_version);
        return kAotKeyMismatch;
    }
    if (artifact.key.build_id != live.build_id) {
        error = "artifact was produced by a different SwiftVM build";
        return kAotKeyMismatch;
    }
    if (artifact.key.config_hash != live.config_hash) {
        error = "runtime Config differs from the one the artifact was compiled under";
        return kAotKeyMismatch;
    }
    if (artifact.key.env_hash != live.env_hash) {
        error = "SVM_*/SWIFT_* environment differs from the compile-time environment";
        return kAotKeyMismatch;
    }
    // Internal consistency of the carried guest image. The authoritative
    // guest check is the per-unit byte hash below; this catches an artifact
    // whose segment table was edited without touching the key.
    if (HashGuestImage(artifact.segments, artifact.guest.entry) != artifact.key.guest_id) {
        error = "guest image hash does not match the artifact key";
        return kAotKeyMismatch;
    }

    // --- pass 1: nothing is installed until every unit checks out ---------
    std::vector<u64> block_hashes;
    for (const auto& unit : artifact.units) {
        if (unit.blocks.empty()) {
            error = fmt::format("unit {:#x} has no blocks", unit.guest_start);
            return kAotBadFormat;
        }
        if (static_cast<u64>(unit.code_offset) + unit.code_size > artifact.code.size()) {
            error = fmt::format("unit {:#x} code range is out of bounds", unit.guest_start);
            return kAotBadFormat;
        }
        for (const auto& block : unit.blocks) {
            u64 hash{};
            if (!HashGuestRange(guest_memory_base, guest_addr_mask, block.guest_start,
                                block.guest_end, hash)) {
                error = fmt::format("guest range [{:#x},{:#x}) is not readable",
                                    block.guest_start, block.guest_end);
                return kAotGuestMismatch;
            }
            if (hash != block.guest_bytes_hash) {
                error = fmt::format(
                        "guest bytes at [{:#x},{:#x}) are not what unit {:#x} was compiled from",
                        block.guest_start, block.guest_end, unit.guest_start);
                return kAotGuestMismatch;
            }
            if (block.code_offset % 4 != 0 || block.code_offset >= unit.code_size) {
                error = fmt::format("unit {:#x} has an out-of-range block entry",
                                    unit.guest_start);
                return kAotBadFormat;
            }
        }
    }

    // --- pass 2: replay the L2 dispatch assignment ------------------------
    // Generated code branches through raw slot indices and a slot's identity
    // depends on the insertion order of colliding keys, so the assignment is
    // reproduced verbatim rather than recomputed. In a fresh process the
    // table is empty, so a conflict here means the artifact is inconsistent.
    auto& table = address_space.GetCodeCacheTable();
    for (const auto& [key, index] : artifact.dispatch_slots) {
        if (!table.PutAt(index, static_cast<std::size_t>(key))) {
            table.Clear();
            error = fmt::format("dispatch slot {} is already claimed by another key", index);
            return kAotDispatchConflict;
        }
    }
    report.dispatch_slots = static_cast<u32>(artifact.dispatch_slots.size());

    // --- pass 3: install ---------------------------------------------------
    CodeMap().clear();
    for (const auto& unit : artifact.units) {
        auto module = address_space.GetModule(unit.guest_start);
        auto [idx, buffer] = module->AllocCodeCache(unit.code_size);
        if (idx == backend::INVALID_CACHE_ID) {
            error = fmt::format("no code cache room for unit {:#x}", unit.guest_start);
            return kAotNoRoom;
        }
        std::memcpy(buffer.rw_data, artifact.code.data() + unit.code_offset, unit.code_size);
        std::string reloc_error;
        if (!backend::ApplyRelocations(
                    buffer.rw_data, unit.code_size, unit.relocs, host_image, &reloc_error)) {
            error = fmt::format("relocation failed for unit {:#x}: {}", unit.guest_start,
                                reloc_error);
            return kAotRelocFailed;
        }
        buffer.Flush();

        // Publish the same shape a normal compile would have left behind, so
        // SMC invalidation, the fault table and the dispatcher all see what
        // they expect. A block-mode unit stays a block: turning it into a
        // one-block function would claim a whole-function translation that
        // does not exist.
        ir::AddressNode* node = nullptr;
        backend::JitCache* jit_cache = nullptr;
        if (unit.is_function) {
            auto* function = new ir::Function(ir::Location{unit.guest_start});
            VAddr max_end = unit.guest_start;
            for (const auto& block : unit.blocks) {
                auto* ir_block = new ir::Block(ir::Location{block.guest_start});
                ir_block->SetEndLocation(ir::Location{block.guest_end});
                function->AddBlock(ir_block);
                max_end = std::max<VAddr>(max_end, block.guest_end);
            }
            function->SetEndLocation(ir::Location{max_end});
            node = function;
            jit_cache = &function->GetJitCache();
        } else {
            auto* block = new ir::Block(ir::Location{unit.guest_start});
            block->SetEndLocation(ir::Location{unit.blocks.front().guest_end});
            node = block;
            jit_cache = &block->GetJitCache();
        }
        jit_cache->jit_state = backend::JitState::Cached;
        jit_cache->cache_id = idx;
        jit_cache->offset_in = buffer.offset;
        jit_cache->cache_size = buffer.size;

        if (!module->Push(node)) {
            if (unit.is_function) {
                delete static_cast<ir::Function*>(node);
            } else {
                delete static_cast<ir::Block*>(node);
            }
            if (auto* cache = module->GetCodeCache(buffer.exec_data)) {
                cache->FreeCode(buffer.exec_data);
            }
            error = fmt::format("another node already owns {:#x}", unit.guest_start);
            return kAotBadFormat;
        }
        module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size, unit.guest_start);

        for (const auto& block : unit.blocks) {
            address_space.PushCodeCache(ir::Location{block.guest_start},
                                        buffer.exec_data + block.code_offset);
            if (!module->GetModuleConfig().read_only) {
                address_space.GetSmcTracker().RegisterNode(
                        module, node, block.guest_start, block.guest_end);
            }
        }
        CodeMap()[unit.code_offset] = {unit.code_size, buffer.exec_data};
        report.units_installed++;
        report.code_bytes += unit.code_size;
    }
    return kAotOk;
}

// --------------------------------------------------------------------------
// Symbol index
// --------------------------------------------------------------------------
bool SymbolIndex::Build(const std::string& artifact_path,
                        const AotImage& artifact,
                        std::string& error) {
    using namespace ELFIO;
    elfio r;
    if (!r.load(artifact_path)) {
        error = "cannot re-open the artifact for its symbol table";
        return false;
    }
    const section* symtab = nullptr;
    for (Elf_Half i = 0; i < r.sections.size(); ++i) {
        if (r.sections[i]->get_type() == SHT_SYMTAB) {
            symtab = r.sections[i];
            break;
        }
    }
    if (!symtab) {
        error = "artifact has no .symtab";
        return false;
    }
    const_symbol_section_accessor syms{r, const_cast<section*>(symtab)};
    for (Elf_Xword i = 0; i < syms.get_symbols_num(); ++i) {
        std::string name;
        Elf64_Addr value{};
        Elf_Xword size{};
        unsigned char bind{}, type{}, other{};
        Elf_Half shndx{};
        if (!syms.get_symbol(i, name, value, size, bind, type, shndx, other)) {
            continue;
        }
        if (type != STT_FUNC || name.empty()) {
            continue;
        }
        if (value < kAotCodeVaddr || value >= kAotCodeVaddr + artifact.code.size()) {
            continue;
        }
        by_name.emplace(name, value - kAotCodeVaddr);
    }
    return true;
}

void* SymbolIndex::Lookup(const std::string& name) const {
    auto it = by_name.find(name);
    if (it == by_name.end()) {
        return nullptr;
    }
    return ResolveCodeOffset(it->second);
}

}  // namespace swift::aot

// --------------------------------------------------------------------------
// C handshake (aot_format.h)
// --------------------------------------------------------------------------
namespace {
std::string g_last_error{"no error"};
unsigned long g_installed_units{};
}  // namespace

extern "C" int swift_aot_init(const SwiftAotRuntime* rt) {
    using namespace swift::aot;
    g_installed_units = 0;
    if (!rt || rt->struct_size != sizeof(SwiftAotRuntime) ||
        rt->abi_version != kSwiftAotAbiVersion || !rt->address_space || !rt->artifact_path) {
        g_last_error = "SwiftAotRuntime is malformed or from a different ABI";
        return kAotBadArgument;
    }
    AotImage artifact{};
    std::string error;
    if (!ReadArtifact(rt->artifact_path, artifact, error)) {
        g_last_error = error;
        return kAotUnreadable;
    }
    LoadReport report{};
    const int status = InstallArtifact(
            *static_cast<swift::runtime::backend::AddressSpace*>(rt->address_space),
            artifact,
            rt->guest_memory_base,
            rt->guest_addr_mask,
            report,
            error);
    if (status != kAotOk) {
        g_last_error = error;
        return status;
    }
    g_last_error = "no error";
    g_installed_units = report.units_installed;
    return kAotOk;
}

extern "C" const char* swift_aot_last_error(void) { return g_last_error.c_str(); }

extern "C" unsigned long swift_aot_installed_units(void) { return g_installed_units; }
