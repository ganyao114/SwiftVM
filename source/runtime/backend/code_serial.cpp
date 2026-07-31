//
// See code_serial.h for the design rationale.
//

#include "runtime/backend/code_serial.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <sys/stat.h>

#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#else
#include <link.h>
#include <cstdio>
#endif

#include <dlfcn.h>
#include <unistd.h>

extern "C" char** environ;

namespace swift::runtime::backend {

// ==========================================================================
// Host image identity
// ==========================================================================
namespace {

// Anchor whose address is guaranteed to be inside the SwiftVM main image.
void HostImageAnchor() {}

HostImageInfo ProbeHostImage() {
    HostImageInfo info{};
#ifdef __APPLE__
    // Image 0 is always the main executable.
    const auto* header = _dyld_get_image_header(0);
    if (!header) {
        return info;
    }
    const auto slide = static_cast<std::uintptr_t>(_dyld_get_image_vmaddr_slide(0));
    const auto* mh = reinterpret_cast<const mach_header_64*>(header);
    if (mh->magic != MH_MAGIC_64) {
        return info;
    }
    std::uintptr_t lo = UINTPTR_MAX;
    std::uintptr_t hi = 0;
    const auto* cmd = reinterpret_cast<const load_command*>(mh + 1);
    for (u32 i = 0; i < mh->ncmds; ++i) {
        if (cmd->cmd == LC_SEGMENT_64) {
            const auto* seg = reinterpret_cast<const segment_command_64*>(cmd);
            if (seg->vmsize != 0 && std::strcmp(seg->segname, "__PAGEZERO") != 0) {
                lo = std::min<std::uintptr_t>(lo, seg->vmaddr + slide);
                hi = std::max<std::uintptr_t>(hi, seg->vmaddr + seg->vmsize + slide);
            }
        }
        cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const u8*>(cmd) +
                                                   cmd->cmdsize);
    }
    if (lo == UINTPTR_MAX || hi <= lo) {
        return info;
    }
    info.base = lo;
    info.size = hi - lo;
#else
    struct Ctx {
        std::uintptr_t lo{UINTPTR_MAX};
        std::uintptr_t hi{0};
        bool done{false};
    } ctx;
    dl_iterate_phdr(
            [](struct dl_phdr_info* pi, size_t, void* raw) -> int {
                auto* c = static_cast<Ctx*>(raw);
                // The first entry is the main program.
                for (int i = 0; i < pi->dlpi_phnum; ++i) {
                    const auto& ph = pi->dlpi_phdr[i];
                    if (ph.p_type != PT_LOAD) continue;
                    const auto start = pi->dlpi_addr + ph.p_vaddr;
                    c->lo = std::min<std::uintptr_t>(c->lo, start);
                    c->hi = std::max<std::uintptr_t>(c->hi, start + ph.p_memsz);
                }
                c->done = true;
                return 1;  // stop after the main object
            },
            &ctx);
    if (!ctx.done || ctx.hi <= ctx.lo) {
        return info;
    }
    info.base = ctx.lo;
    info.size = ctx.hi - ctx.lo;
#endif
    // Sanity: the anchor must be inside what we computed. If not, the probe is
    // wrong and every consumer must treat the cache as unavailable.
    const auto anchor = reinterpret_cast<std::uintptr_t>(&HostImageAnchor);
    if (!info.Contains(anchor)) {
        return HostImageInfo{};
    }
    return info;
}

}  // namespace

const HostImageInfo& GetHostImage() {
    static const HostImageInfo info = ProbeHostImage();
    return info;
}

