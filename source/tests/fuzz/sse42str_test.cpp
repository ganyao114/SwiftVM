// ===========================================================================
// SSE4.2 string compare (PCMPISTRI/M, PCMPESTRI/M + VEX twins) against TWO
// independent oracles: Rosetta and a from-the-SDM model.
// ===========================================================================
//
// WHY TWO ORACLES
// ---------------
// Rosetta 2 is an emulator with its own defects -- thirteen are already on
// record in this tree, the most recent being PTEST's PF -- so "Rosetta agrees"
// is evidence and not proof.  This family is also the one where a plausible
// but wrong reading of the SDM survives every spot check: swap the roles of
// the two operands and `strchr` still returns an index, just the wrong one.
//
// So every row here is checked twice:
//
//   (1) against the LITERAL BYTES real x86-64 wrote under Rosetta
//       (sse42str_rosetta_ref.inc), replayed from the row's own instruction
//       encoding so the two sides cannot assemble different instructions; and
//   (2) against Sdm::Evaluate below, which is written from the SDM's prose
//       (Vol 2B 4.1: the four aggregations, the validity-override table, the
//       four polarities, the two output selections and the six flags) and
//       shares no code with decoder_sse42str.cc.
//
// The model is checked against Rosetta on all 6104 rows BEFORE any SwiftVM
// code runs.  That comparison is the part that settles the conventions:
//
//   WHICH OPERAND IS THE "TEXT".  IntRes1 bit j names element j of the SECOND
//   operand (ModRM.rm), and the index in ECX is an index into it.  The `set`,
//   `sub` and `tail` input pairs are deliberately asymmetric -- a 5-character
//   set against a 16-character sentence, a 3-character needle against a
//   haystack with matches at 0, 3 and 8 -- so a model with the operands
//   swapped disagrees with hardware on most of their rows rather than on none.
//
//   THE VALIDITY OVERRIDE, AND ITS ASYMMETRY.  For "equal ordered" the cell
//   (invalid arr1, valid arr2) is TRUE -- the needle matched to completion --
//   while (valid arr1, invalid arr2) is FALSE: the text ran out mid-needle.
//   The first cut of this file had those two the wrong way round, which is a
//   perfectly plausible reading, and the `nul5_10` pair killed it: needle
//   "ABCDE" against text "ABCDEFGHIJ" is a match at index 0 on hardware and no
//   match at all with the table inverted.  The `zeros` pair makes every cell
//   (invalid, invalid), which is TRUE for equal-each/equal-ordered and FALSE
//   for the other two -- four different answers from one input.
//
//   TERMINATOR GRANULARITY.  The `wnul` pair has a zero BYTE at index 2 and a
//   zero WORD at index 4, so the byte and word data formats see different
//   lengths and therefore different ZF, SF and index.
//
//   SIGNEDNESS.  The `sign` pair holds 0x80 and 0xFF, the extremes of both
//   interpretations, so the "ranges" aggregation gives different answers for
//   imm8[1] = 0 and 1.
//
//   THE EXPLICIT LENGTHS.  kSse42StrLens covers 0, in-range, exactly 16, over
//   16, NEGATIVE (which is used as an absolute value), INT32_MIN, INT64_MIN,
//   and one combination whose low 32 bits differ from its full 64 bits -- so
//   a handler reading the wrong width, or taking a negative length as 0 or as
//   a huge unsigned, is wrong on those rows.
//
// WHAT ONE ROW EXECUTES
// ---------------------
// The row's literal instruction bytes wrapped in the fixed prefix/suffix the
// generator also printed:
//
//     push 0xAD7 ; popfq          -- all six defined flags preset to 1
//     <the instruction>
//     pushfq ; pop rsi ; and rsi, 0x8D5
//
// The preset is what makes a MISSING flag write visible: a flag the handler
// never touches stays 1, and the data contains rows where each of the six is
// architecturally 0.  The test asserts that both values occur for each flag,
// so that property cannot quietly decay.
//
// THE LEGACY / VEX CONTRACT (C3)
// ------------------------------
// ymm0 is preloaded with poison in BOTH halves.  The mask forms write its low
// half; the high half is the witness.  A legacy PCMPISTRM must give the poison
// back, a VEX VPCMPISTRM must give zeros, and every index form -- legacy or
// VEX -- must leave all 32 bytes alone, because it writes no vector register
// at all.  Every row's reference carries the answer literally, and the test
// requires the reference itself to show all three shapes before it starts.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <sys/mman.h>
#include "runtime/backend/smc_tracker.h"
#include "runtime/frontend/x86/decoder.h"
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

using namespace swift::translator::x86;
using namespace swift;

// decoder_sse42str.cc's test hook.  Deliberately not in any header: nothing in
// the runtime calls it, and it exists so this file can reach the evaluators
// directly instead of only through a guest instruction.
//   variant 0 = the reference (cell-at-a-time, the specification)
//   variant 1 = the portable fast path (bitmask algebra, no SIMD)
//   variant 2 = what the runtime calls (NEON where the host has it)
extern "C" u64 SwiftSse42StrEvalVariant(unsigned variant, u64 a_lo, u64 a_hi, u64 b_lo, u64 b_hi,
                                        u64 ctl);

