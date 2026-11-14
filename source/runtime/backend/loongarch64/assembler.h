#pragma once

#include <cstdint>
#include <vector>
#include <xbyak_loongarch64/xbyak_loongarch64.h>

namespace swift::runtime::backend::loongarch64 {
// Architectural register identities. All temporary operands are explicit;
// the exact encoder never reserves a hidden GPR or changes register policy.
struct GPR {
    unsigned code{};

    constexpr explicit GPR(unsigned code = 0) : code(code) {}

    constexpr unsigned Index() const { return code; }

    operator Xbyak_loongarch64::XReg() const { return Xbyak_loongarch64::XReg{code}; }

    friend constexpr bool operator==(GPR, GPR) = default;
};

struct FPR {
    unsigned code{};

    constexpr explicit FPR(unsigned code = 0) : code(code) {}

    operator Xbyak_loongarch64::XReg() const { return Xbyak_loongarch64::XReg{code}; }
};

struct VR {
    unsigned code{};

    constexpr explicit VR(unsigned code = 0) : code(code) {}

    constexpr unsigned Index() const { return code; }

    operator Xbyak_loongarch64::VReg() const { return Xbyak_loongarch64::VReg{code}; }

    friend constexpr bool operator==(VR, VR) = default;
};

using Label = Xbyak_loongarch64::Label;
inline constexpr GPR x0{0}, ra{1}, sp{3};
inline constexpr GPR a0{4}, a1{5}, a2{6}, a3{7}, a4{8}, a5{9}, a6{10}, a7{11};
inline constexpr GPR t0{12}, t1{13}, t2{14}, t3{15}, t4{16}, t5{17}, t6{18}, t7{19}, t8{20};
inline constexpr GPR state{22}, packed_flags{31};
inline constexpr FPR ft0{0}, ft1{1}, ft2{2}, ft3{3}, ft4{4};
inline constexpr VR vr0{0}, vr1{1}, vr2{2}, vr3{3}, vr4{4}, vr5{5};
enum class Precision { S, D };
enum class RMode { RNE, RTZ, RDN, RUP, DYN };
enum class Ordering { AQ, RL, AQRL };
enum class FenceOrder { R, W, RW };

class Assembler : public Xbyak_loongarch64::CodeGenerator {
public:
    explicit Assembler(std::vector<std::uint32_t>& words) : CodeGenerator(words) {}

    Assembler(std::uint8_t* buffer, std::size_t bytes) : CodeGenerator(bytes, buffer) {}

    void Emit32(std::uint32_t word) { dd(word); }

    Assembler& GetCodeBuffer() { return *this; }

    void LI(GPR, std::uint64_t);

    void MV(GPR rd, GPR src) {
        if (rd != src) or_(rd, src, zero);
    }

    void NEG(GPR rd, GPR src) { sub_d(rd, zero, src); }

    void NOT(GPR rd, GPR src) { nor(rd, src, zero); }

    void NOP() { andi(zero, zero, 0); }

    void EBREAK() { break_(0); }

    void RET() { jirl(zero, ra, 0); }

    void JR(GPR target) { jirl(zero, target, 0); }

    void JALR(GPR rd, int offset, GPR target) { jirl(rd, target, offset); }

    void AUIPC(GPR rd, std::uint32_t high);

    void Bind(Label* target) { L(*target); }

    void J(Label* target) { b(*target); }

    void J(int offset) { b(offset); }

#define LA_BRANCH(name, native)                                                                    \
    void name(GPR lhs, GPR rhs, Label* target) { native(lhs, rhs, *target); }                      \
    void name(GPR lhs, GPR rhs, int offset) { native(lhs, rhs, offset); }
    LA_BRANCH(BEQ, beq)
    LA_BRANCH(BNE, bne)
    LA_BRANCH(BLT, blt)
    LA_BRANCH(BGE, bge)
    LA_BRANCH(BLTU, bltu)
    LA_BRANCH(BGEU, bgeu)
#undef LA_BRANCH

    void BEQZ(GPR src, Label* target) { beqz(src, *target); }
    void BEQZ(GPR src, int offset) { beqz(src, offset); }
    void BNEZ(GPR src, Label* target) { bnez(src, *target); }
    void BNEZ(GPR src, int offset) { bnez(src, offset); }

#define LA_BINARY(name, native)                                                                    \
    void name(GPR rd, GPR left, GPR right) { native(rd, left, right); }
    LA_BINARY(ADD, add_d)
    LA_BINARY(ADD_W, add_w)
    LA_BINARY(SUB, sub_d)
    LA_BINARY(SUB_W, sub_w)
    LA_BINARY(AND, and_)
    LA_BINARY(ANDN, andn)
    LA_BINARY(OR, or_)
    LA_BINARY(ORN, orn)
    LA_BINARY(XOR, xor_)
    LA_BINARY(SLT, slt)
    LA_BINARY(SLTU, sltu)
    LA_BINARY(SLL, sll_d)
    LA_BINARY(SRL, srl_d)
    LA_BINARY(SRA, sra_d)
    LA_BINARY(MUL, mul_d)
    LA_BINARY(MULH, mulh_d)
    LA_BINARY(MULHU, mulh_du)
    LA_BINARY(DIV, div_d)
    LA_BINARY(DIVU, div_du)
#undef LA_BINARY

