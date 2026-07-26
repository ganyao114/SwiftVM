//
// Regression tests for the self-contained VEX decoder (vex_decoder.{h,cc}).
//
// Length is the property under the most pressure: the decoder owns instruction
// length for every VEX-prefixed instruction, and a wrong length does not merely
// mis-decode one instruction -- it desynchronizes every instruction after it in
// the block. Each case is therefore checked twice: against distorm (on the
// subset this distorm snapshot still decodes correctly) and against the case's
// own byte count.
//
// The truncation sweep matters just as much: the decoder reads attacker- or
// guest-controlled bytes at the end of a code page, so every prefix of every
// case must be REJECTED rather than read past.
//
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <vector>
#include "distorm.h"
#include "mnemonics.h"
#include "runtime/frontend/x86/vex_decoder.h"

namespace {

using namespace swift::x86;

// Catch2's expression decomposer rejects unparenthesised && / chained
// comparisons, so the condition is wrapped. The message arguments are folded
// into an INFO so a failure still says which case and which observed value.
#define VEXCHK(c, ...)                                                             \
    do {                                                                           \
        INFO(#c);                                                                  \
        REQUIRE((c));                                                              \
    } while (0)

int DistormLen(const unsigned char* c, int n) {
    _CodeInfo ci;
    std::memset(&ci, 0, sizeof(ci));
    ci.code = c; ci.codeLen = n; ci.codeOffset = 0x1000; ci.dt = Decode64Bits;
    _DInst d[2]; unsigned int u = 0;
    distorm_decompose(&ci, d, 2, &u);
    if (!u || d[0].opcode == I_UNDEFINED) return -1;
    return d[0].size;
}

struct Case { const char* name; std::vector<unsigned char> bytes; };

TEST_CASE("VEX decoder length and operand shape") {
    // Every encoding here was built by hand from the Intel manual's VEX layout.
    const std::vector<Case> cases = {
        // C5 form, reg-reg, no imm.  C5 F1 EF C2 = vpxor xmm0,xmm1,xmm2
        {"vpxor xmm0,xmm1,xmm2 (C5,rr)", {0xC5,0xF1,0xEF,0xC2}},
        // C5 form, L=1
        {"vpxor ymm0,ymm1,ymm2 (C5,rr,L1)", {0xC5,0xF5,0xEF,0xC2}},
        // C4 form, reg-reg
        {"vpxor xmm0,xmm1,xmm2 (C4,rr)", {0xC4,0xE1,0x71,0xEF,0xC2}},
        // mod=10 disp32 with base r13 (rm=101 needs B=1)
        {"vpxor xmm0,xmm1,[r13+0x100]", {0xC4,0xC1,0x71,0xEF,0x85,0x00,0x01,0x00,0x00}},
        // mod=01 disp8, base rax
        {"vpxor xmm0,xmm1,[rax+8]", {0xC4,0xE1,0x71,0xEF,0x40,0x08}},
        // mod=00, base rax, no disp
        {"vpxor xmm0,xmm1,[rax]", {0xC4,0xE1,0x71,0xEF,0x00}},
        // SIB: [rax + rcx*4], mod=00 rm=100
        {"vpxor xmm0,xmm1,[rax+rcx*4]", {0xC4,0xE1,0x71,0xEF,0x04,0x88}},
        // SIB with no base: mod=00 rm=100, sib base=101 -> disp32 only
        {"vpxor xmm0,xmm1,[rcx*4+0x10]", {0xC4,0xE1,0x71,0xEF,0x04,0x8D,0x10,0,0,0}},
        // RIP-relative: mod=00 rm=101
        {"vpxor xmm0,xmm1,[rip+0x20]", {0xC4,0xE1,0x71,0xEF,0x05,0x20,0,0,0}},
        // 0F3A map with imm8: vpalignr xmm0,xmm1,xmm2,1
        {"vpalignr xmm0,xmm1,xmm2,1", {0xC4,0xE3,0x71,0x0F,0xC2,0x01}},
        // 0F map with imm8: vpshufd xmm0,xmm1,0x1B
        {"vpshufd xmm0,xmm1,0x1b", {0xC4,0xE1,0x79,0x70,0xC1,0x1B}},
        // 0F38 map, no imm: vpmulld xmm0,xmm1,xmm2
        {"vpmulld xmm0,xmm1,xmm2", {0xC4,0xE2,0x71,0x40,0xC2}},
        // /is4 form: vpblendvb xmm0,xmm1,xmm2,xmm3  (imm8 high nibble = 3)
        {"vpblendvb xmm0,xmm1,xmm2,xmm3", {0xC4,0xE3,0x71,0x4C,0xC2,0x30}},
        // W=1: vmovq xmm0,rax  (66.0F.W1 6E)
        {"vmovq xmm0,rax (W=1)", {0xC4,0xE1,0xF9,0x6E,0xC0}},
        // extended regs: vpxor ymm8,ymm9,ymm10 -> needs R and B
        {"vpxor ymm8,ymm9,ymm10", {0xC4,0x41,0x35,0xEF,0xC2}},
    };
    for (const auto& c : cases) {
        const auto insn = DecodeVexInsn(c.bytes.data(), c.bytes.size());
        VEXCHK(insn.valid, "%s: decoder rejected", c.name);
        if (!insn.valid) continue;
        const int dl = DistormLen(c.bytes.data(), int(c.bytes.size()));
        if (dl > 0) {
            INFO("case=" << c.name << " ours=" << unsigned(insn.length) << " distorm=" << dl);
            CHECK(insn.length == dl);
        }
        // Every case above is exactly as long as the byte vector.
        INFO("case=" << c.name << " len=" << unsigned(insn.length) << " bytes=" << c.bytes.size());
        CHECK(insn.length == c.bytes.size());
    }
    {
        auto i = DecodeVexInsn(cases[0].bytes.data(), cases[0].bytes.size());
        VEXCHK(i.map == VexMap::Map0F && i.pp == VexPP::P66, "C5 map/pp");
        VEXCHK(!i.l && i.vvvv == 1 && i.vvvv_valid, "C5 vvvv=1");
        VEXCHK(i.reg == 0 && i.rm == 2 && i.RmIsRegister(), "C5 reg/rm");
    }
    {
        auto i = DecodeVexInsn(cases[1].bytes.data(), cases[1].bytes.size());
        VEXCHK(i.l, "L=1 not seen");
    }
    {
        auto i = DecodeVexInsn(cases[3].bytes.data(), cases[3].bytes.size());
        VEXCHK(!i.RmIsRegister(), "mem form");
        VEXCHK(i.base == 13 && !i.base_none, "base r13, got %u", i.base);
        VEXCHK(i.displacement == 0x100, "disp %d", i.displacement);
    }
    {
        auto i = DecodeVexInsn(cases[4].bytes.data(), cases[4].bytes.size());
        VEXCHK(i.displacement == 8, "disp8 %d", i.displacement);
    }
    {
        auto i = DecodeVexInsn(cases[6].bytes.data(), cases[6].bytes.size());
        VEXCHK(i.has_sib && !i.index_none, "sib");
        VEXCHK(i.index == 1 && i.scale == 4, "index=%u scale=%u", i.index, i.scale);
        VEXCHK(i.base == 0 && !i.base_none, "base rax");
    }
    {
        auto i = DecodeVexInsn(cases[7].bytes.data(), cases[7].bytes.size());
        VEXCHK(i.has_sib && i.base_none, "no-base sib");
        VEXCHK(i.index == 1 && i.scale == 4, "index/scale");
        VEXCHK(i.displacement == 0x10, "disp %d", i.displacement);
    }
    {
        auto i = DecodeVexInsn(cases[8].bytes.data(), cases[8].bytes.size());
        VEXCHK(i.rip_relative, "rip-rel");
        VEXCHK(i.displacement == 0x20, "disp %d", i.displacement);
    }
    {
        auto i = DecodeVexInsn(cases[9].bytes.data(), cases[9].bytes.size());
        VEXCHK(i.map == VexMap::Map0F3A && i.has_imm8 && i.imm8 == 1, "0F3A imm8");
    }
    {
        auto i = DecodeVexInsn(cases[12].bytes.data(), cases[12].bytes.size());
        VEXCHK(i.has_imm8 && i.is4_register == 3, "is4 reg=%u", i.is4_register);
    }
    {
        auto i = DecodeVexInsn(cases[13].bytes.data(), cases[13].bytes.size());
        VEXCHK(i.w, "W=1 not seen");
    }
    {
        auto i = DecodeVexInsn(cases[14].bytes.data(), cases[14].bytes.size());
        VEXCHK(i.reg == 8 && i.rm == 10 && i.vvvv == 9, "ext regs reg=%u rm=%u vvvv=%u", i.reg,
              i.rm, i.vvvv);
    }
    for (const auto& c : cases) {
        for (size_t n = 0; n < c.bytes.size(); ++n) {
            const auto i = DecodeVexInsn(c.bytes.data(), n);
            VEXCHK(!i.valid, "%s: truncated to %zu accepted", c.name, n);
        }
    }
    {
        const unsigned char sse[] = {0x66, 0x0F, 0xEF, 0xC1};  // pxor xmm0,xmm1
        VEXCHK(!HasVexPrefix(sse, sizeof sse), "legacy SSE flagged as VEX");
        VEXCHK(!DecodeVexInsn(sse, sizeof sse).valid, "legacy SSE decoded");
    }

}

}  // namespace