// ==========================================================================
// AArch64 instruction decoding
// ==========================================================================
namespace {

constexpr u32 kInstSize = 4;

inline u32 ReadInst(std::span<const u8> code, std::size_t offset) {
    u32 v;
    std::memcpy(&v, code.data() + offset, sizeof(v));
    return v;
}

// --- move wide immediate (movn/movz/movk) ---------------------------------
// sf | opc(2) | 100101 | hw(2) | imm16 | Rd
inline bool IsMoveWide(u32 insn) { return (insn & 0x1F800000u) == 0x12800000u; }
inline u32 MoveWideOpc(u32 insn) { return (insn >> 29) & 0x3u; }  // 0=MOVN 2=MOVZ 3=MOVK
inline u32 MoveWideSf(u32 insn) { return (insn >> 31) & 0x1u; }
inline u32 MoveWideShift(u32 insn) { return ((insn >> 21) & 0x3u) * 16u; }
inline u32 MoveWideImm16(u32 insn) { return (insn >> 5) & 0xFFFFu; }
inline u32 MoveWideRd(u32 insn) { return insn & 0x1Fu; }

inline u32 EncodeMoveWide(u32 sf, u32 opc, u32 shift, u32 imm16, u32 rd) {
    return (sf << 31) | (opc << 29) | (0x25u << 23) | ((shift / 16u) << 21) |
           ((imm16 & 0xFFFFu) << 5) | (rd & 0x1Fu);
}

// --- ORR (immediate), used by VIXL for logical-immediate constants --------
// sf | 01 | 100100 | N | immr | imms | Rn | Rd ; Rn == 31 makes it a "mov"
inline bool IsOrrImm(u32 insn) { return (insn & 0x7F800000u) == 0x32000000u; }
inline u32 OrrRn(u32 insn) { return (insn >> 5) & 0x1Fu; }
inline u32 OrrRd(u32 insn) { return insn & 0x1Fu; }

// --- unconditional branch to register (br/blr/ret) ------------------------
inline bool IsBrOrBlr(u32 insn) {
    return (insn & 0xFFFFFC1Fu) == 0xD61F0000u || (insn & 0xFFFFFC1Fu) == 0xD63F0000u;
}
inline u32 BranchRegRn(u32 insn) { return (insn >> 5) & 0x1Fu; }

// --- loads and stores -----------------------------------------------------
// Top-level group: bits 28..25 == x1x0. Rn (base) is always bits 9..5.
inline bool IsLoadStoreGroup(u32 insn) { return (insn & 0x0A000000u) == 0x08000000u; }
inline u32 LoadStoreRn(u32 insn) { return (insn >> 5) & 0x1Fu; }
// LDR (literal) / LDRSW (literal) / prefetch (literal) / SIMD LDR (literal).
inline bool IsLiteralLoad(u32 insn) {
    const u32 masked = insn & 0x3B000000u;
    return masked == 0x18000000u || masked == 0x1C000000u;
}

// --- pc-relative ----------------------------------------------------------
inline bool IsAdr(u32 insn) { return (insn & 0x9F000000u) == 0x10000000u; }
inline bool IsAdrp(u32 insn) { return (insn & 0x9F000000u) == 0x90000000u; }

// --- pc-relative branches -------------------------------------------------
inline bool IsBImm(u32 insn) { return (insn & 0x7C000000u) == 0x14000000u; }  // b / bl
inline s64 BImmOffset(u32 insn) {
    s32 imm26 = static_cast<s32>(insn & 0x03FFFFFFu);
    imm26 = (imm26 << 6) >> 6;  // sign extend
    return static_cast<s64>(imm26) * 4;
}
inline bool IsCondBranch(u32 insn) { return (insn & 0xFF000010u) == 0x54000000u; }
inline bool IsCompareBranch(u32 insn) { return (insn & 0x7E000000u) == 0x34000000u; }
inline s64 Imm19Offset(u32 insn) {
    s32 imm19 = static_cast<s32>((insn >> 5) & 0x7FFFFu);
    imm19 = (imm19 << 13) >> 13;
    return static_cast<s64>(imm19) * 4;
}
inline bool IsTestBranch(u32 insn) { return (insn & 0x7E000000u) == 0x36000000u; }
inline s64 Imm14Offset(u32 insn) {
    s32 imm14 = static_cast<s32>((insn >> 5) & 0x3FFFu);
    imm14 = (imm14 << 18) >> 18;
    return static_cast<s64>(imm14) * 4;
}

// Pending constant materialization tracked per destination register.
struct Pending {
    enum class State : u8 { Empty, Known, Opaque };
    State state{State::Empty};
    u64 value{};
    u32 first_offset{};
    u32 inst_count{};
    bool consumed{};  // saw a modelled use (branch target or memory base)
};

}  // namespace

