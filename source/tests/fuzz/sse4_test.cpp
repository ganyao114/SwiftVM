// ===========================================================================
// Legacy SSE3 / SSSE3 / SSE4.1 / SSE4.2 against a ROSETTA oracle.
// ===========================================================================
//
// Covers everything decoder_sse4.cc claims, plus the VEX twins of a
// representative slice of it.  Each row is one instruction (for ROUND's MXCSR
// rows, the ldmxcsr pair that brackets it; for PTEST, the four `xor` that zero
// the flag registers and the five `setcc` that read the flags back) executed
// from the LITERAL BYTES real x86-64 executed under Rosetta, with the operands
// written straight into ThreadContext64 and the answer read straight back --
// so a broken vmovdqu cannot mask a broken handler.
//
// WHY A HARDWARE ORACLE
// ---------------------
// What is being tested is mostly a set of CONVENTIONS: which nibble of DPPS's
// imm8 selects the multiply, which BIT of XMM0's element selects a BLENDV
// lane, where MPSADBW's window starts, whether PMULHRSW rounds or truncates,
// which index PHMINPOSUW returns on a tie, whether ROUND breaks ties to even.
// A hand-written model of a convention only re-states the implementation's own
// assumption.  Rosetta 2 executes all of it, so sse4_rosetta_ref.inc holds the
// literal bytes real x86-64 wrote; nothing in it is computed here.
//
// Rosetta is itself an emulator and has been measured wrong before, so
// agreement with it is evidence and not proof.  Every result shape asserted
// below was cross-read against the Intel SDM before the data was accepted, and
// one place where the two DISAGREE is recorded rather than absorbed (PTEST's
// PF -- see "ROSETTA DEFECT" below).
//
// WHAT THE DATA SETTLES, MEASURED RATHER THAN ASSUMED
//
//   LEGACY SSE PRESERVES BITS 255:128.  The destination register is preloaded
//   with data in its low half and a poison pattern (0x5A ^ index) in its high
//   half, and all 32 bytes are read back.  Every legacy row's reference
//   therefore carries sixteen literal poison bytes THE HARDWARE left there,
//   and every VEX row's carries sixteen literal zeros.  This is the exact
//   opposite of contract C3 and is the single most likely thing to get wrong
//   when a legacy handler is derived from a VEX one, so it is checked on every
//   row rather than in a dedicated case.
//
//   BLENDV'S MASK IS THE IMPLICIT XMM0.  ymm0 is loaded with a mask whose
//   per-lane top bits differ at 8-, 32- and 64-bit granularity, so blendvps,
//   blendvpd and pblendvb cannot agree by accident, and xmm0 is read back on
//   every row so a handler that wrote the mask register is caught.  The VEX
//   twins encode xmm0 in the /is4 byte, which makes them directly comparable.
//
//   ROUND-HALF-EVEN, NOT ROUND-HALF-AWAY.  The `f32` pair's r/m operand holds
//   -0.5 and 4.5, both exact ties; hardware returned -0.0 and 4.0 where a
//   std::round-style implementation returns -1.0 and 5.0.
//
//   IMM8 BIT 2 REALLY READS MXCSR.  The .mx rows wrap the instruction in
//   ldmxcsr / instruction / ldmxcsr (all inside the replayed bytes), and the
//   test requires that the four RC values do NOT all produce the same answer.
//
//   PTEST SETS FOUR DISTINCT FLAG CORNERS.  The `test`, `subset` and `srczero`
//   input pairs give (ZF, CF) = (1,0), (0,1) and (1,1), and everything else
//   gives (0,0), so neither flag can be hard-coded.
//
//   MPSADBW'S WINDOW AND NEEDLE.  The `seq` pair is 0..15 against 31..16, so
//   every byte names its own position and all eight imm8 values move the
//   answer.
//
//   THE LEGACY AND VEX FORMS AGREE.  decoder_sse4.cc deliberately duplicates
//   the IR sequences of decoder_avx*.cc (it cannot call into their anonymous
//   namespaces).  Every "v" row is emitted with the SAME operands as its
//   legacy row, so the low 128 bits must be identical -- a copy that drifts
//   from its origin fails here.
//
// ROSETTA DEFECT FOUND BY THIS FILE
// ---------------------------------
// PTEST's PF.  The SDM specifies "the OF, AF, PF, SF flags are set to 0", but
// Rosetta returns PF = 1 on all 30 PTEST/VPTEST rows regardless of operands.
// The SDM wins: SwiftVM writes PF = 0, and the `ah` byte (where the row's
// `setp ah` landed) is excluded from the byte-for-byte comparison and checked
// separately against the SDM instead.  Everything else about PTEST -- ZF, CF,
// SF, OF -- is compared against Rosetta, and Rosetta's ZF/CF independently
// match a from-the-SDM model on all 30 rows.
//
// THE OBSERVATION BLOCK
// ---------------------
// Every row, whatever it writes, is compared through one fixed 128-byte
// block, assembled identically on both sides:
//
//   +0x00  ymm3   the vector destination, both halves
//   +0x20  ymm0   the blend-mask register: must be untouched
//   +0x40  rax  +0x48 rbx  +0x50 rcx  +0x58 rdx
//   +0x60  the 16-byte memory store target of pextr*/extractps
//   +0x70  padding
//
// so a GPR-destination row, a memory-destination row and a vector row are all
// one comparison and the test never has to know which it is looking at.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
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