    void ADDI(GPR rd, GPR src, int imm) { addi_d(rd, src, imm); }

    void ADDI_W(GPR rd, GPR src, int imm) { addi_w(rd, src, imm); }

    void SLTUI(GPR rd, GPR src, int imm) { sltui(rd, src, imm); }

    void EXT_W_B(GPR rd, GPR src) { ext_w_b(rd, src); }

    void EXT_W_H(GPR rd, GPR src) { ext_w_h(rd, src); }

    void ALSL_D(GPR rd, GPR shifted, GPR addend, unsigned shift) {
        alsl_d(rd, shifted, addend, shift);
    }

    void ANDI(GPR rd, GPR src, int imm) { andi(rd, src, static_cast<unsigned>(imm) & 4095u); }

    void ORI(GPR rd, GPR src, int imm) { ori(rd, src, static_cast<unsigned>(imm) & 4095u); }

    void XORI(GPR rd, GPR src, int imm) { xori(rd, src, static_cast<unsigned>(imm) & 4095u); }

    void SLLI(GPR rd, GPR src, unsigned bits) { slli_d(rd, src, bits); }

    void SRLI(GPR rd, GPR src, unsigned bits) { srli_d(rd, src, bits); }

    void SRAI(GPR rd, GPR src, unsigned bits) { srai_d(rd, src, bits); }

    void BSTRPICK_D(GPR rd, GPR src, unsigned msb, unsigned lsb) { bstrpick_d(rd, src, msb, lsb); }

    void BSTRPICK_W(GPR rd, GPR src, unsigned msb, unsigned lsb) { bstrpick_w(rd, src, msb, lsb); }

    void BSTRINS_D(GPR rd, GPR src, unsigned msb, unsigned lsb) { bstrins_d(rd, src, msb, lsb); }

    void BSTRINS_W(GPR rd, GPR src, unsigned msb, unsigned lsb) { bstrins_w(rd, src, msb, lsb); }

    void MASKEQZ(GPR rd, GPR src, GPR mask) { maskeqz(rd, src, mask); }

    void MASKNEZ(GPR rd, GPR src, GPR mask) { masknez(rd, src, mask); }

    void ROTRI_D(GPR rd, GPR src, unsigned bits) { rotri_d(rd, src, bits); }

    void ROTRI_W(GPR rd, GPR src, unsigned bits) { rotri_w(rd, src, bits); }

    void ROTRI_H(GPR rd, GPR src, unsigned bits) {
        Emit32(0x004c4000u | ((bits & 0xfu) << 10) | ((src.Index() & 0x1fu) << 5) |
               (rd.Index() & 0x1fu));
    }

    void ROTRI_B(GPR rd, GPR src, unsigned bits) {
        Emit32(0x004c2000u | ((bits & 0x7u) << 10) | ((src.Index() & 0x1fu) << 5) |
               (rd.Index() & 0x1fu));
    }

    void ROTR_D(GPR rd, GPR src, GPR count) { rotr_d(rd, src, count); }

    void ROTR_W(GPR rd, GPR src, GPR count) { rotr_w(rd, src, count); }

    void ROTR_H(GPR rd, GPR src, GPR count) {
        Emit32(0x001a8000u | ((count.Index() & 0x1fu) << 10) | ((src.Index() & 0x1fu) << 5) |
               (rd.Index() & 0x1fu));
    }

    void ROTR_B(GPR rd, GPR src, GPR count) {
        Emit32(0x001a0000u | ((count.Index() & 0x1fu) << 10) | ((src.Index() & 0x1fu) << 5) |
               (rd.Index() & 0x1fu));
    }

    void CRCC_W_B_W(GPR rd, GPR val, GPR crc) { crcc_w_b_w(rd, val, crc); }
    void CRCC_W_H_W(GPR rd, GPR val, GPR crc) { crcc_w_h_w(rd, val, crc); }
    void CRCC_W_W_W(GPR rd, GPR val, GPR crc) { crcc_w_w_w(rd, val, crc); }
    void CRCC_W_D_W(GPR rd, GPR val, GPR crc) { crcc_w_d_w(rd, val, crc); }