ScanResult ScanCodeUnit(std::span<const u8> code,
                        const HostImageInfo& image,
                        u64 guest_window_size) {
    ScanResult result{};
    if (code.size() % kInstSize != 0 || code.empty()) {
        result.reject_reason = "unit size is not a multiple of 4";
        return result;
    }
    if (image.size == 0) {
        result.reject_reason = "host image span unknown";
        return result;
    }
    // Precondition that makes "value inside the host image" unambiguous for
    // guest *address* constants: the guest window must not overlap the image.
    if (guest_window_size != 0 && image.base < guest_window_size) {
        result.reject_reason = "guest window overlaps the host image";
        return result;
    }

    const std::size_t n_inst = code.size() / kInstSize;
    std::array<Pending, 32> pending{};
    // Materializations that landed inside the host image, in program order.
    struct Candidate {
        u32 reg;
        u32 first_offset;
        u32 inst_count;
        u64 value;
        RelocUse use;
    };
    std::vector<Candidate> candidates;

    auto invalidate = [&](u32 reg) {
        if (reg < 31) {
            pending[reg] = Pending{};
        }
    };

    for (std::size_t i = 0; i < n_inst; ++i) {
        const u32 offset = static_cast<u32>(i * kInstSize);
        const u32 insn = ReadInst(code, offset);

        // ---- reject classes we cannot serialize ---------------------------
        if (IsAdr(insn) || IsAdrp(insn)) {
            result.reject_reason = "unit contains adr/adrp";
            return result;
        }
        if (IsLiteralLoad(insn)) {
            result.reject_reason = "unit contains a literal-pool load";
            return result;
        }
        // pc-relative branches must stay inside the unit.
        {
            std::optional<s64> rel;
            if (IsBImm(insn)) {
                rel = BImmOffset(insn);
            } else if (IsCondBranch(insn) || IsCompareBranch(insn)) {
                rel = Imm19Offset(insn);
            } else if (IsTestBranch(insn)) {
                rel = Imm14Offset(insn);
            }
            if (rel) {
                const s64 target = static_cast<s64>(offset) + *rel;
                if (target < 0 || target >= static_cast<s64>(code.size())) {
                    result.reject_reason = "pc-relative branch leaves the unit";
                    return result;
                }
            }
        }

        // ---- constant materialization -------------------------------------
        if (IsMoveWide(insn)) {
            const u32 rd = MoveWideRd(insn);
            if (rd >= 31) {  // xzr/wzr: no state
                continue;
            }
            const u32 opc = MoveWideOpc(insn);
            const u32 sf = MoveWideSf(insn);
            const u64 field = static_cast<u64>(MoveWideImm16(insn)) << MoveWideShift(insn);
            const u64 mask = ~(static_cast<u64>(0xFFFFu) << MoveWideShift(insn));
            if (opc == 3) {  // MOVK: keeps the rest of the register
                if (pending[rd].state == Pending::State::Known) {
                    pending[rd].value = (pending[rd].value & mask) | field;
                    pending[rd].inst_count++;
                } else {
                    // movk without a preceding movz in this unit: the register
                    // holds a runtime value we cannot reason about.
                    pending[rd] = Pending{Pending::State::Opaque, 0, offset, 1, false};
                }
            } else {
                u64 value;
                if (opc == 2) {  // MOVZ
                    value = field;
                } else {  // MOVN
                    value = ~field;
                }
                if (sf == 0) {
                    value &= 0xFFFFFFFFull;
                }
                pending[rd] = Pending{Pending::State::Known, value, offset, 1, false};
            }
            result.materializations++;
            continue;
        }
        if (IsOrrImm(insn) && OrrRn(insn) == 31) {
            // VIXL's one-instruction logical-immediate move. Decoding
            // DecodeBitMasks here would buy nothing: a host address is never a
            // logical immediate, so marking the register Opaque only costs
            // coverage in a case that does not occur.
            const u32 rd = OrrRd(insn);
            if (rd < 31) {
                pending[rd] = Pending{Pending::State::Opaque, 0, offset, 1, false};
            }
            result.materializations++;
            continue;
        }

        // ---- modelled uses -------------------------------------------------
        if (IsBrOrBlr(insn)) {
            const u32 rn = BranchRegRn(insn);
            if (rn < 31 && pending[rn].state != Pending::State::Empty) {
                if (pending[rn].state == Pending::State::Opaque) {
                    result.reject_reason = "indirect branch through an undecodable constant";
                    return result;
                }
                if (!image.Contains(pending[rn].value)) {
                    // A constant branch target outside the host image is a
                    // direct link to another JIT code buffer (Optimizations::
                    // DirectBlockLink / read-only module). Those addresses are
                    // not reproducible across runs.
                    result.reject_reason = "constant branch target outside the host image";
                    return result;
                }
                pending[rn].consumed = true;
                candidates.push_back({rn, pending[rn].first_offset, pending[rn].inst_count,
                                      pending[rn].value, RelocUse::BranchTarget});
                pending[rn] = Pending{};
            }
            continue;
        }
        if (IsLoadStoreGroup(insn)) {
            const u32 rn = LoadStoreRn(insn);
            if (rn < 31 && pending[rn].state == Pending::State::Known &&
                image.Contains(pending[rn].value) && !pending[rn].consumed) {
                pending[rn].consumed = true;
                candidates.push_back({rn, pending[rn].first_offset, pending[rn].inst_count,
                                      pending[rn].value, RelocUse::MemoryBase});
            }
            // A load/store may write its Rt; conservatively drop every
            // materialization it could clobber. Rt is bits 4..0, Rt2 bits
            // 14..10 for pair forms, and the write-back forms update Rn.
            invalidate(insn & 0x1Fu);
            if ((insn & 0x3A000000u) == 0x28000000u) {  // load/store pair
                invalidate((insn >> 10) & 0x1Fu);
            }
            continue;
        }

        // ---- any other instruction: drop the destination's materialization -
        // Rd is bits 4..0 for every data-processing encoding; being generous
        // here (dropping on instructions that write nothing) is safe, it only
        // shortens the window in which a constant is recognized.
        invalidate(insn & 0x1Fu);
    }

    // Completeness guard: a host-image constant that no modelled use consumed
    // means the scanner does not understand how this unit uses it. Refuse the
    // unit rather than leave it unrelocated.
    for (u32 reg = 0; reg < 31; ++reg) {
        if (pending[reg].state == Pending::State::Known && !pending[reg].consumed &&
            image.Contains(pending[reg].value)) {
            result.reject_reason = "host-image constant with no modelled use";
            return result;
        }
    }

    result.relocs.reserve(candidates.size());
    for (const auto& c : candidates) {
        Relocation r{};
        r.code_offset = c.first_offset;
        r.inst_count = static_cast<u16>(c.inst_count);
        r.reg = static_cast<u16>(c.reg);
        r.kind = RelocKind::HostImageAbs64;
        r.use = c.use;
        r.addend = c.value - image.base;
        r.recorded_value = c.value;
        result.relocs.push_back(r);
    }
    result.ok = true;
    return result;
}