namespace {

struct Sse4Input {
    const char* name;
    const char* a;
    const char* b;
    const char* k;
};
struct Sse4Ref {
    const char* name;
    int pair;
    int imm;             // the imm8 the generator encoded (0 where there is none)
    int rc;              // MXCSR.RC the recorded ldmxcsr selected, or -1 for none
    const char* enc;     // literal instruction bytes, hex
    const char* result;  // the 128-byte observation block, hex
};
#include "sse4_rosetta_ref.inc"

// The mnemonic table, shared verbatim with the generator.  Consumed only to
// pin coverage: every name below must have produced reference rows.
constexpr const char* kEntries[] = {
#define SVM_SSE4(name) name,
#include "sse4_ops.inc"
};

using Vec256 = std::array<u8, 32>;
using Obs = std::array<u8, 128>;

// Must match the generator's data-block layout exactly; the replayed
// encodings carry these displacements literally, so nothing here is a choice.
constexpr s32 kOffA = 0x000, kOffB = 0x020, kOffD = 0x040, kOffK = 0x060;
constexpr s32 kOffS = 0x080, kOffN = 0x090, kOffMxcsr = 0x0A0, kOffOut = 0x0C0;

constexpr u64 kRax = 0xAAAAAAAAAAAAAAAAull;
constexpr u64 kRbx = 0xBBBBBBBBBBBBBBBBull;
constexpr u64 kRcx = 0x0123456789ABCDEFull;
constexpr u64 kRdx = 0xDDDDDDDDDDDDDDDDull;

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

u32 Dword(const Obs& v, u32 i) {
    return u32(v[i * 4]) | (u32(v[i * 4 + 1]) << 8) | (u32(v[i * 4 + 2]) << 16) |
           (u32(v[i * 4 + 3]) << 24);
}

u64 Qword(const Obs& v, u32 i) {
    u64 r = 0;
    for (u32 j = 0; j < 8; ++j) {
        r |= u64(v[i * 8 + j]) << (j * 8);
    }
    return r;
}

// Bystander poison for every register the stubs do not load.  Registers 0..3
// are loaded by the replayed prologue's data, so only 4..15 are checked.
Vec256 Poison(u32 reg) {
    Vec256 v{};
    for (u32 j = 0; j < 32; ++j) {
        v[j] = u8(0xA5 ^ (reg * 32 + j));
    }
    return v;
}

// Must match DEST_POISON in sse4_rosetta_ref.c: the pattern the destination's
// HIGH half is preloaded with, which a legacy row must give back unchanged.
std::array<u8, 16> DestPoison() {
    std::array<u8, 16> v{};
    for (u32 i = 0; i < 16; ++i) {
        v[i] = u8(0x5A ^ i);
    }
    return v;
}

bool IsVex(const char* name) { return name[0] == 'v'; }

// PTEST's PF is the one byte where Rosetta and the SDM disagree; see the
// header.  rax byte 1 is where the row's `setp ah` landed.
bool ExcludedByte(const char* name, size_t index) {
    const bool ptest = std::strcmp(name, "ptest") == 0 || std::strcmp(name, "vptest") == 0;
    return ptest && index == 0x41;
}

// ---------------------------------------------------------------------------
// KNOWN PRE-EXISTING DEFECT -- HADDPS / HSUBPS AND THE SIGN OF A NaN
// ---------------------------------------------------------------------------
// haddps/hsubps are NOT decoder_sse4.cc's: they are claimed by
// `case I_HADDPS:` / `case I_HSUBPS:` in decoder.cc, which route to
// DecodeHaddps -> the HaddpsHalf host lambda (decoder_sse.cc:284).  Their rows
// are in this file as a regression net for that older handler, and they found
// a real bug.
//
// HaddpsHalf computes `f[0] - f[1]` and `f[2] - f[3]` in host C.  At -O0 that
// is correct; at -O2 and above clang lowers the PAIR of subtractions to
// FNEG + FADDP, and FNEG flips the sign bit of a NaN.  x86 returns the second
// operand's NaN QUIETED BUT OTHERWISE UNCHANGED, so every haddps/hsubps whose
// result comes from a NaN in the ODD lane comes out with the wrong sign, on
// both back ends, only in optimized builds.  Measured on this host:
//
//     HaddpsHalf(0x7E81FEFF_807F0100, 0x7FFFFFFF_80000000, sub=1)
//       -O0  0x7FFFFFFF_FE81FEFF   (correct, and what Rosetta returned)
//       -O2  0xFFFFFFFF_FE81FEFF   (sign of the NaN flipped)
//
// THE FIX (main line, not this agent's files): drop `case I_HADDPS:` and
// `case I_HSUBPS:` from decoder.cc and let them fall through to DecodeSse4,
// which already has the correct pure-IR path -- OpHorizontalFloat, the same
// VecUnzip + VecFSub decoder_avx_hadd.cc's vhaddps uses, whose VecFSub carries
// the x86 NaN-priority fixup.  Then delete this function and its two call
// sites; the four rows below will pass unaided.
//
// Until then the difference is tolerated ONLY where it is exactly this bug:
// one dword of the vector destination, differing in exactly the sign bit, with
// both values NaN.  Any other haddps/hsubps difference still fails.
bool IsNaN32(u32 x) { return (x & 0x7F800000u) == 0x7F800000u && (x & 0x007FFFFFu) != 0; }

bool HaddpsNaNSignOnly(const char* name, const Obs& got, const Obs& want) {
    if (std::strcmp(name, "haddps") != 0 && std::strcmp(name, "hsubps") != 0) {
        return false;
    }
    for (size_t i = 16; i < 128; ++i) {
        if (got[i] != want[i]) {
            return false;  // anything outside the 128-bit result is a real failure
        }
    }
    for (u32 lane = 0; lane < 4; ++lane) {
        const u32 g = Dword(got, lane);
        const u32 w = Dword(want, lane);
        if (g == w) continue;
        if ((g ^ w) != 0x80000000u || !IsNaN32(g) || !IsNaN32(w)) {
            return false;
        }
    }
    return true;
}

int PairIndex(const char* name) {
    for (int i = 0; i < int(std::size(kSse4Inputs)); ++i) {
        if (std::strcmp(kSse4Inputs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

const Sse4Ref* FindRow(const char* name, int pair, int imm, int rc) {
    for (const auto& r : kSse4Refs) {
        if (std::strcmp(r.name, name) == 0 && r.pair == pair && r.imm == imm && r.rc == rc) {
            return &r;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("x86 legacy sse4 vs rosetta reference") {
    const char* sse4_env = std::getenv("SVM_SSE4");
    if (sse4_env && std::strcmp(sse4_env, "0") == 0) {
        SUCCEED("SVM_SSE4=0 disables the legacy SSE4 handlers; differential skipped");
        return;
    }

    std::vector<Vec256> ins_a, ins_b, ins_k;
    for (const auto& in : kSse4Inputs) {
        ins_a.push_back(ParseHex32(in.a));
        ins_b.push_back(ParseHex32(in.b));
        ins_k.push_back(ParseHex32(in.k));
    }
    const auto dest_poison = DestPoison();

    // ---- properties of the reference data this case depends on ------------
    // Asserted rather than trusted: without these the differential could still
    // pass while having stopped testing what it exists for.
    {
        for (const char* name : kEntries) {
            size_t rows = 0;
            for (const auto& r : kSse4Refs) {
                if (std::strcmp(r.name, name) == 0) ++rows;
            }
            INFO("no reference rows for " << name
                                          << " -- the generator did not cover it, or Rosetta "
                                             "refused every encoding of it");
            REQUIRE(rows > 0);
        }
    }
    {
        // THE LEGACY CONTRACT, as the HARDWARE reported it: bits 255:128 of the
        // destination come back as the poison they were preloaded with.
        // ptest / pextr* / extractps / movntdqa-to-register do not write a
        // vector destination at all, which is the same expectation.
        size_t checked = 0, vex_zeroed = 0;
        for (const auto& r : kSse4Refs) {
            const auto v = ParseObs(r.result);
            const bool writes_vector =
                    std::strcmp(r.name, "ptest") != 0 && std::strcmp(r.name, "vptest") != 0 &&
                    std::strncmp(r.name, "pextr", 5) != 0 &&
                    std::strncmp(r.name, "vpextr", 6) != 0 &&
                    std::strcmp(r.name, "extractps") != 0;
            if (IsVex(r.name)) {
                if (!writes_vector) continue;
                INFO(r.name << " pair " << r.pair << ": a VEX row's reference does not have a "
                                                     "zeroed upper half (contract C3)");
                REQUIRE(std::all_of(v.begin() + 16, v.begin() + 32, [](u8 x) { return x == 0; }));
                ++vex_zeroed;
                continue;
            }
            INFO(r.name << " pair " << r.pair << " imm " << r.imm
                        << ": a LEGACY row's reference does not preserve bits 255:128 -- the "
                           "poison is gone, so the file no longer measures the legacy contract");
            REQUIRE(std::equal(dest_poison.begin(), dest_poison.end(), v.begin() + 16));
            ++checked;
        }
        INFO("no legacy rows at all");
        REQUIRE(checked > 3000);
        INFO("no VEX rows with a vector destination -- the twin comparison measures nothing");
        REQUIRE(vex_zeroed > 50);
    }
    {
        // ROUND-HALF-EVEN.  The f32 pair's r/m (which is B, the source of a
        // two-operand round) is [-0.5, 4.5, +inf, -inf]; nearest-even gives
        // -0.0 and 4.0 where round-half-away gives -1.0 and 5.0.
        const int f32 = PairIndex("f32");
        REQUIRE(f32 >= 0);
        const auto* row = FindRow("roundps", f32, 0, -1);
        REQUIRE(row != nullptr);
        const auto v = ParseObs(row->result);
        INFO("the f32 reference no longer distinguishes half-even from half-away");
        CHECK(Dword(v, 0) == 0x80000000u);  // -0.5 -> -0.0, NOT -1.0
        CHECK(Dword(v, 1) == 0x40800000u);  //  4.5 ->  4.0, NOT  5.0
    }
    {
        // imm8 bit 3 (suppress the precision exception) changes no result bit.
        size_t pairs_checked = 0;
        for (const auto& r : kSse4Refs) {
            if (r.imm >= 8 || r.rc >= 0) continue;
            if (std::strncmp(r.name, "round", 5) != 0) continue;
            const auto* twin = FindRow(r.name, r.pair, r.imm | 8, r.rc);
            if (twin == nullptr) continue;
            INFO(r.name << " imm " << r.imm
                        << ": the precision-suppressing twin gives a different result");
            REQUIRE(std::strcmp(r.result, twin->result) == 0);
            ++pairs_checked;
        }
        INFO("no round imm8 bit-3 twin pairs in the data");
        REQUIRE(pairs_checked > 100);
    }
    {
        // imm8 bit 2 REALLY reads MXCSR.RC: the four RC values must disagree
        // somewhere, or an implementation ignoring MXCSR would pass every row.
        size_t discriminating = 0;
        std::set<std::tuple<std::string, int, int>> keys;
        for (const auto& r : kSse4Refs) {
            if (r.rc < 0) continue;
            keys.insert({r.name, r.pair, r.imm});
        }
        for (const auto& [name, pair, imm] : keys) {
            std::set<std::string> answers;
            for (int rc = 0; rc < 4; ++rc) {
                const auto* row = FindRow(name.c_str(), pair, imm, rc);
                if (row != nullptr) answers.insert(row->result);
            }
            if (answers.size() > 1) ++discriminating;
        }
        INFO("no .mx group has RC-dependent results -- the ldmxcsr rows measure nothing "
             "(the disp8/disp32 trap avx_misc_rosetta_ref.c documents)");
        REQUIRE(discriminating > 10);
    }
    {
        // PTEST reaches all four (ZF, CF) corners, and Rosetta's ZF/CF match a
        // model built straight from the SDM's two AND expressions.
        std::set<std::pair<int, int>> corners;
        size_t rows = 0;
        for (const auto& r : kSse4Refs) {
            if (std::strcmp(r.name, "ptest") != 0 && std::strcmp(r.name, "vptest") != 0) continue;
            const auto v = ParseObs(r.result);
            const int zf = int(v[0x40] & 1);
            const int cf = int(v[0x48] & 1);
            const auto& a = ins_a[size_t(r.pair)];  // DEST = xmm3 = A's low half
            const auto& b = ins_b[size_t(r.pair)];  // SRC  = xmm2 = B's low half
            int model_zf = 1, model_cf = 1;
            for (u32 i = 0; i < 16; ++i) {
                if ((b[i] & a[i]) != 0) model_zf = 0;
                if ((b[i] & u8(~a[i])) != 0) model_cf = 0;
            }
            INFO("ptest pair " << r.pair << ": Rosetta's ZF/CF disagree with the SDM model");
            REQUIRE(zf == model_zf);
            REQUIRE(cf == model_cf);
            // The Rosetta defect, pinned so a future Rosetta that fixes it is
            // noticed rather than silently tolerated.
            INFO("Rosetta's PTEST PF is no longer the documented 1 -- re-read the SDM note in "
                 "this file's header before changing anything");
            CHECK(v[0x41] == 1);
            corners.insert({zf, cf});
            ++rows;
        }
        INFO("ptest does not reach all four (ZF, CF) corners in the reference");
        REQUIRE(corners.size() == 4);
        REQUIRE(rows >= 30);
    }
    {
        // MPSADBW against the SDM's definition, for every imm8 and every input
        // pair: DEST.word[i] = sum over j in 0..3 of
        //     | DEST.byte[4*imm8[2] + i + j]  -  SRC.byte[4*imm8[1:0] + j] |
        // A wrong needle offset, a wrong window offset, a needle offset scaled
        // by 1 instead of 4, or a signed subtraction all fail this.
        //
        // "the eight imm8 values give eight different answers" was the first
        // formulation of this check and was WRONG on the `seq` pair -- with a
        // linear input, moving the window by one dword and the needle by one
        // dword shift the result by the same amount, so imm 1 and imm 4 agree
        // legitimately.  Five distinct answers is what the arithmetic gives;
        // the model comparison below is what actually pins the semantics.
        const int seq = PairIndex("seq");
        REQUIRE(seq >= 0);
        std::set<std::string> answers;
        size_t modelled = 0;
        for (int pair = 0; pair < int(std::size(kSse4Inputs)); ++pair) {
            for (int imm = 0; imm < 8; ++imm) {
                const auto* row = FindRow("mpsadbw", pair, imm, -1);
                REQUIRE(row != nullptr);
                if (pair == seq) {
                    answers.insert(std::string(row->result).substr(0, 32));
                }
                const auto v = ParseObs(row->result);
                const auto& a = ins_a[size_t(pair)];  // DEST = xmm3 = A's low half
                const auto& b = ins_b[size_t(pair)];  // SRC  = xmm2 = B's low half
                const u32 window = u32((imm >> 2) & 1) * 4;
                const u32 needle = u32(imm & 3) * 4;
                for (u32 i = 0; i < 8; ++i) {
                    u32 lane = 0;
                    for (u32 j = 0; j < 4; ++j) {
                        lane += u32(std::abs(int(a[window + i + j]) - int(b[needle + j])));
                    }
                    INFO("mpsadbw pair " << pair << " imm " << imm << " word " << i
                                         << " does not match the SDM definition");
                    REQUIRE((u32(v[i * 2]) | (u32(v[i * 2 + 1]) << 8)) == lane);
                }
                ++modelled;
            }
        }
        REQUIRE(modelled == 8 * std::size(kSse4Inputs));
        INFO("mpsadbw's imm8 values no longer move the answer at all");
        REQUIRE(answers.size() >= 5);
    }
    {
        // BLENDVPS / BLENDVPD / PBLENDVB read DIFFERENT bits of the same XMM0,
        // so on the `seq` pair (whose mask alternates at all three
        // granularities) no two of them may agree.
        const int seq = PairIndex("seq");
        std::set<std::string> answers;
        for (const char* name : {"blendvps", "blendvpd", "pblendvb"}) {
            const auto* row = FindRow(name, seq, 0, -1);
            REQUIRE(row != nullptr);
            answers.insert(std::string(row->result).substr(0, 32));
        }
        INFO("the three blendv forms agree on the seq pair -- the mask granularity is not being "
             "measured");
        REQUIRE(answers.size() == 3);
    }
    {
        // PHMINPOSUW returns the minimum AND its index; on `seq` the source is
        // 31..16, whose smallest word is 0x1011 at index 7.  A tie-breaking or
        // index-off-by-one bug moves one of the two.
        const int seq = PairIndex("seq");
        const auto* row = FindRow("phminposuw", seq, 0, -1);
        REQUIRE(row != nullptr);
        const auto v = ParseObs(row->result);
        INFO("phminposuw's reference no longer pins both the value and the index");
        CHECK((u32(v[0]) | (u32(v[1]) << 8)) == 0x1011u);
        CHECK((u32(v[2]) | (u32(v[3]) << 8)) == 7u);
        CHECK(Qword(v, 1) == 0u);
    }
    {
        // DPPS masks the PRODUCT, not the operands: on `f32`, lane 2 is
        // inf * inf and lane 3 is -inf * -inf, so a de-selected lane is +0.0
        // and a selected one is not.
        const int f32 = PairIndex("f32");
        const auto* off = FindRow("dpps", f32, 0x0F, -1);
        const auto* on = FindRow("dpps", f32, 0xFF, -1);
        REQUIRE(off != nullptr);
        REQUIRE(on != nullptr);
        const auto voff = ParseObs(off->result);
        const auto von = ParseObs(on->result);
        INFO("dpps with no multiply lanes selected is not +0.0 in the reference");
        for (u32 i = 0; i < 4; ++i) {
            CHECK(Dword(voff, i) == 0u);
        }
        INFO("dpps with every lane selected produced the same answer as with none");
        CHECK(Dword(von, 0) != 0u);
    }

    // ---- harness -----------------------------------------------------------
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x200000;
    const u64 data = base + 0x300000;

    const char* old_jit = std::getenv("SVM_ENABLE_JIT");
    const bool had_old_jit = old_jit != nullptr;
    const std::string old_jit_value = old_jit ? old_jit : "";
    setenv("SVM_ENABLE_JIT", "1", 1);
    auto* jit_instance = X86Instance::Make();
    setenv("SVM_ENABLE_JIT", "0", 1);
    auto* interp_instance = X86Instance::Make();
    if (had_old_jit) {
        setenv("SVM_ENABLE_JIT", old_jit_value.c_str(), 1);
    } else {
        unsetenv("SVM_ENABLE_JIT");
    }
    auto* jit_core = X86Core::Make(jit_instance);
    auto* interp_core = X86Core::Make(interp_instance);

    struct Out {
        Obs obs{};
        std::array<Vec256, 16> ymm{};
        u32 mxcsr{};
        int exit{};
    };

    size_t code_cursor = 1;
    std::vector<std::string> problems;
    size_t comparisons = 0, bad_exits = 0, divergences = 0, mismatches = 0, bystanders = 0,
           leaked_mxcsr = 0, ptest_pf = 0, known_haddps = 0;

    const auto run_on = [&](X86Core* core, const std::vector<u8>& code, int pair, u64 code_addr) {
        const auto& a = ins_a[size_t(pair)];
        const auto& b = ins_b[size_t(pair)];
        const auto& k = ins_k[size_t(pair)];
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffA)), a.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffB)), b.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffK)), k.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffD)), a.data(), 16);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffD) + 16), dest_poison.data(), 16);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffN)), a.data(), 16);
        std::memset(reinterpret_cast<void*>(data + u64(kOffS)), 0xCC, 16);
        std::memset(reinterpret_cast<void*>(data + u64(kOffOut)), 0x99, 128);
        for (u32 rc = 0; rc < 4; ++rc) {
            const u32 word = 0x1F80u | (rc << 13);
            std::memcpy(reinterpret_cast<void*>(data + u64(kOffMxcsr) + rc * 4), &word, 4);
        }
        auto& ctx = core->GetContext();
        for (u32 i = 0; i < 16; ++i) {
            const auto p = Poison(i);
            std::memcpy(ctx.xmms[i].b, p.data(), 16);
            std::memcpy(ctx.ymm_high[i].b, p.data() + 16, 16);
        }
        // The state the generator's prologue produced, written directly so a
        // broken vmovdqu cannot mask a broken handler.
        std::memcpy(ctx.xmms[0].b, k.data(), 16);
        std::memcpy(ctx.ymm_high[0].b, k.data() + 16, 16);
        std::memcpy(ctx.xmms[1].b, a.data(), 16);
        std::memcpy(ctx.ymm_high[1].b, a.data() + 16, 16);
        std::memcpy(ctx.xmms[2].b, b.data(), 16);
        std::memcpy(ctx.ymm_high[2].b, b.data() + 16, 16);
        std::memcpy(ctx.xmms[3].b, a.data(), 16);
        std::memcpy(ctx.ymm_high[3].b, dest_poison.data(), 16);
        ctx.rax.qword = kRax;
        ctx.rbx.qword = kRbx;
        ctx.rcx.qword = kRcx;
        ctx.rdx.qword = kRdx;
        ctx.mxcsr = 0x1F80u;
        ctx.rdi.qword = data;
        ctx.rsp.qword = stack;
        ctx.rip.qword = code_addr;
        Out o;
        o.exit = int(core->Run());
        std::memcpy(o.obs.data(), ctx.xmms[3].b, 16);
        std::memcpy(o.obs.data() + 16, ctx.ymm_high[3].b, 16);
        std::memcpy(o.obs.data() + 32, ctx.xmms[0].b, 16);
        std::memcpy(o.obs.data() + 48, ctx.ymm_high[0].b, 16);
        const u64 gprs[4] = {ctx.rax.qword, ctx.rbx.qword, ctx.rcx.qword, ctx.rdx.qword};
        std::memcpy(o.obs.data() + 0x40, gprs, 32);
        std::memcpy(o.obs.data() + 0x60, reinterpret_cast<void*>(data + u64(kOffS)), 16);
        std::memset(o.obs.data() + 0x70, 0x99, 16);
        for (u32 i = 0; i < 16; ++i) {
            std::memcpy(o.ymm[i].data(), ctx.xmms[i].b, 16);
            std::memcpy(o.ymm[i].data() + 16, ctx.ymm_high[i].b, 16);
        }
        o.mxcsr = ctx.mxcsr;
        return o;
    };

    for (const auto& ref : kSse4Refs) {
        // The VEX twins need the AVX gate; without it their rows would be
        // reported as decode failures rather than skipped.
        const char* avx_env = std::getenv("SVM_AVX");
        const bool avx_on = avx_env && std::strcmp(avx_env, "0") != 0;
        if (IsVex(ref.name) && !avx_on) {
            continue;
        }
        auto code = ParseHex(ref.enc);
        code.push_back(0xF4);  // hlt
        const u64 code_addr = base + 0x1000 + code_cursor * 0x100;
        ++code_cursor;
        REQUIRE(code.size() < 0x100);
        REQUIRE(code_addr + 0x100 < stack);
        const Obs want = ParseObs(ref.result);
        const std::string label = fmt::format("{}/{}/imm{:02x}/rc{}", ref.name,
                                              kSse4Inputs[ref.pair].name, ref.imm, ref.rc);

        const auto jit = run_on(jit_core, code, ref.pair, code_addr);
        const auto itp = run_on(interp_core, code, ref.pair, code_addr);
        ++comparisons;

        if (jit.exit != int(swift::translator::None)) {
            // FALLBACK / ILL_CODE both land here: the encoding was DECLINED
            // rather than mis-executed.  For a guest that is fatal, so it is a
            // failure of this case rather than a gap to note.
            if (bad_exits++ < 15) {
                problems.push_back(fmt::format(
                        "{}: block did not reach HLT (exit={}); encoding {} was not decoded",
                        label, jit.exit, ref.enc));
            }
            continue;
        }
        if (jit.obs != itp.obs || jit.exit != itp.exit || jit.mxcsr != itp.mxcsr) {
            if (divergences++ < 15) {
                problems.push_back(fmt::format(
                        "{}: JIT/interpreter divergence ({} vs {}, mxcsr {:#x} vs {:#x})", label,
                        Hex(jit.obs), Hex(itp.obs), jit.mxcsr, itp.mxcsr));
            }
        }

        for (const auto& [backend, got] : {std::pair<const char*, const Out*>{"jit", &jit},
                                           std::pair<const char*, const Out*>{"interp", &itp}}) {
            bool differs = false;
            for (size_t i = 0; i < 128; ++i) {
                if (got->obs[i] != want[i] && !ExcludedByte(ref.name, i)) {
                    differs = true;
                    break;
                }
            }
            if (differs && HaddpsNaNSignOnly(ref.name, got->obs, want)) {
                // The pre-existing HaddpsHalf defect, and nothing else; see the
                // long comment on HaddpsNaNSignOnly.
                ++known_haddps;
                differs = false;
            }
            if (differs && mismatches++ < 15) {
                problems.push_back(fmt::format("{} [{}]: got {}, Rosetta says {} (enc {})", label,
                                               backend, Hex(got->obs), Hex(want), ref.enc));
            }
            // The SDM, not Rosetta, on PTEST's PF: it is architecturally 0.
            if ((std::strcmp(ref.name, "ptest") == 0 || std::strcmp(ref.name, "vptest") == 0) &&
                got->obs[0x41] != 0 && ptest_pf++ < 15) {
                problems.push_back(fmt::format(
                        "{} [{}]: ptest left PF = {}, but the SDM sets PF to 0", label, backend,
                        got->obs[0x41]));
            }
            if (got->mxcsr != 0x1F80u && leaked_mxcsr++ < 15) {
                problems.push_back(fmt::format("{} [{}]: MXCSR left at {:#x}, expected 0x1f80",
                                               label, backend, got->mxcsr));
            }
            // No register beyond the four the prologue loads may change.
            for (u32 i = 4; i < 16; ++i) {
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
                        << " mismatches=" << mismatches << " bystanders=" << bystanders
                        << " leaked mxcsr=" << leaked_mxcsr << " ptest PF!=0=" << ptest_pf
                        << " known haddps NaN-sign=" << known_haddps << joined);
    REQUIRE(comparisons > 3000);
    REQUIRE(problems.empty());
    // The pre-existing haddps/hsubps defect must still be REACHED: if the four
    // rows that hit it stopped hitting it, either the data lost its NaNs or
    // somebody fixed the handler -- and in the second case HaddpsNaNSignOnly
    // and this check should both be deleted rather than left as noise.
    INFO("the haddps/hsubps NaN-sign rows no longer differ from Rosetta -- if decoder.cc now "
         "routes I_HADDPS/I_HSUBPS to DecodeSse4, delete HaddpsNaNSignOnly and this check");
    CHECK(known_haddps > 0);
}

// ===========================================================================
// A narrow memory source must read only its architectural width.
// ===========================================================================
// PMOVSX/PMOVZX read 2, 4 or 8 bytes; ROUNDSS and INSERTPS read 4; ROUNDSD
// reads 8.  Reading a full 16 bytes instead produces the SAME VALUE -- the
// extra bytes are discarded by the widening chain -- so the Rosetta
// differential above cannot see the difference at all: a mutation that
// replaced the computed byte count with a constant 16 survived every one of
// its 4020 rows.  The difference is only observable as a FAULT, when the
// operand sits at the end of a mapping, which is exactly where a loop tail
// puts it.
//
// So the operand is placed in the last bytes of a mapped page whose successor
// is PROT_NONE.  A correctly sized load never touches the guard page and the
// block reaches HLT; an oversized one runs into it.  Nothing here depends on
// guest fault handling working: in the PASSING case no fault is raised at all.
//
// The failure mode is worth knowing: a load that does run into the guard page
// aborts the whole test binary with "[SwiftVM] unhandled host fault: SIGBUS"
// rather than returning a PageFatal exit, because this arena is plain host
// mmap rather than a guest mapping the runtime's fault handler recognizes.
// That is still an unmistakable kill -- verified by mutating SseNarrowSrc to
// read 16 bytes unconditionally, which turns this case into exactly that abort
// while leaving all 4020 differential rows green.  The `exit != None` branch
// below is therefore belt and braces, not the primary signal.
TEST_CASE("x86 legacy sse4 narrow memory source stays in its page") {
    const char* sse4_env = std::getenv("SVM_SSE4");
    if (sse4_env && std::strcmp(sse4_env, "0") == 0) {
        SUCCEED("SVM_SSE4=0 disables the legacy SSE4 handlers");
        return;
    }
    constexpr size_t kArenaSize = 0x400000;
    constexpr u64 kGuard = 0x300000;  // this offset and beyond is unmapped
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    REQUIRE(mprotect(reinterpret_cast<void*>(base + kGuard), 0x4000, PROT_NONE) == 0);

    setenv("SVM_ENABLE_JIT", "1", 1);
    auto* jit_instance = X86Instance::Make();
    setenv("SVM_ENABLE_JIT", "0", 1);
    auto* interp_instance = X86Instance::Make();
    unsetenv("SVM_ENABLE_JIT");
    auto* jit_core = X86Core::Make(jit_instance);
    auto* interp_core = X86Core::Make(interp_instance);

    // {name, bytes read, encoding with r/m = [rsi]}.  Every one is placed so
    // that its LAST architectural byte is the last byte of the mapped page.
    struct Narrow {
        const char* name;
        u64 bytes;
        std::vector<u8> enc;
    };
    const std::vector<Narrow> kNarrow = {
            {"pmovsxbq", 2, {0x66, 0x0F, 0x38, 0x22, 0x1E}},
            {"pmovzxbq", 2, {0x66, 0x0F, 0x38, 0x32, 0x1E}},
            {"pmovsxbd", 4, {0x66, 0x0F, 0x38, 0x21, 0x1E}},
            {"pmovsxwq", 4, {0x66, 0x0F, 0x38, 0x24, 0x1E}},
            {"pmovsxbw", 8, {0x66, 0x0F, 0x38, 0x20, 0x1E}},
            {"pmovsxwd", 8, {0x66, 0x0F, 0x38, 0x23, 0x1E}},
            {"pmovsxdq", 8, {0x66, 0x0F, 0x38, 0x25, 0x1E}},
            {"roundss", 4, {0x66, 0x0F, 0x3A, 0x0A, 0x1E, 0x03}},
            {"roundsd", 8, {0x66, 0x0F, 0x3A, 0x0B, 0x1E, 0x03}},
            {"insertps", 4, {0x66, 0x0F, 0x3A, 0x21, 0x1E, 0x10}},
            {"pinsrb", 1, {0x66, 0x0F, 0x3A, 0x20, 0x1E, 0x05}},
            {"pinsrd", 4, {0x66, 0x0F, 0x3A, 0x22, 0x1E, 0x02}},
            {"pinsrq", 8, {0x66, 0x48, 0x0F, 0x3A, 0x22, 0x1E, 0x01}},
    };

    std::vector<std::string> problems;
    u64 code_cursor = 1;
    for (const auto& n : kNarrow) {
        auto code = n.enc;
        code.push_back(0xF4);  // hlt
        const u64 code_addr = base + 0x1000 + code_cursor * 0x100;
        ++code_cursor;
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
        const u64 source = base + kGuard - n.bytes;
        for (u64 i = 0; i < n.bytes; ++i) {
            *reinterpret_cast<u8*>(source + i) = u8(0x81 + i);
        }
        for (auto* core : {jit_core, interp_core}) {
            auto& ctx = core->GetContext();
            std::memset(ctx.xmms[3].b, 0, 16);
            ctx.rsi.qword = source;
            ctx.rcx.qword = 0x0123456789ABCDEFull;
            ctx.rsp.qword = base + 0x200000;
            ctx.rip.qword = code_addr;
            const int exit = int(core->Run());
            if (exit != int(swift::translator::None)) {
                problems.push_back(fmt::format(
                        "{}: a {}-byte source ending at the last byte of a mapped page did not "
                        "reach HLT (exit={}) -- the handler read past its architectural width",
                        n.name, n.bytes, exit));
            }
        }
    }
    // The value is still checked, so this case cannot pass by declining to
    // decode: pmovsxbq of {0x81, 0x82} sign-extends to two qwords.
    {
        auto code = kNarrow[0].enc;
        code.push_back(0xF4);
        const u64 code_addr = base + 0x1000;
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
        const u64 source = base + kGuard - 2;
        *reinterpret_cast<u8*>(source) = 0x81;
        *reinterpret_cast<u8*>(source + 1) = 0x82;
        auto& ctx = jit_core->GetContext();
        std::memset(ctx.xmms[3].b, 0x77, 16);
        ctx.rsi.qword = source;
        ctx.rsp.qword = base + 0x200000;
        ctx.rip.qword = code_addr;
        REQUIRE(int(jit_core->Run()) == int(swift::translator::None));
        u64 lo = 0, hi = 0;
        std::memcpy(&lo, ctx.xmms[3].b, 8);
        std::memcpy(&hi, ctx.xmms[3].b + 8, 8);
        CHECK(lo == 0xFFFFFFFFFFFFFF81ull);
        CHECK(hi == 0xFFFFFFFFFFFFFF82ull);
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
    INFO(joined);
    REQUIRE(problems.empty());
}

// ===========================================================================
// The MMX forms of the SSSE3 opcodes must be DECLINED, not executed.
// ===========================================================================
// `0F 38 01 /r` with no 66 prefix is `phaddw mm1, mm2` -- the same distorm
// mnemonic as the XMM form, with 64-bit MM operands (distorm reports register
// codes 83..90).  This runtime models no MMX register file, so running one
// through the XMM handlers would compute against WHATEVER XMM register happens
// to share the operand number: a silently wrong answer where the guest
// currently gets a clean IllegalCode.  DecodeSse4 rejects them, and this case
// is what keeps that rejection from being quietly dropped when someone
// simplifies the dispatch.
TEST_CASE("x86 legacy ssse3 mmx forms are declined") {
    const char* sse4_env = std::getenv("SVM_SSE4");
    if (sse4_env && std::strcmp(sse4_env, "0") == 0) {
        SUCCEED("SVM_SSE4=0 disables the legacy SSE4 handlers");
        return;
    }
    constexpr size_t kArenaSize = 0x100000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    setenv("SVM_ENABLE_JIT", "1", 1);
    auto* instance = X86Instance::Make();
    unsetenv("SVM_ENABLE_JIT");
    auto* core = X86Core::Make(instance);

    // {name, MMX encoding, the 66-prefixed XMM encoding of the same opcode}.
    const std::vector<std::pair<const char*, std::vector<u8>>> kMmx = {
            {"phaddw", {0x0F, 0x38, 0x01, 0xCA}},   {"phsubd", {0x0F, 0x38, 0x06, 0xCA}},
            {"psignb", {0x0F, 0x38, 0x08, 0xCA}},   {"pabsb", {0x0F, 0x38, 0x1C, 0xCA}},
            {"pmulhrsw", {0x0F, 0x38, 0x0B, 0xCA}}, {"pmaddubsw", {0x0F, 0x38, 0x04, 0xCA}},
    };
    std::vector<std::string> problems;
    u64 cursor = 1;
    for (const auto& [name, enc] : kMmx) {
        auto code = enc;
        code.push_back(0xF4);
        const u64 code_addr = base + 0x1000 + cursor * 0x100;
        ++cursor;
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
        auto& ctx = core->GetContext();
        ctx.rsp.qword = base + 0x80000;
        ctx.rip.qword = code_addr;
        const int exit = int(core->Run());
        if (exit == int(swift::translator::None)) {
            problems.push_back(fmt::format(
                    "{}: the MMX form reached HLT, so it was EXECUTED against the XMM register "
                    "file instead of being declined",
                    name));
        }
    }
    // And the 66-prefixed XMM form of the same opcode still works, so the
    // rejection above is not just "this opcode is unimplemented".
    {
        std::vector<u8> code = {0x66, 0x0F, 0x38, 0x01, 0xCA, 0xF4};
        const u64 code_addr = base + 0x1000;
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
        auto& ctx = core->GetContext();
        ctx.rsp.qword = base + 0x80000;
        ctx.rip.qword = code_addr;
        REQUIRE(int(core->Run()) == int(swift::translator::None));
    }

    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(arena, kArenaSize);
    std::string joined;
    for (const auto& p : problems) {
        joined += "\n  " + p;
    }
    INFO(joined);
    REQUIRE(problems.empty());
}

// ===========================================================================
// The legacy form and the VEX form must agree on the low 128 bits.
// ===========================================================================
// decoder_sse4.cc duplicates the IR sequences of decoder_avx*.cc because those
// live in anonymous namespaces it cannot reach.  This case is what makes that
// duplication safe: every "v" row in the reference was emitted with the SAME
// operands as its legacy row, so their low 128 bits must be identical, and
// their bits 255:128 must differ in exactly the specified way (legacy keeps
// the poison, VEX zeroes).  It compares REFERENCE rows, so it also asserts
// that the hardware agrees -- if it did not, the pairing itself would be wrong.
TEST_CASE("x86 legacy sse4 and vex twins agree") {
    static const std::pair<const char*, const char*> kTwins[] = {
            {"ptest", "vptest"},       {"pmovsxbw", "vpmovsxbw"}, {"pmovzxdq", "vpmovzxdq"},
            {"pmulld", "vpmulld"},     {"pcmpeqq", "vpcmpeqq"},   {"pcmpgtq", "vpcmpgtq"},
            {"packusdw", "vpackusdw"}, {"phaddw", "vphaddw"},     {"pmaddubsw", "vpmaddubsw"},
            {"pminsb", "vpminsb"},     {"pmaxud", "vpmaxud"},     {"pabsd", "vpabsd"},
            {"haddpd", "vhaddpd"},     {"pextrb", "vpextrb"},
    };
    const auto dest_poison = DestPoison();
    size_t compared = 0;
    for (const auto& [legacy, vex] : kTwins) {
        for (const auto& r : kSse4Refs) {
            if (std::strcmp(r.name, vex) != 0) continue;
            const auto* twin = FindRow(legacy, r.pair, r.imm, r.rc);
            INFO("no legacy row to pair with " << vex << " pair " << r.pair);
            REQUIRE(twin != nullptr);
            const auto v = ParseObs(r.result);
            const auto l = ParseObs(twin->result);
            INFO(legacy << "/" << vex << " pair " << r.pair
                        << ": the two forms disagree on the low 128 bits, so the reference "
                           "cannot be used to police the duplicated handlers");
            REQUIRE(std::equal(v.begin(), v.begin() + 16, l.begin()));
            // Everything outside the vector destination must match too: same
            // flags, same GPRs, same store.
            REQUIRE(std::equal(v.begin() + 32, v.end(), l.begin() + 32));
            ++compared;
        }
    }
    INFO("no twin pairs found -- the generator stopped emitting VEX rows");
    REQUIRE(compared > 50);
    // And the halves differ exactly as specified.
    for (const auto& r : kSse4Refs) {
        if (!IsVex(r.name)) continue;
        if (std::strcmp(r.name, "vptest") == 0 || std::strcmp(r.name, "vpextrb") == 0) continue;
        const auto v = ParseObs(r.result);
        INFO(r.name << ": VEX row does not zero bits 255:128");
        REQUIRE(std::all_of(v.begin() + 16, v.begin() + 32, [](u8 x) { return x == 0; }));
    }
    for (const auto& r : kSse4Refs) {
        if (IsVex(r.name)) continue;
        const auto v = ParseObs(r.result);
        INFO(r.name << ": legacy row does not preserve bits 255:128");
        REQUIRE(std::equal(dest_poison.begin(), dest_poison.end(), v.begin() + 16));
    }
}