namespace {

struct Sse42StrRef {
    const char* name;
    int pair;
    int imm;
    int lens;  // index into kSse42StrLens, or -1 for the implicit forms
    int mem;   // 1 when the second operand is [rdi + DATA_B]
    const char* enc;
    const char* result;
};
struct Sse42StrPair {
    const char* name;
    const char* a;
    const char* b;
};
#include "sse42str_rosetta_ref.inc"

using Vec256 = std::array<u8, 32>;
using Obs = std::array<u8, 128>;

// Must match sse42str_rosetta_ref.c exactly; the replayed encodings carry
// these displacements literally, so nothing here is a choice.
constexpr s32 kOffA = 0x000, kOffB = 0x020, kOffK = 0x040, kOffO = 0x080;

constexpr u64 kRaxPoison = 0xAAAAAAAAAAAAAAAAull;
constexpr u64 kRdxPoison = 0xDDDDDDDDDDDDDDDDull;
constexpr u64 kRcxPoison = 0x1122334455667788ull;
constexpr u64 kRbxPoison = 0xBBBBBBBBBBBBBBBBull;
constexpr u64 kFlagsMask = 0x8D5ull;

// The observation block, byte offsets shared with the generator.
constexpr size_t kObsYmm0 = 0x00, kObsRcx = 0x20, kObsRsi = 0x28, kObsRax = 0x30,
                 kObsRdx = 0x38, kObsYmm1 = 0x40, kObsYmm2 = 0x60;

u8 Nib(char ch) { return u8(ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10); }

Vec256 ParseHex32(const char* h) {
    Vec256 v{};
    for (u32 i = 0; i < 32; ++i) {
        v[i] = u8((Nib(h[i * 2]) << 4) | Nib(h[i * 2 + 1]));
    }
    return v;
}

Obs ParseObs(const char* h) {
    Obs v{};
    for (u32 i = 0; i < 128; ++i) {
        v[i] = u8((Nib(h[i * 2]) << 4) | Nib(h[i * 2 + 1]));
    }
    return v;
}

std::vector<u8> ParseHex(const char* h) {
    std::vector<u8> v;
    for (size_t i = 0; h[i] != '\0' && h[i + 1] != '\0'; i += 2) {
        v.push_back(u8((Nib(h[i]) << 4) | Nib(h[i + 1])));
    }
    return v;
}

template <size_t N>
std::string Hex(const std::array<u8, N>& v) {
    std::string s;
    for (const u8 x : v) {
        s += fmt::format("{:02x}", x);
    }
    return s;
}

u64 QwordAt(const Obs& v, size_t offset) {
    u64 r = 0;
    for (u32 j = 0; j < 8; ++j) {
        r |= u64(v[offset + j]) << (j * 8);
    }
    return r;
}

// ymm0's preload, from the generator's K_POISON_LO / K_POISON_HI.
Vec256 MaskPoison() {
    Vec256 v{};
    for (u32 i = 0; i < 16; ++i) {
        v[i] = u8(0xC3u ^ i);
        v[16 + i] = u8(0x5Au ^ i);
    }
    return v;
}

// Bystander poison for every register the replayed prologue does not load.
Vec256 Poison(u32 reg) {
    Vec256 v{};
    for (u32 j = 0; j < 32; ++j) {
        v[j] = u8(0xA5 ^ (reg * 32 + j));
    }
    return v;
}

// Restore SVM_ENABLE_JIT on the way out.  Catch2 runs the cases in this
// binary in one process and in a randomized order, so a case that leaves the
// variable changed silently reconfigures whatever runs next -- which is
// exactly what happened: leaking it here made xsave_test's Rosetta case fail,
// but only in a full-suite run and only with SVM_SSE42STR unset, because with
// the gate off these cases return before touching the environment at all.
struct ScopedJitEnv {
    bool had;
    std::string value;
    ScopedJitEnv() {
        const char* old = std::getenv("SVM_ENABLE_JIT");
        had = old != nullptr;
        if (had) value = old;
    }
    ~ScopedJitEnv() {
        if (had) {
            setenv("SVM_ENABLE_JIT", value.c_str(), 1);
        } else {
            unsetenv("SVM_ENABLE_JIT");
        }
    }
};

bool IsVex(const char* name) { return name[0] == 'v'; }
bool IsMaskForm(const char* name) { return std::strlen(name) > 0 && name[std::strlen(name) - 1] == 'm'; }

// REX.W / VEX.W read back out of the row's OWN instruction bytes rather than
// carried as a separate field: a row whose recorded width disagreed with the
// bytes it replays would be undetectable otherwise.
bool RowIsWide(const std::vector<u8>& enc) {
    if (enc.size() >= 2 && enc[0] == 0x66) {
        return (enc[1] & 0xF0) == 0x40 && (enc[1] & 0x08) != 0;  // REX with W
    }
    if (enc.size() >= 3 && enc[0] == 0xC4) {
        return ((enc[2] >> 7) & 1) != 0;  // VEX.W
    }
    return false;
}

// 0x60..0x63, found by walking the prefixes rather than by assuming which of
// them are present: a REX byte appears for a register above 7 whether or not
// REX.W is set, so "the opcode is at a fixed offset" is wrong for exactly the
// encodings this file exists to cover.
u8 RowOpcode(const std::vector<u8>& enc) {
    if (enc.size() >= 4 && enc[0] == 0xC4) {
        return enc[3];
    }
    size_t i = 0;
    while (i < enc.size() && enc[i] == 0x66) {
        ++i;
    }
    if (i < enc.size() && (enc[i] & 0xF0) == 0x40) {
        ++i;  // REX
    }
    return (i + 2 < enc.size()) ? enc[i + 2] : 0;  // skip 0F 3A
}

// ---------------------------------------------------------------------------
// The SDM model.  Deliberately shares nothing with decoder_sse42str.cc: the
// matrix is materialized in full and aggregated afterwards, where the handler
// evaluates cells lazily.
// ---------------------------------------------------------------------------
struct SdmResult {
    u32 intres2;
    u32 index;
    u32 len1, len2;
    u32 flags;              // in EFLAGS positions, masked to kFlagsMask
    std::array<u8, 16> xmm; // the mask forms' XMM0
};

struct Sdm {
    static u32 Saturate(u64 raw, bool wide, u32 n) {
        // The architectural length is SIGNED at its architectural width; the
        // absolute value is used and saturates at n.
        s64 v = wide ? s64(raw) : s64(s32(u32(raw)));
        u64 magnitude = v < 0 ? u64(0) - u64(v) : u64(v);
        return magnitude >= n ? n : u32(magnitude);
    }

    static void Split(const Vec256& src, bool words, bool is_signed, std::array<s32, 16>& out) {
        if (words) {
            for (u32 i = 0; i < 8; ++i) {
                const u32 raw = u32(src[i * 2]) | (u32(src[i * 2 + 1]) << 8);
                out[i] = is_signed ? s32(s16(raw)) : s32(raw);
            }
            return;
        }
        for (u32 i = 0; i < 16; ++i) {
            out[i] = is_signed ? s32(s8(src[i])) : s32(src[i]);
        }
    }

    static u32 ImplicitLength(const std::array<s32, 16>& e, u32 n) {
        u32 len = n;
        for (u32 i = 0; i < n; ++i) {
            if (e[i] == 0) {
                len = i;
                break;
            }
        }
        return len;
    }

    static SdmResult Evaluate(const Vec256& a, const Vec256& b, u32 imm, bool explicit_length,
                              u64 raw1, u64 raw2, bool wide) {
        const bool words = (imm & 1) != 0;
        const bool is_signed = (imm & 2) != 0;
        const u32 aggregation = (imm >> 2) & 3;
        const u32 polarity = (imm >> 4) & 3;
        const bool msb = (imm & 0x40) != 0;
        const u32 n = words ? 8 : 16;

        std::array<s32, 16> arr1{}, arr2{};
        Split(a, words, is_signed, arr1);
        Split(b, words, is_signed, arr2);

        SdmResult out{};
        out.len1 = explicit_length ? Saturate(raw1, wide, n) : ImplicitLength(arr1, n);
        out.len2 = explicit_length ? Saturate(raw2, wide, n) : ImplicitLength(arr2, n);

        // The full comparison matrix, with the SDM's valid/invalid override
        // applied to every cell.
        bool m[16][16] = {};
        for (u32 i = 0; i < n; ++i) {
            for (u32 j = 0; j < n; ++j) {
                const bool v1 = i < out.len1;
                const bool v2 = j < out.len2;
                if (v1 && v2) {
                    m[i][j] = aggregation == 1
                                      ? ((i % 2 == 0) ? arr2[j] >= arr1[i] : arr2[j] <= arr1[i])
                                      : arr1[i] == arr2[j];
                } else if (!v1 && !v2) {
                    m[i][j] = aggregation == 2 || aggregation == 3;
                } else if (!v1) {
                    // arr1 (the needle) ran out: "equal ordered" has matched
                    // to completion, so the cell is TRUE.  The mirror case
                    // below -- arr2 ran out mid-needle -- is FALSE for every
                    // aggregation.  This asymmetry is the single thing about
                    // the family a plausible reading gets backwards.
                    m[i][j] = aggregation == 3;
                } else {
                    m[i][j] = false;
                }
            }
        }

        u32 res1 = 0;
        for (u32 j = 0; j < n; ++j) {
            bool set = false;
            if (aggregation == 0) {
                for (u32 i = 0; i < n; ++i) {
                    set = set || m[i][j];
                }
            } else if (aggregation == 1) {
                for (u32 i = 0; i + 1 < n; i += 2) {
                    set = set || (m[i][j] && m[i + 1][j]);
                }
            } else if (aggregation == 2) {
                set = m[j][j];
            } else {
                set = true;
                for (u32 i = 0; i + j < n; ++i) {
                    set = set && m[i][j + i];
                }
            }
            if (set) {
                res1 |= 1u << j;
            }
        }

        const u32 all = (1u << n) - 1u;
        u32 res2 = res1;
        if (polarity == 1) {
            res2 = (~res1) & all;
        } else if (polarity == 3) {
            res2 = (res1 ^ ((1u << out.len2) - 1u)) & all;
        }
        out.intres2 = res2 & all;

        out.index = n;
        if (out.intres2 != 0) {
            if (msb) {
                for (u32 j = 0; j < n; ++j) {
                    if (out.intres2 & (1u << j)) {
                        out.index = j;
                    }
                }
            } else {
                for (u32 j = 0; j < n; ++j) {
                    if (out.intres2 & (1u << j)) {
                        out.index = j;
                        break;
                    }
                }
            }
        }

        // CF bit 0, PF bit 2, AF bit 4, ZF bit 6, SF bit 7, OF bit 11.
        out.flags = 0;
        if (out.intres2 != 0) out.flags |= 1u << 0;
        if (out.len2 < n) out.flags |= 1u << 6;
        if (out.len1 < n) out.flags |= 1u << 7;
        if (out.intres2 & 1u) out.flags |= 1u << 11;
        // PF and AF are architecturally 0, so no bits are added for them.

        out.xmm.fill(0);
        if (!msb) {
            out.xmm[0] = u8(out.intres2 & 0xFF);
            out.xmm[1] = u8((out.intres2 >> 8) & 0xFF);
        } else if (words) {
            for (u32 j = 0; j < 8; ++j) {
                const u8 fill = (out.intres2 >> j) & 1 ? 0xFF : 0x00;
                out.xmm[j * 2] = fill;
                out.xmm[j * 2 + 1] = fill;
            }
        } else {
            for (u32 j = 0; j < 16; ++j) {
                out.xmm[j] = ((out.intres2 >> j) & 1) ? 0xFF : 0x00;
            }
        }
        return out;
    }
};

}  // namespace