bool ApplyRelocations(u8* rw_code,
                      std::size_t code_size,
                      std::span<const Relocation> relocs,
                      const HostImageInfo& image,
                      std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (image.size == 0) {
        return fail("host image span unknown");
    }
    for (const auto& r : relocs) {
        if (r.kind != RelocKind::HostImageAbs64) {
            return fail("unknown relocation kind");
        }
        if (r.inst_count == 0 || r.inst_count > 4 || r.reg >= 31) {
            return fail("malformed relocation");
        }
        const std::size_t end = static_cast<std::size_t>(r.code_offset) +
                                static_cast<std::size_t>(r.inst_count) * kInstSize;
        if (r.code_offset % kInstSize != 0 || end > code_size) {
            return fail("relocation out of range");
        }
        if (r.addend >= image.size) {
            return fail("relocation addend outside the host image");
        }
        // Verify the bytes still say what we recorded: the run must decode
        // back to recorded_value with the recorded register.
        u64 observed = 0;
        bool first = true;
        for (u16 k = 0; k < r.inst_count; ++k) {
            u32 insn;
            std::memcpy(&insn, rw_code + r.code_offset + k * kInstSize, sizeof(insn));
            if (!IsMoveWide(insn) || MoveWideRd(insn) != r.reg) {
                return fail("relocation site is not a move-wide run");
            }
            const u32 opc = MoveWideOpc(insn);
            const u64 field = static_cast<u64>(MoveWideImm16(insn)) << MoveWideShift(insn);
            const u64 keep = ~(static_cast<u64>(0xFFFFu) << MoveWideShift(insn));
            if (first) {
                if (opc == 2) {
                    observed = field;
                } else if (opc == 0) {
                    observed = ~field;
                } else {
                    return fail("relocation run does not start with movz/movn");
                }
                if (MoveWideSf(insn) == 0) {
                    observed &= 0xFFFFFFFFull;
                }
                first = false;
            } else {
                if (opc != 3) {
                    return fail("relocation run has a non-movk continuation");
                }
                observed = (observed & keep) | field;
            }
        }
        if (observed != r.recorded_value) {
            return fail("relocation site value does not match the record");
        }

        // Re-materialize. The new value must fit the same number of slots; it
        // always does, because both values live inside one image whose span is
        // far below 2^48 and the run is rewritten slot by slot with explicit
        // movz+movk (never the movn form).
        const u64 value = image.base + r.addend;
        // movz+movk only needs the non-zero halfwords.
        u32 nonzero = 0;
        for (u32 s = 0; s < 4; ++s) {
            if (((value >> (16 * s)) & 0xFFFFull) != 0) {
                nonzero++;
            }
        }
        if (nonzero == 0) {
            nonzero = 1;
        }
        if (nonzero > r.inst_count) {
            return fail("rebased value needs more move-wide slots than recorded");
        }
        u32 written = 0;
        bool emitted_first = false;
        for (u32 s = 0; s < 4 && written < r.inst_count; ++s) {
            const u32 half = static_cast<u32>((value >> (16 * s)) & 0xFFFFull);
            // Zero halfwords need no slot: the leading movz clears them, and a
            // movz #0 is emitted below only if the whole value is zero.
            if (half == 0) {
                continue;
            }
            const u32 insn = EncodeMoveWide(1, emitted_first ? 3u : 2u, s * 16, half, r.reg);
            std::memcpy(rw_code + r.code_offset + written * kInstSize, &insn, sizeof(insn));
            written++;
            emitted_first = true;
        }
        if (!emitted_first) {
            const u32 insn = EncodeMoveWide(1, 2u, 0, 0, r.reg);
            std::memcpy(rw_code + r.code_offset, &insn, sizeof(insn));
            written = 1;
        }
        // Pad any leftover slot with a movk of the halfword already written
        // there (an idempotent no-op) so the instruction count is preserved.
        for (u32 k = written; k < r.inst_count; ++k) {
            const u32 half = static_cast<u32>((value >> (16 * (k))) & 0xFFFFull);
            const u32 insn = EncodeMoveWide(1, 3u, (k) * 16, half, r.reg);
            std::memcpy(rw_code + r.code_offset + k * kInstSize, &insn, sizeof(insn));
        }
    }
    return true;
}