    // LoongArch LBT extension exact encodings (verified against Binutils 2.44 & 3A6000 silicon).
#define LA_LBT_X86_2R(name, word)                                                                  \
    void name(GPR rj, GPR rk) {                                                                    \
        Emit32((word) | ((rk.Index() & 0x1fu) << 10) | ((rj.Index() & 0x1fu) << 5));               \
    }
    LA_LBT_X86_2R(X86ADD_B, 0x003f0004u)
    LA_LBT_X86_2R(X86ADD_H, 0x003f0005u)
    LA_LBT_X86_2R(X86ADD_W, 0x003f0006u)
    LA_LBT_X86_2R(X86ADD_D, 0x003f0007u)
    LA_LBT_X86_2R(X86SUB_B, 0x003f0008u)
    LA_LBT_X86_2R(X86SUB_H, 0x003f0009u)
    LA_LBT_X86_2R(X86SUB_W, 0x003f000au)
    LA_LBT_X86_2R(X86SUB_D, 0x003f000bu)
    LA_LBT_X86_2R(X86ADC_B, 0x003f000cu)
    LA_LBT_X86_2R(X86ADC_H, 0x003f000du)
    LA_LBT_X86_2R(X86ADC_W, 0x003f000eu)
    LA_LBT_X86_2R(X86ADC_D, 0x003f000fu)
    LA_LBT_X86_2R(X86SBC_B, 0x003f0010u)
    LA_LBT_X86_2R(X86SBC_H, 0x003f0011u)
    LA_LBT_X86_2R(X86SBC_W, 0x003f0012u)
    LA_LBT_X86_2R(X86SBC_D, 0x003f0013u)
    LA_LBT_X86_2R(X86SLL_B, 0x003f0014u)
    LA_LBT_X86_2R(X86SLL_H, 0x003f0015u)
    LA_LBT_X86_2R(X86SLL_W, 0x003f0016u)
    LA_LBT_X86_2R(X86SLL_D, 0x003f0017u)
    LA_LBT_X86_2R(X86SRL_B, 0x003f0018u)
    LA_LBT_X86_2R(X86SRL_H, 0x003f0019u)
    LA_LBT_X86_2R(X86SRL_W, 0x003f001au)
    LA_LBT_X86_2R(X86SRL_D, 0x003f001bu)
    LA_LBT_X86_2R(X86SRA_B, 0x003f001cu)
    LA_LBT_X86_2R(X86SRA_H, 0x003f001du)
    LA_LBT_X86_2R(X86SRA_W, 0x003f001eu)
    LA_LBT_X86_2R(X86SRA_D, 0x003f001fu)
    LA_LBT_X86_2R(X86ROTR_B, 0x003f8000u)
    LA_LBT_X86_2R(X86ROTR_H, 0x003f8001u)
    LA_LBT_X86_2R(X86ROTR_D, 0x003f8002u)
    LA_LBT_X86_2R(X86ROTR_W, 0x003f8003u)
    LA_LBT_X86_2R(X86ROTL_B, 0x003f8004u)
    LA_LBT_X86_2R(X86ROTL_H, 0x003f8005u)
    LA_LBT_X86_2R(X86ROTL_W, 0x003f8006u)
    LA_LBT_X86_2R(X86ROTL_D, 0x003f8007u)
    LA_LBT_X86_2R(X86AND_B, 0x003f8010u)
    LA_LBT_X86_2R(X86AND_H, 0x003f8011u)
    LA_LBT_X86_2R(X86AND_W, 0x003f8012u)
    LA_LBT_X86_2R(X86AND_D, 0x003f8013u)
    LA_LBT_X86_2R(X86OR_B, 0x003f8014u)
    LA_LBT_X86_2R(X86OR_H, 0x003f8015u)
    LA_LBT_X86_2R(X86OR_W, 0x003f8016u)
    LA_LBT_X86_2R(X86OR_D, 0x003f8017u)
    LA_LBT_X86_2R(X86XOR_B, 0x003f8018u)
    LA_LBT_X86_2R(X86XOR_H, 0x003f8019u)
    LA_LBT_X86_2R(X86XOR_W, 0x003f801au)
    LA_LBT_X86_2R(X86XOR_D, 0x003f801bu)
#undef LA_LBT_X86_2R

#define LA_LBT_X86_1R(name, word)                                                                  \
    void name(GPR rj) { Emit32((word) | ((rj.Index() & 0x1fu) << 5)); }
    LA_LBT_X86_1R(X86INC_B, 0x00008000u)
    LA_LBT_X86_1R(X86INC_H, 0x00008001u)
    LA_LBT_X86_1R(X86INC_W, 0x00008002u)
    LA_LBT_X86_1R(X86INC_D, 0x00008003u)
    LA_LBT_X86_1R(X86DEC_B, 0x00008004u)
    LA_LBT_X86_1R(X86DEC_H, 0x00008005u)
    LA_LBT_X86_1R(X86DEC_W, 0x00008006u)
    LA_LBT_X86_1R(X86DEC_D, 0x00008007u)
#undef LA_LBT_X86_1R

#define LA_LBT_3R(name, word)                                                                      \
    void name(GPR rd, GPR rj, GPR rk) {                                                            \
        Emit32((word) | ((rk.Index() & 0x1fu) << 10) | ((rj.Index() & 0x1fu) << 5) |               \
               (rd.Index() & 0x1fu));                                                              \
    }
    LA_LBT_3R(ADC_B, 0x00300000u)
    LA_LBT_3R(ADC_H, 0x00308000u)
    LA_LBT_3R(ADC_W, 0x00310000u)
    LA_LBT_3R(ADC_D, 0x00318000u)
    LA_LBT_3R(SBC_B, 0x00320000u)
    LA_LBT_3R(SBC_H, 0x00328000u)
    LA_LBT_3R(SBC_W, 0x00330000u)
    LA_LBT_3R(SBC_D, 0x00338000u)
#undef LA_LBT_3R

#define LA_LBT_WIDTH_2R(stem)                                                                      \
    void stem(unsigned bits, GPR rj, GPR rk) {                                                     \
        switch (bits) {                                                                            \
            case 8:                                                                                \
                stem##_B(rj, rk);                                                                  \
                break;                                                                             \
            case 16:                                                                               \
                stem##_H(rj, rk);                                                                  \
                break;                                                                             \
            case 32:                                                                               \
                stem##_W(rj, rk);                                                                  \
                break;                                                                             \
            default:                                                                               \
                stem##_D(rj, rk);                                                                  \
                break;                                                                             \
        }                                                                                          \
    }
    LA_LBT_WIDTH_2R(X86ADD)
    LA_LBT_WIDTH_2R(X86SUB)
    LA_LBT_WIDTH_2R(X86ADC)
    LA_LBT_WIDTH_2R(X86SBC)
    LA_LBT_WIDTH_2R(X86AND)
    LA_LBT_WIDTH_2R(X86OR)
    LA_LBT_WIDTH_2R(X86XOR)
    LA_LBT_WIDTH_2R(X86SLL)
    LA_LBT_WIDTH_2R(X86SRL)
    LA_LBT_WIDTH_2R(X86SRA)
    LA_LBT_WIDTH_2R(X86ROTR)
    LA_LBT_WIDTH_2R(X86ROTL)
#undef LA_LBT_WIDTH_2R

#define LA_LBT_WIDTH_1R(stem)                                                                      \
    void stem(unsigned bits, GPR rj) {                                                             \
        switch (bits) {                                                                            \
            case 8:                                                                                \
                stem##_B(rj);                                                                      \
                break;                                                                             \
            case 16:                                                                               \
                stem##_H(rj);                                                                      \
                break;                                                                             \
            case 32:                                                                               \
                stem##_W(rj);                                                                      \
                break;                                                                             \
            default:                                                                               \
                stem##_D(rj);                                                                      \
                break;                                                                             \
        }                                                                                          \
    }
    LA_LBT_WIDTH_1R(X86INC)
    LA_LBT_WIDTH_1R(X86DEC)
#undef LA_LBT_WIDTH_1R

#define LA_LBT_WIDTH_3R(stem)                                                                      \
    void stem(unsigned bits, GPR rd, GPR rj, GPR rk) {                                             \
        switch (bits) {                                                                            \
            case 8:                                                                                \
                stem##_B(rd, rj, rk);                                                              \
                break;                                                                             \
            case 16:                                                                               \
                stem##_H(rd, rj, rk);                                                              \
                break;                                                                             \
            case 32:                                                                               \
                stem##_W(rd, rj, rk);                                                              \
                break;                                                                             \
            default:                                                                               \
                stem##_D(rd, rj, rk);                                                              \
                break;                                                                             \
        }                                                                                          \
    }
    LA_LBT_WIDTH_3R(ADC)
    LA_LBT_WIDTH_3R(SBC)
#undef LA_LBT_WIDTH_3R

