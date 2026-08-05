// ===========================================================================
// FMA3 against a ROSETTA oracle: vfmadd / vfmsub / vfnmadd / vfnmsub in all
// three operand orders, packed and scalar, single and double, VEX.128 and
// VEX.256, plus vfmaddsub / vfmsubadd.
// ===========================================================================
//
// WHY A HARDWARE ORACLE
// ---------------------
// Unicorn 2.1.4 refuses every VEX.L=1 encoding (UC_ERR_INSN_INVALID), so the
// 256-bit forms have no emulator oracle at all.  But even at 128 bits a model
// written here would be worthless for the two things that actually go wrong in
// FMA:
//
//   * FUSION.  A hand model would be written as `a*b + c` in C++, which is
//     free to round the product first -- reproducing the very bug it is meant
//     to detect.  The reference values are the bits real x86 produced, and
//     Rosetta runs on AArch64, whose FMA is also genuinely fused, so the
//     oracle cannot accidentally agree with an unfused implementation.
//   * THE 132/213/231 NUMBERING.  Which encoded operand is the multiplicand,
//     which the multiplier and which the addend is a decoding fact, and a
//     model would simply restate whatever the implementation assumed.  Here
//     three DIFFERENT values sit in three DIFFERENT registers and the answer
//     comes from hardware.
//
// Rosetta is an emulator with measured defects of its own (VPSLLVQ's shift
// count truncated to 32 bits, XSAVE writing extra bytes, a vptest PF that
// varies with the surrounding program), so agreement is evidence and not
// proof.  Everything below that looked surprising was cross-read against the
// Intel SDM before the data was accepted.
//
// WHY THE ROWS CARRY THE ENCODING
// -------------------------------
// Each row holds the LITERAL BYTES the generator executed and this file
// replays them, rather than both sides re-encoding from avx_fma_ops.inc.  With
// 60 mnemonics separated by one opcode byte and one VEX.W bit, a single wrong
// table entry would otherwise make both sides test the same wrong instruction
// and the differential would pass vacuously.  All 240 distinct encodings in
// the data were additionally disassembled and confirmed to be the intended
// mnemonic at the intended width before the data was accepted.
//
// WHAT THE DATA SETTLES, MEASURED RATHER THAN ASSUMED
//
//   The operand numbering.  Input case `order32` is (op1, op2, op3) =
//   (C, A, B) = (5, 2, 3) in lane 0, for which the three orders give 17, 13
//   and 11 -- three different numbers.  Hardware said 17 for 132, 13 for 213
//   and 11 for 231, which is the SDM's DEST*SRC3+SRC2 / SRC2*DEST+SRC3 /
//   SRC2*SRC3+DEST.  The "operand order has teeth" block below re-derives that
//   the rows for the three orders actually differ, so a future regeneration
//   that flattened the inputs would fail here rather than quietly stop testing
//   it.
//
//   Fusion.  The `fuse*` cases choose A = 1+2^-j, B = 1+2^-k, C = +-(1+2^-m)
//   with every pairwise exponent sum past the mantissa width, so EVERY pairwise
//   product has bits below the format's precision and EVERY operand order
//   cancels its leading 1 -- which is what promotes those lost bits into the
//   answer.  The "fusion has teeth" block recomputes each such row the
//   DOUBLE-ROUNDED way and requires, per mnemonic, that hardware disagreed:
//   it proves that an implementation built from VecFMul + VecFAdd fails this
//   file, rather than hoping so.
//
//   NaN propagation priority.  `nanprio32` / `nanprio64` put NaNs with
//   DISTINGUISHABLE payloads in two or three operand positions at once -- the
//   only way the priority rule is observable at all, since with a single NaN
//   every rule gives the same answer.  Measured result: the winner is the
//   first NaN in the ARITHMETIC order (multiplicand, multiplier, addend) after
//   the 132/213/231 permutation has been applied, NOT the first in the encoded
//   operand order.  For `vfmadd231ps` with a NaN in VEX.vvvv and another in
//   the destination the answer is vvvv's, while `vfmadd132ps` on the same
//   registers answers with the destination's -- because 132 makes the
//   destination the multiplicand.  A signalling NaN comes back quieted, a
//   negative NaN keeps its sign, and Inf*0 with a NaN addend returns the
//   addend rather than the indefinite.
//
//   Contract C3.  ymm0 is loaded with C -- whose bytes 16..31 are nonzero in
//   every input case -- before every row, so a VEX.128 row's reference carries
//   sixteen literal zero bytes the HARDWARE wrote.  A scalar row carries them
//   at VEX.L=1 too, because the scalar forms are LIG.
//
//   The scalar merge.  A scalar row's bytes above lane 0 must equal C's, since
//   x86 takes them from the DESTINATION register -- which is operand 1, and so
//   a different one of the three arithmetic roles for each of 132/213/231.
//   Asserted directly against the reference bytes.
//
//   Bystanders.  ymm3..ymm15 are poisoned per register and per byte, and ymm1
//   and ymm2 hold the sources and are never written by any row, so a handler
//   that wrote the wrong register or disturbed a source is caught.
//
// Each block is a SINGLE instruction plus a HLT: the operand registers are
// written straight into ThreadContext64 and the answer read straight back, so
// a broken vmovdqu cannot mask a broken handler.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <sys/mman.h>
#include "runtime/common/svm_config.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/frontend/x86/decoder.h"
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