TEST_CASE("x86 sse4.2 string compare vs rosetta and the SDM") {
    const char* gate = std::getenv("SVM_SSE42STR");
    if (gate && std::strcmp(gate, "0") == 0) {
        SUCCEED("SVM_SSE42STR=0 disables the handlers; differential skipped");
        return;
    }
    const char* avx_env = std::getenv("SVM_AVX");
    const bool avx_on = avx_env && std::strcmp(avx_env, "0") != 0;

    std::vector<Vec256> ins_a, ins_b;
    for (const auto& p : kSse42StrPairs) {
        ins_a.push_back(ParseHex32(p.a));
        ins_b.push_back(ParseHex32(p.b));
    }
    const auto mask_poison = MaskPoison();
    const auto prefix = ParseHex(kSse42StrPrefix);
    const auto suffix = ParseHex(kSse42StrSuffix);

    // The wrapper is what makes the flag comparison meaningful; pin it here so
    // a regenerated .inc that changed it fails loudly instead of silently
    // measuring something else.
    REQUIRE(std::string(kSse42StrPrefix) == "68d70a00009d");            // push 0xAD7; popfq
    REQUIRE(std::string(kSse42StrSuffix) == "9c5e4881e6d5080000");      // pushfq; pop rsi; and

    // ---- ORACLE 1 vs ORACLE 2: the SDM model against every Rosetta row -----
    // Runs before any SwiftVM code, so a disagreement here is a statement
    // about the SDM and Rosetta and not about this front end.
    {
        size_t modelled = 0;
        std::vector<std::string> disagreements;
        for (const auto& r : kSse42StrRefs) {
            const auto bytes = ParseHex(r.enc);
            const bool wide = RowIsWide(bytes);
            const bool explicit_length = r.lens >= 0;
            const u64 raw1 = explicit_length ? kSse42StrLens[r.lens][0] : 0;
            const u64 raw2 = explicit_length ? kSse42StrLens[r.lens][1] : 0;
            const auto want = ParseObs(r.result);
            const auto model = Sdm::Evaluate(ins_a[size_t(r.pair)], ins_b[size_t(r.pair)],
                                             u32(r.imm), explicit_length, raw1, raw2, wide);
            const u64 flags = QwordAt(want, kObsRsi) & kFlagsMask;
            const u64 rcx = QwordAt(want, kObsRcx);
            const std::string label =
                    fmt::format("{}/{}/imm{:02x}/lens{}/mem{}/w{}", r.name,
                                kSse42StrPairs[r.pair].name, r.imm, r.lens, r.mem, int(wide));
            if (flags != model.flags && disagreements.size() < 12) {
                disagreements.push_back(fmt::format(
                        "{}: flags {:#05x} from hardware, {:#05x} from the SDM model", label,
                        flags, model.flags));
            }
            if (IsMaskForm(r.name)) {
                if (!std::equal(model.xmm.begin(), model.xmm.end(), want.begin() + kObsYmm0) &&
                    disagreements.size() < 12) {
                    disagreements.push_back(fmt::format("{}: XMM0 disagrees with the SDM model",
                                                        label));
                }
                if (rcx != kRcxPoison && disagreements.size() < 12) {
                    disagreements.push_back(
                            fmt::format("{}: a mask form wrote RCX on hardware", label));
                }
            } else {
                if (rcx != u64(model.index) && disagreements.size() < 12) {
                    disagreements.push_back(fmt::format(
                            "{}: ECX {:#x} from hardware, {:#x} from the SDM model", label, rcx,
                            model.index));
                }
            }
            ++modelled;
        }
        std::string joined;
        for (const auto& d : disagreements) {
            joined += "\n  " + d;
        }
        INFO("Rosetta and a from-the-SDM model disagree.  The SDM wins: fix the model only "
             "after re-reading Vol 2B 4.1, and record the defect in this header." << joined);
        REQUIRE(disagreements.empty());
        REQUIRE(modelled > 6000);
    }

    // ---- properties of the reference data this case depends on ------------
    {
        // Every mnemonic produced rows.
        for (const char* name : {"pcmpistri", "pcmpistrm", "pcmpestri", "pcmpestrm",
                                 "vpcmpistri", "vpcmpistrm", "vpcmpestri", "vpcmpestrm"}) {
            size_t rows = 0;
            for (const auto& r : kSse42StrRefs) {
                if (std::strcmp(r.name, name) == 0) ++rows;
            }
            INFO("no reference rows for " << name);
            REQUIRE(rows > 0);
        }
    }
    {
        // Contract C3, as the HARDWARE reported it.
        size_t legacy_kept = 0, vex_zeroed = 0, vex_index_kept = 0;
        for (const auto& r : kSse42StrRefs) {
            const auto v = ParseObs(r.result);
            const bool zeroes = IsVex(r.name) && IsMaskForm(r.name);
            if (zeroes) {
                INFO(r.name << " pair " << r.pair
                            << ": a VEX mask row's reference does not zero ymm0[255:128]");
                REQUIRE(std::all_of(v.begin() + kObsYmm0 + 16, v.begin() + kObsYmm0 + 32,
                                    [](u8 x) { return x == 0; }));
                ++vex_zeroed;
                continue;
            }
            INFO(r.name << " pair " << r.pair
                        << ": the high half of ymm0 is not the poison it was preloaded with -- "
                           "the file no longer measures the legacy contract");
            REQUIRE(std::equal(mask_poison.begin() + 16, mask_poison.end(),
                               v.begin() + kObsYmm0 + 16));
            if (IsVex(r.name)) {
                ++vex_index_kept;
            } else {
                ++legacy_kept;
            }
        }
        INFO("too few legacy rows to measure the preserve-upper-half contract");
        REQUIRE(legacy_kept > 3000);
        INFO("no VEX mask rows -- contract C3 measures nothing");
        REQUIRE(vex_zeroed > 200);
        INFO("no VEX index rows -- 'a VEX instruction with no vector destination zeroes nothing' "
             "measures nothing");
        REQUIRE(vex_index_kept > 200);
    }
    {
        // Each of the six flags takes BOTH values in the data.  Without this
        // the all-ones preset would let a handler that never writes some flag
        // pass every row.
        for (const u32 position : {0u, 2u, 4u, 6u, 7u, 11u}) {
            bool saw_zero = false, saw_one = false;
            for (const auto& r : kSse42StrRefs) {
                const u64 flags = QwordAt(ParseObs(r.result), kObsRsi);
                ((flags >> position) & 1) ? saw_one = true : saw_zero = true;
            }
            INFO("flag bit " << position
                             << " never takes both values, so a handler that leaves it at the "
                                "preset would pass every row");
            // PF (2) and AF (4) are architecturally 0 always, so only the
            // zero side is reachable for them -- and that is exactly the
            // assertion, because the preset made them 1.
            if (position == 2 || position == 4) {
                REQUIRE(saw_zero);
                INFO("PF/AF are architecturally 0; a 1 in the reference means hardware "
                     "disagrees with the SDM and the deviation must be recorded");
                REQUIRE(!saw_one);
            } else {
                REQUIRE(saw_zero);
                REQUIRE(saw_one);
            }
        }
    }
    {
        // imm8 bit 7 is reserved and hardware ignores it: the |0x80 rows must
        // match their base rows exactly.
        size_t pairs_checked = 0;
        for (const auto& r : kSse42StrRefs) {
            if ((r.imm & 0x80) == 0 || r.lens >= 0 || r.mem) continue;
            for (const auto& base : kSse42StrRefs) {
                if (std::strcmp(base.name, r.name) != 0 || base.pair != r.pair ||
                    base.imm != (r.imm & 0x7F) || base.lens >= 0 || base.mem) {
                    continue;
                }
                INFO(r.name << " imm " << r.imm << ": imm8 bit 7 changed the answer");
                REQUIRE(std::strcmp(r.result, base.result) == 0);
                ++pairs_checked;
                break;
            }
        }
        INFO("no imm8 bit-7 twin pairs in the data");
        REQUIRE(pairs_checked >= 40);
    }
    {
        // Every dimension of imm8 must MOVE the answer somewhere, or the sweep
        // is decorative.  Compared on the whole observation block.
        const auto find = [&](const char* name, int pair, int imm, int lens, int mem) -> const
                          Sse42StrRef* {
            for (const auto& r : kSse42StrRefs) {
                if (std::strcmp(r.name, name) == 0 && r.pair == pair && r.imm == imm &&
                    r.lens == lens && r.mem == mem) {
                    return &r;
                }
            }
            return nullptr;
        };
        const auto differs = [&](const char* name, int pair, int a, int b) {
            const auto* x = find(name, pair, a, -1, 0);
            const auto* y = find(name, pair, b, -1, 0);
            return x && y && std::strcmp(x->result, y->result) != 0;
        };
        size_t aggregation_moves = 0, polarity_moves = 0, direction_moves = 0, format_moves = 0,
               maskshape_moves = 0;
        for (int pair = 0; pair < int(std::size(kSse42StrPairs)); ++pair) {
            for (int base : {0x00, 0x01, 0x02, 0x03}) {
                if (differs("pcmpistri", pair, base, base | 0x04)) ++aggregation_moves;
                if (differs("pcmpistri", pair, base, base | 0x08)) ++aggregation_moves;
                if (differs("pcmpistri", pair, base | 0x08, (base | 0x08) | 0x10))
                    ++polarity_moves;
                if (differs("pcmpistri", pair, base | 0x08, (base | 0x08) | 0x30))
                    ++polarity_moves;
                if (differs("pcmpistri", pair, base, base | 0x40)) ++direction_moves;
                if (differs("pcmpistrm", pair, base, base | 0x40)) ++maskshape_moves;
            }
            if (differs("pcmpistri", pair, 0x04, 0x05)) ++format_moves;
            if (differs("pcmpistri", pair, 0x04, 0x06)) ++format_moves;
        }
        INFO("imm8[3:2] (the aggregation) never changes the answer");
        REQUIRE(aggregation_moves >= 10);
        INFO("imm8[5:4] (the polarity) never changes the answer -- both the plain and the "
             "masked negation must be exercised");
        REQUIRE(polarity_moves >= 10);
        INFO("imm8[6] never changes the INDEX, so the msb/lsb selection is untested");
        REQUIRE(direction_moves >= 5);
        INFO("imm8[6] never changes the MASK SHAPE, so bit/byte expansion is untested");
        REQUIRE(maskshape_moves >= 5);
        INFO("imm8[1:0] (the data format) never changes the answer");
        REQUIRE(format_moves >= 4);
    }
    {
        // The explicit lengths really are read, are absolute-valued, and are
        // read at the right WIDTH.  Combo 12 is {0x100000003, 0x200000005}:
        // as EAX/EDX it is (3, 5); as RAX/RDX it saturates to (16, 16).
        const auto find_len = [&](const char* name, int pair, int imm, int lens,
                                  bool wide) -> const Sse42StrRef* {
            for (const auto& r : kSse42StrRefs) {
                if (std::strcmp(r.name, name) != 0 || r.pair != pair || r.imm != imm ||
                    r.lens != lens || r.mem) {
                    continue;
                }
                if (RowIsWide(ParseHex(r.enc)) == wide) return &r;
            }
            return nullptr;
        };
        // Only the RESULT part of the observation block: bytes 0x30..0x40 hold
        // the captured RAX/RDX, which are the row's INPUT lengths and
        // therefore differ between any two length combinations by
        // construction.  Comparing them would make every check below trivially
        // "different" and measure nothing.
        const auto result_only = [](const Sse42StrRef* r) {
            const auto v = ParseObs(r->result);
            return std::string(v.begin(), v.begin() + 0x30);
        };
        size_t width_moves = 0, negative_matches = 0, saturation_matches = 0;
        for (int pair = 0; pair < 3; ++pair) {
            for (int imm : {0x00, 0x0D, 0x1A, 0x4C}) {
                const auto* narrow = find_len("pcmpestri", pair, imm, 12, false);
                const auto* wide = find_len("pcmpestri", pair, imm, 12, true);
                if (narrow && wide && result_only(narrow) != result_only(wide)) {
                    ++width_moves;
                }
                // {-3,-5} must behave exactly like {3,5}: same absolute value.
                const auto* negative = find_len("pcmpestri", pair, imm, 6, false);
                const auto* positive = find_len("pcmpestri", pair, imm, 1, false);
                if (negative && positive && result_only(negative) == result_only(positive)) {
                    ++negative_matches;
                }
                // {20,20} must behave exactly like {16,16}: both saturate.
                const auto* over = find_len("pcmpestri", pair, imm, 5, false);
                const auto* exact = find_len("pcmpestri", pair, imm, 3, false);
                if (over && exact && result_only(over) == result_only(exact)) {
                    ++saturation_matches;
                }
            }
        }
        INFO("REX.W never changes the answer, so the 32/64-bit length width is untested");
        REQUIRE(width_moves >= 4);
        INFO("a negative length does not behave as its absolute value in the reference");
        REQUIRE(negative_matches >= 8);
        INFO("an out-of-range length does not saturate in the reference");
        REQUIRE(saturation_matches >= 8);
    }

    // ---- harness -----------------------------------------------------------
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x200000;
    const u64 data = base + 0x300000;

    ScopedJitEnv jit_env;
    setenv("SVM_ENABLE_JIT", "1", 1);
    auto* jit_instance = X86Instance::Make();
    setenv("SVM_ENABLE_JIT", "0", 1);
    auto* interp_instance = X86Instance::Make();
    auto* jit_core = X86Core::Make(jit_instance);
    auto* interp_core = X86Core::Make(interp_instance);

    struct Out {
        Obs obs{};
        std::array<Vec256, 16> ymm{};
        u64 rbx{};
        int exit{};
    };

    const auto run_on = [&](X86Core* core, const std::vector<u8>& code, const Sse42StrRef& ref,
                            u64 code_addr) {
        const auto& a = ins_a[size_t(ref.pair)];
        const auto& b = ins_b[size_t(ref.pair)];
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffA)), a.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffB)), b.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffK)), mask_poison.data(), 32);
        std::memset(reinterpret_cast<void*>(data + u64(kOffO)), 0x99, 128);
        auto& ctx = core->GetContext();
        for (u32 i = 0; i < 16; ++i) {
            const auto p = Poison(i);
            std::memcpy(ctx.xmms[i].b, p.data(), 16);
            std::memcpy(ctx.ymm_high[i].b, p.data() + 16, 16);
        }
        // The state the generator's prologue produced, written directly so a
        // broken vmovdqu cannot mask a broken handler.
        std::memcpy(ctx.xmms[0].b, mask_poison.data(), 16);
        std::memcpy(ctx.ymm_high[0].b, mask_poison.data() + 16, 16);
        std::memcpy(ctx.xmms[1].b, a.data(), 16);
        std::memcpy(ctx.ymm_high[1].b, a.data() + 16, 16);
        std::memcpy(ctx.xmms[2].b, b.data(), 16);
        std::memcpy(ctx.ymm_high[2].b, b.data() + 16, 16);
        ctx.rax.qword = ref.lens >= 0 ? kSse42StrLens[ref.lens][0] : kRaxPoison;
        ctx.rdx.qword = ref.lens >= 0 ? kSse42StrLens[ref.lens][1] : kRdxPoison;
        ctx.rcx.qword = kRcxPoison;
        ctx.rbx.qword = kRbxPoison;
        ctx.rdi.qword = data;
        ctx.rsp.qword = stack;
        ctx.rip.qword = code_addr;
        Out o;
        o.exit = int(core->Run());
        std::memcpy(o.obs.data() + kObsYmm0, ctx.xmms[0].b, 16);
        std::memcpy(o.obs.data() + kObsYmm0 + 16, ctx.ymm_high[0].b, 16);
        const u64 rcx = ctx.rcx.qword;
        const u64 rsi = ctx.rsi.qword;
        const u64 rax = ctx.rax.qword;
        const u64 rdx = ctx.rdx.qword;
        std::memcpy(o.obs.data() + kObsRcx, &rcx, 8);
        std::memcpy(o.obs.data() + kObsRsi, &rsi, 8);
        std::memcpy(o.obs.data() + kObsRax, &rax, 8);
        std::memcpy(o.obs.data() + kObsRdx, &rdx, 8);
        std::memcpy(o.obs.data() + kObsYmm1, ctx.xmms[1].b, 16);
        std::memcpy(o.obs.data() + kObsYmm1 + 16, ctx.ymm_high[1].b, 16);
        std::memcpy(o.obs.data() + kObsYmm2, ctx.xmms[2].b, 16);
        std::memcpy(o.obs.data() + kObsYmm2 + 16, ctx.ymm_high[2].b, 16);
        for (u32 i = 0; i < 16; ++i) {
            std::memcpy(o.ymm[i].data(), ctx.xmms[i].b, 16);
            std::memcpy(o.ymm[i].data() + 16, ctx.ymm_high[i].b, 16);
        }
        o.rbx = ctx.rbx.qword;
        return o;
    };

    size_t code_cursor = 1;
    std::vector<std::string> problems;
    size_t comparisons = 0, bad_exits = 0, divergences = 0, mismatches = 0, bystanders = 0;

    for (const auto& ref : kSse42StrRefs) {
        // The VEX twins need the AVX gate; without it their rows would be
        // reported as decode failures rather than skipped.
        if (IsVex(ref.name) && !avx_on) {
            continue;
        }
        std::vector<u8> code = prefix;
        for (const u8 byte : ParseHex(ref.enc)) {
            code.push_back(byte);
        }
        for (const u8 byte : suffix) {
            code.push_back(byte);
        }
        code.push_back(0xF4);  // hlt
        const u64 code_addr = base + 0x1000 + code_cursor * 0x100;
        ++code_cursor;
        REQUIRE(code.size() < 0x100);
        REQUIRE(code_addr + 0x100 < stack);
        const Obs want = ParseObs(ref.result);
        const std::string label = fmt::format("{}/{}/imm{:02x}/lens{}/mem{}", ref.name,
                                              kSse42StrPairs[ref.pair].name, ref.imm, ref.lens,
                                              ref.mem);

        const auto jit = run_on(jit_core, code, ref, code_addr);
        const auto itp = run_on(interp_core, code, ref, code_addr);
        ++comparisons;

        if (jit.exit != int(swift::translator::None)) {
            // FALLBACK / ILL_CODE both land here: the encoding was DECLINED
            // rather than mis-executed.  For a guest that is fatal.
            if (bad_exits++ < 15) {
                problems.push_back(fmt::format(
                        "{}: block did not reach HLT (exit={}); encoding {} was not decoded",
                        label, jit.exit, ref.enc));
            }
            continue;
        }
        if (jit.obs != itp.obs || jit.exit != itp.exit) {
            if (divergences++ < 15) {
                problems.push_back(fmt::format("{}: JIT/interpreter divergence ({} vs {})", label,
                                               Hex(jit.obs), Hex(itp.obs)));
            }
        }

        for (const auto& [backend, got] : {std::pair<const char*, const Out*>{"jit", &jit},
                                           std::pair<const char*, const Out*>{"interp", &itp}}) {
            if (got->obs != want && mismatches++ < 15) {
                problems.push_back(fmt::format("{} [{}]: got {}, Rosetta says {} (enc {})", label,
                                               backend, Hex(got->obs), Hex(want), ref.enc));
            }
            if (got->rbx != kRbxPoison && bystanders++ < 15) {
                problems.push_back(fmt::format("{} [{}]: bystander rbx clobbered ({:#x})", label,
                                               backend, got->rbx));
            }
            for (u32 i = 3; i < 16; ++i) {
                if (got->ymm[i] != Poison(i) && bystanders++ < 15) {
                    problems.push_back(fmt::format("{} [{}]: bystander ymm{} clobbered, {} != {}",
                                                   label, backend, i, Hex(got->ymm[i]),
                                                   Hex(Poison(i))));
                }
            }
        }
    }

    X86Core::Destroy(jit_core);
    X86Core::Destroy(interp_core);
    X86Instance::Destroy(jit_instance);
    X86Instance::Destroy(interp_instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(arena, kArenaSize);

    std::string joined;
    for (const auto& p : problems) {
        joined += "\n  " + p;
    }
    INFO("comparisons=" << comparisons << " undecoded=" << bad_exits
                        << " jit/interp divergences=" << divergences
                        << " mismatches=" << mismatches << " bystanders=" << bystanders << joined);
    REQUIRE(comparisons > (avx_on ? 6000 : 4000));
    REQUIRE(problems.empty());
}

