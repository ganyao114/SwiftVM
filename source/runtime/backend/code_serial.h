//
// Serialization + relocation of compiled host code.
//
// This component is deliberately independent of the JIT disk cache that is
// its first consumer: the AOT pre-compiler (docs/aot-design.md) needs exactly
// the same three capabilities and is meant to link against this file directly.
//
//   1. Take a finished host code unit (the bytes JitContext::Flush would copy
//      into a CodeCache) and discover every position in it that depends on a
//      *runtime* address -- ScanCodeUnit().
//   2. Serialize the bytes + the relocation list + the guest-side identity of
//      the unit -- SerialUnit / BlobWriter / BlobReader.
//   3. Re-bind the unit against a new set of runtime addresses --
//      ApplyRelocations().
//
// plus the validity key (ValidityKey) that decides whether a serialized unit
// may be revived at all.
//
// ---------------------------------------------------------------------------
// Why post-emission decoding is exact here
// ---------------------------------------------------------------------------
// docs/aot-design.md §5 warns that scanning cannot tell "a bl" from "a
// constant that happens to look like a bl". That is true for instruction
// streams with embedded data. It is not true here, and ScanCodeUnit enforces
// the preconditions that make it not true:
//
//   * AArch64 is fixed-width, and a compiled unit is one contiguous stream
//     starting at offset 0, so every 4-byte word is an instruction.
//   * VIXL's MacroAssembler::Mov(Register, uint64_t) never uses a literal
//     pool (it is movz/movn/movk/orr-immediate only -- see
//     MoveImmediateHelper / OneInstrMoveImmediateHelper), and the backend
//     emits no other literal. ScanCodeUnit *rejects* any unit that contains a
//     literal load, an adr/adrp, or an out-of-unit pc-relative branch other
//     than a caller-declared direct-link BL site. Those declared words are
//     generated from center-table metadata, not inferred from arbitrary code.
//   * A materialized constant that lands inside the SwiftVM host image but is
//     not consumed by a modelled use (an indirect branch target or a
//     load/store base) also rejects the unit. Missing a *use* class therefore
//     costs cache coverage, never correctness.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "runtime/common/types.h"
#include "runtime/include/config.h"

namespace swift::runtime::backend {

struct ModuleConfig;

// --------------------------------------------------------------------------
// Host image identity
// --------------------------------------------------------------------------
// Every absolute host address the backend bakes into generated code (host
// helpers reached through CallLambda, &HostMemMove, &X87Dispatch,
// &unaligned_atomic_lock, ...) lives in the SwiftVM main image, which is
// position independent. One slide therefore rebases all of them.
struct HostImageInfo {
    std::uintptr_t base{};
    std::size_t size{};

    [[nodiscard]] bool Contains(u64 value) const {
        return size != 0 && value >= base && value < base + size;
    }
};

// Mapped span of the SwiftVM main executable in this process. Computed once.
const HostImageInfo& GetHostImage();

// --------------------------------------------------------------------------
// Relocations
// --------------------------------------------------------------------------
enum class RelocKind : u16 {
    None = 0,
    // A movz/movn/movk run materializing (host image base + addend) into a
    // general purpose register. Re-materialized in place at load time; the
    // instruction count is preserved because the sequence is rewritten with
    // the same number of movz/movk slots that the original occupied.
    HostImageAbs64 = 1,
};

enum class RelocUse : u16 {
    Unknown = 0,
    BranchTarget = 1,  // br / blr on the materialized register
    MemoryBase = 2,    // base register of a load/store
};

struct Relocation {
    u32 code_offset{};      // byte offset of the first instruction of the run
    u16 inst_count{};       // 4-byte instructions forming the run
    u16 reg{};              // destination register code (verification)
    RelocKind kind{RelocKind::None};
    RelocUse use{RelocUse::Unknown};
    u64 addend{};           // value - host_image_base at record time
    u64 recorded_value{};   // full value at record time (verification)
};

struct ScanResult {
    bool ok{false};
    std::string reject_reason{};
    std::vector<Relocation> relocs{};
    // Diagnostics: number of constant materializations seen, and how many
    // landed inside the host image.
    u32 materializations{};
};

// Decode `code` (a whole compiled unit, 4-byte aligned, size % 4 == 0) and
// produce its relocation list. `guest_window_size` is the size of the bounded
// guest address window (Config::guest_addr_mask + 1, or 0 when unbounded); it
// is used only for the disjointness precondition described in the header
// comment.
ScanResult ScanCodeUnit(std::span<const u8> code,
                        const HostImageInfo& image,
                        u64 guest_window_size,
                        std::span<const u32> external_bl_offsets = {});

// Rewrite the movz/movk runs listed in `relocs` so they materialize
// (image.base + addend). `rw_code` must point at a writable alias of the unit
// (CodeBuffer::rw_data); the caller flushes the caches afterwards.
bool ApplyRelocations(u8* rw_code,
                      std::size_t code_size,
                      std::span<const Relocation> relocs,
                      const HostImageInfo& image,
                      std::string* error);

// --------------------------------------------------------------------------
// Serialized unit
// --------------------------------------------------------------------------
// One guest basic block inside a compiled unit: its guest byte range, the
// offset of its entry point inside the unit's host code, and the hash of the
// guest bytes the translation was produced from.
struct SerialBlock {
    u64 guest_start{};
    u64 guest_end{};
    u32 code_offset{};
    u64 guest_bytes_hash{};
};

// One direct-link branch site inside the unit. The code byte at
// `code_offset` is always serialized as the unlinked `bl region_tramp` form;
// its process-relative immediate is rewritten again when the unit is revived.
// kind stores LinkSiteKind as a byte without coupling the generic serializer
// to LinkManager's runtime-only data structures.
struct SerialLinkSite {
    u32 code_offset{};
    u64 guest_target{};
    u8 kind{};
};

struct SerialUnit {
    u64 guest_start{};
    u8 is_function{};
    std::vector<u8> code{};
    std::vector<SerialBlock> blocks{};
    std::vector<Relocation> relocs{};
    std::vector<SerialLinkSite> link_sites{};
};

// --------------------------------------------------------------------------
// Blob I/O
// --------------------------------------------------------------------------
class BlobWriter {
public:
    void U8(u8 v);
    void U16(u16 v);
    void U32(u32 v);
    void U64(u64 v);
    void Bytes(const void* p, std::size_t n);
    [[nodiscard]] const std::vector<u8>& Data() const { return buf; }
    [[nodiscard]] std::size_t Size() const { return buf.size(); }

private:
    std::vector<u8> buf{};
};

class BlobReader {
public:
    BlobReader(const u8* data, std::size_t size) : p(data), end(data + size) {}

