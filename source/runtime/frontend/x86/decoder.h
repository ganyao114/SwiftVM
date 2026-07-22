//
// Created by SwiftGan on 2021/1/1.
//

#pragma once

#include "distorm.h"
#include "mnemonics.h"
#include "cpu.h"
#include "runtime/include/config.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/backend/context.h"

namespace swift::x86 {

// Guest->host address bias used by host helpers (rep movs/stos) that execute
// with raw guest pointers. Installed by the embedding translator when guest
// addresses are virtualized (memory_base); 0 = identity.
void SetGuestMemBias(u64 bias);
[[nodiscard]] u64 GetGuestMemBias();

using VAddr = u64;
using namespace swift::runtime;

enum class Cond : u8 {
    EQ = 0, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL, NV,
    AT, AE, BT, BE, SN, NS, PA, NP, HS = CS, LO = CC, OF = VS, NO = VC
};

DECLARE_ENUM_FLAG_OPERATORS(Cond)

struct X86RegInfo {
    enum Index : u8 {
        Rax = 0,
        Rcx,
        Rdx,
        Rbx,
        Rsp,
        Rbp,
        Rsi,
        Rdi,
        R8,
        R9,
        R10,
        R11,
        R12,
        R13,
        R14,
        R15,
        ES,
        CS,
        SS,
        DS,
        FS,
        GS,
        Rip,
        Xmm0,
        Xmm1,
        Xmm2,
        Xmm3,
        Xmm4,
        Xmm5,
        Xmm6,
        Xmm7,
        Xmm8,
        Xmm9,
        Xmm10,
        Xmm11,
        Xmm12,
        Xmm13,
        Xmm14,
        Xmm15,
        Ymm0,
        Ymm1,
        Ymm2,
        Ymm3,
        Ymm4,
        Ymm5,
        Ymm6,
        Ymm7,
        Ymm8,
        Ymm9,
        Ymm10,
        Ymm11,
        Ymm12,
        Ymm13,
        Ymm14,
        Ymm15
    };

