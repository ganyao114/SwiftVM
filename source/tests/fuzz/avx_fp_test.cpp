// ===========================================================================
// AVX floating-point family against a ROSETTA oracle.
// ===========================================================================
//
// Unicorn 2.1.4 refuses every VEX.L=1 encoding (UC_ERR_INSN_INVALID), so the
// 256-bit forms have no emulator oracle at all.  For the FLOAT family that gap
// matters even at 128 bits: the parts that go wrong are NaN propagation,
// min/max operand order, signed zero and denormals, and a hand-written model
// of those would just encode the same misunderstanding as the implementation
// it is supposed to check.  Rosetta 2 on macOS 26/27 executes AVX including
// the full 256-bit register file, so avx_fp_rosetta_ref.inc holds the literal
// bytes real x86-64 hardware wrote to memory -- nothing in it is computed here.
//
// The instruction table (avx_fp_ops.inc) is shared VERBATIM with the generator
// (avx_fp_rosetta_ref.c), so the two sides cannot drift onto different
// opcodes, and every encoding the generator builds was disassembled and
// confirmed to be the intended mnemonic before the data was captured.
//
// WHAT THE REFERENCE DATA SETTLES, measured rather than assumed:
//
//   Operand-1 priority on NaN.  In pair f32nanA lane 7 both operands are NaN
//   (A = signalling 0x7F800333, B = quiet 0x7FC00222) and vaddps returned
//   0x7FC00333 -- operand 1's NaN, quieted.  The MIRROR pair f32nanB returned
//   0x7FC00222, again operand 1's.  ARM64's FADD returns a default NaN for
//   both, so this pins that the backend's EmitVecFloatNaNFixup is doing real
//   work rather than being incidentally right.
//
//   min/max are NOT commutative and do NOT quiet.  vmaxps on f32nanA lane 7
//   returned 0x7FC00222 (operand 2, quiet) and on the mirror 0x7F800333 --
//   operand 2 again, and STILL SIGNALLING.  x86 MIN/MAX return operand 2
//   bit-for-bit whenever the ordered comparison is false; ARM's FMAX would
//   return the quieted NaN in both cases and would be identical across the
//   mirror.  This is exactly the pair of rows a naive lowering fails.
//
//   Contract C3 is measured, not assumed.  ymm0 is poisoned with 0xA5^index
//   before every instruction and the generator reads back all 32 bytes, so a
//   VEX.128 form's reference row carries sixteen literal zero bytes that the
//   hardware wrote.  An implementation that skipped ZeroYmmHigh shows the
//   poison instead.
//
//   vmovmskps ymm combines as `lo | hi << 4`.  Pair signbits gives 0x5 and
//   0xE for the two halves; the 256-bit row is 0xE5, not 0x5E.  vmovmskpd is
//   `lo | hi << 2`: 0x0 and 0x3 give 0x0C.
//
// Each block under test is a SINGLE instruction (plus, for the flag-setting
// compares, a deterministic flag seed and five SETcc captures): the operand
// registers are written straight into ThreadContext64 and the result read
// straight back out, so a broken vmovdqu cannot mask a broken vaddps.
// ymm_high is poisoned per register and per byte beforehand, so a handler that
// writes only the low half, writes the wrong register's upper half, or leaves
// a bystander dirty is caught too.
//
// Every form is run in BOTH the reg-reg and the reg-mem operand shape against
// the same reference.  That is exact rather than approximate: the two shapes
// differ only in where the source bytes come from and both are fed identical
// bytes, so x86 defines them to produce identical results.  What the second
// shape exercises is SwiftVM's VEX address folding and its split 32-byte load.

#include <algorithm>
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
#include "runtime/frontend/x86/decoder.h"
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

using namespace swift::translator::x86;
using namespace swift;