// ==========================================================================
// Blob I/O
// ==========================================================================
void BlobWriter::U8(u8 v) { buf.push_back(v); }
void BlobWriter::U16(u16 v) { Bytes(&v, sizeof(v)); }
void BlobWriter::U32(u32 v) { Bytes(&v, sizeof(v)); }
void BlobWriter::U64(u64 v) { Bytes(&v, sizeof(v)); }
void BlobWriter::Bytes(const void* p, std::size_t n) {
    const auto* b = static_cast<const u8*>(p);
    buf.insert(buf.end(), b, b + n);
}

bool BlobReader::U8(u8& v) { return Bytes(&v, sizeof(v)); }
bool BlobReader::U16(u16& v) { return Bytes(&v, sizeof(v)); }
bool BlobReader::U32(u32& v) { return Bytes(&v, sizeof(v)); }
bool BlobReader::U64(u64& v) { return Bytes(&v, sizeof(v)); }
bool BlobReader::Bytes(void* out, std::size_t n) {
    if (static_cast<std::size_t>(end - p) < n) {
        return false;
    }
    std::memcpy(out, p, n);
    p += n;
    return true;
}

void WriteUnit(BlobWriter& w, const SerialUnit& unit) {
    w.U64(unit.guest_start);
    w.U8(unit.is_function);
    w.U32(static_cast<u32>(unit.code.size()));
    w.Bytes(unit.code.data(), unit.code.size());
    w.U32(static_cast<u32>(unit.blocks.size()));
    for (const auto& b : unit.blocks) {
        w.U64(b.guest_start);
        w.U64(b.guest_end);
        w.U32(b.code_offset);
        w.U64(b.guest_bytes_hash);
    }
    w.U32(static_cast<u32>(unit.relocs.size()));
    for (const auto& r : unit.relocs) {
        w.U32(r.code_offset);
        w.U16(r.inst_count);
        w.U16(r.reg);
        w.U16(static_cast<u16>(r.kind));
        w.U16(static_cast<u16>(r.use));
        w.U64(r.addend);
        w.U64(r.recorded_value);
    }
}