    u8 code;
    Index index;
    ir::ValueType type;
    bool high;
};

constexpr X86RegInfo x86_regs_table[] = {
        {_RegisterType::R_RAX, X86RegInfo::Rax, ir::ValueType::U64, false},
        {_RegisterType::R_RCX, X86RegInfo::Rcx, ir::ValueType::U64, false},
        {_RegisterType::R_RDX, X86RegInfo::Rdx, ir::ValueType::U64, false},
        {_RegisterType::R_RBX, X86RegInfo::Rbx, ir::ValueType::U64, false},
        {_RegisterType::R_RSP, X86RegInfo::Rsp, ir::ValueType::U64, false},
        {_RegisterType::R_RBP, X86RegInfo::Rbp, ir::ValueType::U64, false},
        {_RegisterType::R_RSI, X86RegInfo::Rsi, ir::ValueType::U64, false},
        {_RegisterType::R_RDI, X86RegInfo::Rdi, ir::ValueType::U64, false},
        {_RegisterType::R_R8, X86RegInfo::R8, ir::ValueType::U64, false},
        {_RegisterType::R_R9, X86RegInfo::R9, ir::ValueType::U64, false},
        {_RegisterType::R_R10, X86RegInfo::R10, ir::ValueType::U64, false},
        {_RegisterType::R_R11, X86RegInfo::R11, ir::ValueType::U64, false},
        {_RegisterType::R_R12, X86RegInfo::R12, ir::ValueType::U64, false},
        {_RegisterType::R_R13, X86RegInfo::R13, ir::ValueType::U64, false},
        {_RegisterType::R_R14, X86RegInfo::R14, ir::ValueType::U64, false},
        {_RegisterType::R_R15, X86RegInfo::R15, ir::ValueType::U64, false},
        {_RegisterType::R_EAX, X86RegInfo::Rax, ir::ValueType::U32, false},
        {_RegisterType::R_ECX, X86RegInfo::Rcx, ir::ValueType::U32, false},
        {_RegisterType::R_EDX, X86RegInfo::Rdx, ir::ValueType::U32, false},
        {_RegisterType::R_EBX, X86RegInfo::Rbx, ir::ValueType::U32, false},
        {_RegisterType::R_ESP, X86RegInfo::Rsp, ir::ValueType::U32, false},
        {_RegisterType::R_EBP, X86RegInfo::Rbp, ir::ValueType::U32, false},
        {_RegisterType::R_ESI, X86RegInfo::Rsi, ir::ValueType::U32, false},
        {_RegisterType::R_EDI, X86RegInfo::Rdi, ir::ValueType::U32, false},
        {_RegisterType::R_R8D, X86RegInfo::R8, ir::ValueType::U32, false},
        {_RegisterType::R_R9D, X86RegInfo::R9, ir::ValueType::U32, false},
        {_RegisterType::R_R10D, X86RegInfo::R10, ir::ValueType::U32, false},
        {_RegisterType::R_R11D, X86RegInfo::R11, ir::ValueType::U32, false},
        {_RegisterType::R_R12D, X86RegInfo::R12, ir::ValueType::U32, false},
        {_RegisterType::R_R13D, X86RegInfo::R13, ir::ValueType::U32, false},
        {_RegisterType::R_R14D, X86RegInfo::R14, ir::ValueType::U32, false},
        {_RegisterType::R_R15D, X86RegInfo::R15, ir::ValueType::U32, false},
        {_RegisterType::R_AX, X86RegInfo::Rax, ir::ValueType::U16, false},
        {_RegisterType::R_CX, X86RegInfo::Rcx, ir::ValueType::U16, false},
        {_RegisterType::R_DX, X86RegInfo::Rdx, ir::ValueType::U16, false},
        {_RegisterType::R_BX, X86RegInfo::Rbx, ir::ValueType::U16, false},
        {_RegisterType::R_SP, X86RegInfo::Rsp, ir::ValueType::U16, false},
        {_RegisterType::R_BP, X86RegInfo::Rbp, ir::ValueType::U16, false},
        {_RegisterType::R_SI, X86RegInfo::Rsi, ir::ValueType::U16, false},
        {_RegisterType::R_DI, X86RegInfo::Rdi, ir::ValueType::U16, false},
        {_RegisterType::R_R8W, X86RegInfo::R8, ir::ValueType::U16, false},
        {_RegisterType::R_R9W, X86RegInfo::R9, ir::ValueType::U16, false},
        {_RegisterType::R_R10W, X86RegInfo::R10, ir::ValueType::U16, false},
        {_RegisterType::R_R11W, X86RegInfo::R11, ir::ValueType::U16, false},
        {_RegisterType::R_R12W, X86RegInfo::R12, ir::ValueType::U16, false},
        {_RegisterType::R_R13W, X86RegInfo::R13, ir::ValueType::U16, false},
        {_RegisterType::R_R14W, X86RegInfo::R14, ir::ValueType::U16, false},
        {_RegisterType::R_R15W, X86RegInfo::R15, ir::ValueType::U16, false},
        {_RegisterType::R_AL, X86RegInfo::Rax, ir::ValueType::U8, false},
        {_RegisterType::R_CL, X86RegInfo::Rcx, ir::ValueType::U8, false},
        {_RegisterType::R_DL, X86RegInfo::Rdx, ir::ValueType::U8, false},
        {_RegisterType::R_BL, X86RegInfo::Rbx, ir::ValueType::U8, false},
        {_RegisterType::R_AH, X86RegInfo::Rax, ir::ValueType::U8, true},
        {_RegisterType::R_CH, X86RegInfo::Rcx, ir::ValueType::U8, true},
        {_RegisterType::R_DH, X86RegInfo::Rdx, ir::ValueType::U8, true},
        {_RegisterType::R_BH, X86RegInfo::Rbx, ir::ValueType::U8, true},
        {_RegisterType::R_R8B, X86RegInfo::R8, ir::ValueType::U8, false},
        {_RegisterType::R_R9B, X86RegInfo::R9, ir::ValueType::U8, false},
        {_RegisterType::R_R10B, X86RegInfo::R10, ir::ValueType::U8, false},
        {_RegisterType::R_R11B, X86RegInfo::R11, ir::ValueType::U8, false},
        {_RegisterType::R_R12B, X86RegInfo::R12, ir::ValueType::U8, false},
        {_RegisterType::R_R13B, X86RegInfo::R13, ir::ValueType::U8, false},
        {_RegisterType::R_R14B, X86RegInfo::R14, ir::ValueType::U8, false},
        {_RegisterType::R_R15B, X86RegInfo::R15, ir::ValueType::U8, false},
        {_RegisterType::R_SPL, X86RegInfo::Rsp, ir::ValueType::U8, false},
        {_RegisterType::R_BPL, X86RegInfo::Rbp, ir::ValueType::U8, false},
        {_RegisterType::R_SIL, X86RegInfo::Rsi, ir::ValueType::U8, false},
        {_RegisterType::R_DIL, X86RegInfo::Rdi, ir::ValueType::U8, false},
        {_RegisterType::R_ES, X86RegInfo::ES, ir::ValueType::U16, false},
        {_RegisterType::R_CS, X86RegInfo::CS, ir::ValueType::U16, false},
        {_RegisterType::R_SS, X86RegInfo::SS, ir::ValueType::U16, false},
        {_RegisterType::R_DS, X86RegInfo::DS, ir::ValueType::U16, false},
        {_RegisterType::R_FS, X86RegInfo::FS, ir::ValueType::U16, false},
        {_RegisterType::R_GS, X86RegInfo::GS, ir::ValueType::U16, false},
        {_RegisterType::R_RIP, X86RegInfo::Rip, ir::ValueType::U64, false},
        {_RegisterType::R_ST0, X86RegInfo::Xmm0, ir::ValueType::V128, false},
        {_RegisterType::R_ST1, X86RegInfo::Xmm1, ir::ValueType::V128, false},
        {_RegisterType::R_ST2, X86RegInfo::Xmm2, ir::ValueType::V128, false},
        {_RegisterType::R_ST3, X86RegInfo::Xmm3, ir::ValueType::V128, false},
        {_RegisterType::R_ST4, X86RegInfo::Xmm4, ir::ValueType::V128, false},
        {_RegisterType::R_ST5, X86RegInfo::Xmm5, ir::ValueType::V128, false},
        {_RegisterType::R_ST6, X86RegInfo::Xmm6, ir::ValueType::V128, false},
        {_RegisterType::R_ST7, X86RegInfo::Xmm7, ir::ValueType::V128, false},
        {_RegisterType::R_MM0, X86RegInfo::Xmm0, ir::ValueType::V64, false},
        {_RegisterType::R_MM1, X86RegInfo::Xmm1, ir::ValueType::V64, false},
        {_RegisterType::R_MM2, X86RegInfo::Xmm2, ir::ValueType::V64, false},
        {_RegisterType::R_MM3, X86RegInfo::Xmm3, ir::ValueType::V64, false},
        {_RegisterType::R_MM4, X86RegInfo::Xmm4, ir::ValueType::V64, false},
        {_RegisterType::R_MM5, X86RegInfo::Xmm5, ir::ValueType::V64, false},
        {_RegisterType::R_MM6, X86RegInfo::Xmm6, ir::ValueType::V64, false},
        {_RegisterType::R_MM7, X86RegInfo::Xmm7, ir::ValueType::V64, false},
        {_RegisterType::R_XMM0, X86RegInfo::Xmm0, ir::ValueType::V128, false},
        {_RegisterType::R_XMM1, X86RegInfo::Xmm1, ir::ValueType::V128, false},
        {_RegisterType::R_XMM2, X86RegInfo::Xmm2, ir::ValueType::V128, false},
        {_RegisterType::R_XMM3, X86RegInfo::Xmm3, ir::ValueType::V128, false},
        {_RegisterType::R_XMM4, X86RegInfo::Xmm4, ir::ValueType::V128, false},
        {_RegisterType::R_XMM5, X86RegInfo::Xmm5, ir::ValueType::V128, false},
        {_RegisterType::R_XMM6, X86RegInfo::Xmm6, ir::ValueType::V128, false},
        {_RegisterType::R_XMM7, X86RegInfo::Xmm7, ir::ValueType::V128, false},
        {_RegisterType::R_XMM8, X86RegInfo::Xmm8, ir::ValueType::V128, false},
        {_RegisterType::R_XMM9, X86RegInfo::Xmm9, ir::ValueType::V128, false},
        {_RegisterType::R_XMM10, X86RegInfo::Xmm10, ir::ValueType::V128, false},
        {_RegisterType::R_XMM11, X86RegInfo::Xmm11, ir::ValueType::V128, false},
        {_RegisterType::R_XMM12, X86RegInfo::Xmm12, ir::ValueType::V128, false},
        {_RegisterType::R_XMM13, X86RegInfo::Xmm13, ir::ValueType::V128, false},
        {_RegisterType::R_XMM14, X86RegInfo::Xmm14, ir::ValueType::V128, false},
        {_RegisterType::R_XMM15, X86RegInfo::Xmm15, ir::ValueType::V128, false},
        {_RegisterType::R_XMM0, X86RegInfo::Ymm0, ir::ValueType::V256, false},
        {_RegisterType::R_YMM1, X86RegInfo::Ymm1, ir::ValueType::V256, false},
        {_RegisterType::R_YMM2, X86RegInfo::Ymm2, ir::ValueType::V256, false},
        {_RegisterType::R_YMM3, X86RegInfo::Ymm3, ir::ValueType::V256, false},
        {_RegisterType::R_YMM4, X86RegInfo::Ymm4, ir::ValueType::V256, false},
        {_RegisterType::R_YMM5, X86RegInfo::Ymm5, ir::ValueType::V256, false},
        {_RegisterType::R_YMM6, X86RegInfo::Ymm6, ir::ValueType::V256, false},
        {_RegisterType::R_YMM7, X86RegInfo::Ymm7, ir::ValueType::V256, false},
        {_RegisterType::R_YMM8, X86RegInfo::Ymm8, ir::ValueType::V256, false},
        {_RegisterType::R_YMM9, X86RegInfo::Ymm9, ir::ValueType::V256, false},
        {_RegisterType::R_YMM10, X86RegInfo::Ymm10, ir::ValueType::V256, false},
        {_RegisterType::R_YMM11, X86RegInfo::Ymm11, ir::ValueType::V256, false},
        {_RegisterType::R_YMM12, X86RegInfo::Ymm12, ir::ValueType::V256, false},
        {_RegisterType::R_YMM13, X86RegInfo::Ymm13, ir::ValueType::V256, false},
        {_RegisterType::R_YMM14, X86RegInfo::Ymm14, ir::ValueType::V256, false},
        {_RegisterType::R_YMM15, X86RegInfo::Ymm15, ir::ValueType::V256, false}
};

ir::Uniform ToReg(const X86RegInfo& info);

ir::Uniform ToVReg(const X86RegInfo& info);

class X64Decoder {
public:
    X64Decoder(VAddr start, runtime::MemoryInterface *memory, ir::Assembler* visitor, bool is_64bit);