namespace {

// --------------------------------------------------------------------------
// Minimal local assembler.  Deliberately NOT shared with x86_fuzz.cpp: these
// helpers are a dozen lines each, and duplicating them is far better than
// coupling two test files that are edited independently.
// --------------------------------------------------------------------------
struct CodeBuf {
    std::vector<u8> c;
    void B(u8 v) { c.push_back(v); }
    void D(u32 v) {
        for (int i = 0; i < 4; ++i) B(u8(v >> (8 * i)));
    }
};

// r13 holds the data pointer; it encodes as rm = 101, which always needs a
// displacement byte even for a zero offset.
constexpr u8 kDataReg = 13;

struct MemOp {
    s32 disp{0};
};

void EmitModRMReg(CodeBuf& b, u8 reg_field, u8 rm_reg) {
    b.B(u8(0xC0 | ((reg_field & 7) << 3) | (rm_reg & 7)));
}

void EmitModRMMem(CodeBuf& b, u8 reg_field, const MemOp& m) {
    if (m.disp >= -128 && m.disp <= 127) {
        b.B(u8(0x40 | ((reg_field & 7) << 3) | (kDataReg & 7)));
        b.B(u8(m.disp));
    } else {
        b.B(u8(0x80 | ((reg_field & 7) << 3) | (kDataReg & 7)));
        b.D(u32(m.disp));
    }
}

// Three-byte VEX.  Every field is passed UN-inverted, exactly as in the
// generator's vex3(), so the two programs build byte-identical encodings.
void EmitVexC4(CodeBuf& b, u8 pp, u8 mmmmm, u8 vvvv, bool l, u8 r, u8 x, u8 bb, bool w = false) {
    b.B(0xC4);
    b.B(u8(((~r & 1) << 7) | ((~x & 1) << 6) | ((~bb & 1) << 5) | (mmmmm & 0x1F)));
    b.B(u8(((w ? 1 : 0) << 7) | ((~vvvv & 0xF) << 3) | ((l ? 1 : 0) << 2) | (pp & 3)));
}

// The ENCODED vvvv field must be 1111b for a two-operand form.  EmitVexC4
// stores ~vvvv, so that is requested by passing 0 -- passing 0xF would encode
// xmm15 as a source and make the opcode undecodable.
constexpr u8 kVexNoSrc1 = 0;

void VexRR(CodeBuf& b, u8 pp, u8 op, u8 dst, u8 src1, u8 src2, bool l) {
    EmitVexC4(b, pp, 1, src1, l, u8(dst >> 3), 0, u8(src2 >> 3));
    b.B(op);
    EmitModRMReg(b, dst, src2);
}

void VexRM(CodeBuf& b, u8 pp, u8 op, u8 dst, u8 src1, const MemOp& m, bool l) {
    EmitVexC4(b, pp, 1, src1, l, u8(dst >> 3), 0, u8(kDataReg >> 3));
    b.B(op);
    EmitModRMMem(b, dst, m);
}

// setcc [r13+disp].  r13 needs REX.B, and the SETcc group is /0.
void SetccMem(CodeBuf& b, u8 op2, const MemOp& m) {
    b.B(0x41);  // REX.B
    b.B(0x0F);
    b.B(op2);
    EmitModRMMem(b, 0, m);
}

// mov eax, 0x7FFFFFFF ; add eax, 1  ->  OF=1 SF=1 AF=1 CF=0 ZF=0.
// Seeding the flags this way makes the compare's architectural CLEARING of OF
// and SF observable instead of a no-op.
void SeedFlags(CodeBuf& b) {
    b.B(0xB8);
    b.D(0x7FFFFFFFu);
    b.B(0x83);
    b.B(0xC0);
    b.B(0x01);
}

// --------------------------------------------------------------------------
// Reference data.
// --------------------------------------------------------------------------
struct AvxFpInput {
    const char* name;
    const char* a;
    const char* b;
};
struct AvxFpRef {
    const char* name;
    int width;  // 128 or 256: the VEX.L the generator used
    int pair;
    int imm;  // compare predicate, or -1
    const char* result;
};
#include "avx_fp_rosetta_ref.inc"

// --------------------------------------------------------------------------
// The instruction table, shared verbatim with the generator.
// --------------------------------------------------------------------------
enum class Shape { Packed, Scalar, Unary, Widen, Narrow, Cmp, Comis, Movmsk };

struct Entry {
    const char* name;
    u8 pp;
    u8 opcode;
    u32 lane;
    Shape shape;
};

constexpr Entry kEntries[] = {
#define SVM_AVXFP_PACKED(name, pp, opcode, lane) {#name, pp, u8(opcode), lane, Shape::Packed},
#define SVM_AVXFP_SCALAR(name, pp, opcode, lane) {#name, pp, u8(opcode), lane, Shape::Scalar},
#define SVM_AVXFP_UNARY(name, pp, opcode, lane) {#name, pp, u8(opcode), lane, Shape::Unary},
#define SVM_AVXFP_WIDEN(name, pp, opcode) {#name, pp, u8(opcode), 32, Shape::Widen},
#define SVM_AVXFP_NARROW(name, pp, opcode) {#name, pp, u8(opcode), 64, Shape::Narrow},
#define SVM_AVXFP_CMP(name, pp, opcode, lane, scalar) {#name, pp, u8(opcode), lane, Shape::Cmp},
#define SVM_AVXFP_COMIS(name, pp, opcode, lane) {#name, pp, u8(opcode), lane, Shape::Comis},
#define SVM_AVXFP_MOVMSK(name, pp, opcode, lane) {#name, pp, u8(opcode), lane, Shape::Movmsk},
#include "avx_fp_ops.inc"
};

// Whether a Cmp entry is the scalar (ss/sd) form; the packed/scalar split is
// carried by pp, which is exactly how the decoder distinguishes them.
constexpr bool CmpIsScalar(const Entry& e) { return e.pp == 2 || e.pp == 3; }

using Vec256 = std::array<u8, 32>;

Vec256 ParseHex(const char* h) {
    Vec256 v{};
    const auto nib = [](char ch) -> u8 { return u8(ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10); };
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

// True when `got` differs from `want` ONLY in that some lane holds the
// POSITIVE default QNaN where the hardware produced the NEGATIVE one.
//
// x86 SQRT of a negative operand yields the "QNaN floating-point indefinite",
// which has the SIGN BIT SET: 0xFFC00000 (f32) / 0xFFF8000000000000 (f64).
// ARM64's FSQRT yields the positive default NaN instead, and the backend's
// EmitVecFUnary (translator_alu.cpp) applies no NaN fixup -- unlike VecFAdd /
// VecFSub / VecFMul / VecFDiv, whose EmitVecFloatNaNFixup already materializes
// the correct negative constant.  So this is a BACKEND gap in one emitter, not
// a decoder bug, and it is shared with the legacy SSE sqrtps/sqrtpd/sqrtss/
// sqrtsd paths, which go through the same IR opcode.
//
// The deviation is pinned rather than silenced, and pinned by DIAGNOSIS rather
// than by "it differs": any lane that disagrees in some other way still counts
// as a mismatch, so the exemption cannot hide a second, unrelated bug.
bool OnlySqrtNanSign(const Vec256& got, const Vec256& want, u32 lane_bits) {
    const size_t step = lane_bits / 8;
    for (size_t off = 0; off < got.size(); off += step) {
        if (std::equal(got.begin() + off, got.begin() + off + step, want.begin() + off)) {
            continue;
        }
        // Little-endian: the sign bit is the top bit of the LAST byte.
        const size_t top = off + step - 1;
        if (got[top] != u8(want[top] & 0x7F)) {
            return false;
        }
        if (!std::equal(got.begin() + off, got.begin() + top, want.begin() + off)) {
            return false;
        }
        // And the value really is the default QNaN, not any other NaN.
        const bool f32 = lane_bits == 32;
        if (want[top] != (f32 ? 0xFFu : 0xFFu) || want[top - 1] != (f32 ? 0xC0u : 0xF8u)) {
            return false;
        }
        for (size_t i = off; i + 2 < top; ++i) {
            if (want[i] != 0) {
                return false;
            }
        }
    }
    return true;
}

// Must agree with SVM_AVXFP_POISON in avx_fp_ops.inc for register 0, which is
// what the generator loaded into ymm0.
Vec256 Poison(u32 reg) {
    Vec256 v{};
    for (u32 j = 0; j < 32; ++j) {
        v[j] = u8(0xA5 ^ (reg * 32 + j));
    }
    return v;
}

}  // namespace

TEST_CASE("x86 avx fp vs rosetta reference") {
    if (!swift::runtime::GetSvmConfig().avx) {
        SUCCEED("SVM_AVX is not set; VEX floating-point Rosetta differential skipped");
        return;
    }

    // ---- properties of the reference data this case depends on ------------
    // Asserted rather than trusted: if a future regeneration or an edit to the
    // input vectors removed one of these, the differential would still pass
    // while having stopped testing the thing it exists for.
    std::vector<Vec256> ins_a, ins_b;
    for (const auto& in : kAvxFpInputs) {
        ins_a.push_back(ParseHex(in.a));
        ins_b.push_back(ParseHex(in.b));
    }
    REQUIRE(std::size(kAvxFpInputs) >= 4);
    {
        // Both 128-bit lanes must differ on both sides for at least most
        // pairs, or an implementation deriving the upper half from the lower
        // half's operands could pass.
        size_t discriminating = 0;
        for (size_t i = 0; i < ins_a.size(); ++i) {
            const auto& a = ins_a[i];
            const auto& b = ins_b[i];
            if (!std::equal(a.begin(), a.begin() + 16, a.begin() + 16) &&
                !std::equal(b.begin(), b.begin() + 16, b.begin() + 16)) {
                ++discriminating;
            }
        }
        INFO("input pairs whose two 128-bit lanes differ on both sides: " << discriminating);
        REQUIRE(discriminating >= std::size(kAvxFpInputs) - 2);
    }
    {
        // The NaN mirror must actually discriminate: vminps/vmaxps on the
        // mirrored pairs must give DIFFERENT answers, which is the property
        // ARM's FMIN/FMAX does not have.
        int nan_a = -1, nan_b = -1;
        for (u32 i = 0; i < std::size(kAvxFpInputs); ++i) {
            if (std::strcmp(kAvxFpInputs[i].name, "f32nanA") == 0) nan_a = int(i);
            if (std::strcmp(kAvxFpInputs[i].name, "f32nanB") == 0) nan_b = int(i);
        }
        REQUIRE(nan_a >= 0);
        REQUIRE(nan_b >= 0);
        for (const char* op : {"vminps", "vmaxps"}) {
            const char* ra = nullptr;
            const char* rb = nullptr;
            for (const auto& r : kAvxFpRefs) {
                if (std::strcmp(r.name, op) != 0 || r.width != 256) continue;
                if (r.pair == nan_a) ra = r.result;
                if (r.pair == nan_b) rb = r.result;
            }
            REQUIRE(ra != nullptr);
            REQUIRE(rb != nullptr);
            INFO(op << " gives the same answer on the mirrored NaN pairs -- the reference data "
                       "no longer distinguishes x86 min/max from ARM FMIN/FMAX");
            REQUIRE(std::strcmp(ra, rb) != 0);
        }
    }
    {
        // Every 128-bit row of a form that writes a vector must carry sixteen
        // ZERO bytes in its upper half: that is contract C3 as the hardware
        // reported it, against a poisoned destination.
        size_t checked = 0;
        for (const auto& r : kAvxFpRefs) {
            if (r.width != 128) continue;
            const Entry* e = nullptr;
            for (const auto& c : kEntries) {
                if (std::strcmp(c.name, r.name) == 0) e = &c;
            }
            REQUIRE(e != nullptr);
            if (e->shape == Shape::Comis || e->shape == Shape::Movmsk) continue;
            const auto v = ParseHex(r.result);
            INFO(r.name << " pair " << r.pair
                        << ": the 128-bit reference does not have a zeroed upper half");
            REQUIRE(std::all_of(v.begin() + 16, v.end(), [](u8 x) { return x == 0; }));
            ++checked;
        }
        REQUIRE(checked > 0);
    }

    // ---- harness -----------------------------------------------------------
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x200000;
    const u64 data = base + 0x300000;

    const MemOp ma{0x100};    // A
    const MemOp mb{0x140};    // B
    const MemOp mout{0x180};  // flag / mask capture

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
        std::array<Vec256, 16> ymm{};  // xmms[i] in [0..15], ymm_high[i] in [16..31]
        Vec256 mem{};
        u64 rax{};
        int exit{};
    };

