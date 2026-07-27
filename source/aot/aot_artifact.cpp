//
// See aot_artifact.h / aot_format.h.
//

#include "aot/aot_artifact.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <elfio/elfio.hpp>
#include "fmt/format.h"

namespace swift::aot {

namespace {

using swift::runtime::backend::BlobReader;
using swift::runtime::backend::BlobWriter;
using swift::runtime::backend::HashBytes;
using swift::runtime::backend::HashU64;

constexpr u64 kFnvOffset = 0xCBF29CE484222325ull;

void WriteUnitRecord(BlobWriter& w, const AotUnit& u) {
    w.U64(u.guest_start);
    w.U8(u.is_function);
    w.U32(u.code_offset);
    w.U32(u.code_size);
    w.U32(static_cast<u32>(u.blocks.size()));
    for (const auto& b : u.blocks) {
        w.U64(b.guest_start);
        w.U64(b.guest_end);
        w.U32(b.code_offset);
        w.U64(b.guest_bytes_hash);
    }
    w.U32(static_cast<u32>(u.relocs.size()));
    for (const auto& r : u.relocs) {
        w.U32(r.code_offset);
        w.U16(r.inst_count);
        w.U16(r.reg);
        w.U16(static_cast<u16>(r.kind));
        w.U16(static_cast<u16>(r.use));
        w.U64(r.addend);
        w.U64(r.recorded_value);
    }
}

bool ReadUnitRecord(BlobReader& r, AotUnit& u) {
    u32 count{};
    if (!r.U64(u.guest_start) || !r.U8(u.is_function) || !r.U32(u.code_offset) ||
        !r.U32(u.code_size)) {
        return false;
    }
    if (u.code_size == 0 || u.code_size % 4 != 0 || u.code_offset % 4 != 0) {
        return false;
    }
    if (!r.U32(count) || count > r.Remaining() / 8) {
        return false;
    }
    u.blocks.resize(count);
    for (auto& b : u.blocks) {
        if (!r.U64(b.guest_start) || !r.U64(b.guest_end) || !r.U32(b.code_offset) ||
            !r.U64(b.guest_bytes_hash)) {
            return false;
        }
        if (b.code_offset >= u.code_size || b.guest_end < b.guest_start) {
            return false;
        }
    }
    if (!r.U32(count) || count > r.Remaining() / 8) {
        return false;
    }
    u.relocs.resize(count);
    for (auto& rel : u.relocs) {
        u16 kind{};
        u16 use{};
        if (!r.U32(rel.code_offset) || !r.U16(rel.inst_count) || !r.U16(rel.reg) ||
            !r.U16(kind) || !r.U16(use) || !r.U64(rel.addend) || !r.U64(rel.recorded_value)) {
            return false;
        }
        rel.kind = static_cast<swift::runtime::backend::RelocKind>(kind);
        rel.use = static_cast<swift::runtime::backend::RelocUse>(use);
    }
    return true;
}

}  // namespace

u64 HashGuestImage(const std::vector<AotGuestSegment>& segments, u64 entry) {
    u64 h = HashU64(entry, kFnvOffset);
    h = HashU64(segments.size(), h);
    for (const auto& s : segments) {
        h = HashU64(s.vaddr, h);
        h = HashU64(s.memsz, h);
        h = HashU64(s.flags, h);
        h = HashU64(s.data.size(), h);
        h = HashBytes(s.data.data(), s.data.size(), h);
    }
    return h;
}

// --------------------------------------------------------------------------
// Blob codec
// --------------------------------------------------------------------------
std::vector<u8> EncodeInfoBlob(const AotImage& image) {
    BlobWriter payload;
    payload.U64(image.guest.entry);
    payload.U64(image.guest.phdr);
    payload.U64(image.guest.phentsize);
    payload.U64(image.guest.phnum);
    payload.U64(image.guest.brk_start);
    payload.U32(static_cast<u32>(image.guest.path.size()));
    payload.Bytes(image.guest.path.data(), image.guest.path.size());
    payload.U32(image.stub_offset);
    payload.U32(static_cast<u32>(image.code.size()));
    payload.U64(image.code_hash);

    payload.U32(image.stats.symbols_seen);
    payload.U32(image.stats.symbols_zero_size);
    payload.U32(image.stats.addrs_attempted);
    payload.U32(image.stats.units_emitted);
    payload.U32(image.stats.fail_translate);
    payload.U32(image.stats.fail_block_mode);
    payload.U32(image.stats.fail_scan);
    payload.U32(image.stats.fail_guest_hash);
    payload.U32(image.stats.symbols_stubbed);
    payload.U32(image.stats.sweep_rounds);
    payload.U32(image.stats.sweep_addrs);
    payload.U32(image.stats.sweep_units);

    payload.U32(static_cast<u32>(image.segments.size()));
    for (const auto& s : image.segments) {
        payload.U64(s.vaddr);
        payload.U64(s.memsz);
        payload.U32(s.flags);
        payload.U64(s.data.size());
    }

    payload.U64(image.dispatch_slots.size());
    for (const auto& [key, index] : image.dispatch_slots) {
        payload.U64(key);
        payload.U32(index);
    }

    payload.U64(image.units.size());
    for (const auto& u : image.units) {
        WriteUnitRecord(payload, u);
    }

    BlobWriter blob;
    blob.Bytes(kAotMagic, sizeof(kAotMagic));
    blob.U64(kAotFormatVersion);
    blob.U64(image.key.format_version);
    blob.U64(image.key.build_id);
    blob.U64(image.key.config_hash);
    blob.U64(image.key.env_hash);
    blob.U64(image.key.guest_id);
    blob.U64(payload.Size());
    blob.U64(HashBytes(payload.Data().data(), payload.Size(), kFnvOffset));
    blob.Bytes(payload.Data().data(), payload.Size());
    return blob.Data();
}

bool DecodeInfoBlob(const u8* data, std::size_t size, AotImage& out, std::string& error) {
    auto fail = [&](const char* msg) {
        error = msg;
        return false;
    };
    BlobReader r{data, size};
    char magic[sizeof(kAotMagic)];
    u64 format{};
    u64 payload_size{};
    u64 payload_hash{};
    if (!r.Bytes(magic, sizeof(magic)) || std::memcmp(magic, kAotMagic, sizeof(kAotMagic)) != 0) {
        return fail("bad magic");
    }
    if (!r.U64(format) || format != kAotFormatVersion) {
        return fail("unsupported artifact format version");
    }
    if (!r.U64(out.key.format_version) || !r.U64(out.key.build_id) ||
        !r.U64(out.key.config_hash) || !r.U64(out.key.env_hash) || !r.U64(out.key.guest_id) ||
        !r.U64(payload_size) || !r.U64(payload_hash)) {
        return fail("truncated header");
    }
    if (payload_size != r.Remaining()) {
        return fail("payload size mismatch");
    }
    if (HashBytes(data + size - payload_size,
                  static_cast<std::size_t>(payload_size),
                  kFnvOffset) != payload_hash) {
        return fail("payload checksum mismatch");
    }

    u32 code_size{};
    u32 path_len{};
    if (!r.U64(out.guest.entry) || !r.U64(out.guest.phdr) || !r.U64(out.guest.phentsize) ||
        !r.U64(out.guest.phnum) || !r.U64(out.guest.brk_start) || !r.U32(path_len)) {
        return fail("truncated guest descriptor");
    }
    if (path_len > 4096 || path_len > r.Remaining()) {
        return fail("bad guest path length");
    }
    out.guest.path.resize(path_len);
    if (path_len != 0 && !r.Bytes(out.guest.path.data(), path_len)) {
        return fail("truncated guest path");
    }
    if (!r.U32(out.stub_offset) || !r.U32(code_size) || !r.U64(out.code_hash)) {
        return fail("truncated guest descriptor");
    }
    out.code.resize(code_size);  // filled from .svmaot.text by the caller

    if (!r.U32(out.stats.symbols_seen) || !r.U32(out.stats.symbols_zero_size) ||
        !r.U32(out.stats.addrs_attempted) || !r.U32(out.stats.units_emitted) ||
        !r.U32(out.stats.fail_translate) || !r.U32(out.stats.fail_block_mode) ||
        !r.U32(out.stats.fail_scan) || !r.U32(out.stats.fail_guest_hash) ||
        !r.U32(out.stats.symbols_stubbed) || !r.U32(out.stats.sweep_rounds) ||
        !r.U32(out.stats.sweep_addrs) || !r.U32(out.stats.sweep_units)) {
        return fail("truncated stats");
    }

    u32 seg_count{};
    if (!r.U32(seg_count) || seg_count > 1024) {
        return fail("bad segment count");
    }
    out.segments.resize(seg_count);
    for (auto& s : out.segments) {
        u64 filesz{};
        if (!r.U64(s.vaddr) || !r.U64(s.memsz) || !r.U32(s.flags) || !r.U64(filesz)) {
            return fail("truncated segment descriptor");
        }
        if (filesz > s.memsz) {
            return fail("segment filesz exceeds memsz");
        }
        s.data.resize(static_cast<std::size_t>(filesz));  // filled from .svmaot.segN
    }

    u64 slot_count{};
    if (!r.U64(slot_count) || slot_count > r.Remaining() / 12) {
        return fail("bad dispatch slot count");
    }
    out.dispatch_slots.reserve(static_cast<std::size_t>(slot_count));
    for (u64 i = 0; i < slot_count; ++i) {
        u64 key{};
        u32 index{};
        if (!r.U64(key) || !r.U32(index)) {
            return fail("truncated dispatch slot");
        }
        out.dispatch_slots.emplace_back(key, index);
    }

    u64 unit_count{};
    if (!r.U64(unit_count) || unit_count > r.Remaining() / 24) {
        return fail("bad unit count");
    }
    out.units.resize(static_cast<std::size_t>(unit_count));
    for (auto& u : out.units) {
        if (!ReadUnitRecord(r, u)) {
            return fail("truncated unit record");
        }
        if (static_cast<u64>(u.code_offset) + u.code_size > code_size) {
            return fail("unit code range outside .svmaot.text");
        }
    }
    if (!r.Empty()) {
        return fail("trailing bytes in payload");
    }
    return true;
}

// --------------------------------------------------------------------------
// ELF writer
// --------------------------------------------------------------------------
bool WriteArtifact(const std::string& guest_elf_path,
                   const AotImage& image,
                   const std::string& out_path,
                   std::string& error) {
    using namespace ELFIO;

    elfio reader;
    if (!reader.load(guest_elf_path)) {
        error = "cannot re-open the guest ELF: " + guest_elf_path;
        return false;
    }

    // Guest address -> unit, so a STT_FUNC can find its compiled code. Guest
    // binaries alias heavily (173 aliased addresses in func_tests_x86_64:
    // `__stack_chk_fail`/`__stack_chk_fail_local`, ...), and every alias must
    // land on the same unit.
    // Guest address -> (offset in .svmaot.text, length). Unit entries first,
    // then every block entry inside a unit: a symbol whose address is interior
    // to another unit (a shared tail, a `.cold` fragment folded into its
    // parent) is *covered*, and pointing it at the failure stub would make the
    // symbol table claim it was not compiled.
    std::map<u64, std::pair<u64, u64>> by_guest;
    for (const auto& u : image.units) {
        for (const auto& b : u.blocks) {
            by_guest.emplace(b.guest_start,
                             std::pair<u64, u64>{u.code_offset + b.code_offset,
                                                 u.code_size - b.code_offset});
        }
    }
    for (const auto& u : image.units) {
        by_guest[u.guest_start] = {u.code_offset, u.code_size};
    }

    elfio w;
    w.create(ELFCLASS64, ELFDATA2LSB);
    w.set_os_abi(ELFOSABI_LINUX);
    w.set_type(ET_DYN);
    w.set_machine(EM_AARCH64);
    w.set_flags(0);

    // --- copy every original section, in order ----------------------------
    // Index 0 is the null section in both files; ELFIO's create() also made
    // .shstrtab, so the mapping is not the identity and every st_shndx must
    // be translated through `shndx_map`.
    std::vector<Elf_Half> shndx_map(reader.sections.size(), SHN_UNDEF);
    std::vector<section*> copied(reader.sections.size(), nullptr);
    section* out_symtab = nullptr;
    section* out_strtab = nullptr;
    Elf_Half orig_symtab_index = 0;

    for (Elf_Half i = 1; i < reader.sections.size(); ++i) {
        const section* src = reader.sections[i];
        section* dst = w.sections.add(src->get_name());
        dst->set_type(src->get_type());
        dst->set_flags(src->get_flags());
        dst->set_address(src->get_address());
        dst->set_addr_align(src->get_addr_align());
        dst->set_entry_size(src->get_entry_size());
        dst->set_info(src->get_info());
        if (src->get_type() == SHT_NOBITS) {
            dst->set_size(src->get_size());
        } else if (src->get_data() && src->get_size() != 0) {
            dst->set_data(src->get_data(), static_cast<Elf_Word>(src->get_size()));
        }
        shndx_map[i] = static_cast<Elf_Half>(dst->get_index());
        copied[i] = dst;
        if (src->get_type() == SHT_SYMTAB) {
            out_symtab = dst;
            orig_symtab_index = i;
        }
    }
    // sh_link is a section index and must be translated too.
    for (Elf_Half i = 1; i < reader.sections.size(); ++i) {
        const Elf_Word link = reader.sections[i]->get_link();
        if (copied[i] && link != 0 && link < shndx_map.size()) {
            copied[i]->set_link(shndx_map[link]);
        }
    }
    if (out_symtab) {
        out_strtab = copied[reader.sections[orig_symtab_index]->get_link()];
    }

    // --- .svmaot.text -----------------------------------------------------
    section* code_sec = w.sections.add(kAotCodeSectionName);
    code_sec->set_type(SHT_PROGBITS);
    code_sec->set_flags(SHF_ALLOC | SHF_EXECINSTR);
    code_sec->set_address(kAotCodeVaddr);
    code_sec->set_addr_align(16);
    code_sec->set_data(reinterpret_cast<const char*>(image.code.data()),
                       static_cast<Elf_Word>(image.code.size()));
    const Elf_Half code_shndx = static_cast<Elf_Half>(code_sec->get_index());

    // --- .svmaot.segN -----------------------------------------------------
    // Non-alloc on purpose: the copied original sections already carry the
    // SHF_ALLOC addresses a third-party parser cares about, and two allocated
    // sections claiming the same sh_addr would be a needlessly confusing file.
    for (std::size_t i = 0; i < image.segments.size(); ++i) {
        section* s = w.sections.add(fmt::format("{}{}", kAotSegSectionPrefix, i));
        s->set_type(SHT_PROGBITS);
        s->set_flags(0);
        s->set_addr_align(16);
        s->set_data(reinterpret_cast<const char*>(image.segments[i].data.data()),
                    static_cast<Elf_Word>(image.segments[i].data.size()));
    }

    // --- .svmaot.info -----------------------------------------------------
    {
        const auto blob = EncodeInfoBlob(image);
        section* s = w.sections.add(kAotInfoSectionName);
        s->set_type(SHT_PROGBITS);
        s->set_flags(0);
        s->set_addr_align(8);
        s->set_data(reinterpret_cast<const char*>(blob.data()),
                    static_cast<Elf_Word>(blob.size()));
    }

    // --- rewrite the symbol table ----------------------------------------
    if (out_symtab && out_strtab) {
        const_symbol_section_accessor src_syms{reader, reader.sections[orig_symtab_index]};
        // Rebuild the table in place: ELFIO has no per-symbol setter, so the
        // copied bytes are edited directly. Entry layout is fixed by the ABI
        // (Elf64_Sym: name/info/other/shndx/value/size).
        std::vector<char> bytes(out_symtab->get_data(),
                                out_symtab->get_data() + out_symtab->get_size());
        const std::size_t n = static_cast<std::size_t>(src_syms.get_symbols_num());
        if (bytes.size() < n * 24) {
            error = "symtab is shorter than its symbol count";
            return false;
        }
        for (std::size_t i = 0; i < n; ++i) {
            char* e = bytes.data() + i * 24;
            u8 info{};
            u16 shndx{};
            u64 value{};
            u64 size{};
            std::memcpy(&info, e + 4, 1);
            std::memcpy(&shndx, e + 6, 2);
            std::memcpy(&value, e + 8, 8);
            std::memcpy(&size, e + 16, 8);
            const u8 type = info & 0xF;

            // Translate the section index for everything; only STT_FUNC also
            // moves.
            u16 new_shndx = shndx;
            if (shndx != SHN_UNDEF && shndx < SHN_LORESERVE && shndx < shndx_map.size()) {
                new_shndx = shndx_map[shndx];
            }
            // STT_GNU_IFUNC is rewritten alongside STT_FUNC (aot_format.h,
            // IsCompilableFuncType): leaving it alone would leave the original
            // x86-64 st_value in an AArch64 object.
            if (IsCompilableFuncType(type) && shndx != SHN_UNDEF && shndx < SHN_LORESERVE) {
                auto it = by_guest.find(value);
                if (it != by_guest.end()) {
                    value = kAotCodeVaddr + it->second.first;
                    size = it->second.second;
                } else {
                    value = kAotCodeVaddr + image.stub_offset;
                    size = kAotStubSize;
                }
                new_shndx = code_shndx;
            }
            std::memcpy(e + 6, &new_shndx, 2);
            std::memcpy(e + 8, &value, 8);
            std::memcpy(e + 16, &size, 8);
        }
        out_symtab->set_data(bytes.data(), static_cast<Elf_Word>(bytes.size()));
    }

    // --- program headers --------------------------------------------------
    // One PT_LOAD per original PT_LOAD holding that segment's SHF_ALLOC
    // sections, plus one for .svmaot.text. The artifact's loader does not use
    // these (it maps .svmaot.segN), but an ET_DYN without a sane phdr table
    // is not a file anyone else can read.
    for (Elf_Half i = 0; i < reader.segments.size(); ++i) {
        const ELFIO::segment* src = reader.segments[i];
        if (src->get_type() != PT_LOAD) {
            continue;
        }
        std::vector<section*> members;
        for (Elf_Half s = 1; s < reader.sections.size(); ++s) {
            const section* sec = reader.sections[s];
            if (!(sec->get_flags() & SHF_ALLOC) || !copied[s]) {
                continue;
            }
            const u64 a = sec->get_address();
            if (a >= src->get_virtual_address() &&
                a < src->get_virtual_address() + src->get_memory_size()) {
                members.push_back(copied[s]);
            }
        }
        if (members.empty()) {
            continue;
        }
        std::sort(members.begin(), members.end(), [](const section* a, const section* b) {
            return a->get_address() < b->get_address();
        });
        ELFIO::segment* seg = w.segments.add();
        seg->set_type(PT_LOAD);
        seg->set_virtual_address(members.front()->get_address());
        seg->set_physical_address(members.front()->get_address());
        seg->set_flags(src->get_flags());
        seg->set_align(src->get_align());
        for (auto* m : members) {
            seg->add_section(m, m->get_addr_align());
        }
    }
    {
        ELFIO::segment* seg = w.segments.add();
        seg->set_type(PT_LOAD);
        seg->set_virtual_address(kAotCodeVaddr);
        seg->set_physical_address(kAotCodeVaddr);
        seg->set_flags(PF_R | PF_X);
        seg->set_align(0x1000);
        seg->add_section(code_sec, code_sec->get_addr_align());
    }

    // Entry point: the artifact is not directly executable, and pointing
    // e_entry at a guest x86 address would be a lie about what is there.
    // Point it at the failure stub.
    w.set_entry(kAotCodeVaddr + image.stub_offset);

    if (!w.save(out_path)) {
        error = "failed to write " + out_path;
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// ELF reader
// --------------------------------------------------------------------------
bool ReadArtifact(const std::string& path, AotImage& out, std::string& error) {
    using namespace ELFIO;
    elfio r;
    if (!r.load(path)) {
        error = "cannot open artifact " + path;
        return false;
    }
    if (r.get_class() != ELFCLASS64 || r.get_machine() != EM_AARCH64) {
        error = "artifact is not an AArch64 ELF64";
        return false;
    }
    const section* info = r.sections[kAotInfoSectionName];
    const section* code = r.sections[kAotCodeSectionName];
    if (!info || !code || !info->get_data() || !code->get_data()) {
        error = "artifact has no AOT sections";
        return false;
    }
    if (!DecodeInfoBlob(reinterpret_cast<const u8*>(info->get_data()),
                        static_cast<std::size_t>(info->get_size()),
                        out,
                        error)) {
        return false;
    }
    if (code->get_size() != out.code.size()) {
        error = "the code section size disagrees with the metadata";
        return false;
    }
    std::memcpy(out.code.data(), code->get_data(), out.code.size());
    if (swift::runtime::backend::HashBytes(out.code.data(), out.code.size(), kFnvOffset) !=
        out.code_hash) {
        error = "the compiled code section does not match its recorded hash";
        return false;
    }
    if (out.stub_offset + kAotStubSize > out.code.size()) {
        error = "failure stub outside the code section";
        return false;
    }

    for (std::size_t i = 0; i < out.segments.size(); ++i) {
        const auto name = fmt::format("{}{}", kAotSegSectionPrefix, i);
        const section* s = r.sections[name];
        if (!s || s->get_size() != out.segments[i].data.size()) {
            error = "missing or mis-sized " + name;
            return false;
        }
        if (!out.segments[i].data.empty()) {
            std::memcpy(out.segments[i].data.data(), s->get_data(), out.segments[i].data.size());
        }
    }
    return true;
}

}  // namespace swift::aot