    void X86MFFLAG(GPR rd, unsigned mask) {
        Emit32(0x005c0000u | ((mask & 0xffu) << 10) | (rd.Index() & 0x1fu));
    }

    void X86MTFLAG(GPR rj, unsigned mask) {
        Emit32(0x005c0020u | ((mask & 0xffu) << 10) | (rj.Index() & 0x1fu));
    }

    void SETX86J(GPR rd, unsigned cond) {
        Emit32(0x00368000u | ((cond & 0xfu) << 10) | (rd.Index() & 0x1fu));
    }

#define LA_LBT_X86_IMM(stem, subop)                                                                \
    void stem##_B(GPR rj, unsigned imm) {                                                          \
        Emit32(0x00542000u | ((imm & 0x7u) << 10) | ((rj.Index() & 0x1fu) << 5) | (subop));        \
    }                                                                                              \
    void stem##_H(GPR rj, unsigned imm) {                                                          \
        Emit32(0x00544000u | ((imm & 0xfu) << 10) | ((rj.Index() & 0x1fu) << 5) | ((subop) + 1));  \
    }                                                                                              \
    void stem##_W(GPR rj, unsigned imm) {                                                          \
        Emit32(0x00548000u | ((imm & 0x1fu) << 10) | ((rj.Index() & 0x1fu) << 5) | ((subop) + 2)); \
    }                                                                                              \
    void stem##_D(GPR rj, unsigned imm) {                                                          \
        Emit32(0x00550000u | ((imm & 0x3fu) << 10) | ((rj.Index() & 0x1fu) << 5) | ((subop) + 3)); \
    }
    LA_LBT_X86_IMM(X86SLLI, 0u)
    LA_LBT_X86_IMM(X86SRLI, 4u)
    LA_LBT_X86_IMM(X86SRAI, 8u)
    LA_LBT_X86_IMM(X86ROTRI, 12u)
    LA_LBT_X86_IMM(X86ROTLI, 20u)