    void Decode();

private:

    enum SSEMCSREnables : u32 {
        IM = 1 << 7,
        DM = 1 << 8,
        ZM = 1 << 9,
        OM = 1 << 10,
        UM = 1 << 11,
        PM = 1 << 12
    };

    enum SSEMXCSRModes : u32 { FZ = 1 << 13, DAZ = 1 << 14, RN = 1 << 15 };

    enum SSEMXCSRExceptions : u32 {
        PE = 1 << 16,
        UE = 1 << 17,
        OE = 1 << 18,
        ZE = 1 << 19,
        DE = 1 << 20,
        IE = 1 << 21
    };

    struct Operand {
        ir::OperandOp::Type op_type{ir::OperandOp::Plus};
        ir::DataClass left{};
        ir::DataClass right{};
        u8 ext{};

        [[nodiscard]] ir::Operand ToIROperand() const {
            if (right.Null()) {
                if (ext) {
                    return ir::Operand{left, ir::Imm(ext), ir::OperandLsl};
                } else {
                    return ir::Operand{left};
                }
            } else {
                return ir::Operand{left, right, {op_type, ext}};
            }
        }

        [[nodiscard]] bool IsImm() const {
            return left.IsImm() && right.Null() && !ext;
        }

        [[nodiscard]] bool OnlyLeft() const {
            return !left.Null() && right.Null();
        }

