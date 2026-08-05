// Differential test for the VEX integer / data-rearrangement family
// (source/runtime/frontend/x86/decoder_avx_int.cc) against reference values
// produced by executing the same encodings on real x86-64 hardware under
// Rosetta 2.
//
// WHY A ROSETTA REFERENCE AND NOT UNICORN
// ---------------------------------------
// Unicorn 2.1.4 rejects every VEX.L=1 encoding with UC_ERR_INSN_INVALID, so the
// 256-bit forms have no emulator oracle at all.  Rosetta 2 executes AVX2
// including the full 256-bit register file, so avx_int_rosetta_ref.c runs each
// encoding there and records the 32 bytes the hardware produced.  Nothing in
// the reference table is hand-computed.
//
// WHAT THIS CASE IS FOR
// ---------------------
// 1. PER-LANE VERSUS CROSS-LANE.  Contract C1 splits a YMM into two independent
//    V128 values, which is only sound because AVX2 defines nearly all of this
//    family per 128-bit lane.  Input pair "laneidx" makes the two readings
//    disagree in 30 of 32 bytes for any shuffle-like operation, and pair
//    "permidx" is built so vpermd's every output dword comes from the OTHER
//    128-bit lane.  A wrong split cannot coincidentally match.
// 2. CONTRACT C3.  The generator poisons ymm0 before each instruction and reads
//    all 32 bytes back, so a VEX.128 form that fails to zero bits 255:128 shows
//    up as poison in the reference comparison rather than passing silently.
// 3. SHAPE EQUIVALENCE.  Every instruction is run in both its register-source
//    and its memory-source form; both must produce the identical architectural
//    result, which is what catches an operand read at the wrong width.
// 4. JIT VERSUS INTERPRETER.  Both backends run every block.  They implement
//    the Vec* opcodes independently (translator_alu.cpp versus
//    interpreter.cpp), and this family relies on some fine print -- notably
//    that VecTableLookup8 masks its control byte with 0x8F -- where the two
//    could plausibly disagree.
// 5. BYSTANDERS.  Every register other than the operands is poisoned per
//    register and per byte; any of them changing means a handler wrote the
//    wrong register.
//
// The instruction table is avx_int_ops.inc, included VERBATIM by this file and
// by the generator, so the two sides cannot drift apart.
//
// The mini-assembler pieces below (CodeBuf, MemOp, the VEX emitters) are
// deliberately a local copy of the ones in x86_fuzz.cpp rather than shared:
// they are short, and duplicating them keeps this file independent of a
// 400 KB translation unit that other work is editing concurrently.

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <sys/mman.h>
#include "runtime/common/svm_config.h"
#include "runtime/backend/smc_tracker.h"
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

using namespace swift;
using namespace swift::translator::x86;