#undef LA_LBT_X86_IMM

    void SEQZ(GPR rd, GPR src) { sltui(rd, src, 1); }

    void SNEZ(GPR rd, GPR src) { sltu(rd, zero, src); }

#define LA_LOAD(name, native)                                                                      \
    void name(GPR rd, int offset, GPR base) { native(rd, base, offset); }
    LA_LOAD(LBU, ld_bu)
    LA_LOAD(LHU, ld_hu)
    LA_LOAD(LWU, ld_wu)
    LA_LOAD(LD, ld_d)
    LA_LOAD(SB, st_b)
    LA_LOAD(SH, st_h)
    LA_LOAD(SW, st_w)
    LA_LOAD(SD, st_d)
#undef LA_LOAD

    void FENCE(FenceOrder pred, FenceOrder succ) {
        // LoongArch v1.10 / LA664 fine-grained ordering hints (backward-compatible
        // with v1.00 cores, which treat non-zero hints identically to dbar 0).
        if (pred == FenceOrder::R && succ == FenceOrder::RW) {
            dbar(0x14);  // Load-Acquire: order prior R before subsequent R/W
        } else if (pred == FenceOrder::RW && succ == FenceOrder::W) {
            dbar(0x12);  // Store-Release: order prior R/W before subsequent W
        } else if (pred == FenceOrder::R && succ == FenceOrder::R) {
            dbar(0x15);  // Load-Load ordering only
        } else if (pred == FenceOrder::W && succ == FenceOrder::W) {
            dbar(0x1a);  // Store-Store ordering only
        } else {
            dbar(0x10);  // Full RW->RW memory ordering without pipeline sync drain
        }
    }

    void LR_W(Ordering, GPR rd, GPR address) { ll_w(rd, address, 0); }

    void LR_D(Ordering, GPR rd, GPR address) { ll_d(rd, address, 0); }

    void SC_W(Ordering, GPR status, GPR value, GPR address) {
        MV(status, value);
        sc_w(status, address, 0);
        XORI(status, status, 1);
    }

    void SC_D(Ordering, GPR status, GPR value, GPR address) {
        MV(status, value);
        sc_d(status, address, 0);
        XORI(status, status, 1);
    }

#define LA_AMO(name, native)                                                                       \
    void name(Ordering, GPR rd, GPR value, GPR address) { native(rd, value, address); }
    LA_AMO(AMOSWAP_W, amswap_w)
    LA_AMO(AMOSWAP_D, amswap_d)
    LA_AMO(AMOADD_W, amadd_w)
    LA_AMO(AMOADD_D, amadd_d)
    LA_AMO(AMOAND_W, amand_w)
    LA_AMO(AMOAND_D, amand_d)
    LA_AMO(AMOOR_W, amor_w)
    LA_AMO(AMOOR_D, amor_d)
    LA_AMO(AMOXOR_W, amxor_w)
    LA_AMO(AMOXOR_D, amxor_d)
#undef LA_AMO

    void FRCSR(GPR rd) { movfcsr2gr(rd, Xbyak_loongarch64::XReg{0}); }

    void FSCSR(GPR src) { movgr2fcsr(Xbyak_loongarch64::XReg{0}, src); }

    void FMV_W_X(FPR rd, GPR src) { movgr2fr_w(rd, src); }

    void FMV_D_X(FPR rd, GPR src) { movgr2fr_d(rd, src); }

    void FMV_X_W(GPR rd, FPR src) { movfr2gr_s(rd, src); }

    void FMV_X_D(GPR rd, FPR src) { movfr2gr_d(rd, src); }

#define LA_FP_BINARY(name, native)                                                                 \
    void name(FPR rd, FPR lhs, FPR rhs, Precision bits, RMode) {                                   \
        if (bits == Precision::S)                                                                  \
            native##_s(rd, lhs, rhs);                                                              \
        else                                                                                       \
            native##_d(rd, lhs, rhs);                                                              \
    }
    LA_FP_BINARY(FADD, fadd)
    LA_FP_BINARY(FSUB, fsub)
    LA_FP_BINARY(FMUL, fmul)
    LA_FP_BINARY(FDIV, fdiv)
#undef LA_FP_BINARY
#define LA_FP_FUSED(name, native)                                                                  \
    void name(FPR rd, FPR lhs, FPR rhs, FPR addend, Precision bits, RMode) {                       \
        if (bits == Precision::S)                                                                  \
            native##_s(rd, lhs, rhs, addend);                                                      \
        else                                                                                       \
            native##_d(rd, lhs, rhs, addend);                                                      \
    }
    LA_FP_FUSED(FMADD, fmadd)
    LA_FP_FUSED(FMSUB, fmsub)
    LA_FP_FUSED(FNMADD, fnmadd)
    LA_FP_FUSED(FNMSUB, fnmsub)