        [[nodiscard]] ir::Imm ToImm() const {
            return left.imm;
        }

        [[nodiscard]] ir::DataClass Left() const {
            return left;
        }
    };

    static bool IsV(_RegisterType reg);

    ir::DataClass GetOperand(const Operand &operand);

    ir::Value R(_RegisterType reg);

    ir::Value V(_RegisterType reg);

    void R(_RegisterType reg, ir::Value value);

    void V(_RegisterType reg, ir::Value value);

    void Interrupt(InterruptReason reason);

    ir::Value ToValue(const ir::DataClass &data);

    ir::DataClass Src(_DInst& insn, _Operand& operand);

    void Dst(_DInst& insn, _Operand& operand, const ir::DataClass &value);

    Operand GetAddress(_DInst& insn, _Operand& operand);

    ir::Value Pop(ir::ValueType size);

    void Push(ir::Value value, ir::ValueType size);

    ir::BOOL CheckCond(Cond cond);

    void CondGoto(ir::BOOL cond, ir::Lambda then_, ir::Location else_);

    bool DecodeSwitch(_DInst& insn);

    void DecodeMov(_DInst& insn);

    void DecodeMovs(_DInst& insn);

    void DecodeStos(_DInst& insn);

    void DecodeCpuid(_DInst& insn);
    void DecodeMovzx(_DInst& insn);