// ===========================================================================
// The second operand's memory form must read only its own 16 bytes.
// ===========================================================================
// A 16-byte load at the very end of a mapping is legal; a 32-byte one is not.
// The Rosetta differential above cannot see the difference -- the extra bytes
// never reach the result -- so a handler that read a full YMM would pass all
// 6104 rows.  Placing the operand in the last 16 bytes of a mapped page whose
// successor is PROT_NONE turns that into a fault instead.
//
// Nothing here depends on guest fault handling: in the PASSING case no fault
// is raised at all.  The failure mode is an abort of the whole test binary
// with "[SwiftVM] unhandled host fault: SIGBUS", because this arena is plain
// host mmap rather than a guest mapping the runtime's handler recognizes.
TEST_CASE("x86 sse4.2 string compare reads only 16 bytes of memory") {
    const char* gate = std::getenv("SVM_SSE42STR");
    if (gate && std::strcmp(gate, "0") == 0) {
        SUCCEED("SVM_SSE42STR=0 disables the handlers; guard-page case skipped");
        return;
    }
    constexpr size_t kPage = 0x4000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    // Three pages: code+stack, the operand page, and a guard.
    void* arena = mmap(nullptr, kPage * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    REQUIRE(mprotect(reinterpret_cast<void*>(base + kPage * 3), kPage, PROT_NONE) == 0);
    const u64 operand = base + kPage * 3 - 16;  // last 16 bytes before the guard

    ScopedJitEnv jit_env;
    setenv("SVM_ENABLE_JIT", "1", 1);
    auto* instance = X86Instance::Make();
    auto* core = X86Core::Make(instance);

    // pcmpistri xmm1, [rdi], 0x1a  --  the encoding glibc's strlen uses.
    const std::vector<u8> code = {0x66, 0x0F, 0x3A, 0x63, 0x0F, 0x1A, 0xF4};
    std::memcpy(reinterpret_cast<void*>(base + 0x100), code.data(), code.size());
    for (u32 i = 0; i < 16; ++i) {
        reinterpret_cast<u8*>(operand)[i] = u8('a' + i);
    }
    auto& ctx = core->GetContext();
    std::memset(&ctx, 0, sizeof(ctx));
    for (u32 i = 0; i < 16; ++i) {
        ctx.xmms[1].b[i] = u8('a' + i);
    }
    ctx.rdi.qword = operand;
    ctx.rsp.qword = base + kPage * 2;
    ctx.rip.qword = base + 0x100;
    const int exit = int(core->Run());
    INFO("the block did not reach HLT, so either the handler is not claiming pcmpistri or the "
         "16-byte load ran into the guard page");
    REQUIRE(exit == int(swift::translator::None));
    // imm8 0x1a = unsigned bytes, equal each, negative polarity, lsb index:
    // identical 16-byte strings with no terminator give IntRes2 == 0 -> 16.
    INFO("pcmpistri on two identical, unterminated 16-byte strings must return 16");
    REQUIRE(ctx.rcx.qword == 16);

    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(arena, kPage * 4);
}

// ===========================================================================
// Register aliasing and REX-extended registers.
// ===========================================================================
// Every Rosetta row above uses xmm1 as the first operand and xmm2 as the
// second, so two things they cannot reach are checked here:
//
//   * XMM0 -- the mask forms' IMPLICIT destination -- being ALSO an input.
//     `pcmpistrm xmm0, xmm0, imm8` reads and writes the same register, and a
//     handler that wrote the destination before reading its sources would
//     compute against its own output.
//   * Register numbers 8..15, which need REX.R/REX.B in the legacy encoding
//     and VEX.R/VEX.B in the VEX one.  A dropped REX bit silently reads or
//     writes xmm0..7 instead, and every reference row would still pass.
//
// There is no new oracle here: the expectation comes from Sdm::Evaluate, the
// model the 6104 Rosetta rows above already validated.  This case tests the
// front end's operand plumbing, not the semantics.
TEST_CASE("x86 sse4.2 string compare aliasing and REX-extended registers") {
    const char* gate = std::getenv("SVM_SSE42STR");
    if (gate && std::strcmp(gate, "0") == 0) {
        SUCCEED("SVM_SSE42STR=0 disables the handlers; aliasing case skipped");
        return;
    }
    const char* avx_env = std::getenv("SVM_AVX");
    const bool avx_on = avx_env && std::strcmp(avx_env, "0") != 0;

    struct AliasCase {
        const char* label;
        const char* enc;
        int reg1;  // ModRM.reg, REX.R / VEX.R folded in
        int rm;    // ModRM.rm; -1 means the memory operand at [rsi]
    };
    // The flag words come from the same push/popfq + pushfq wrapper the
    // reference rows use, so a row here and a row there are comparable.
    static const AliasCase kCases[] = {
            // legacy, XMM0 as an input of a form that also writes it
            {"pcmpistrm xmm0,xmm2,0c", "660f3a62c20c", 0, 2},
            {"pcmpistrm xmm1,xmm0,0c", "660f3a62c80c", 1, 0},
            {"pcmpistrm xmm0,xmm0,0c", "660f3a62c00c", 0, 0},
            {"pcmpistrm xmm0,xmm0,4c", "660f3a62c04c", 0, 0},
            {"pcmpistri xmm0,xmm2,1a", "660f3a63c21a", 0, 2},
            // legacy, REX.R + REX.B
            {"pcmpistri xmm15,xmm14,1a", "66450f3a63fe1a", 15, 14},
            {"pcmpistrm xmm15,xmm14,4c", "66450f3a62fe4c", 15, 14},
            {"pcmpestri xmm15,xmm14,0d", "66450f3a61fe0d", 15, 14},
            {"pcmpestrm.W xmm15,xmm14,0d", "664d0f3a60fe0d", 15, 14},
            {"pcmpistri xmm9,xmm3,1a", "66440f3a63cb1a", 9, 3},
            // legacy, memory operand on a base other than the one the
            // reference rows use
            {"pcmpistri xmm3,[rsi],1a", "660f3a631e1a", 3, -1},
            {"pcmpistrm xmm0,[rsi],4c", "660f3a62064c", 0, -1},
            // VEX
            {"vpcmpistrm xmm0,xmm2,4c", "c4e37962c24c", 0, 2},
            {"vpcmpistrm xmm0,xmm0,4c", "c4e37962c04c", 0, 0},
            {"vpcmpistrm xmm15,xmm14,4c", "c4437962fe4c", 15, 14},
            {"vpcmpistri xmm15,xmm14,1a", "c4437963fe1a", 15, 14},
            {"vpcmpestri.W xmm0,xmm2,0d", "c4e3f961c20d", 0, 2},
            {"vpcmpistri xmm3,[rsi],1a", "c4e379631e1a", 3, -1},
    };

    std::vector<Vec256> pool_a, pool_b;
    for (const auto& p : kSse42StrPairs) {
        pool_a.push_back(ParseHex32(p.a));
        pool_b.push_back(ParseHex32(p.b));
    }
    // A distinct 16-byte value per xmm register, drawn from the same inputs
    // the reference rows use so the terminators and ranges are meaningful.
    const auto reg_data = [&](u32 i) {
        return (i % 2) ? pool_b[(i / 2) % pool_b.size()] : pool_a[(i / 2) % pool_a.size()];
    };
    const Vec256 mem_data = pool_b[3];
    const auto mask_poison = MaskPoison();
    const auto prefix = ParseHex(kSse42StrPrefix);
    const auto suffix = ParseHex(kSse42StrSuffix);

    constexpr size_t kArenaSize = 0x100000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x80000;
    const u64 data = base + 0xC0000;

    ScopedJitEnv jit_env;
    setenv("SVM_ENABLE_JIT", "1", 1);
    auto* jit_instance = X86Instance::Make();
    setenv("SVM_ENABLE_JIT", "0", 1);
    auto* interp_instance = X86Instance::Make();
    auto* jit_core = X86Core::Make(jit_instance);
    auto* interp_core = X86Core::Make(interp_instance);

    // Lengths for the explicit-length cases: -3 and 5, so the absolute value
    // and the two different lengths both matter.
    constexpr u64 kRawLen1 = 0xFFFFFFFFFFFFFFFDull;
    constexpr u64 kRawLen2 = 5;

    std::vector<std::string> problems;
    size_t checked = 0;
    size_t cursor = 1;
    for (const auto& c : kCases) {
        const auto enc = ParseHex(c.enc);
        const bool vex = enc[0] == 0xC4;
        if (vex && !avx_on) {
            continue;
        }
        const bool wide = RowIsWide(enc);
        const u8 opcode = RowOpcode(enc);
        REQUIRE(opcode >= 0x60);
        REQUIRE(opcode <= 0x63);
        const bool mask_form = opcode == 0x60 || opcode == 0x62;
        const bool explicit_length = opcode == 0x60 || opcode == 0x61;
        const u8 imm8 = enc.back();

        std::vector<u8> code = prefix;
        code.insert(code.end(), enc.begin(), enc.end());
        code.insert(code.end(), suffix.begin(), suffix.end());
        code.push_back(0xF4);
        const u64 code_addr = base + 0x1000 + cursor * 0x100;
        ++cursor;

        const Vec256 src1 = reg_data(u32(c.reg1));
        const Vec256 src2 = c.rm < 0 ? mem_data : reg_data(u32(c.rm));
        const auto model = Sdm::Evaluate(src1, src2, imm8, explicit_length, kRawLen1, kRawLen2,
                                         wide);

        for (const char* backend : {"jit", "interp"}) {
            auto* core = std::strcmp(backend, "jit") == 0 ? jit_core : interp_core;
            std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
            std::memcpy(reinterpret_cast<void*>(data), mem_data.data(), 32);
            auto& ctx = core->GetContext();
            for (u32 i = 0; i < 16; ++i) {
                const auto v = reg_data(i);
                std::memcpy(ctx.xmms[i].b, v.data(), 16);
                std::memcpy(ctx.ymm_high[i].b, mask_poison.data() + 16, 16);
            }
            ctx.rax.qword = explicit_length ? kRawLen1 : 0;
            ctx.rdx.qword = explicit_length ? kRawLen2 : 0;
            ctx.rcx.qword = kRcxPoison;
            ctx.rsi.qword = data;
            ctx.rsp.qword = stack;
            ctx.rip.qword = code_addr;
            const int exit = int(core->Run());
            if (exit != int(swift::translator::None)) {
                problems.push_back(fmt::format("{} [{}]: block did not reach HLT (exit={})",
                                               c.label, backend, exit));
                continue;
            }
            if (mask_form) {
                std::array<u8, 16> got{};
                std::memcpy(got.data(), ctx.xmms[0].b, 16);
                if (got != model.xmm) {
                    problems.push_back(fmt::format("{} [{}]: XMM0 {} != model {}", c.label,
                                                   backend, Hex(got), Hex(model.xmm)));
                }
                std::array<u8, 16> high{};
                std::memcpy(high.data(), ctx.ymm_high[0].b, 16);
                const bool zeroed = std::all_of(high.begin(), high.end(),
                                                [](u8 x) { return x == 0; });
                const bool kept = std::equal(high.begin(), high.end(), mask_poison.begin() + 16);
                if (vex ? !zeroed : !kept) {
                    problems.push_back(fmt::format(
                            "{} [{}]: ymm0[255:128] is {} but contract C3 wants {}", c.label,
                            backend, Hex(high), vex ? "zeros" : "the poison"));
                }
                if (ctx.rcx.qword != kRcxPoison) {
                    problems.push_back(fmt::format("{} [{}]: a mask form wrote RCX", c.label,
                                                   backend));
                }
            } else {
                if (ctx.rcx.qword != u64(model.index)) {
                    problems.push_back(fmt::format("{} [{}]: ECX {:#x} != model {:#x}", c.label,
                                                   backend, ctx.rcx.qword, model.index));
                }
                std::array<u8, 32> ymm0{};
                std::memcpy(ymm0.data(), ctx.xmms[0].b, 16);
                std::memcpy(ymm0.data() + 16, ctx.ymm_high[0].b, 16);
                std::array<u8, 32> want0{};
                std::memcpy(want0.data(), reg_data(0).data(), 16);
                std::memcpy(want0.data() + 16, mask_poison.data() + 16, 16);
                if (ymm0 != want0) {
                    problems.push_back(fmt::format("{} [{}]: an index form touched ymm0", c.label,
                                                   backend));
                }
            }
            if ((ctx.rsi.qword & kFlagsMask) != model.flags) {
                problems.push_back(fmt::format("{} [{}]: flags {:#05x} != model {:#05x}", c.label,
                                               backend, ctx.rsi.qword & kFlagsMask, model.flags));
            }
            // The source registers must be unchanged, including when one of
            // them is the destination's register number.
            for (u32 i = 0; i < 16; ++i) {
                if (mask_form && i == 0) continue;
                std::array<u8, 16> got{};
                std::memcpy(got.data(), ctx.xmms[i].b, 16);
                std::array<u8, 16> want{};
                std::memcpy(want.data(), reg_data(i).data(), 16);
                if (got != want) {
                    problems.push_back(fmt::format("{} [{}]: xmm{} clobbered", c.label, backend,
                                                   i));
                    break;
                }
            }
            ++checked;
        }
    }

    X86Core::Destroy(jit_core);
    X86Core::Destroy(interp_core);
    X86Instance::Destroy(jit_instance);
    X86Instance::Destroy(interp_instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(arena, kArenaSize);

    std::string joined;
    for (const auto& p : problems) {
        joined += "\n  " + p;
    }
    INFO("checked=" << checked << joined);
    REQUIRE(problems.empty());
    REQUIRE(checked >= (avx_on ? 30u : 22u));
}


// ===========================================================================
// The three evaluators must agree, bit for bit, everywhere.
// ===========================================================================
// decoder_sse42str.cc carries the original cell-at-a-time evaluator as its
// SPECIFICATION and a bitmask/NEON one as the implementation.  That is only
// safe if the two are pinned to each other over more inputs than the 6104
// Rosetta rows reach -- the rows cover 128 of the 256 imm8 values, 16 of the
// 289 explicit length pairs, and twelve operand pairs.
//
// This case covers EVERY imm8 (all 256, including the reserved bit 7), EVERY
// explicit length pair (17 x 17, including the out-of-range 17..31 encodings
// the IR can never produce but the helper must survive), the implicit form,
// and twenty operand pairs -- the twelve Rosetta ones plus eight built to
// stress the algebra's edges: both operands empty, no terminator at all,
// terminators at opposite ends, and the 0x00 / 0x7F / 0x80 / 0xFF corners
// where signed and unsigned "ranges" disagree.
//
// It also covers the PORTABLE path on an ARM host, which no other test does:
// the runtime picks NEON here, so without variant 1 the scalar fallback would
// only ever be exercised on a RISC-V build nobody runs the suite on.
TEST_CASE("x86 sse4.2 string compare evaluators agree") {
    struct Operands {
        const char* name;
        u64 a_lo, a_hi, b_lo, b_hi;
    };
    std::vector<Operands> cases;
    for (const auto& p : kSse42StrPairs) {
        const auto a = ParseHex32(p.a);
        const auto b = ParseHex32(p.b);
        u64 al, ah, bl, bh;
        std::memcpy(&al, a.data(), 8);
        std::memcpy(&ah, a.data() + 8, 8);
        std::memcpy(&bl, b.data(), 8);
        std::memcpy(&bh, b.data() + 8, 8);
        cases.push_back({p.name, al, ah, bl, bh});
    }
    cases.push_back({"both empty", 0, 0, 0, 0});
    cases.push_back({"a empty", 0, 0, 0x0807060504030201ull, 0x100F0E0D0C0B0A09ull});
    cases.push_back({"b empty", 0x0807060504030201ull, 0x100F0E0D0C0B0A09ull, 0, 0});
    cases.push_back({"identical, no terminator", 0x0807060504030201ull, 0x100F0E0D0C0B0A09ull,
                     0x0807060504030201ull, 0x100F0E0D0C0B0A09ull});
    cases.push_back({"terminators at opposite ends", 0x0807060504030200ull,
                     0x100F0E0D0C0B0A09ull, 0x0807060504030201ull, 0x000F0E0D0C0B0A09ull});
    cases.push_back({"all ones", ~u64(0), ~u64(0), ~u64(0), ~u64(0)});
    cases.push_back({"signed corners", 0x7F80FF007F80FF00ull, 0x01FE8081017E8081ull,
                     0x80FF007F80FF007Full, 0xFE8081017E808101ull});
    cases.push_back({"one element apart", 0x0807060504030201ull, 0x100F0E0D0C0B0A09ull,
                     0x0807060504030202ull, 0x100F0E0D0C0B0A09ull});

    size_t compared = 0;
    std::vector<std::string> problems;
    const auto check = [&](const Operands& o, u64 ctl, const char* shape) {
        const u64 want = SwiftSse42StrEvalVariant(0, o.a_lo, o.a_hi, o.b_lo, o.b_hi, ctl);
        for (unsigned variant : {1u, 2u}) {
            const u64 got = SwiftSse42StrEvalVariant(variant, o.a_lo, o.a_hi, o.b_lo, o.b_hi, ctl);
            if (got != want && problems.size() < 12) {
                problems.push_back(fmt::format(
                        "{} / {} / ctl {:#x}: variant {} returned {:#x}, the reference "
                        "(specification) says {:#x}",
                        o.name, shape, ctl, variant, got, want));
            }
        }
        ++compared;
    };
    for (const auto& o : cases) {
        for (u32 imm = 0; imm < 256; ++imm) {
            check(o, imm, "implicit");
            for (u32 len1 = 0; len1 <= 16; ++len1) {
                for (u32 len2 = 0; len2 <= 16; ++len2) {
                    check(o, u64(imm) | (u64(1) << 8) | (u64(len1) << 9) | (u64(len2) << 14),
                          "explicit");
                }
            }
        }
    }
    // Out-of-range length encodings: the IR saturates before packing, so these
    // are unreachable in practice, but the helper must not index out of its
    // arrays if it is ever handed one.
    for (const auto& o : cases) {
        for (u32 len = 17; len < 32; ++len) {
            for (u32 imm : {0x00u, 0x04u, 0x08u, 0x0Cu, 0x1Au, 0x4Cu}) {
                check(o, u64(imm) | (u64(1) << 8) | (u64(len) << 9) | (u64(len) << 14),
                      "explicit out of range");
            }
        }
    }

    std::string joined;
    for (const auto& p : problems) {
        joined += "\n  " + p;
    }
    INFO("compared=" << compared << joined);
    REQUIRE(problems.empty());
    INFO("the input space collapsed -- this case is meant to cover every imm8 and every "
         "length pair");
    REQUIRE(compared > 1400000);
}

// ===========================================================================
// The fast evaluator must actually be fast.
// ===========================================================================
// A TRIPWIRE, not a benchmark: it exists so that deleting the fast path, or
// accidentally routing the runtime back to the reference, fails a test instead
// of quietly costing an order of magnitude.  The bound is 4x where the
// measured ratio is 30x-90x, so host load cannot make it flaky.
//
// The comparison is interleaved inside ONE process and reduced with MIN over
// nine repetitions.  That is the only shape that means anything here: this
// host runs other agents' builds, and wall-clock numbers taken from separate
// runs vary by 3x (measured).  Set SVM_SSE42STR_BENCH=1 to print the table.
TEST_CASE("x86 sse4.2 string compare fast evaluator is fast") {
    struct Point {
        u64 a_lo, a_hi, b_lo, b_hi, ctl;
    };
    // One workload per aggregation, named by the libc routine whose ifunc
    // picks that imm8: 0x00 equal-any (strchr/strspn), 0x04 ranges,
    // 0x1a equal-each (strcmp), 0x0c equal-ordered (strstr).
    struct Workload {
        const char* name;
        u32 imm;
    };
    static const Workload kWorkloads[] = {
            {"equal any     (imm 0x00)", 0x00},
            {"ranges        (imm 0x04)", 0x04},
            {"equal each    (imm 0x1a)", 0x1A},
            {"equal ordered (imm 0x0c)", 0x0C},
    };
    const bool print = std::getenv("SVM_SSE42STR_BENCH") != nullptr;

    double total_reference = 0.0;
    double total_runtime = 0.0;
    std::string table;
    for (const auto& w : kWorkloads) {
        std::vector<Point> points;
        for (const auto& pair : kSse42StrPairs) {
            const auto a = ParseHex32(pair.a);
            const auto b = ParseHex32(pair.b);
            Point pt{};
            std::memcpy(&pt.a_lo, a.data(), 8);
            std::memcpy(&pt.a_hi, a.data() + 8, 8);
            std::memcpy(&pt.b_lo, b.data(), 8);
            std::memcpy(&pt.b_hi, b.data() + 8, 8);
            pt.ctl = w.imm;
            points.push_back(pt);
        }
        constexpr u32 kPasses = 2000;
        constexpr u32 kReps = 9;
        double best[3] = {1e18, 1e18, 1e18};
        u64 sink = 0;
        for (u32 rep = 0; rep < kReps; ++rep) {
            for (unsigned variant = 0; variant < 3; ++variant) {
                const auto t0 = std::chrono::steady_clock::now();
                for (u32 pass = 0; pass < kPasses; ++pass) {
                    for (const auto& pt : points) {
                        sink += SwiftSse42StrEvalVariant(variant, pt.a_lo, pt.a_hi, pt.b_lo,
                                                         pt.b_hi, pt.ctl);
                    }
                }
                const auto t1 = std::chrono::steady_clock::now();
                const double ns =
                        double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                                       .count()) /
                        double(kPasses * points.size());
                best[variant] = std::min(best[variant], ns);
            }
        }
        REQUIRE(sink != 0);
        const double ratio = best[0] / best[2];
        total_reference += best[0];
        total_runtime += best[2];
        table += fmt::format("\n  {}  reference {:6.2f} ns  portable {:6.2f} ns  "
                             "runtime {:6.2f} ns  speedup {:5.1f}x",
                             w.name, best[0], best[1], best[2], ratio);
    }
    if (print) {
        WARN("Sse42StrEval, min of 9 interleaved repetitions:" << table);
    }
    // Summed over the four aggregations rather than per-workload: "equal each"
    // is the one the reference already did in a single pass, so its individual
    // ratio is only ~4x and would make a per-workload bound tight enough to
    // flap.  The measured aggregate ratio is ~9x, so 4x is a two-fold margin.
    INFO("the runtime's evaluator is no longer meaningfully faster than the reference -- either "
         "the fast path was removed or Sse42StrEval no longer calls it:"
         << table);
    REQUIRE(total_reference > total_runtime * 4.0);
}