#undef LA_FP_FUSED

    void FSQRT(FPR rd, FPR src, Precision bits, RMode) {
        if (bits == Precision::S)
            fsqrt_s(rd, src);
        else
            fsqrt_d(rd, src);
    }

    void FSGNJ(FPR rd, FPR magnitude, FPR sign, Precision bits) {
        if (bits == Precision::S)
            fcopysign_s(rd, magnitude, sign);
        else
            fcopysign_d(rd, magnitude, sign);
    }

    void FCLASS(GPR rd, FPR src, Precision bits, FPR temporary);
    void FEQ(GPR rd, FPR lhs, FPR rhs, Precision bits, unsigned condition);
    void FLT(GPR rd, FPR lhs, FPR rhs, Precision bits, unsigned condition);
    void FloatToInteger(GPR, FPR, unsigned integer_bits, Precision, RMode, FPR temporary);
    void IntegerToFloat(FPR, GPR, unsigned integer_bits, Precision, FPR temporary);
#define LA_FP_TO_INT(integer_name, integer_bits, float_name, float_precision)                      \
    void FCVT_##integer_name##_##float_name(GPR rd, FPR src, RMode rounding, FPR temporary) {      \
        FloatToInteger(rd, src, integer_bits, Precision::float_precision, rounding, temporary);    \
    }
    LA_FP_TO_INT(W, 32, S, S)
    LA_FP_TO_INT(W, 32, D, D)
    LA_FP_TO_INT(L, 64, S, S)
    LA_FP_TO_INT(L, 64, D, D)
#undef LA_FP_TO_INT
#define LA_INT_TO_FP(float_name, float_precision, integer_name, integer_bits)                      \
    void FCVT_##float_name##_##integer_name(FPR rd, GPR src, RMode, FPR temporary) {               \
        IntegerToFloat(rd, src, integer_bits, Precision::float_precision, temporary);              \
    }
    LA_INT_TO_FP(S, S, W, 32)
    LA_INT_TO_FP(D, D, W, 32)
    LA_INT_TO_FP(S, S, L, 64)
    LA_INT_TO_FP(D, D, L, 64)
#undef LA_INT_TO_FP

    void FCVT_D_S(FPR rd, FPR src, RMode) { fcvt_d_s(rd, src); }

    void FCVT_S_D(FPR rd, FPR src, RMode) { fcvt_s_d(rd, src); }

    // -------------------------------------------------------------------------
    // LSX 128-bit SIMD instructions (verified against GNU Binutils 2.44)
    // -------------------------------------------------------------------------
    void VLD(VR vd, GPR rj, int si12) { vld(vd, rj, si12); }
    void VST(VR vd, GPR rj, int si12) { vst(vd, rj, si12); }
    void VINSGR2VR_D(VR vd, GPR rj, unsigned idx) { vinsgr2vr_d(vd, rj, idx); }
    void VPICKVE2GR_DU(GPR rd, VR vj, unsigned idx) { vpickve2gr_du(rd, vj, idx); }

    void VREPLGR2VR(unsigned bits, VR vd, GPR rj) {
        switch (bits) {
            case 8:
                vreplgr2vr_b(vd, rj);
                break;
            case 16:
                vreplgr2vr_h(vd, rj);
                break;
            case 32:
                vreplgr2vr_w(vd, rj);
                break;
            default:
                vreplgr2vr_d(vd, rj);
                break;
        }
    }

    void VAND_V(VR vd, VR vj, VR vk) { vand_v(vd, vj, vk); }
    void VOR_V(VR vd, VR vj, VR vk) { vor_v(vd, vj, vk); }
    void VXOR_V(VR vd, VR vj, VR vk) { vxor_v(vd, vj, vk); }
    // Note: LoongArch vandn.v computes (~vj) & vk.
    void VANDN_V(VR vd, VR vj, VR vk) { vandn_v(vd, vj, vk); }
    void VBITSEL_V(VR vd, VR vj, VR vk, VR va) { vbitsel_v(vd, vj, vk, va); }
    void VBSLL_V(VR vd, VR vj, unsigned bytes) { vbsll_v(vd, vj, bytes); }
    void VBSRL_V(VR vd, VR vj, unsigned bytes) { vbsrl_v(vd, vj, bytes); }
    void VSHUF4I_H(VR vd, VR vj, unsigned imm8) { vshuf4i_h(vd, vj, imm8); }
    void VSHUF4I_W(VR vd, VR vj, unsigned imm8) { vshuf4i_w(vd, vj, imm8); }

    void VANDI_B(VR vd, VR vj, unsigned imm8) {
        Emit32(0x73d00000u | ((imm8 & 0xffu) << 10) | (vj.Index() << 5) | vd.Index());
    }

    void VSHUF_B(VR vd, VR vj, VR vk, VR va) {
        Emit32(0x0d500000u | (va.Index() << 15) | (vk.Index() << 10) | (vj.Index() << 5) |
               vd.Index());
    }

    void VPCNT_D(VR vd, VR vj) { Emit32(0x729c2c00u | (vj.Index() << 5) | vd.Index()); }

    void VMSKLTZ(unsigned bits, VR vd, VR vj) {
        const std::uint32_t op = bits == 8    ? 0x729c4000u
                                 : bits == 16 ? 0x729c4400u
                                 : bits == 32 ? 0x729c4800u
                                              : 0x729c4c00u;
        Emit32(op | (vj.Index() << 5) | vd.Index());
    }

    void VSLTI_B(VR vd, VR vj, int si5) {
        Emit32(0x72860000u | ((static_cast<unsigned>(si5) & 0x1fu) << 10) | (vj.Index() << 5) |
               vd.Index());
    }

    void VSLTI_BU(VR vd, VR vj, unsigned ui5) {
        Emit32(0x72880000u | ((ui5 & 0x1fu) << 10) | (vj.Index() << 5) | vd.Index());
    }

    void VSLTI_U(unsigned bits, VR vd, VR vj, unsigned ui5) {
        const std::uint32_t op = bits == 8    ? 0x72880000u
                                 : bits == 16 ? 0x72888000u
                                 : bits == 32 ? 0x72890000u
                                              : 0x72898000u;
        Emit32(op | ((ui5 & 0x1fu) << 10) | (vj.Index() << 5) | vd.Index());
    }

    void EmitLSX3R(std::uint32_t op, VR vd, VR vj, VR vk) {
        Emit32(op | (vk.Index() << 10) | (vj.Index() << 5) | vd.Index());
    }