    void DecodeMovsx(_DInst& insn);

    void DecodeAddSub(_DInst& insn, bool sub, bool save_res = true, bool exchange = false);

    void DecodeCondJump(_DInst& insn, Cond cond);

    void DecodeZeroCheckJump(_DInst& insn, _RegisterType reg);

    void DecodeAddSubWithCarry(_DInst& insn, bool sub);

    void DecodeIncAndDec(_DInst& insn, bool dec);

    void DecodeAnd(_DInst& insn, bool save_result = false);

    void DecodeLea(_DInst& insn);

    void DecodeMulOneOperand(_DInst& insn, bool sign);

    void DecodeIMul(_DInst& insn);

    void DecodeDiv(_DInst& insn, bool sign);

    void DecodeNeg(_DInst& insn);

    void DecodeNot(_DInst& insn);

    void DecodeXchg(_DInst& insn);

    void DecodeSetCC(_DInst& insn, Cond cond);

    void DecodeCondMov(_DInst& insn, Cond cond);

    void DecodeOr(_DInst& insn);

    void DecodeXor(_DInst& insn);

    void DecodePush(_DInst& insn);

    void DecodePop(_DInst& insn);

    void DecodePushA(_DInst& insn);

    void DecodePopA(_DInst& insn);

    void DecodeShlShr(_DInst& insn, bool shr);

    void DecodeSar(_DInst& insn);

    // kind: 0 = shl, 1 = shr, 2 = sar
    void DecodeShift(_DInst& insn, int kind);

    enum class ArithOp { Add, Adc, Sub, Sbb };

    // ARM flag-setting arithmetic always reports C as NOT-borrow, so after a
    // sub-family op the stored carry has the inverse of the x86 CF semantics.
    // The backend offers no way to rewrite a single flag bit, so the decoder
    // tracks the polarity of the stored carry and compensates at CF consumers
    // (jcc / setcc / cmov / adc / sbb / lahf). Valid within a translation
    // block; resets to Unknown at block entry (best effort across blocks, see
    // report).
    enum class CarryPolarity { Unknown, Direct, Inverted };

    // left (op) right at the given x86 width with flags per flag_mask. For
    // widths < 32 (and for 32 bit carry ops with mismatched polarity) the host
    // flag computation is only exact at wider widths, so the NZCV-defining op
    // runs in a wider container on operands shifted left; PF/AF and the result
    // value come from a second, unshifted add whose host flags provably never
    // pollute the sticky flags register (its N/C/V are always 0 and its Z is
    // only set when the true Z is set).
    ir::Value ArithWithFlags(ir::Value left, ir::Value right, ArithOp op, u32 width,
                             ir::Flags flag_mask);

    // Current CF as a 0/1 value, honoring the tracked carry polarity (and
    // the runtime polarity byte at block entry).
    ir::Value CarryValue();

    // Runtime carry polarity byte (ThreadContext64::carry_inverted): written
    // by every carry-defining op so CF consumers in LATER blocks can recover
    // the architectural CF from the stored host carry.
    static ir::Uniform PolarityUniform() {
        return ir::Uniform{offsetof(ThreadContext64, carry_inverted), ir::ValueType::U8};
    }
    void StorePolarity(bool inverted);

    void DecodeCmp(_DInst& insn);

    void DecodeAndNot(_DInst& insn);

    void SaveLogicFlags(ir::Value result, u32 width);

    // Extend a value to a (wider) type, signed or unsigned.
    ir::Value Extend(ir::Value value, ir::ValueType type, bool sign);

    // Narrow (or widen) a value to a type safely: SetType on a U64-producing
    // instruction would make its emitter use 32 bit registers on 64 bit
    // operands (e.g. invalid W shifts), so narrowing goes through an explicit
    // ZeroExtend32 (W-normalize) plus a W-register-safe SetType.
    ir::Value NarrowTo(ir::Value value, ir::ValueType type);

    // Segment override base: FS/GS read the 64-bit bases from the context
    // (TLS); other segments keep the legacy selector * 16 model.
    ir::Value SegmentBase(_RegisterType segment);

    // ---- SSE support ----------------------------------------------------
    // xmm slots are accessed as two 64-bit uniform halves (the x86 config
    // runs no uniform-caching pass, so scalar and V128 uniform views of the
    // same slot alias safely in both backends). Vector ALU semantics the IR
    // cannot express (the JIT Vec4* emitters are stubs) go through per-half
    // host helpers, mirroring the RepMovs/RepStos pattern.
    ir::Value XmmLo(_RegisterType reg);

