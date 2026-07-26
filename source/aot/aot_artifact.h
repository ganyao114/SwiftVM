//
// In-memory form of an AOT artifact plus its ELF and blob codecs.
// See aot_format.h for the layout and the design rationale.
//
#pragma once

#include <string>
#include <vector>
#include "aot/aot_format.h"
#include "runtime/backend/code_serial.h"
#include "runtime/common/types.h"

namespace swift::aot {

using swift::runtime::backend::Relocation;
using swift::runtime::backend::SerialBlock;
using swift::runtime::backend::ValidityKey;

// One compiled guest function. `code_offset`/`code_size` index AotImage::code
// rather than carrying the bytes inline: the bytes live in `.svmaot.text` so
// the rewritten symbols point at something real, and a second copy in the
// metadata blob would be a second thing that can disagree.
struct AotUnit {
    u64 guest_start{};
    u8 is_function{1};
    u32 code_offset{};
    u32 code_size{};
    std::vector<SerialBlock> blocks{};
    std::vector<Relocation> relocs{};
};

// A PT_LOAD of the guest ELF, carried verbatim. `data.size()` is p_filesz;
// [vaddr + filesz, vaddr + memsz) is zero (.bss).
struct AotGuestSegment {
    u64 vaddr{};
    u64 memsz{};
    u32 flags{};  // p_flags
    std::vector<u8> data{};
};

// What the loader needs to rebuild swift::linux::LoadedImage without the
// original ELF.
struct AotGuestImageInfo {
    u64 entry{};
    u64 phdr{};
    u64 phentsize{};
    u64 phnum{};
    u64 brk_start{};
    // realpath() of the guest ELF at compile time. It reaches the guest as
    // AT_EXECFN and as the answer to readlink("/proc/self/exe"), so it is part
    // of the initial state and has to be reproduced, not re-derived from
    // wherever the artifact happens to sit.
    std::string path{};
};

// Compile-time census, reported by the tool and kept in the blob so `info`
// can print it without re-deriving anything.
struct AotStats {
    u32 symbols_seen{};        // STT_FUNC entries in the guest .symtab
    u32 symbols_zero_size{};   // of those, st_size == 0
    u32 addrs_attempted{};     // distinct entry addresses we tried
    u32 units_emitted{};       // distinct compiled units in the artifact
    u32 fail_translate{};      // addresses the function path could not compile
    u32 fail_block_mode{};     // compiled, but as a block unit (not serializable)
    u32 fail_scan{};           // ScanCodeUnit refused the unit
    u32 fail_guest_hash{};     // guest bytes not readable through the window
    u32 symbols_stubbed{};     // STT_FUNC pointed at the failure stub
};

struct AotImage {
    ValidityKey key{};  // key.guest_id is the guest *content* hash, see below
    AotGuestImageInfo guest{};
    AotStats stats{};

    // Concatenated compiled units, 16-byte aligned, failure stub last.
    std::vector<u8> code{};
    u32 stub_offset{};
    // Content hash of `code`. The metadata blob carries its own checksum, so
    // without this a corrupted `.svmaot.text` would be the one damaged part of
    // the artifact that loads happily and then executes garbage
    // (docs/aot-design.md §7.5 wants "corrupted artifact" rejected, not
    // survived).
    u64 code_hash{};

    std::vector<AotUnit> units{};
    std::vector<AotGuestSegment> segments{};
    // L2 dispatch-table assignment observed while compiling. A slot index is
    // not a function of the key (colliding keys probe forward), and generated
    // code branches through raw slot indices, so the assignment is replayed
    // verbatim at load. In a fresh process the table starts empty, which
    // makes the replay conflict-free unless the artifact itself is corrupt.
    std::vector<std::pair<u64, u32>> dispatch_slots{};
};

// Content hash of the guest image: every PT_LOAD descriptor and its file
// bytes, plus the entry point. This is what binds an artifact to one guest
// binary; it replaces code_serial's ComputeGuestId(), which hashes argv and
// would differ between `svm_aot compile <guest>` and `svm_aot run <artifact>`.
u64 HashGuestImage(const std::vector<AotGuestSegment>& segments, u64 entry);

// --------------------------------------------------------------------------
// Blob codec (the payload of `.svmaot.info`)
// --------------------------------------------------------------------------
std::vector<u8> EncodeInfoBlob(const AotImage& image);
bool DecodeInfoBlob(const u8* data, std::size_t size, AotImage& out, std::string& error);

// --------------------------------------------------------------------------
// ELF writer / reader
// --------------------------------------------------------------------------
// Reads `guest_elf_path`, copies its sections and symbols, rewrites every
// STT_FUNC per `image`, and writes the artifact to `out_path`.
bool WriteArtifact(const std::string& guest_elf_path,
                   const AotImage& image,
                   const std::string& out_path,
                   std::string& error);

// Reads back an artifact written by WriteArtifact. Only the AOT sections are
// decoded; the copied guest sections are ignored (the loader maps
// `.svmaot.segN`, which the writer cross-checked against them).
bool ReadArtifact(const std::string& path, AotImage& out, std::string& error);

}  // namespace swift::aot