#define LA_LSX_WIDTH_XBYAK(name, stem)                                                             \
    void name(unsigned bits, VR vd, VR vj, VR vk) {                                                \
        switch (bits) {                                                                            \
            case 8:                                                                                \
                stem##_b(vd, vj, vk);                                                              \
                break;                                                                             \
            case 16:                                                                               \
                stem##_h(vd, vj, vk);                                                              \
                break;                                                                             \
            case 32:                                                                               \
                stem##_w(vd, vj, vk);                                                              \
                break;                                                                             \
            default:                                                                               \
                stem##_d(vd, vj, vk);                                                              \
                break;                                                                             \
        }                                                                                          \
    }
    LA_LSX_WIDTH_XBYAK(VADD, vadd)
    LA_LSX_WIDTH_XBYAK(VMUL, vmul)
    LA_LSX_WIDTH_XBYAK(VSEQ, vseq)
    LA_LSX_WIDTH_XBYAK(VILVL, vilvl)
    LA_LSX_WIDTH_XBYAK(VILVH, vilvh)
    LA_LSX_WIDTH_XBYAK(VPICKEV, vpickev)
    LA_LSX_WIDTH_XBYAK(VPICKOD, vpickod)
#undef LA_LSX_WIDTH_XBYAK

    void VMIN(unsigned bits, bool is_signed, VR vd, VR vj, VR vk) {
        if (is_signed) {
            if (bits == 8)
                vmin_b(vd, vj, vk);
            else if (bits == 16)
                vmin_h(vd, vj, vk);
            else if (bits == 32)
                vmin_w(vd, vj, vk);
            else
                vmin_d(vd, vj, vk);
        } else {
            if (bits == 8)
                vmin_bu(vd, vj, vk);
            else if (bits == 16)
                vmin_hu(vd, vj, vk);
            else if (bits == 32)
                vmin_wu(vd, vj, vk);
            else
                vmin_du(vd, vj, vk);
        }
    }

    void VMAX(unsigned bits, bool is_signed, VR vd, VR vj, VR vk) {
        if (is_signed) {
            if (bits == 8)
                vmax_b(vd, vj, vk);
            else if (bits == 16)
                vmax_h(vd, vj, vk);
            else if (bits == 32)
                vmax_w(vd, vj, vk);
            else
                vmax_d(vd, vj, vk);
        } else {
            if (bits == 8)
                vmax_bu(vd, vj, vk);
            else if (bits == 16)
                vmax_hu(vd, vj, vk);
            else if (bits == 32)
                vmax_wu(vd, vj, vk);
            else
                vmax_du(vd, vj, vk);
        }
    }

    void VSUB(unsigned bits, VR vd, VR vj, VR vk) {
        const std::uint32_t op = bits == 8    ? 0x700c0000u
                                 : bits == 16 ? 0x700c8000u
                                 : bits == 32 ? 0x700d0000u
                                              : 0x700d8000u;
        EmitLSX3R(op, vd, vj, vk);
    }

    void VSLT(unsigned bits, bool is_signed, VR vd, VR vj, VR vk) {
        const std::uint32_t base = is_signed ? 0x70060000u : 0x70080000u;
        const std::uint32_t step = bits == 8 ? 0u : bits == 16 ? 0x8000u : bits == 32 ? 0x10000u : 0x18000u;
        EmitLSX3R(base + step, vd, vj, vk);
    }

    void VSADD(unsigned bits, bool is_signed, VR vd, VR vj, VR vk) {
        const std::uint32_t base = is_signed ? 0x70460000u : 0x704a0000u;
        const std::uint32_t step = bits == 8 ? 0u : bits == 16 ? 0x8000u : bits == 32 ? 0x10000u : 0x18000u;
        EmitLSX3R(base + step, vd, vj, vk);
    }

    void VSSUB(unsigned bits, bool is_signed, VR vd, VR vj, VR vk) {
        const std::uint32_t base = is_signed ? 0x70480000u : 0x704c0000u;
        const std::uint32_t step = bits == 8 ? 0u : bits == 16 ? 0x8000u : bits == 32 ? 0x10000u : 0x18000u;
        EmitLSX3R(base + step, vd, vj, vk);
    }

    void VAVGR_U(unsigned bits, VR vd, VR vj, VR vk) {
        const std::uint32_t step = bits == 8 ? 0u : bits == 16 ? 0x8000u : bits == 32 ? 0x10000u : 0x18000u;
        EmitLSX3R(0x706a0000u + step, vd, vj, vk);
    }

    void VMUH_H(VR vd, VR vj, VR vk) { EmitLSX3R(0x70868000u, vd, vj, vk); }
    void VMUH_HU(VR vd, VR vj, VR vk) { EmitLSX3R(0x70888000u, vd, vj, vk); }
    void VMADDWOD_W_H(VR vd, VR vj, VR vk) { EmitLSX3R(0x70ae8000u, vd, vj, vk); }
    void VABSD_BU(VR vd, VR vj, VR vk) { EmitLSX3R(0x70620000u, vd, vj, vk); }
    void VHADDW_HU_BU(VR vd, VR vj, VR vk) { EmitLSX3R(0x70580000u, vd, vj, vk); }
    void VHADDW_WU_HU(VR vd, VR vj, VR vk) { EmitLSX3R(0x70588000u, vd, vj, vk); }
    void VHADDW_DU_WU(VR vd, VR vj, VR vk) { EmitLSX3R(0x70590000u, vd, vj, vk); }

    void VMULWEV(unsigned src_bits, bool is_signed, VR vd, VR vj, VR vk) {
        const std::uint32_t base = is_signed ? 0x70900000u : 0x70980000u;
        const std::uint32_t step = src_bits == 8 ? 0u : src_bits == 16 ? 0x8000u : 0x10000u;
        EmitLSX3R(base + step, vd, vj, vk);
    }

    void VSLLWIL_ZERO(unsigned src_bits, bool is_signed, VR vd, VR vj) {
        const std::uint32_t base = is_signed ? 0x73080000u : 0x730c0000u;
        const std::uint32_t tag = src_bits == 8 ? 0x2000u : src_bits == 16 ? 0x4000u : 0x8000u;
        Emit32(base | tag | (vj.Index() << 5) | vd.Index());
    }

    void VSSRANI_ZERO(unsigned src_bits, bool unsigned_result, VR vd, VR vj) {
        const std::uint32_t base = unsigned_result ? 0x73640000u : 0x73600000u;
        const std::uint32_t tag = src_bits == 16 ? 0x4000u : src_bits == 32 ? 0x8000u : 0x10000u;
        Emit32(base | tag | (vj.Index() << 5) | vd.Index());
    }