    bool U8(u8& v);
    bool U16(u16& v);
    bool U32(u32& v);
    bool U64(u64& v);
    bool Bytes(void* out, std::size_t n);
    [[nodiscard]] bool Empty() const { return p >= end; }
    [[nodiscard]] std::size_t Remaining() const { return static_cast<std::size_t>(end - p); }

private:
    const u8* p;
    const u8* end;
};

void WriteUnit(BlobWriter& w, const SerialUnit& unit);
bool ReadUnit(BlobReader& r, SerialUnit& unit);

// --------------------------------------------------------------------------
// Validity key
// --------------------------------------------------------------------------
// Everything that can change the *meaning* of a serialized unit. A mismatch
// on any field must reject the whole file.
//
// format_version  bumped whenever this file's layout or the scanner changes
// build_id        identity of the SwiftVM binary that produced the code; it
//                 subsumes every compile-time constant (State offsets,
//                 ScratchBudget, opcode semantics, emitter changes)
// config_hash     the runtime Config fields that reach codegen
// env_hash        every SVM_*/SWIFT_* environment variable, by raw value
// guest_id        identity of the guest image (naming/coarse check only; the
//                 authoritative guest check is SerialBlock::guest_bytes_hash)
struct ValidityKey {
    u64 format_version{};
    u64 build_id{};
    u64 config_hash{};
    u64 env_hash{};
    u64 guest_id{};

    bool operator==(const ValidityKey&) const = default;
};

constexpr u64 kCacheFormatVersion = 4;

u64 HashBytes(const void* data, std::size_t size, u64 seed);
u64 HashU64(u64 value, u64 seed);

// Identity of the running SwiftVM binary: its on-disk size + mtime + inode,
// which changes on every relink. Falls back to a mapped-image content hash if
// the executable path cannot be resolved.
u64 ComputeBuildId();

// Hash of every environment variable whose name starts with SVM_ or SWIFT_,
// sorted by name, raw values, excluding the cache's own SVM_JIT_CACHE* knobs.
// Hashing raw strings rather than interpreted semantics is deliberate: it can
// only over-invalidate, and it automatically covers switches added later.
u64 ComputeEnvHash();

// Hash of every Config field that reaches codegen. memory_base contributes
// only its page offset and nullness -- its value lives in a register (pt) and
// never in an instruction immediate.
u64 ComputeConfigHash(const Config& config);
u64 ComputeConfigHash(const Config& config, const ModuleConfig& module_config);

// Identity of the guest image. By default this preserves the legacy all-argv
// hash. SVM_JIT_CACHE_EXEC_ID=1 instead uses only argv[1]'s path + file
// identity, with a separate hash domain; argv[0] is already covered by
// build_id and argv[2..] are guest data. Used to name the cache file and as a
// coarse header check; SerialBlock::guest_bytes_hash is the authoritative
// load-time safety check.
u64 ComputeGuestId();

}  // namespace swift::runtime::backend