using namespace swift::translator::x86;
using namespace swift;

namespace {

struct AvxFmaInput {
    const char* name;
    const char* a;  // ymm1, VEX.vvvv, operand 2
    const char* b;  // ymm2, ModRM.r/m, operand 3
    const char* c;  // ymm0, the destination, operand 1
};
struct AvxFmaRef {
    const char* name;
    int width;  // 128 or 256: the VEX.L the generator encoded
    int input;
    const char* enc;     // literal instruction bytes, hex
    const char* result;  // the 32 bytes of ymm0 afterwards, hex
};
#include "avx_fma_rosetta_ref.inc"

// The instruction table, shared verbatim with the generator.  The encoding
// comes from each row, so this exists to pin COVERAGE (every mnemonic must
// have produced rows) and to know which mnemonics are scalar.
struct Entry {
    const char* name;
    int opcode;
    int w;
    bool scalar;
};
constexpr Entry kEntries[] = {
#define SVM_FMA(name, opcode, w, shape) {#name, opcode, w, (#shape)[0] == 'S'},
#include "avx_fma_ops.inc"
#undef SVM_FMA
};

using Vec256 = std::array<u8, 32>;

u8 Nib(char ch) { return u8(ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10); }

Vec256 ParseHex32(const char* h) {
    Vec256 v{};
    for (u32 i = 0; i < 32; ++i) {
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

std::string Hex(const Vec256& v) {
    std::string s;
    for (const u8 x : v) {
        s += fmt::format("{:02x}", x);
    }
    return s;
}

Vec256 Poison(u32 reg) {
    Vec256 v{};
    for (u32 j = 0; j < 32; ++j) {
        v[j] = u8(0xA5 ^ (reg * 32 + j));
    }
    return v;
}

const Entry* FindEntry(const char* name) {
    for (const auto& e : kEntries) {
        if (std::strcmp(e.name, name) == 0) return &e;
    }
    return nullptr;
}

// The three arithmetic roles, from the mnemonic's order digits.  op1 = C
// (the destination), op2 = A (VEX.vvvv), op3 = B (r/m).
struct Roles {
    int factor1;  // 0 = A, 1 = B, 2 = C
    int factor2;
    int addend;
};
Roles RolesOf(const char* name) {
    // The digits are the three characters before the two-letter type suffix.
    const size_t n = std::strlen(name);
    const char* d = name + n - 5;
    if (std::strncmp(d, "132", 3) == 0) return {2, 1, 0};  // op1*op3 + op2
    if (std::strncmp(d, "213", 3) == 0) return {0, 2, 1};  // op2*op1 + op3
    return {0, 1, 2};                                      // op2*op3 + op1
}

// Sign of the product / of the addend, from the mnemonic's prefix.
struct Signs {
    bool negate_product;
    bool negate_addend;
};
Signs SignsOf(const char* name) {
    if (std::strncmp(name, "vfnmadd", 7) == 0) return {true, false};
    if (std::strncmp(name, "vfnmsub", 7) == 0) return {true, true};
    if (std::strncmp(name, "vfmsub", 6) == 0 && std::strncmp(name, "vfmsubadd", 9) != 0)
        return {false, true};
    return {false, false};
}

u64 LaneBits(const Vec256& v, u32 lane, u32 bits) {
    u64 x = 0;
    std::memcpy(&x, v.data() + lane * (bits / 8), bits / 8);
    return x;
}

// The DOUBLE-ROUNDED answer for one lane: round the product to the lane's
// precision, then add.  `volatile` on the intermediate is load-bearing -- with
// it the compiler cannot contract the two operations back into an fma and turn
// this reference-for-the-wrong-answer into the right one.
u64 DoubleRoundedLane(u64 x, u64 y, u64 z, u32 bits, Signs s) {
    if (bits == 32) {
        float a{}, b{}, c{};
        const u32 xi = u32(x), yi = u32(y), zi = u32(z);
        std::memcpy(&a, &xi, 4);
        std::memcpy(&b, &yi, 4);
        std::memcpy(&c, &zi, 4);
        volatile float product = a * b;
        if (s.negate_product) product = -product;
        if (s.negate_addend) c = -c;
        const float r = product + c;
        u32 out;
        std::memcpy(&out, &r, 4);
        return out;
    }
    double a{}, b{}, c{};
    std::memcpy(&a, &x, 8);
    std::memcpy(&b, &y, 8);
    std::memcpy(&c, &z, 8);
    volatile double product = a * b;
    if (s.negate_product) product = -product;
    if (s.negate_addend) c = -c;
    const double r = product + c;
    u64 out;
    std::memcpy(&out, &r, 8);
    return out;
}

bool IsAddSub(const char* name) {
    return std::strncmp(name, "vfmaddsub", 9) == 0 || std::strncmp(name, "vfmsubadd", 9) == 0;
}

}  // namespace

TEST_CASE("x86 avx fma vs rosetta reference") {
    if (!swift::runtime::GetSvmConfig().avx) {
        SUCCEED("SVM_AVX is not set; FMA3 Rosetta differential skipped");
        return;
    }

    std::vector<Vec256> ins_a, ins_b, ins_c;
    for (const auto& in : kAvxFmaInputs) {
        ins_a.push_back(ParseHex32(in.a));
        ins_b.push_back(ParseHex32(in.b));
        ins_c.push_back(ParseHex32(in.c));
    }

    // ---- properties of the reference data this case depends on -------------
    // Asserted rather than trusted: without these the differential could pass
    // while having stopped testing what it exists for.
    {
        // Every mnemonic in the shared table must have produced rows.
        for (const auto& e : kEntries) {
            size_t rows = 0;
            for (const auto& r : kAvxFmaRefs) {
                if (std::strcmp(r.name, e.name) == 0) ++rows;
            }
            INFO("no reference rows for " << e.name
                                          << " -- the generator did not cover it, or Rosetta "
                                             "refused every encoding of it");
            REQUIRE(rows > 0);
        }
        INFO("the shared table lost mnemonics");
        REQUIRE(std::size(kEntries) == 60u);
    }
    {
        // Contract C3, as the HARDWARE reported it.  Every VEX.128 row, and
        // every SCALAR row at either width (the scalar forms are LIG), must
        // carry sixteen zero bytes above the result -- against a destination
        // that held C's nonzero upper half beforehand.
        size_t checked = 0;
        for (const auto& r : kAvxFmaRefs) {
            const Entry* e = FindEntry(r.name);
            REQUIRE(e != nullptr);
            if (r.width != 128 && !e->scalar) continue;
            const auto v = ParseHex32(r.result);
            INFO(r.name << " L" << r.width << " case " << r.input
                        << ": the reference does not have a zeroed upper half");
            REQUIRE(std::all_of(v.begin() + 16, v.end(), [](u8 x) { return x == 0; }));
            ++checked;
        }
        INFO("no VEX.128 or scalar rows at all");
        REQUIRE(checked > 1000);
    }
    {
        // A VEX.256 packed row must NOT have a zeroed upper half for every
        // input -- otherwise the 256-bit path would be indistinguishable from
        // a 128-bit one that happened to zero correctly.
        size_t nonzero_upper = 0;
        for (const auto& r : kAvxFmaRefs) {
            const Entry* e = FindEntry(r.name);
            if (r.width != 256 || e->scalar) continue;
            const auto v = ParseHex32(r.result);
            if (!std::all_of(v.begin() + 16, v.end(), [](u8 x) { return x == 0; })) {
                ++nonzero_upper;
            }
        }
        INFO("every 256-bit packed reference has a zero upper half; the second lane is untested");
        REQUIRE(nonzero_upper > 500);
    }
    {
        // The scalar merge, measured: bytes above lane 0 come from the
        // DESTINATION register (which held C), not from either source.
        size_t checked = 0;
        for (const auto& r : kAvxFmaRefs) {
            const Entry* e = FindEntry(r.name);
            if (!e->scalar) continue;
            const auto got = ParseHex32(r.result);
            const auto& c = ins_c[size_t(r.input)];
            const size_t lane_bytes = e->w ? 8 : 4;
            INFO(r.name << " L" << r.width << " case " << r.input
                        << ": the reference's untouched lanes do not match the destination");
            REQUIRE(std::equal(got.begin() + long(lane_bytes), got.begin() + 16,
                               c.begin() + long(lane_bytes)));
            ++checked;
        }
        REQUIRE(checked > 400);
    }
    // A pair of mnemonics is "separated" if SOME reference row of the same
    // width and input distinguishes them.  Demanding that EVERY row separate
    // them would be wrong rather than strict: a scalar row reads only lane 0,
    // and an input built for f64 lanes has zeros in the f32 lane 0 of every
    // operand -- 0*0+0 is 0 whichever order computed it.  What must hold is
    // that no pair goes entirely undistinguished.
    const auto separated = [&](const char* left, const char* right) {
        for (const auto& r : kAvxFmaRefs) {
            if (std::strcmp(r.name, left) != 0) continue;
            for (const auto& o : kAvxFmaRefs) {
                if (o.input != r.input || o.width != r.width) continue;
                if (std::strcmp(o.name, right) != 0) continue;
                if (std::strcmp(r.result, o.result) != 0) return true;
            }
        }
        return false;
    };
    {
        // THE OPERAND ORDER HAS TEETH.  Every mnemonic's 132 form must be
        // distinguishable from its 213 and its 231 form by the data, so an
        // implementation that permuted the numbering is caught rather than
        // merely suspected.
        size_t checked = 0;
        for (const auto& e : kEntries) {
            const std::string name(e.name);
            const size_t at = name.find("132");
            if (at == std::string::npos) continue;
            for (const char* digits : {"213", "231"}) {
                std::string other = name;
                other.replace(at, 3, digits);
                INFO("no input distinguishes " << name << " from " << other
                                               << "; the operand-order numbering is untested for "
                                                  "this mnemonic");
                REQUIRE(separated(e.name, other.c_str()));
                ++checked;
            }
        }
        REQUIRE(checked == 40u);
    }
    {
        // THE SIGN FLAGS HAVE TEETH.  vfmadd must be distinguishable from
        // vfmsub, vfnmadd and vfnmsub of the same order and type.
        size_t checked = 0;
        for (const auto& e : kEntries) {
            if (std::strncmp(e.name, "vfmadd", 6) != 0 || IsAddSub(e.name)) continue;
            const std::string tail = std::string(e.name).substr(6);  // "132ps" etc
            for (const char* prefix : {"vfmsub", "vfnmadd", "vfnmsub"}) {
                const std::string other = std::string(prefix) + tail;
                INFO("no input distinguishes " << e.name << " from " << other
                                               << "; the product/addend sign flags are untested "
                                                  "for this mnemonic");
                REQUIRE(separated(e.name, other.c_str()));
                ++checked;
            }
        }
        REQUIRE(checked == 36u);
        // And addsub must be distinguishable from subadd, which is the only
        // thing that tells the two lane-parity patterns apart.
        size_t addsub_checked = 0;
        for (const auto& e : kEntries) {
            if (std::strncmp(e.name, "vfmaddsub", 9) != 0) continue;
            std::string other(e.name);
            other.replace(0, 9, "vfmsubadd");
            INFO("no input distinguishes " << e.name << " from " << other);
            REQUIRE(separated(e.name, other.c_str()));
            ++addsub_checked;
        }
        REQUIRE(addsub_checked == 6u);
    }
    {
        // THE NaN PRIORITY HAS TEETH.  A single NaN operand is returned by
        // every conceivable rule, so the priority is only observable where two
        // or three NaNs with different payloads meet.  Require that the
        // `nanprio*` rows actually distinguish the three operand orders --
        // which they can only do through the priority, since the arithmetic
        // itself is NaN either way.
        size_t distinguishing = 0;
        for (const auto& r : kAvxFmaRefs) {
            if (std::strncmp(kAvxFmaInputs[size_t(r.input)].name, "nanprio", 7) != 0) continue;
            const std::string name(r.name);
            const size_t at = name.find("132");
            if (at == std::string::npos) continue;
            for (const char* digits : {"213", "231"}) {
                std::string other = name;
                other.replace(at, 3, digits);
                for (const auto& o : kAvxFmaRefs) {
                    if (o.input != r.input || o.width != r.width) continue;
                    if (other != o.name) continue;
                    if (std::strcmp(r.result, o.result) != 0) ++distinguishing;
                }
            }
        }
        INFO("the nanprio inputs no longer distinguish the operand orders; the NaN propagation "
             "priority has stopped being tested");
        REQUIRE(distinguishing > 100);
    }
    {
        // FUSION HAS TEETH.  Recompute every `fuse*` row the DOUBLE-ROUNDED way
        // -- round the product to the lane's precision, then add -- and require
        // that hardware disagreed.  This is the assertion that proves an
        // implementation built from VecFMul + VecFAdd would FAIL this file: if
        // the reference data ever stopped containing a lane where the two
        // differ, the differential would silently start accepting double
        // rounding and this fires instead.
        //
        // Required per MNEMONIC, not just in aggregate: a single global count
        // could be satisfied entirely by, say, the pd forms while every ps form
        // went unchecked.
        size_t rows_exposing = 0;
        std::array<bool, std::size(kEntries)> exposed_entry{};
        for (const auto& r : kAvxFmaRefs) {
            if (std::strncmp(kAvxFmaInputs[size_t(r.input)].name, "fuse", 4) != 0) continue;
            const Entry* e = FindEntry(r.name);
            const auto want = ParseHex32(r.result);
            const auto& a = ins_a[size_t(r.input)];
            const auto& b = ins_b[size_t(r.input)];
            const auto& c = ins_c[size_t(r.input)];
            const std::array<const Vec256*, 3> ops = {&a, &b, &c};
            const Roles roles = RolesOf(r.name);
            const Signs signs = SignsOf(r.name);
            const bool addsub = IsAddSub(r.name);
            const bool subtract_even = std::strncmp(r.name, "vfmaddsub", 9) == 0;
            const u32 bits = e->w ? 64 : 32;
            const u32 lanes = e->scalar ? 1u : (r.width == 256 ? 256u : 128u) / bits;
            bool exposed = false;
            for (u32 lane = 0; lane < lanes; ++lane) {
                Signs lane_signs = signs;
                if (addsub) {
                    lane_signs.negate_addend = ((lane % 2) == 0) == subtract_even;
                }
                const u64 unfused =
                        DoubleRoundedLane(LaneBits(*ops[size_t(roles.factor1)], lane, bits),
                                          LaneBits(*ops[size_t(roles.factor2)], lane, bits),
                                          LaneBits(*ops[size_t(roles.addend)], lane, bits), bits,
                                          lane_signs);
                if (unfused != LaneBits(want, lane, bits)) exposed = true;
            }
            if (exposed) {
                ++rows_exposing;
                exposed_entry[size_t(e - kEntries)] = true;
            }
        }
        for (size_t i = 0; i < std::size(kEntries); ++i) {
            INFO("no reference row distinguishes a fused " << kEntries[i].name
                                                           << " from a double-rounded one; the "
                                                              "fuse* inputs stopped doing their "
                                                              "job for this mnemonic");
            REQUIRE(exposed_entry[i]);
        }
        REQUIRE(rows_exposing > 300);
    }

    // ---- harness -----------------------------------------------------------
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x200000;
    // The generator's data block: A at +0x00, B at +0x20, C at +0x40, capture
    // at +0x60, addressed through rdi.  The replayed encodings carry the
    // displacement 0x20 literally, so this layout is not a choice here.
    const u64 data = base + 0x300000;
    constexpr s32 kOffA = 0x00, kOffB = 0x20, kOffC = 0x40;

    const char* old_jit = swift::runtime::GetRawSvmConfigEnvForTest("SVM_ENABLE_JIT");
    const bool had_old_jit = old_jit != nullptr;
    const std::string old_jit_value = old_jit ? old_jit : "";
    swift::runtime::SetSvmConfigEnvForTest("SVM_ENABLE_JIT", "1", 1);
    auto* jit_instance = X86Instance::Make();
    swift::runtime::SetSvmConfigEnvForTest("SVM_ENABLE_JIT", "0", 1);
    auto* interp_instance = X86Instance::Make();
    if (had_old_jit) {
        swift::runtime::SetSvmConfigEnvForTest("SVM_ENABLE_JIT", old_jit_value.c_str(), 1);
    } else {
        swift::runtime::UnsetSvmConfigEnvForTest("SVM_ENABLE_JIT");
    }
    auto* jit_core = X86Core::Make(jit_instance);
    auto* interp_core = X86Core::Make(interp_instance);

    struct Out {
        std::array<Vec256, 16> ymm{};
        int exit{};
    };

    size_t code_cursor = 1;
    std::vector<std::string> problems;
    size_t comparisons = 0, bad_exits = 0, divergences = 0, mismatches = 0, bystanders = 0;

    const auto run_on = [&](X86Core* core, const std::vector<u8>& code, const Vec256& a,
                            const Vec256& b, const Vec256& c, u64 code_addr) {
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffA)), a.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffB)), b.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffC)), c.data(), 32);
        auto& ctx = core->GetContext();
        for (u32 i = 0; i < 16; ++i) {
            const auto p = Poison(i);
            std::memcpy(ctx.xmms[i].b, p.data(), 16);
            std::memcpy(ctx.ymm_high[i].b, p.data() + 16, 16);
        }
        // ymm0 = C (the destination AND operand 1), ymm1 = A, ymm2 = B -- the
        // same state the generator's prologue produced.
        std::memcpy(ctx.xmms[0].b, c.data(), 16);
        std::memcpy(ctx.ymm_high[0].b, c.data() + 16, 16);
        std::memcpy(ctx.xmms[1].b, a.data(), 16);
        std::memcpy(ctx.ymm_high[1].b, a.data() + 16, 16);
        std::memcpy(ctx.xmms[2].b, b.data(), 16);
        std::memcpy(ctx.ymm_high[2].b, b.data() + 16, 16);
        ctx.rdi.qword = data;
        ctx.rsp.qword = stack;
        ctx.rip.qword = code_addr;
        Out o;
        o.exit = int(core->Run());
        for (u32 i = 0; i < 16; ++i) {
            std::memcpy(o.ymm[i].data(), ctx.xmms[i].b, 16);
            std::memcpy(o.ymm[i].data() + 16, ctx.ymm_high[i].b, 16);
        }
        return o;
    };

    for (const auto& ref : kAvxFmaRefs) {
        auto code = ParseHex(ref.enc);
        code.push_back(0xF4);  // hlt
        const u64 code_addr = base + 0x1000 + code_cursor * 0x100;
        ++code_cursor;
        REQUIRE(code.size() < 0x100);
        REQUIRE(code_addr + 0x100 < stack);
        const Vec256 want = ParseHex32(ref.result);
        const auto& a = ins_a[size_t(ref.input)];
        const auto& b = ins_b[size_t(ref.input)];
        const auto& c = ins_c[size_t(ref.input)];
        const std::string label =
                fmt::format("{}.L{}/{}", ref.name, ref.width, kAvxFmaInputs[ref.input].name);

        const auto jit = run_on(jit_core, code, a, b, c, code_addr);
        const auto itp = run_on(interp_core, code, a, b, c, code_addr);
        ++comparisons;

        if (jit.exit != int(swift::translator::None)) {
            // FALLBACK / ILL_CODE both land here: the encoding was DECLINED
            // rather than mis-executed.  For a guest that is fatal, so it is a
            // failure of this case rather than a gap to note.
            if (bad_exits++ < 15) {
                problems.push_back(fmt::format(
                        "{}: block did not reach HLT (exit={}); encoding {} was not decoded", label,
                        jit.exit, ref.enc));
            }
            continue;
        }
        if (jit.ymm != itp.ymm || jit.exit != itp.exit) {
            if (divergences++ < 15) {
                problems.push_back(fmt::format("{}: JIT/interpreter divergence (ymm0 {} vs {})",
                                               label, Hex(jit.ymm[0]), Hex(itp.ymm[0])));
            }
        }

        for (const auto& [backend, got] : {std::pair<const char*, const Out*>{"jit", &jit},
                                           std::pair<const char*, const Out*>{"interp", &itp}}) {
            if (got->ymm[0] != want && mismatches++ < 15) {
                problems.push_back(fmt::format("{} [{}]: got {}, Rosetta says {} (enc {})", label,
                                               backend, Hex(got->ymm[0]), Hex(want), ref.enc));
            }
            // The two sources are read-only in every FMA form, so they must
            // come back untouched -- and no register beyond them and the
            // destination may change, in particular no bystander's UPPER half.
            if (got->ymm[1] != a && bystanders++ < 15) {
                problems.push_back(fmt::format("{} [{}]: source ymm1 was modified, {} != {}", label,
                                               backend, Hex(got->ymm[1]), Hex(a)));
            }
            if (got->ymm[2] != b && bystanders++ < 15) {
                problems.push_back(fmt::format("{} [{}]: source ymm2 was modified, {} != {}", label,
                                               backend, Hex(got->ymm[2]), Hex(b)));
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

    // One joined INFO rather than a loop of UNSCOPED_INFO: Catch2 clears
    // unscoped messages at the next assertion, so a loop before several CHECKs
    // attaches every message to the FIRST one and the later failures print
    // nothing.
    std::string report;
    for (size_t i = 0; i < problems.size() && i < 40; ++i) {
        report += problems[i];
        report += '\n';
    }
    INFO(report);
    CHECK(bad_exits == 0);
    CHECK(divergences == 0);
    CHECK(mismatches == 0);
    CHECK(bystanders == 0);
    // Pinned so a coverage regression -- rows lost in regeneration, or a
    // mnemonic dropped from avx_fma_ops.inc -- cannot pass as success.
    CHECK(comparisons == std::size(kAvxFmaRefs));
    CHECK(std::size(kAvxFmaRefs) == 3360u);
}
