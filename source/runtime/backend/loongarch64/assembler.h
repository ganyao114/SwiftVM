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

using Label = Xbyak_loongarch64::Label;
inline constexpr GPR x0{0}, ra{1}, sp{3};
inline constexpr GPR a0{4}, a1{5}, a2{6}, a3{7}, a4{8}, a5{9}, a6{10}, a7{11};
inline constexpr GPR t0{12}, t1{13}, t2{14}, t3{15}, t4{16}, t5{17}, t6{18};
inline constexpr GPR state{22}, packed_flags{31};
inline constexpr FPR ft0{0}, ft1{1}, ft2{2}, ft3{3}, ft4{4};
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
#define LA_BINARY(name, native)                                                                    \
    void name(GPR rd, GPR left, GPR right) { native(rd, left, right); }
    LA_BINARY(ADD, add_d)
    LA_BINARY(SUB, sub_d)
    LA_BINARY(AND, and_)
    LA_BINARY(OR, or_)
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

    void ANDI(GPR rd, GPR src, int imm) { andi(rd, src, static_cast<unsigned>(imm) & 4095u); }

    void ORI(GPR rd, GPR src, int imm) { ori(rd, src, static_cast<unsigned>(imm) & 4095u); }

    void XORI(GPR rd, GPR src, int imm) { xori(rd, src, static_cast<unsigned>(imm) & 4095u); }

    void SLLI(GPR rd, GPR src, unsigned bits) { slli_d(rd, src, bits); }

    void SRLI(GPR rd, GPR src, unsigned bits) { srli_d(rd, src, bits); }

    void SRAI(GPR rd, GPR src, unsigned bits) { srai_d(rd, src, bits); }

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

    void FENCE(FenceOrder, FenceOrder) { dbar(0); }

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
};
}  // namespace swift::runtime::backend::loongarch64