#define LA_LSX_SHIFT_IMM(name, stem)                                                               \
    void name(unsigned bits, VR vd, VR vj, unsigned imm) {                                         \
        switch (bits) {                                                                            \
            case 8:                                                                                \
                stem##_b(vd, vj, imm);                                                             \
                break;                                                                             \
            case 16:                                                                               \
                stem##_h(vd, vj, imm);                                                             \
                break;                                                                             \
            case 32:                                                                               \
                stem##_w(vd, vj, imm);                                                             \
                break;                                                                             \
            default:                                                                               \
                stem##_d(vd, vj, imm);                                                             \
                break;                                                                             \
        }                                                                                          \
    }
    LA_LSX_SHIFT_IMM(VSLLI, vslli)
    LA_LSX_SHIFT_IMM(VSRLI, vsrli)
    LA_LSX_SHIFT_IMM(VSRAI, vsrai)
#undef LA_LSX_SHIFT_IMM

    void VSLL(unsigned bits, VR vd, VR vj, VR vk) {
        const std::uint32_t step = bits == 8 ? 0u : bits == 16 ? 0x8000u : bits == 32 ? 0x10000u : 0x18000u;
        EmitLSX3R(0x70e80000u + step, vd, vj, vk);
    }

    void VSRL(unsigned bits, VR vd, VR vj, VR vk) {
        const std::uint32_t step = bits == 8 ? 0u : bits == 16 ? 0x8000u : bits == 32 ? 0x10000u : 0x18000u;
        EmitLSX3R(0x70ea0000u + step, vd, vj, vk);
    }

    void VSRA(unsigned bits, VR vd, VR vj, VR vk) {
        const std::uint32_t step = bits == 8 ? 0u : bits == 16 ? 0x8000u : bits == 32 ? 0x10000u : 0x18000u;
        EmitLSX3R(0x70ec0000u + step, vd, vj, vk);
    }
};
}  // namespace swift::runtime::backend::loongarch64