bool ReadUnit(BlobReader& r, SerialUnit& unit) {
    u32 code_size{};
    u32 count{};
    if (!r.U64(unit.guest_start) || !r.U8(unit.is_function) || !r.U32(code_size)) {
        return false;
    }
    if (code_size == 0 || code_size % 4 != 0 || code_size > r.Remaining()) {
        return false;
    }
    unit.code.resize(code_size);
    if (!r.Bytes(unit.code.data(), code_size)) {
        return false;
    }
    if (!r.U32(count) || count > r.Remaining() / 8) {
        return false;
    }
    unit.blocks.resize(count);
    for (auto& b : unit.blocks) {
        if (!r.U64(b.guest_start) || !r.U64(b.guest_end) || !r.U32(b.code_offset) ||
            !r.U64(b.guest_bytes_hash)) {
            return false;
        }
        if (b.code_offset >= code_size || b.guest_end < b.guest_start) {
            return false;
        }
    }
    if (!r.U32(count) || count > r.Remaining() / 8) {
        return false;
    }
    unit.relocs.resize(count);
    for (auto& rel : unit.relocs) {
        u16 kind{};
        u16 use{};
        if (!r.U32(rel.code_offset) || !r.U16(rel.inst_count) || !r.U16(rel.reg) ||
            !r.U16(kind) || !r.U16(use) || !r.U64(rel.addend) || !r.U64(rel.recorded_value)) {
            return false;
        }
        rel.kind = static_cast<RelocKind>(kind);
        rel.use = static_cast<RelocUse>(use);
    }
    return true;
}

// ==========================================================================
// Validity key
// ==========================================================================
u64 HashBytes(const void* data, std::size_t size, u64 seed) {
    // FNV-1a 64.
    const auto* p = static_cast<const u8*>(data);
    u64 h = seed;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

u64 HashU64(u64 value, u64 seed) { return HashBytes(&value, sizeof(value), seed); }

namespace {
constexpr u64 kFnvOffset = 0xCBF29CE484222325ull;
// Domain-separate the executable-only guest identity from the legacy
// all-argv identity. OFF intentionally retains the exact old hash so existing
// cache entries remain usable; ON cannot alias it merely because argv happens
// to contain the same bytes as the file identity fields.
constexpr u64 kExecGuestIdDomain = 0x53564D4558454349ull;  // "SVMEXECI"

std::string SelfExePath() {
#ifdef __APPLE__
    char buf[4096];
    u32 size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        return std::string{buf};
    }
    return {};
#else
    char buf[4096];
    const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string{buf};
#endif
}

std::vector<std::string> ProcessArgv() {
    std::vector<std::string> out;
#ifdef __APPLE__
    char*** argv_p = _NSGetArgv();
    int* argc_p = _NSGetArgc();
    if (argv_p && argc_p) {
        for (int i = 0; i < *argc_p; ++i) {
            if ((*argv_p)[i]) out.emplace_back((*argv_p)[i]);
        }
    }
#else
    if (FILE* f = std::fopen("/proc/self/cmdline", "rb")) {
        std::string cur;
        int c;
        while ((c = std::fgetc(f)) != EOF) {
            if (c == 0) {
                out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(static_cast<char>(c));
            }
        }
        if (!cur.empty()) out.push_back(cur);
        std::fclose(f);
    }
#endif
    return out;
}

u64 HashFileIdentity(const std::string& path, u64 seed) {
    struct stat st {};
    if (path.empty() || ::stat(path.c_str(), &st) != 0) {
        return HashBytes(path.data(), path.size(), seed);
    }
    u64 h = HashBytes(path.data(), path.size(), seed);
    h = HashU64(static_cast<u64>(st.st_size), h);
    h = HashU64(static_cast<u64>(st.st_mtime), h);
    h = HashU64(static_cast<u64>(st.st_ino), h);
    return h;
}
}  // namespace

