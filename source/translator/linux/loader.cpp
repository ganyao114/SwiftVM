//
// Created by 甘尧 on 2024/6/22.
// Rewritten as a guest ARM64 Linux ELF loader on top of elfio.
//

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include "base/common_funcs.h"
#include "base/logging.h"
#include "elfio/elfio.hpp"
#include "loader.h"

namespace swift::linux {

// Linux auxv entry types we emit.
enum GuestAuxv : u64 {
    AT_NULL = 0,
    AT_PHDR = 3,
    AT_PHENT = 4,
    AT_PHNUM = 5,
    AT_PAGESZ = 6,
    AT_BASE = 7,
    AT_FLAGS = 8,
    AT_ENTRY = 9,
    AT_UID = 11,
    AT_EUID = 12,
    AT_GID = 13,
    AT_EGID = 14,
    AT_HWCAP = 16,
    AT_CLKTCK = 17,
    AT_SECURE = 23,
    AT_RANDOM = 25,
    AT_EXECFN = 31,
};

LoadedImage ElfLoader::Load(const std::string& path) {
    ELFIO::elfio reader;
    if (!reader.load(path)) {
        PANIC("Failed to parse guest ELF file! file = {}", path);
    }
    if (reader.get_class() != ELFIO::ELFCLASS64) {
        PANIC("Guest ELF is not 64-bit! file = {}", path);
    }
    GuestISA isa;
    switch (reader.get_machine()) {
        case ELFIO::EM_AARCH64:
            isa = GuestISA::kArm64;
            break;
        case ELFIO::EM_X86_64:
            isa = GuestISA::kX86_64;
            break;
        default:
            PANIC("Guest ELF has an unsupported e_machine! file = {}, e_machine = {}",
                  path,
                  reader.get_machine());
    }
    auto elf_type = reader.get_type();
    if (elf_type != ELFIO::ET_EXEC && elf_type != ELFIO::ET_DYN) {
        PANIC("Guest ELF is not an executable! file = {}, e_type = {}", path, elf_type);
    }

    auto& segments = reader.segments;
    if (segments.size() == 0) {
        PANIC("Guest ELF has no program headers! file = {}", path);
    }
    for (const auto& seg : segments) {
        if (seg->get_type() == ELFIO::PT_INTERP) {
            PANIC("Dynamically linked guest ELF is not supported yet (PT_INTERP = {})! file = {}",
                  std::string(seg->get_data(), seg->get_file_size()),
                  path);
        }
    }

    // Address span of all PT_LOAD segments (host-page aligned).
    VAddr min_vaddr = UINT64_MAX;
    VAddr max_end = 0;
    for (const auto& seg : segments) {
        if (seg->get_type() != ELFIO::PT_LOAD) continue;
        min_vaddr = std::min(min_vaddr, seg->get_virtual_address());
        max_end = std::max(max_end, seg->get_virtual_address() + seg->get_memory_size());
    }
    if (min_vaddr == UINT64_MAX) {
        PANIC("Guest ELF has no PT_LOAD segments! file = {}", path);
    }
    const VAddr span_start = GuestMemory::RoundDownHostPage(min_vaddr);
    const VAddr span_end = GuestMemory::RoundHostPage(max_end);
    const u64 span = span_end - span_start;

    // Address modes:
    //  - ET_EXEC: the guest runs at its *linked* addresses (guest load bias
    //    0); the host cannot necessarily map there (macOS pagezero bans the
    //    low 4GB), so the image is reserved at a host-chosen address and the
    //    guest->host bias is installed in GuestMemory ("memory_base" mode).
    //    Every guest memory access (JIT pt register, interpreter, syscall
    //    layer, instruction fetch) applies that bias.
    //  - ET_DYN (static PIE): self-relocating, no absolute-address problem,
    //    so it stays on the proven identity path for now (see below).
    VAddr guest_base = 0;
    if (elf_type == ELFIO::ET_EXEC) {
        if (!memory->MapImageAnywhere(span_start, span)) {
            PANIC("Failed to reserve guest address span for image! file = {}", path);
        }
        LOG_INFO("ET_EXEC loaded in memory_base (bias) mode: guest {:#x} host_bias {:#x}",
                 span_start,
                 memory->GetBias());
    } else {
        // Static PIE: self-relocating, so it has no absolute-address problem
        // and stays on the proven identity path (guest addr == host addr,
        // Config::memory_base = nullptr, zero-overhead fast path).
        // TODO: move PIE onto the bias path as well once the x86 frontend's
        // rep movs/stos host helpers (decoder.cc RepMovs/RepStos*) translate
        // guest pointers — they currently dereference them directly, which
        // only works identity mapped.
        auto base = memory->MapAnywhere(span);
        if (!base) {
            PANIC("Failed to reserve guest address span for image! file = {}", path);
        }
        guest_base = base - span_start;
        LOG_INFO("ET_DYN loaded identity-mapped: guest base {:#x}", guest_base);
    }

    // Copy segment file contents into the reservation (guest addresses; the
    // anonymous reservation already zero-fills .bss, p_memsz > p_filesz).
    for (const auto& seg : segments) {
        if (seg->get_type() != ELFIO::PT_LOAD) continue;
        const VAddr dst = guest_base + seg->get_virtual_address();
        if (seg->get_file_size() > 0) {
            memory->WriteBytes(dst, {reinterpret_cast<const u8*>(seg->get_data()), seg->get_file_size()});
        }
        LOG_INFO("Loaded segment guest {:#x} (file {:#x} / mem {:#x})",
                 dst,
                 seg->get_file_size(),
                 seg->get_memory_size());
    }

    // Locate the program header table inside the loaded image for AT_PHDR.
    const u64 phoff = reader.get_segments_offset();
    const u64 phnum = reader.segments.size();
    const u64 phentsize = reader.get_segment_entry_size();
    VAddr phdr_addr = 0;
    for (const auto& seg : segments) {
        if (seg->get_type() != ELFIO::PT_LOAD) continue;
        const u64 seg_off = seg->get_offset();
        if (phoff >= seg_off && phoff + phnum * phentsize <= seg_off + seg->get_file_size()) {
            phdr_addr = guest_base + seg->get_virtual_address() + (phoff - seg_off);
            break;
        }
    }
    if (!phdr_addr) {
        PANIC("Program headers are not covered by any PT_LOAD segment! file = {}", path);
    }

    LoadedImage image{};
    // Resolve to an absolute path so that readlink("/proc/self/exe") always
    // returns an absolute path — glibc's _dl_get_origin asserts linkval[0]=='/'
    // and aborts (exit 134) if the value is relative.
    if (char* resolved = realpath(path.c_str(), nullptr)) {
        image.path = resolved;
        free(resolved);
    } else {
        image.path = path;  // realpath failed (e.g. file already deleted); keep as-is
    }
    image.isa = isa;
    image.entry = guest_base + reader.get_entry();
    image.load_bias = guest_base;
    image.phdr = phdr_addr;
    image.phentsize = phentsize;
    image.phnum = phnum;
    image.brk_start = guest_base + span_end;
    LOG_INFO("Guest image loaded: entry {:#x} guest_bias {:#x} host_bias {:#x} phdr {:#x} phnum {} brk {:#x}",
             image.entry,
             image.load_bias,
             memory->GetBias(),
             image.phdr,
             image.phnum,
             image.brk_start);
    return image;
}

VAddr SetupInitialStack(GuestMemory& memory,
                        const LoadedImage& image,
                        const std::vector<std::string>& args,
                        const std::vector<std::string>& envs) {
    // Prefer the classic high guest address; if the corresponding host range
    // (guest + bias) is not mappable, take any host range and translate it
    // back to a guest address.
    VAddr stack_top = kGuestStackTop;
    if (!memory.MapFixed(stack_top - kGuestStackSize, kGuestStackSize)) {
        stack_top = 0;
    }
    if (!stack_top) {
        auto base = memory.MapAnywhere(kGuestStackSize);
        if (!base) {
            PANIC("Failed to map guest main stack");
        }
        stack_top = base + kGuestStackSize;
        LOG_WARNING("Guest stack placed at host-chosen guest {:#x}", stack_top);
    }

    VAddr sp = stack_top;

    // Push blobs (strings, AT_RANDOM payload) top-down first.
    auto push_bytes = [&](const void* data, size_t len) {
        sp -= len;
        memory.WriteBytes(sp, {reinterpret_cast<const u8*>(data), len});
        return sp;
    };
    auto push_string = [&](const std::string& str) { return push_bytes(str.data(), str.size() + 1); };

    u8 random_bytes[16];
    arc4random_buf(random_bytes, sizeof(random_bytes));
    const VAddr random_ptr = push_bytes(random_bytes, sizeof(random_bytes));

    const VAddr execfn_ptr = push_string(image.path);

    std::vector<VAddr> env_ptrs;
    env_ptrs.reserve(envs.size());
    for (const auto& env : envs) {
        env_ptrs.push_back(push_string(env));
    }

    std::vector<VAddr> arg_ptrs;
    arg_ptrs.reserve(args.size());
    for (const auto& arg : args) {
        arg_ptrs.push_back(push_string(arg));
    }

    const std::pair<GuestAuxv, u64> auxv[] = {
            {AT_PHDR, image.phdr},
            {AT_PHENT, image.phentsize},
            {AT_PHNUM, image.phnum},
            {AT_PAGESZ, GuestMemory::kGuestPageSize},
            {AT_BASE, 0},  // no interpreter (static)
            {AT_FLAGS, 0},
            {AT_ENTRY, image.entry},
            {AT_UID, 1000},
            {AT_EUID, 1000},
            {AT_GID, 1000},
            {AT_EGID, 1000},
            {AT_HWCAP, 0},
            {AT_CLKTCK, 100},
            {AT_SECURE, 0},
            {AT_RANDOM, random_ptr},
            {AT_EXECFN, execfn_ptr},
    };

    // Vector table: argc, argv..., NULL, envp..., NULL, auxv pairs..., AT_NULL.
    const u64 word_count = 1 + arg_ptrs.size() + 1 + env_ptrs.size() + 1 + 2 * std::size(auxv) + 2;
    sp = RoundDown(sp - word_count * sizeof(u64), static_cast<u64>(16));

    auto cursor = static_cast<u64*>(memory.ToHost(sp));
    auto push_word = [&](u64 value) { *cursor++ = value; };

    push_word(arg_ptrs.size());
    for (const auto ptr : arg_ptrs) push_word(ptr);
    push_word(0);
    for (const auto ptr : env_ptrs) push_word(ptr);
    push_word(0);
    for (const auto& [type, value] : auxv) {
        push_word(type);
        push_word(value);
    }
    push_word(AT_NULL);
    push_word(0);

    LOG_INFO("Guest stack ready: sp {:#x} argc {}", sp, arg_ptrs.size());
    return sp;
}

}  // namespace swift::linux