    size_t code_cursor = 1;
    std::vector<std::string> problems;
    size_t comparisons = 0, bad_exits = 0, divergences = 0, mismatches = 0, bystanders = 0;

    const auto run_on = [&](X86Core* core, const CodeBuf& code, const Vec256& a, const Vec256& b,
                            u64 code_addr) {
        std::memcpy(reinterpret_cast<void*>(code_addr), code.c.data(), code.c.size());
        std::memcpy(reinterpret_cast<void*>(data + u64(ma.disp)), a.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(mb.disp)), b.data(), 32);
        std::memset(reinterpret_cast<void*>(data + u64(mout.disp)), 0xCC, 32);
        auto& ctx = core->GetContext();
        for (u32 i = 0; i < 16; ++i) {
            const auto p = Poison(i);
            std::memcpy(ctx.xmms[i].b, p.data(), 16);
            std::memcpy(ctx.ymm_high[i].b, p.data() + 16, 16);
        }
        // ymm1 = A, ymm2 = B; ymm0 keeps its poison, matching the stub the
        // generator ran (vmovdqu ymm0,[poison] / ymm1,[A] / ymm2,[B]).
        std::memcpy(ctx.xmms[1].b, a.data(), 16);
        std::memcpy(ctx.ymm_high[1].b, a.data() + 16, 16);
        std::memcpy(ctx.xmms[2].b, b.data(), 16);
        std::memcpy(ctx.ymm_high[2].b, b.data() + 16, 16);
        ctx.rax.qword = 0xDEADBEEFDEADBEEFull;
        ctx.r13.qword = data;
        ctx.rsp.qword = stack;
        ctx.rip.qword = code_addr;
        Out o;
        o.exit = int(core->Run());
        for (u32 i = 0; i < 16; ++i) {
            std::memcpy(o.ymm[i].data(), ctx.xmms[i].b, 16);
            std::memcpy(o.ymm[i].data() + 16, ctx.ymm_high[i].b, 16);
        }
        o.rax = ctx.rax.qword;
        std::memcpy(o.mem.data(), reinterpret_cast<void*>(data + u64(mout.disp)), 32);
        return o;
    };

    // Where the answer is read from: the destination register, the capture
    // slot in memory (SETcc bytes), or eax (vmovmsk*).
    enum class Where { Ymm0, Memory, Rax32 };
    // Expect::SqrtNanSign is the one documented deviation; see OnlySqrtNanSign.
    enum class Expect { Match, SqrtNanSign };
    size_t known_sqrt_nan_sign = 0;

    const auto check = [&](const std::string& label, CodeBuf code, int pair, const Vec256& want,
                           Where where, Expect expect = Expect::Match, u32 lane_bits = 32) {
        code.c.push_back(0xF4);  // hlt
        const u64 code_addr = base + 0x1000 + code_cursor * 0x80;
        ++code_cursor;
        REQUIRE(code.c.size() < 0x80);
        REQUIRE(code_addr + 0x80 < stack);
        const auto& a = ins_a[size_t(pair)];
        const auto& b = ins_b[size_t(pair)];
        const auto jit = run_on(jit_core, code, a, b, code_addr);
        const auto itp = run_on(interp_core, code, a, b, code_addr);
        ++comparisons;

        if (jit.exit != int(swift::translator::None)) {
            // FALLBACK / ILL_CODE both land here: the form was DECLINED by the
            // decoder rather than mis-executed.  That is a coverage hole, not a
            // wrong answer, but it is still a failure for this case.
            if (bad_exits++ < 15) {
                problems.push_back(fmt::format(
                        "{}: block did not reach HLT (exit={}); the VEX float form was not decoded",
                        label, jit.exit));
            }
            return;
        }
        if (jit.ymm != itp.ymm || jit.mem != itp.mem || jit.rax != itp.rax ||
            jit.exit != itp.exit) {
            if (divergences++ < 15) {
                problems.push_back(fmt::format(
                        "{}: JIT/interpreter divergence (ymm0 {} vs {}, mem {} vs {}, "
                        "rax {:#x} vs {:#x})",
                        label, Hex(jit.ymm[0]), Hex(itp.ymm[0]), Hex(jit.mem), Hex(itp.mem),
                        jit.rax, itp.rax));
            }
        }

        for (const auto& [backend, got] : {std::pair<const char*, const Out*>{"jit", &jit},
                                           std::pair<const char*, const Out*>{"interp", &itp}}) {
            if (where == Where::Rax32) {
                // vmovmsk* writes a 32-bit GPR, so the upper 32 bits of rax
                // must be ZEROED, not preserved.  The reference stores the
                // mask as four little-endian bytes at the front of its slot.
                const u64 want_rax = u64(want[0]) | (u64(want[1]) << 8) | (u64(want[2]) << 16) |
                                     (u64(want[3]) << 24);
                if (got->rax != want_rax && mismatches++ < 15) {
                    problems.push_back(fmt::format("{} [{}]: rax {:#018x}, Rosetta says {:#018x}",
                                                   label, backend, got->rax, want_rax));
                }
                continue;
            }
            const Vec256& g = (where == Where::Memory) ? got->mem : got->ymm[0];
            if (g != want) {
                if (expect == Expect::SqrtNanSign && OnlySqrtNanSign(g, want, lane_bits)) {
                    // The documented EmitVecFUnary deviation, and nothing else.
                    ++known_sqrt_nan_sign;
                } else if (mismatches++ < 15) {
                    problems.push_back(fmt::format("{} [{}]: got {}, Rosetta says {}", label,
                                                   backend, Hex(g), Hex(want)));
                }
            }
            // No register other than the destination and the two sources may
            // change -- in particular no bystander's UPPER half may be
            // disturbed by the two-halves split.
            for (u32 i = (where == Where::Ymm0 ? 1u : 0u); i < 16; ++i) {
                if (i == 1 || i == 2) continue;
                if (got->ymm[i] != Poison(i) && bystanders++ < 15) {
                    problems.push_back(fmt::format("{} [{}]: bystander ymm{} clobbered, {} != {}",
                                                   label, backend, i, Hex(got->ymm[i]),
                                                   Hex(Poison(i))));
                }
            }
        }
    };

    // ---- drive every reference row ----------------------------------------
    for (const auto& ref : kAvxFpRefs) {
        const Entry* e = nullptr;
        for (const auto& c : kEntries) {
            if (std::strcmp(c.name, ref.name) == 0) e = &c;
        }
        if (e == nullptr) {
            FAIL("reference row for a mnemonic absent from avx_fp_ops.inc: " << ref.name);
        }
        const Vec256 want = ParseHex(ref.result);
        const bool l = ref.width == 256;
        const std::string pname = kAvxFpInputs[ref.pair].name;
        const std::string tag = fmt::format("{}.L{}/{}", ref.name, ref.width, pname);
        // Only the sqrt forms may take the documented NaN-sign exemption.
        const bool is_sqrt = std::strncmp(ref.name, "vsqrt", 5) == 0;
        const Expect expect = is_sqrt ? Expect::SqrtNanSign : Expect::Match;

        switch (e->shape) {
            case Shape::Packed:
            case Shape::Scalar: {
                // dst = vvvv OP r/m.  src1 is ymm1 and the destination ymm0,
                // deliberately different registers: a scalar implementation
                // that merged the untouched lanes from the DESTINATION (the
                // legacy SSE rule) instead of from vvvv is caught here.
                {
                    CodeBuf b;
                    VexRR(b, e->pp, e->opcode, 0, 1, 2, l);
                    check(tag + ".rr", b, ref.pair, want, Where::Ymm0, expect, e->lane);
                }
                {
                    CodeBuf b;
                    VexRM(b, e->pp, e->opcode, 0, 1, mb, l);
                    check(tag + ".rm", b, ref.pair, want, Where::Ymm0, expect, e->lane);
                }
                break;
            }
            case Shape::Unary:
            case Shape::Widen:
            case Shape::Narrow: {
                {
                    CodeBuf b;
                    VexRR(b, e->pp, e->opcode, 0, kVexNoSrc1, 1, l);
                    check(tag + ".rr", b, ref.pair, want, Where::Ymm0, expect, e->lane);
                }
                {
                    CodeBuf b;
                    VexRM(b, e->pp, e->opcode, 0, kVexNoSrc1, ma, l);
                    check(tag + ".rm", b, ref.pair, want, Where::Ymm0, expect, e->lane);
                }
                break;
            }
            case Shape::Cmp: {
                const bool scalar_form = CmpIsScalar(*e);
                const bool use_l = scalar_form ? false : l;
                const std::string ctag = fmt::format("{}.imm{}", tag, ref.imm);
                {
                    CodeBuf b;
                    VexRR(b, e->pp, e->opcode, 0, 1, 2, use_l);
                    b.B(u8(ref.imm));
                    check(ctag + ".rr", b, ref.pair, want, Where::Ymm0);
                }
                {
                    CodeBuf b;
                    VexRM(b, e->pp, e->opcode, 0, 1, mb, use_l);
                    b.B(u8(ref.imm));
                    check(ctag + ".rm", b, ref.pair, want, Where::Ymm0);
                }
                break;
            }
            case Shape::Comis: {
                // The flags are read back with SETcc into the capture slot, in
                // the same order and at the same offsets the generator used:
                // CF, PF, ZF, OF, SF.
                const auto emit_tail = [&](CodeBuf& b) {
                    SetccMem(b, 0x92, MemOp{mout.disp + 0});  // setb  <- CF
                    SetccMem(b, 0x9A, MemOp{mout.disp + 1});  // setp  <- PF
                    SetccMem(b, 0x94, MemOp{mout.disp + 2});  // sete  <- ZF
                    SetccMem(b, 0x90, MemOp{mout.disp + 3});  // seto  <- OF
                    SetccMem(b, 0x98, MemOp{mout.disp + 4});  // sets  <- SF
                };
                {
                    CodeBuf b;
                    SeedFlags(b);
                    VexRR(b, e->pp, e->opcode, 1, kVexNoSrc1, 2, false);
                    emit_tail(b);
                    check(tag + ".rr", b, ref.pair, want, Where::Memory);
                }
                {
                    CodeBuf b;
                    SeedFlags(b);
                    VexRM(b, e->pp, e->opcode, 1, kVexNoSrc1, mb, false);
                    emit_tail(b);
                    check(tag + ".rm", b, ref.pair, want, Where::Memory);
                }
                break;
            }
            case Shape::Movmsk: {
                // Register source only: the memory form is #UD.  ModRM.reg is
                // the GPR (eax = 0) and ModRM.rm the vector source (ymm1), so
                // the two numbers DIFFER -- the case a decoder that confuses
                // reg with rm gets wrong.
                CodeBuf b;
                VexRR(b, e->pp, e->opcode, 0, kVexNoSrc1, 1, l);
                check(tag + ".rr", b, ref.pair, want, Where::Rax32);
                break;
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
    // nothing.  A scoped INFO stays live until the end of the test case.
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
    // Pinned so a coverage regression -- an opcode silently dropped from
    // avx_fp_ops.inc, or reference rows lost in regeneration -- cannot pass as
    // success.  Every row is run in two operand shapes except vmovmsk*, whose
    // memory form does not exist.
    size_t expected = 0;
    for (const auto& ref : kAvxFpRefs) {
        const Entry* e = nullptr;
        for (const auto& c : kEntries) {
            if (std::strcmp(c.name, ref.name) == 0) e = &c;
        }
        expected += (e != nullptr && e->shape == Shape::Movmsk) ? 1u : 2u;
    }
    CHECK(comparisons == expected);
    CHECK(std::size(kAvxFpRefs) == 1488u);
    // The ONE known deviation, pinned by exact count so it can neither grow
    // Pinned at zero: sqrt of a negative operand now returns the negative
    // "QNaN indefinite" that real hardware returns, on both backends. The fix
    // is in JitTranslator::EmitVecFUnary (translator_alu.cpp) and
    // Interpreter::RunVecFUnary (interp/interpreter.cpp) -- both were needed,
    // and fixing only one produced a JIT/interpreter divergence instead. It
    // also repairs the legacy SSE sqrt* forms, which share the IR opcode.
    // Kept as an assertion so a reintroduced exemption cannot pass unnoticed.
    CHECK(known_sqrt_nan_sign == 0u);
}