    ir::Value XmmHi(_RegisterType reg);

    void XmmLo(_RegisterType reg, ir::Value value);

    void XmmHi(_RegisterType reg, ir::Value value);

    struct VecHalves {
        ir::Value lo, hi;
    };

    // Load an xmm register or a 128-bit memory operand as two U64 halves.
    VecHalves LoadSrcHalves(_DInst& insn, _Operand& op);

    // Single-half variants (dead loads break the register allocator, see
    // DecodeVecIROp).
    ir::Value LoadSrcLo(_DInst& insn, _Operand& op);
    ir::Value LoadSrcHi(_DInst& insn, _Operand& op);

    // Fold a memory/register address operand to a single address value
    // (TSO forms only encode [base], see Src()).
    ir::Value FlatAddress(_DInst& insn, _Operand& op);

    // dst(xmm) op= src(xmm/m128), computed per 64-bit half by a host helper.
    using VecHalfFn = u64 (*)(u64, u64);
    void DecodeVecHalfOp(_DInst& insn, VecHalfFn fn);
    // Distinct helpers for the lo / hi result halves (unpack-style ops).
    void DecodeVecHalfOp(_DInst& insn, VecHalfFn fn_lo, VecHalfFn fn_hi);
    // Same but sourced from the HIGH halves of both operands (punpckh*).
    void DecodeVecHalfOpHigh(_DInst& insn, VecHalfFn fn_lo, VecHalfFn fn_hi);
    // Both helpers take the LOW halves (punpcklbw / punpcklwd).
    void DecodePunpckLo(_DInst& insn, VecHalfFn fn_lo, VecHalfFn fn_hi);

    // dst(xmm) op= src(xmm/m128) with pure-IR ops on the halves.
    enum class VecIROp { Xor, Or, And, AndNot, AddQ, SubQ, Punpckldq, Punpckhdq,
                         Punpcklqdq, Punpckhqdq, Muludq };
    void DecodeVecIROp(_DInst& insn, VecIROp op);

    void DecodeMovd(_DInst& insn);
    void DecodeMovq(_DInst& insn);
    // movdqa/movdqu/movaps/movups: plain 128-bit moves.
    void DecodeMovVec(_DInst& insn);
    void DecodeMovsd(_DInst& insn);
    void DecodeMovss(_DInst& insn);
    // movlpd/movlps/movhpd/movhps: m64 <-> xmm low/high half.
    void DecodeMovHalf(_DInst& insn, bool high);
    void DecodeMovhlps(_DInst& insn, bool low_to_high);
    void DecodeMovmsk(_DInst& insn, bool pd);
    void DecodePshufd(_DInst& insn);
    void DecodeShufps(_DInst& insn, bool pd);
    // pslldq/psrldq: the byte shift amount is a decode-time constant.
    void DecodePshiftDQ(_DInst& insn, bool left);
    // psllw/pslld/psllq/psrlw/psrld/psrlq (imm8 and xmm-count forms).
    void DecodePshift(_DInst& insn, bool left, int kind);
    void DecodePalignr(_DInst& insn);
    void DecodePshufb(_DInst& insn);
    void DecodePmovmskb(_DInst& insn);
    void DecodeMxcsr(_DInst& insn, bool load);
    void DecodeFxsave(_DInst& insn, bool restore);
    void DecodeUcomisd(_DInst& insn);
    // bsf / bsr (and tzcnt aliased to bsf with BMI1 hidden).
    void DecodeBitScan(_DInst& insn, bool reverse);
    // lock cmpxchg (single-threaded model: plain load/compare/store).
    void DecodeCmpxchg(_DInst& insn);
    // rol / ror (value-exact; CF/OF left unchanged, see implementation).
    void DecodeRotate(_DInst& insn, bool left);
    // bt / bts / btr / btc (kind 0..3); CF = extracted bit.
    void DecodeBt(_DInst& insn, int kind);

    VAddr start;
    VAddr pc;
    ir::Assembler* assembler;
    runtime::MemoryInterface *memory;
    bool end_decode{false};
    bool is_64bit{false};
    VAddr addr_mask{UINT64_MAX};
    CarryPolarity carry_{CarryPolarity::Unknown};
};

void FromHost(backend::State *state, ThreadContext64 *ctx);
void ToHost(backend::State *state, ThreadContext64 *ctx);

}  // namespace swift::x86