namespace {

using u8 = swift::u8;
using u32 = swift::u32;
using u64 = swift::u64;

// ---- minimal x86-64 encoder ----------------------------------------------

struct CodeBuf {
    std::vector<u8> c;
    void B(u8 v) { c.push_back(v); }
};

// The harness keeps the data pointer in r13, so the memory forms below all
// address [r13 + disp8].  r13's high bit is what makes the 3-byte C4 form
// mandatory: the 2-byte C5 form has no VEX.B field.
constexpr u8 kDataReg = 13;

// All fields UN-inverted; this function does the inverting, exactly like vex3()
// in avx_int_rosetta_ref.c.  pp: 0=none 1=66 2=F3 3=F2.  mm: 1=0F 2=0F38 3=0F3A.
void EmitVexC4(CodeBuf& b, u8 pp, u8 mm, u8 vvvv, bool l, u8 r, u8 x, u8 bb, bool w) {
    b.B(0xC4);
    b.B(u8(((~r & 1) << 7) | ((~x & 1) << 6) | ((~bb & 1) << 5) | (mm & 0x1F)));
    b.B(u8(((w ? 1 : 0) << 7) | ((~vvvv & 0xF) << 3) | ((l ? 1 : 0) << 2) | (pp & 3)));
}

void EmitModRMReg(CodeBuf& b, u8 reg, u8 rm) {
    b.B(u8(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

// [r13 + disp8].  r13 is 101b, which forces mod=01 with an explicit disp8 even
// when the displacement is zero.
void EmitModRMMem(CodeBuf& b, u8 reg, int disp) {
    b.B(u8(0x40 | ((reg & 7) << 3) | (kDataReg & 7)));
    b.B(u8(disp));
}

// "No src1": the ENCODED vvvv field must be 1111, and EmitVexC4 stores ~vvvv,
// so that is requested by passing 0.  Passing 0xF would encode xmm15.
constexpr u8 kVexNoSrc1 = 0;

using Vec256 = std::array<u8, 32>;

Vec256 ParseHex(const char* h) {
    Vec256 v{};
    const auto nib = [](char ch) -> u8 {
        return u8(ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10);
    };
    for (u32 i = 0; i < 32; ++i) {
        v[i] = u8((nib(h[i * 2]) << 4) | nib(h[i * 2 + 1]));
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

// Row shapes produced by avx_int_rosetta_ref.c.
struct AvxIntInput {
    const char* name;
    const char* a;
    const char* b;
};
struct AvxIntRef {
    const char* name;
    int width;
    unsigned imm;
    int pair;
    const char* result;
};

#include "avx_int_rosetta_ref.inc"

// The shared table, materialized once per shape.
struct AluOp {
    const char* name;
    u8 pp, mm, op, w, lmask;
};
struct ShiftXOp {
    const char* name;
    u8 pp, mm, op, lmask;
};
struct ShiftIOp {
    const char* name;
    u8 pp, mm, op, ext, lmask;
    unsigned imm;
};
struct UnOp {
    const char* name;
    u8 pp, mm, op, w, lmask;
    u32 sb128, sb256;
};
struct AluIOp {
    const char* name;
    u8 pp, mm, op, w, lmask;
    unsigned imm;
};
struct UnIOp {
    const char* name;
    u8 pp, mm, op, w, lmask;
    unsigned imm;
};

constexpr AluOp kAlu[] = {
#define SVM_AVX_INT_ALU(name, pp, mm, op, w, lmask) {#name, pp, mm, u8(op), w, lmask},
#include "avx_int_ops.inc"
};
constexpr ShiftXOp kShiftX[] = {
#define SVM_AVX_INT_SHIFTX(name, pp, mm, op, lmask) {#name, pp, mm, u8(op), lmask},
#include "avx_int_ops.inc"
};
constexpr ShiftIOp kShiftI[] = {
#define SVM_AVX_INT_SHIFTI(name, pp, mm, op, ext, imm, lmask) \
    {#name, pp, mm, u8(op), ext, lmask, imm},
#include "avx_int_ops.inc"
};
constexpr UnOp kUn[] = {
#define SVM_AVX_INT_UN(name, pp, mm, op, w, lmask, sb128, sb256) \
    {#name, pp, mm, u8(op), w, lmask, sb128, sb256},
#include "avx_int_ops.inc"
};
constexpr AluIOp kAluI[] = {
#define SVM_AVX_INT_ALUI(name, pp, mm, op, w, imm, lmask) {#name, pp, mm, u8(op), w, lmask, imm},
#include "avx_int_ops.inc"
};
constexpr UnIOp kUnI[] = {
#define SVM_AVX_INT_UNI(name, pp, mm, op, w, imm, lmask) {#name, pp, mm, u8(op), w, lmask, imm},
#include "avx_int_ops.inc"
};

// Data-slot displacements, matching DATA_* in avx_int_rosetta_ref.c.
constexpr int kOffA = 0x00;
constexpr int kOffB = 0x20;
constexpr int kOffM = 0x40;

}  // namespace

TEST_CASE("x86 avx integer vs rosetta reference") {
    if (!swift::runtime::GetSvmConfig().avx) {
        SUCCEED("SVM_AVX is not set; VEX integer Rosetta differential skipped");
        return;
    }

    // ---- inputs, and a self-check on what they can discriminate ----------
    std::vector<Vec256> ins_a, ins_b;
    for (const auto& in : kAvxIntInputs) {
        ins_a.push_back(ParseHex(in.a));
        ins_b.push_back(ParseHex(in.b));
    }
    const Vec256 blend_mask = ParseHex(kAvxIntMask);
    const Vec256 poison_dst = ParseHex(kAvxIntPoison);
    REQUIRE(ins_a.size() == std::size(kAvxIntInputs));

    // The "permidx" pair exists to separate a per-lane vpermd from the real
    // cross-lane one.  Recompute both readings here rather than trusting the
    // comment: if they ever coincide, the whole case stops proving anything.
    {
        int permidx = -1;
        for (u32 i = 0; i < std::size(kAvxIntInputs); ++i) {
            if (std::strcmp(kAvxIntInputs[i].name, "permidx") == 0) {
                permidx = int(i);
            }
        }
        REQUIRE(permidx >= 0);
        const auto& a = ins_a[u32(permidx)];
        const auto& b = ins_b[u32(permidx)];
        const auto dword = [](const Vec256& v, u32 i) {
            return u32(v[i * 4]) | (u32(v[i * 4 + 1]) << 8) | (u32(v[i * 4 + 2]) << 16) |
                   (u32(v[i * 4 + 3]) << 24);
        };
        Vec256 cross{}, per_lane{};
        for (u32 i = 0; i < 8; ++i) {
            const u32 index = dword(a, i) & 7;
            const u32 whole = dword(b, index);
            // A per-lane (wrong) reading would index within the output's own
            // 128-bit lane, i.e. mask to 2 bits and stay in that half.
            const u32 lane_local = dword(b, (i / 4) * 4 + (index & 3));
            for (u32 k = 0; k < 4; ++k) {
                cross[i * 4 + k] = u8(whole >> (k * 8));
                per_lane[i * 4 + k] = u8(lane_local >> (k * 8));
            }
        }
        REQUIRE(cross != per_lane);
        bool found = false;
        for (const auto& ref : kAvxIntRefs) {
            if (std::strcmp(ref.name, "vpermd") == 0 && ref.pair == permidx) {
                CHECK(Hex(cross) == ref.result);
                found = true;
            }
        }
        REQUIRE(found);
    }

    // ---- arena and the two cores ------------------------------------------
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 data = base + 0x300000;
    const u64 stack = base + 0x200000;

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

    // Per register AND per byte, so a write to the wrong register is not just
    // detectable but identifiable.
    const auto poison = [](u32 reg) {
        Vec256 v{};
        for (u32 j = 0; j < 32; ++j) {
            v[j] = u8(0xA5 ^ (reg * 32 + j));
        }
        return v;
    };

    struct Out {
        std::array<Vec256, 16> ymm{};
        int exit{};
    };

    size_t code_cursor = 1;
    std::vector<std::string> problems;
    size_t comparisons = 0, bad_exits = 0, divergences = 0, mismatches = 0, bystanders = 0;

    // ymm0 = the poisoned destination, ymm1 = A, ymm2 = B, ymm3 = the blend
    // mask -- the same assignment the generator's prologue establishes.
    const auto run_on = [&](X86Core* core, const CodeBuf& code, const Vec256& a, const Vec256& b,
                            u64 code_addr) {
        std::memcpy(reinterpret_cast<void*>(code_addr), code.c.data(), code.c.size());
        std::memcpy(reinterpret_cast<void*>(data + kOffA), a.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + kOffB), b.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + kOffM), blend_mask.data(), 32);
        auto& ctx = core->GetContext();
        for (u32 i = 0; i < 16; ++i) {
            const auto p = poison(i);
            std::memcpy(ctx.xmms[i].b, p.data(), 16);
            std::memcpy(ctx.ymm_high[i].b, p.data() + 16, 16);
        }
        const Vec256* seed[4] = {&poison_dst, &a, &b, &blend_mask};
        for (u32 i = 0; i < 4; ++i) {
            std::memcpy(ctx.xmms[i].b, seed[i]->data(), 16);
            std::memcpy(ctx.ymm_high[i].b, seed[i]->data() + 16, 16);
        }
        ctx.r13.qword = data;
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

    const auto check = [&](const std::string& label, CodeBuf code, int pair, const Vec256& want) {
        code.B(0xF4);  // hlt
        const u64 code_addr = base + code_cursor * 0x100;
        ++code_cursor;
        REQUIRE(code.c.size() < 0x100);
        const auto& a = ins_a[u32(pair)];
        const auto& b = ins_b[u32(pair)];
        const auto jit = run_on(jit_core, code, a, b, code_addr);
        const auto itp = run_on(interp_core, code, a, b, code_addr);
        ++comparisons;
        if (jit.exit != int(swift::translator::None)) {
            if (bad_exits++ < 12) {
                problems.push_back(fmt::format("{}: jit exit {} (block not translated)",
                                               label,
                                               jit.exit));
            }
            return;
        }
        if (jit.ymm != itp.ymm || jit.exit != itp.exit) {
            if (divergences++ < 12) {
                problems.push_back(fmt::format("{}: jit/interp divergence\n  jit    {}\n  interp {}",
                                               label,
                                               Hex(jit.ymm[0]),
                                               Hex(itp.ymm[0])));
            }
        }
        for (const auto& [who, got] : {std::pair{"jit", &jit}, std::pair{"interp", &itp}}) {
            if (got->ymm[0] != want) {
                if (mismatches++ < 12) {
                    problems.push_back(fmt::format("{}/{}: want {}\n            got  {}",
                                                   label,
                                                   who,
                                                   Hex(want),
                                                   Hex(got->ymm[0])));
                }
            }
            for (u32 i = 4; i < 16; ++i) {
                if (got->ymm[i] != poison(i)) {
                    if (bystanders++ < 12) {
                        problems.push_back(
                                fmt::format("{}/{}: ymm{} clobbered", label, who, i));
                    }
                }
            }
        }
    };

    // ---- encoders for each shape, mirroring the generator exactly ---------
    const auto vex_rr = [](CodeBuf& b, u8 pp, u8 mm, u8 op, u8 dst, u8 src1, u8 src2, bool l,
                           bool w) {
        EmitVexC4(b, pp, mm, src1, l, u8(dst >> 3), 0, u8(src2 >> 3), w);
        b.B(op);
        EmitModRMReg(b, dst, src2);
    };
    const auto vex_rm = [](CodeBuf& b, u8 pp, u8 mm, u8 op, u8 dst, u8 src1, int disp, bool l,
                           bool w) {
        EmitVexC4(b, pp, mm, src1, l, u8(dst >> 3), 0, u8(kDataReg >> 3), w);
        b.B(op);
        EmitModRMMem(b, dst, disp);
    };

    // ---- the one place the oracle is not trusted --------------------------
    // Rosetta 2 truncates the VPSLLVQ / VPSRLVQ shift count to 32 BITS.  The
    // Intel SDM uses the whole 64-bit qword ("IF COUNT_SRC[63:0] < 64"), so a
    // count whose low dword is below 64 but whose full value is not must give
    // zero, and Rosetta instead performs the shift.  Probed directly rather
    // than inferred: with value 1 and count 0x0000000100000001 (2^32 + 1)
    // Rosetta returns 2, while count 0x100 -- whose low dword IS >= 64 --
    // correctly returns 0, which rules out a low-BYTE reading and pins it to
    // the low dword.  Only the "signbnd" pair's upper lane has a count of that
    // shape, so exactly two rows are affected; they are excluded and counted so
    // the exclusion cannot silently grow.
    size_t rosetta_defects = 0;
    const auto untrusted = [&](const AvxIntRef& ref) {
        return ref.width == 256 && std::strcmp(kAvxIntInputs[ref.pair].name, "signbnd") == 0 &&
               (std::strcmp(ref.name, "vpsllvq") == 0 || std::strcmp(ref.name, "vpsrlvq") == 0);
    };

    // ---- drive every reference row ----------------------------------------
    size_t handled_rows = 0;
    for (const auto& ref : kAvxIntRefs) {
        if (untrusted(ref)) {
            ++rosetta_defects;
            ++handled_rows;
            continue;
        }
        const Vec256 want = ParseHex(ref.result);
        const bool l = ref.width == 256;
        const std::string tag =
                fmt::format("{}.L{}.i{:02x}/{}", ref.name, ref.width, ref.imm,
                            kAvxIntInputs[ref.pair].name);
        bool matched = false;

        for (const auto& op : kAlu) {
            if (std::strcmp(op.name, ref.name) != 0 || ref.imm != 0) {
                continue;
            }
            matched = true;
            {
                CodeBuf b;
                vex_rr(b, op.pp, op.mm, op.op, 0, 1, 2, l, op.w != 0);
                check(tag + ".rr", b, ref.pair, want);
            }
            {
                CodeBuf b;
                vex_rm(b, op.pp, op.mm, op.op, 0, 1, kOffB, l, op.w != 0);
                check(tag + ".rm", b, ref.pair, want);
            }
        }
        for (const auto& op : kShiftX) {
            if (std::strcmp(op.name, ref.name) != 0 || ref.imm != 0) {
                continue;
            }
            matched = true;
            {
                CodeBuf b;
                vex_rr(b, op.pp, op.mm, op.op, 0, 1, 2, l, false);
                check(tag + ".rr", b, ref.pair, want);
            }
            {
                CodeBuf b;
                vex_rm(b, op.pp, op.mm, op.op, 0, 1, kOffB, l, false);
                check(tag + ".rm", b, ref.pair, want);
            }
        }
        for (const auto& op : kShiftI) {
            if (std::strcmp(op.name, ref.name) != 0 || op.imm != ref.imm) {
                continue;
            }
            matched = true;
            // NDD: destination is VEX.vvvv, source is r/m, ModRM.reg is /n.
            CodeBuf b;
            EmitVexC4(b, op.pp, op.mm, 0, l, 0, 0, 0, false);
            b.B(op.op);
            EmitModRMReg(b, op.ext, 1);
            b.B(u8(op.imm));
            check(tag + ".ri", b, ref.pair, want);
        }
        for (const auto& op : kUn) {
            if (std::strcmp(op.name, ref.name) != 0 || ref.imm != 0) {
                continue;
            }
            matched = true;
            {
                CodeBuf b;
                vex_rr(b, op.pp, op.mm, op.op, 0, kVexNoSrc1, 1, l, op.w != 0);
                check(tag + ".rr", b, ref.pair, want);
            }
            {
                // Memory form of the same operand: [r13+A] holds exactly what
                // ymm1 was seeded with, so the result must be identical.  Note
                // this checks the VALUE, not the access width -- a handler that
                // read 16 bytes where the architecture reads 2 would still pass,
                // because the surrounding bytes are A's own and go unused.  The
                // access width is only observable as a fault, which this harness
                // does not provoke.
                CodeBuf b;
                vex_rm(b, op.pp, op.mm, op.op, 0, kVexNoSrc1, kOffA, l, op.w != 0);
                check(tag + ".rm", b, ref.pair, want);
            }
        }
        for (const auto& op : kAluI) {
            if (std::strcmp(op.name, ref.name) != 0 || op.imm != ref.imm) {
                continue;
            }
            matched = true;
            {
                CodeBuf b;
                vex_rr(b, op.pp, op.mm, op.op, 0, 1, 2, l, op.w != 0);
                b.B(u8(op.imm));
                check(tag + ".rr", b, ref.pair, want);
            }
            {
                CodeBuf b;
                vex_rm(b, op.pp, op.mm, op.op, 0, 1, kOffB, l, op.w != 0);
                b.B(u8(op.imm));
                check(tag + ".rm", b, ref.pair, want);
            }
        }
        for (const auto& op : kUnI) {
            if (std::strcmp(op.name, ref.name) != 0 || op.imm != ref.imm) {
                continue;
            }
            matched = true;
            {
                CodeBuf b;
                vex_rr(b, op.pp, op.mm, op.op, 0, kVexNoSrc1, 1, l, op.w != 0);
                b.B(u8(op.imm));
                check(tag + ".rr", b, ref.pair, want);
            }
            {
                CodeBuf b;
                vex_rm(b, op.pp, op.mm, op.op, 0, kVexNoSrc1, kOffA, l, op.w != 0);
                b.B(u8(op.imm));
                check(tag + ".rm", b, ref.pair, want);
            }
        }

        // ---- the four one-off shapes -------------------------------------
        if (std::strcmp(ref.name, "vpblendvb") == 0) {
            matched = true;
            {
                CodeBuf b;
                vex_rr(b, 1, 3, 0x4C, 0, 1, 2, l, false);
                b.B(u8(3 << 4));  // /is4: the mask register is the high nibble
                check(tag + ".rr", b, ref.pair, want);
            }
            {
                CodeBuf b;
                vex_rm(b, 1, 3, 0x4C, 0, 1, kOffB, l, false);
                b.B(u8(3 << 4));
                check(tag + ".rm", b, ref.pair, want);
            }
        }
        if (std::strcmp(ref.name, "vinserti128") == 0) {
            matched = true;
            {
                CodeBuf b;
                vex_rr(b, 1, 3, 0x38, 0, 1, 2, true, false);
                b.B(u8(ref.imm));
                check(tag + ".rr", b, ref.pair, want);
            }
            {
                CodeBuf b;
                vex_rm(b, 1, 3, 0x38, 0, 1, kOffB, true, false);
                b.B(u8(ref.imm));
                check(tag + ".rm", b, ref.pair, want);
            }
        }
        if (std::strcmp(ref.name, "vextracti128") == 0) {
            matched = true;
            // The destination is the r/m operand; ModRM.reg holds the source.
            CodeBuf b;
            EmitVexC4(b, 1, 3, kVexNoSrc1, true, 0, 0, 0, false);
            b.B(0x39);
            EmitModRMReg(b, 1, 0);
            b.B(u8(ref.imm));
            check(tag + ".rr", b, ref.pair, want);
        }
        if (std::strcmp(ref.name, "vbroadcasti128") == 0) {
            matched = true;
            CodeBuf b;
            vex_rm(b, 1, 2, 0x5A, 0, kVexNoSrc1, kOffA, true, false);
            check(tag + ".rm", b, ref.pair, want);
        }

        if (matched) {
            ++handled_rows;
        } else {
            FAIL("reference row for unhandled mnemonic: " << ref.name);
        }
    }

    X86Core::Destroy(jit_core);
    X86Core::Destroy(interp_core);
    X86Instance::Destroy(jit_instance);
    X86Instance::Destroy(interp_instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(arena, kArenaSize);

    for (size_t i = 0; i < problems.size() && i < 40; ++i) {
        UNSCOPED_INFO(problems[i]);
    }
    CHECK(handled_rows == std::size(kAvxIntRefs));
    CHECK(rosetta_defects == 2u);
    CHECK(bad_exits == 0);
    CHECK(divergences == 0);
    CHECK(mismatches == 0);
    CHECK(bystanders == 0);
    // Guards against the table silently shrinking to nothing.
    CHECK(comparisons > 2000);
}