u64 ComputeBuildId() {
    static const u64 id = [] {
        u64 h = HashFileIdentity(SelfExePath(), kFnvOffset);
        // Guard against a stale mtime by folding in the image span too.
        h = HashU64(GetHostImage().size, h);
        return h;
    }();
    return id;
}

u64 ComputeEnvHash() {
    std::vector<std::string> entries;
    for (char** e = environ; e && *e; ++e) {
        const std::string_view sv{*e};
        if (sv.rfind("SVM_", 0) != 0 && sv.rfind("SWIFT_", 0) != 0) {
            continue;
        }
        // The cache's own knobs do not change codegen.
        if (sv.rfind("SVM_JIT_CACHE", 0) == 0) {
            continue;
        }
        entries.emplace_back(sv);
    }
    std::sort(entries.begin(), entries.end());
    u64 h = kFnvOffset;
    for (const auto& s : entries) {
        h = HashBytes(s.data(), s.size(), h);
        h = HashU64(0, h);
    }
    return h;
}

u64 ComputeConfigHash(const Config& config) {
    u64 h = kFnvOffset;
    h = HashU64(config.loc_start, h);
    h = HashU64(config.loc_end, h);
    h = HashU64(config.enable_jit ? 1 : 0, h);
    h = HashU64(config.enable_asm_interp ? 1 : 0, h);
    h = HashU64(config.has_local_operation ? 1 : 0, h);
    h = HashU64(static_cast<u64>(config.backend_isa), h);
    h = HashU64(config.uniform_buffer_size, h);
    h = HashU64(config.static_program ? 1 : 0, h);
    h = HashU64(static_cast<u64>(config.global_opts), h);
    h = HashU64(static_cast<u64>(config.arm64_features), h);
    h = HashU64(static_cast<u64>(config.tso_mode), h);
    h = HashU64(config.stack_alignment, h);
    h = HashU64(config.guest_addr_mask, h);
    // memory_base is *not* hashed by value: it is an mmap result that varies
    // per run and never reaches an instruction immediate (the JIT keeps it in
    // the pt register). Only its nullness and page offset can steer codegen
    // (translator_mem.cpp's statically_unaligned decision).
    h = HashU64(config.memory_base ? 1 : 0, h);
    h = HashU64(reinterpret_cast<u64>(config.memory_base) & 0xFFFull, h);
    h = HashU64(config.page_table ? 1 : 0, h);
    // Static register mapping: changes which guest registers live in host
    // registers, i.e. every emitted instruction.
    h = HashU64(config.buffers_static_alloc.size(), h);
    for (const auto& desc : config.buffers_static_alloc) {
        h = HashU64(desc.offset, h);
        h = HashU64(desc.size, h);
        h = HashU64(desc.reg, h);
        h = HashU64(desc.is_float ? 1 : 0, h);
    }
    // These ranges steer block-local XMM Load/StoreUniform forwarding, so a
    // cache image compiled for one frontend state layout cannot be reused for
    // another one.
    h = HashU64(config.xmm_uniform_ranges.size(), h);
    for (const auto& range : config.xmm_uniform_ranges) {
        h = HashU64(range.offset, h);
        h = HashU64(range.size, h);
    }
    return h;
}

u64 ComputeGuestId() {
    static const u64 id = [] {
        const auto argv = ProcessArgv();
        u64 h = kFnvOffset;

        const char* exec_id = std::getenv("SVM_JIT_CACHE_EXEC_ID");
        if (exec_id && exec_id[0] && std::strcmp(exec_id, "0") != 0) {
            h = HashU64(kExecGuestIdDomain, h);
            // argv[0] is the SwiftVM launcher, whose code identity is already
            // covered by build_id. argv[1] is the guest ELF selected by the
            // Linux frontend. Later arguments are copied into the guest's
            // initial stack and cannot steer translation/code generation.
            if (argv.size() >= 2) {
                h = HashFileIdentity(argv[1], h);
            }
            return h;
        }

        // Compatibility mode (the default): preserve the legacy all-argv key
        // byte-for-byte, including its lack of separators between arguments.
        for (std::size_t i = 0; i < argv.size(); ++i) {
            h = HashBytes(argv[i].data(), argv[i].size(), h);
        }
        if (argv.size() >= 2) {
            h = HashFileIdentity(argv[1], h);
        }
        return h;
    }();
    return id;
}

}  // namespace swift::runtime::backend
