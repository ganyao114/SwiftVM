//
// Created by SwiftGan on 2021/1/1.
//

#pragma once

#include "cpu.h"
#include "distorm.h"
#include "mnemonics.h"
#include "runtime/backend/context.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/include/config.h"
#include "vex_decoder.h"

namespace swift::x86 {

// Guest->host address bias used by host helpers (rep movs/stos) that execute
// with raw guest pointers. Installed by the embedding translator when guest
// addresses are virtualized (memory_base); 0 = identity.
void SetGuestMemBias(u64 bias);
[[nodiscard]] u64 GetGuestMemBias();

// Memory ordering mode installed by the embedding translator (from
// Config::tso_mode). AcqRel routes every guest memory access through the
// ordering-enforcing IR ops; Relaxed/Hardware keep plain accesses (Hardware
// relies on the host already running a TSO memory model). LOCK-prefixed
// instructions always emit ordered accesses regardless of this mode.
void SetTsoMode(runtime::TsoMode mode);
[[nodiscard]] runtime::TsoMode GetTsoMode();

using VAddr = u64;
using namespace swift::runtime;

enum class Cond : u8 {
    EQ = 0,
    NE,
    CS,
    CC,
    MI,
    PL,
    VS,
    VC,
    HI,
    LS,
    GE,
    LT,
    GT,
    LE,
    AL,
    NV,
    AT,
    AE,
    BT,
    BE,
    SN,
    NS,
    PA,
    NP,
    HS = CS,
    LO = CC,
    OF = VS,
    NO = VC
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
        // YMM entries address the HIGH half (ymm_high[i]) and are V128, never
        // V256: a 256-bit IR value cannot be register-allocated (ARM64 V regs
        // are 128-bit), so every 256-bit operation is split into two V128 ones.
        // The low half of YMMi is the Xmmi entry above — same storage.
        // (The R_YMM0 code was previously mistyped as R_XMM0, which made
        // x86_regs_table[R_YMM0] report a V256 XMM slot.)
        {_RegisterType::R_YMM0, X86RegInfo::Ymm0, ir::ValueType::V128, false},
        {_RegisterType::R_YMM1, X86RegInfo::Ymm1, ir::ValueType::V128, false},
        {_RegisterType::R_YMM2, X86RegInfo::Ymm2, ir::ValueType::V128, false},
        {_RegisterType::R_YMM3, X86RegInfo::Ymm3, ir::ValueType::V128, false},
        {_RegisterType::R_YMM4, X86RegInfo::Ymm4, ir::ValueType::V128, false},
        {_RegisterType::R_YMM5, X86RegInfo::Ymm5, ir::ValueType::V128, false},
        {_RegisterType::R_YMM6, X86RegInfo::Ymm6, ir::ValueType::V128, false},
        {_RegisterType::R_YMM7, X86RegInfo::Ymm7, ir::ValueType::V128, false},
        {_RegisterType::R_YMM8, X86RegInfo::Ymm8, ir::ValueType::V128, false},
        {_RegisterType::R_YMM9, X86RegInfo::Ymm9, ir::ValueType::V128, false},
        {_RegisterType::R_YMM10, X86RegInfo::Ymm10, ir::ValueType::V128, false},
        {_RegisterType::R_YMM11, X86RegInfo::Ymm11, ir::ValueType::V128, false},
        {_RegisterType::R_YMM12, X86RegInfo::Ymm12, ir::ValueType::V128, false},
        {_RegisterType::R_YMM13, X86RegInfo::Ymm13, ir::ValueType::V128, false},
        {_RegisterType::R_YMM14, X86RegInfo::Ymm14, ir::ValueType::V128, false},
        {_RegisterType::R_YMM15, X86RegInfo::Ymm15, ir::ValueType::V128, false}};

ir::Uniform ToReg(const X86RegInfo& info);

ir::Uniform ToVReg(const X86RegInfo& info);

class X64Decoder {
public:
    X64Decoder(VAddr start,
               runtime::MemoryInterface* memory,
               ir::Assembler* visitor,
               bool is_64bit);

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

        [[nodiscard]] bool IsImm() const { return left.IsImm() && right.Null() && !ext; }

        [[nodiscard]] bool OnlyLeft() const { return !left.Null() && right.Null(); }

        [[nodiscard]] ir::Imm ToImm() const { return left.imm; }

        [[nodiscard]] ir::DataClass Left() const { return left; }
    };

    static bool IsV(_RegisterType reg);

    ir::DataClass GetOperand(const Operand& operand);

    ir::Value R(_RegisterType reg);

    ir::Value V(_RegisterType reg);

    void R(_RegisterType reg, ir::Value value);

    void V(_RegisterType reg, ir::Value value);

    void Interrupt(InterruptReason reason);

    ir::Value ToValue(const ir::DataClass& data);

    ir::DataClass Src(_DInst& insn, _Operand& operand, bool force_tso = false);

    void Dst(_DInst& insn, _Operand& operand, const ir::DataClass& value, bool force_tso = false);

    // Memory ordering: AcqRel mode orders every access; a LOCK prefix orders
    // just that instruction's accesses (force_tso covers the implicitly
    // locked memory XCHG, which carries no LOCK prefix in the encoding).
    [[nodiscard]] bool TsoOrdered(const _DInst& insn) const;
    ir::Value MemLoad(const ir::Operand& addr, ir::ValueType type, bool tso);
    void MemStore(const ir::Operand& addr, ir::Value value, bool tso);

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
    void DecodeTimestamp(bool rdtscp);
    void DecodeRandom(_DInst& insn);
    void DecodeRandomRegister(_RegisterType reg, u32 width);
    void DecodeMovbe(_DInst& insn);
    void DecodeMovnti(_DInst& insn);
    void DecodeXlat(_DInst& insn);
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
    void DecodePushf(_DInst& insn);
    void DecodePopf(_DInst& insn);

    void DecodePushA(_DInst& insn);

    void DecodePopA(_DInst& insn);

    void DecodeShlShr(_DInst& insn, bool shr);

    void DecodeSar(_DInst& insn);
    void DecodeDoubleShift(_DInst& insn, bool right);
    void DecodeRotateCarry(_DInst& insn, bool left);

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
    ir::Value ArithWithFlags(
            ir::Value left, ir::Value right, ArithOp op, u32 width, ir::Flags flag_mask);

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
    static ir::Uniform DirectionUniform() {
        return ir::Uniform{offsetof(ThreadContext64, direction), ir::ValueType::U8};
    }
    ir::Value DirectionValue();
    void StoreDirection(bool set);

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
    // The x86 config runs no uniform-caching pass, so scalar and V128 uniform
    // views of the same xmm slot alias safely in both backends.
    ir::Value XmmRead(_RegisterType reg);

    void XmmWrite(_RegisterType reg, ir::Value value);

    ir::Value LoadSrcVec(_DInst& insn, _Operand& op);

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

    void DecodeVec4Add(_DInst& insn);

    enum class VecBitwiseOp { Xor, Or, And, AndNot };
    void DecodeVecBitwise(_DInst& insn, VecBitwiseOp op);

    enum class VecIntOp { Add, Sub, CmpEq, CmpGt };
    void DecodeVecInt(_DInst& insn, VecIntOp op, u32 lane_bits);
    void DecodeVecAvg(_DInst& insn, u32 lane_bits);
    void DecodeVecMinMax(_DInst& insn, bool max, u32 lane_bits, bool is_signed);
    void DecodeVecMul(_DInst& insn, u32 lane_bits);
    void DecodeVecMulHigh16(_DInst& insn, bool is_signed);
    void DecodeVecSat(_DInst& insn, bool sub, u32 lane_bits, bool is_signed);
    void DecodeVecPack(_DInst& insn, u32 source_bits, bool unsigned_destination);
    void DecodeVecAbsDiffSum8(_DInst& insn);
    void DecodeVecMadd16(_DInst& insn);
    void DecodeMaskmovdqu(_DInst& insn);

    void DecodeVecZip(_DInst& insn, u32 lane_bits, bool high);
    void DecodeVecDupPairs32(_DInst& insn, bool odd);

    void DecodeMuludq(_DInst& insn);

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
    // psraw/psrad: arithmetic right shift (imm8 and xmm-count forms).
    void DecodePshiftA(_DInst& insn, int kind);
    // pshuflw/pshufhw: word shuffle within low/high qword.
    void DecodePshufw(_DInst& insn, bool high);
    enum class VecFloatOp { Add, Sub, Mul, Div };
    // SSE2 scalar conversions.
    void DecodeCvtsi2ss(_DInst& insn);
    void DecodeCvtsi2sd(_DInst& insn);
    void DecodeCvttss2si(_DInst& insn);
    void DecodeCvttsd2si(_DInst& insn);
    void DecodeCvtFloatToInt(_DInst& insn, u32 source_bits);
    void DecodePackedConvert(_DInst& insn, u32 kind);
    void DecodeCvtsd2ss(_DInst& insn);
    void DecodeCvtss2sd(_DInst& insn);
    void DecodePackedFloatOp(_DInst& insn, VecFloatOp op, u32 lane_bits);
    void DecodeUcomis(_DInst& insn, u32 lane_bits);
    void DecodeScalarFloatOp(_DInst& insn, VecFloatOp op, u32 lane_bits = 32);
    void DecodeFloatCompareMask(_DInst& insn, u32 lane_bits, u32 predicate, bool scalar);
    void DecodeFloatMinMax(_DInst& insn, u32 lane_bits, bool maximum, bool scalar);
    void DecodeFloatUnary(_DInst& insn, u32 lane_bits, u32 kind, bool scalar);
    // pextrw / pinsrw: gpr <-> xmm word lane (imm8 selects the lane).
    void DecodePextrw(_DInst& insn);
    void DecodePinsrw(_DInst& insn);
    // SSE3: movddup duplicates the source low qword into both halves.
    void DecodeMovddup(_DInst& insn);
    // SSE3: haddps/hsubps horizontal add/sub of adjacent dword pairs.
    // lzcnt (leading-zero count) and crc32 (SSE4.2 CRC-32C).
    void DecodeLzcnt(_DInst& insn);
    void DecodeCrc32(_DInst& insn);
    // String ops: lods / cmps / scas (single-step and REP forms; DF assumed 0).
    void DecodeLods(_DInst& insn);
    void DecodeCmps(_DInst& insn);
    void DecodeScas(_DInst& insn);
    // popcnt / bswap.
    void DecodePopcnt(_DInst& insn);
    void DecodeBswap(_DInst& insn);
    // loop/loopz/loopnz / enter / cmpxchg8b.
    void DecodeLoop(_DInst& insn);
    void DecodeEnter(_DInst& insn);
    void DecodeCmpxchg8b(_DInst& insn);
    void DecodeCmpxchg16b(_DInst& insn);
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

    // ---- AVX / VEX support ----------------------------------------------
    // Gate for every VEX handler.  AVX stays behind SVM_AVX=1 until the
    // Unicorn differential fuzzer covers it; with the gate off the V*
    // opcodes fall through DecodeSwitch's default and the block traps as
    // FALLBACK, exactly as before AVX decoding existed.  No CPUID bit is
    // advertised either way, so a well-behaved guest never emits VEX at all.
    [[nodiscard]] static bool AvxEnabled();
    // SVM_SSE4 -- the legacy SSE3/SSSE3/SSE4.1 escape hatch, default ON.
    // CPUID consults it so the reported feature bits can never outrun the
    // decoder, the same coherence rule AVX/XSAVE follow.
    [[nodiscard]] static bool Sse4Enabled();
    // SVM_SSE42STR -- the SSE4.2 string-compare escape hatch, default ON.
    // Separate from Sse4Enabled because the two families are separate files
    // with separate CPUID bits: SSE3/SSSE3/SSE4.1 come from decoder_sse4.cc,
    // SSE4.2 (leaf 1 ECX bit 20) is exactly the pcmpXstrY family in
    // decoder_sse42str.cc and nothing else.  DecodeCpuid must consult THIS
    // one before setting bit 20, or the gate and the advertisement drift.
    [[nodiscard]] static bool Sse42StrEnabled();

    // Raw VEX prefix fields, parsed from the instruction bytes rather than
    // taken from distorm.  This is NOT redundant: this distorm snapshot
    // predates AVX2 and therefore has no 256-bit table entry for the packed
    // *integer* opcodes (VPXOR/VPAND/VPOR/VPADDx/VPCMPx/VPSHUFB/...).  For
    // those it silently reports XMM operands with size 128 even when VEX.L=1,
    // with no error flag.  Trusting distorm's operand sizes would therefore
    // execute a 256-bit vpxor as a 128-bit one AND zero the upper half — a
    // wrong answer with no diagnostic.  `l` below is always authoritative.
    struct VexInfo {
        bool valid{false};  // instruction really carries a C4/C5 VEX prefix
        bool l{false};      // VEX.L: 0 = 128-bit, 1 = 256-bit
        bool w{false};      // VEX.W (operand-size / opcode extension)
        // VEX.vvvv is stored INVERTED in the encoding, so the "no third
        // operand" marker 0b1111 un-inverts to register 0 and is otherwise
        // indistinguishable from a genuine xmm0 source.  vvvv is the decoded
        // register number; check vvvv_unused before treating it as one.
        u8 vvvv{0};              // 0..15
        bool vvvv_unused{true};  // raw field was 0b1111
        u8 mmmmm{1};             // opcode map: 1 = 0F, 2 = 0F38, 3 = 0F3A
        u8 pp{0};                // implied prefix: 0 none, 1 = 66, 2 = F3, 3 = F2
        // NOTE: of the fields above only `valid` and `l` are currently read
        // (by IsVex128 / the 256-bit dispatch). The handlers take their
        // operands from distorm's operand list and distinguish the 2- and
        // 3-operand shapes by that list, so vvvv/vvvv_unused/w/pp/mmmmm are
        // parsed but unconsumed. They are kept because the moment a handler
        // has to disambiguate an encoding distorm reports ambiguously, the
        // raw prefix is the only authority — which is already true for `l`
        // (see DecodeVex).
    };

    // Parse the VEX prefix of the instruction currently being decoded.
    // Returns valid=false when the bytes are not a VEX prefix.
    [[nodiscard]] VexInfo DecodeVex() const;

    // True when the instruction is a VEX.128 (L=0) form that Agent A's
    // handlers may execute.  L=1 forms belong to the 256-bit handlers; until
    // those exist a VEX.L=1 encoding must NOT be run through a 128-bit path.
    [[nodiscard]] bool IsVex128(const VexInfo& vex) const;

    // 0..15 for any XMM or YMM distorm register code.  The two register files
    // are the same architectural registers, so handlers work in this index
    // space and pick the half they want explicitly.
    [[nodiscard]] static u32 VecIndex(_RegisterType reg);
    // The XMM (low half) register code for a vector register index, so the
    // whole existing SSE helper set (XmmRead / XmmLo / LoadSrcVec / ...)
    // applies unchanged to a YMM operand's low 128 bits.
    [[nodiscard]] static _RegisterType XmmOf(u32 index);

    // The ModRM.rm register number (0..15, VEX.B folded in) of the instruction
    // currently being decoded, taken from the raw encoding.
    //
    // Needed because this distorm snapshot's VPMOVMSKB VEX entry
    // (externals/distorm/insts.c, II_V_66_0F_D7) carries NO operand
    // descriptors — unlike every neighbouring V* entry — so distorm reports
    // ModRM.reg for BOTH operands and sizes the destination as 64-bit. The
    // source register is therefore wrong whenever the destination GPR number
    // differs from the source vector register number, silently returning
    // another register's mask (`vpmovmskb eax, ymm1` yields ymm0's). The
    // legacy 66 0F D7 form decodes correctly.
    //
    // Callers must only use this for register-form (mod == 11) operands; it
    // does not decode SIB or displacement. Returns UINT32_MAX if the
    // instruction is not VEX-encoded or the bytes are unavailable.
    [[nodiscard]] u32 VexRmRegister() const;

    // ymm_high[index] as a V128 uniform / value.
    [[nodiscard]] static ir::Uniform YmmHighUniform(u32 index);
    ir::Value YmmHighRead(u32 index);
    void YmmHighWrite(u32 index, ir::Value value);
    // 64-bit halves of ymm_high[index], for helper-call based paths.
    ir::Value YmmHighLo(u32 index);
    ir::Value YmmHighHi(u32 index);
    void YmmHighLo(u32 index, ir::Value value);
    void YmmHighHi(u32 index, ir::Value value);

    // C3: every VEX.128 instruction zeroes bits 255:128 of its destination.
    // This is THE semantic difference from legacy SSE (which preserves them),
    // and omitting it produces stale-upper-half bugs that only show up once
    // the register is later read as a YMM.  Call after writing the low half.
    void ZeroYmmHigh(u32 index);

    // The register operand encoded in VEX.vvvv, as an XMM code.  VEX made the
    // SSE two-operand forms non-destructive: `vpxor dst, src1, src2` reads
    // src1 from vvvv instead of clobbering dst.  distorm surfaces it as
    // ops[1] with ops[2] holding the r/m operand, and that operand list is
    // what the handlers consume — VexInfo::vvvv is NOT cross-checked against
    // it today (see the note on VexInfo).
    [[nodiscard]] static _RegisterType VexSrc1(const _DInst& insn);
    // The r/m operand of a 3-operand VEX form is simply ops[2]; it is passed
    // straight to the existing LoadSrcVec / LoadSrcHalves helpers, which need
    // a mutable _Operand&, so there is no accessor for it.

    // Single entry point for every VEX opcode routed from DecodeSwitch:
    // applies the SVM_AVX gate, the VEX.L==0 gate and the operand-shape check
    // in one place, then dispatches. false == "do not translate this block".
    bool DecodeAvx(_DInst& insn);

    // dst = src1 (op) src2 over 128 bits, then zero bits 255:128.
    void DecodeVexBitwise(_DInst& insn, VecBitwiseOp op);
    void DecodeVexInt(_DInst& insn, VecIntOp op, u32 lane_bits);
    // vmovdqu/vmovdqa/vmovups/vmovaps: 128-bit reg<->reg / reg<->mem move.
    void DecodeVexMovVec(_DInst& insn);
    // vmovd/vmovq: gpr/m <-> xmm, upper bits of the destination zeroed.
    void DecodeVexMovd(_DInst& insn);
    void DecodeVexMovq(_DInst& insn);
    // vzeroupper / vzeroall.
    void DecodeVzero(bool all);

    // ---- VEX floating point (decoder_avx_fp.cc) --------------------------
    // Single entry point, tried from the VEX dispatch once the prefix is
    // decoded and pc has advanced past the instruction (RIP-relative folding
    // depends on that, matching GetAddress's convention). Returns false for
    // anything unmodelled, which must trap the block as FALLBACK: a VEX float
    // run at the wrong width or with swapped operands silently produces wrong
    // data rather than faulting.
    bool DecodeAvxFp(const VexInsn& v);

    ir::Value VexAddress(const VexInsn& v);
    ir::Value VexLoadVec(const VexInsn& v);
    VecHalves VexLoadVec256(const VexInsn& v);
    ir::Value VexLoadScalar(const VexInsn& v, u32 lane_bits);
    ir::Value VexLoadScalarVec(const VexInsn& v, u32 lane_bits);
    void VexWrite128(u32 index, ir::Value value);
    void VexWrite256(u32 index, ir::Value lo, ir::Value hi);

    void DecodeAvxFpArith(const VexInsn& v, VecFloatOp op, u32 lane_bits);
    void DecodeAvxFpArithScalar(const VexInsn& v, VecFloatOp op, u32 lane_bits);
    void DecodeAvxFpBitwise(const VexInsn& v, VecBitwiseOp op);
    void DecodeAvxFpMinMax(const VexInsn& v, u32 lane_bits, bool maximum, bool scalar);
    void DecodeAvxFpSqrt(const VexInsn& v, u32 lane_bits, bool scalar);
    void DecodeAvxFpCmpMask(const VexInsn& v, u32 lane_bits, bool scalar);
    void DecodeAvxFpComis(const VexInsn& v, u32 lane_bits);
    void DecodeAvxFpCvtLanewise(const VexInsn& v, u32 kind);
    void DecodeAvxFpCvtPs2Pd(const VexInsn& v);
    void DecodeAvxFpCvtPd2Ps(const VexInsn& v);
    void DecodeAvxFpMovmsk(const VexInsn& v, u32 lane_bits);

    // ---- VEX floating point, second wave (decoder_avx_fp.cc) -------------
    // DecodeAvxFp dispatches Base (the original 0F-map family) then Fp2. Both
    // 128- and 256-bit forms of every opcode below are claimed here, so the
    // "only L=1 implemented" asymmetry that made the VEX.128 twins fatal
    // cannot recur.
    bool DecodeAvxFpBase(const VexInsn& v);
    bool DecodeAvxFp2(const VexInsn& v);
    void VexWriteHalves(u32 index, ir::Value lo, ir::Value hi);
    void DecodeAvxFpMovScalar(const VexInsn& v, u32 lane_bits, bool store);
    void DecodeAvxFpMovLoHi(const VexInsn& v, bool high, bool store);
    void DecodeAvxFpMovDDup(const VexInsn& v);
    void DecodeAvxFpCvtScalarFloat(const VexInsn& v, u32 src_bits);
    void DecodeAvxFpCvtSi2Scalar(const VexInsn& v, u32 dst_bits);
    void DecodeAvxFpCvtScalar2Si(const VexInsn& v, u32 src_bits, bool truncate);
    void DecodeAvxFpCvtWiden(const VexInsn& v, u32 kind);
    void DecodeAvxFpCvtNarrow(const VexInsn& v, u32 kind);
    void DecodeAvxFpByteShift(const VexInsn& v, bool left);
    void DecodeAvxFpPTest(const VexInsn& v);
    void DecodeAvxFpExtract(const VexInsn& v, u32 element_bits);
    void DecodeAvxFpInsert(const VexInsn& v, u32 element_bits);

    // ---- BMI1 / BMI2 (decoder_bmi.cc) ------------------------------------
    // VEX-encoded but NOT vector: these operate on GPRs, VEX.L must be 0, and
    // vvvv carries a second source or the destination. Gated on SVM_BMI so the
    // CPUID BMI bits and the implementation can be advertised together —
    // glibc's ifunc requires AVX2+BMI2 jointly before it selects the AVX2
    // string variants, and every one of those contains BMI instructions.
    [[nodiscard]] static bool BmiEnabled();
    bool DecodeBmi(const VexInsn& v);

    ir::Value BmiSrc(const VexInsn& v, u32 width);
    void BmiWriteFlagsNZ(ir::Value result, u32 width);
    void DecodeBmiAndn(const VexInsn& v, u32 width);
    void DecodeBmiBls(const VexInsn& v, u32 width, u32 kind);
    void DecodeBmiBzhi(const VexInsn& v, u32 width);
    void DecodeBmiBextr(const VexInsn& v, u32 width);
    void DecodeBmiShiftX(const VexInsn& v, u32 width, u32 kind);
    void DecodeBmiRorx(const VexInsn& v, u32 width);
    void DecodeBmiMulx(const VexInsn& v, u32 width);
    void DecodeBmiDepExt(const VexInsn& v, u32 width, bool deposit);
    // TZCNT/LZCNT are legacy-encoded (F3 0F BC/BD) but belong to BMI1/LZCNT;
    // both fall back to today's BSF/BSR behaviour when the gate is off.
    void DecodeTzcnt(_DInst& insn);
    void DecodeLzcntBmi(_DInst& insn);

    // ---- VEX integer / data movement (decoder_avx_int.cc) ----------------
    // Same contract as DecodeAvxFp: returns false for anything unmodelled, and
    // every false return happens BEFORE any IR is emitted, so a decline never
    // leaves a half-built block behind. The two families share no
    // (map, opcode) pair, so dispatch order between them does not matter.
    using AvxIntBinFn = ir::Value (*)(ir::Assembler*, ir::Value, ir::Value, u32, u32);
    using AvxIntUnFn = ir::Value (*)(ir::Assembler*, ir::Value, u32);

    bool DecodeAvxInt(const VexInsn& v);

    ir::Value AvxIntNarrowSrc(const VexInsn& v, u32 bytes);
    void DecodeAvxIntBinary(const VexInsn& v, AvxIntBinFn fn, u32 param);
    void DecodeAvxIntUnary(const VexInsn& v, AvxIntUnFn fn, u32 param);
    void DecodeAvxIntZeroDst(const VexInsn& v);
    void DecodeAvxIntShiftCount(const VexInsn& v, u32 kind, u32 lane_bits);
    void DecodeAvxIntShiftImm(const VexInsn& v, u32 kind, u32 lane_bits);
    void DecodeAvxIntExtend(const VexInsn& v, u32 src_bits, u32 dst_bits, bool is_signed);
    void DecodeAvxIntBroadcast(const VexInsn& v, u32 element_bits);
    void DecodeAvxIntBroadcast128(const VexInsn& v);
    void DecodeAvxIntInsert128(const VexInsn& v);
    void DecodeAvxIntExtract128(const VexInsn& v);
    void DecodeAvxIntPerm2i128(const VexInsn& v);
    void DecodeAvxIntPermq(const VexInsn& v);
    void DecodeAvxIntPermd(const VexInsn& v);
    void DecodeAvxIntBlendv(const VexInsn& v);

    // ---- VEX / SSE widening multiply (decoder_avx_mul.cc) ----------------
    bool DecodeAvxMul(const VexInsn& v);
    void DecodeAvxMulWiden(const VexInsn& v, bool is_signed);
    void DecodeSseMulWiden(_DInst& insn, bool is_signed);

    // ---- VEX horizontal / pairwise (decoder_avx_hadd.cc) -----------------
    // vhadd*/vhsub*/vphadd*/vphsub*/vpmaddubsw, both widths. Same decline
    // contract as DecodeAvxInt, and it reuses DecodeAvxIntBinary above rather
    // than adding a driver: every opcode it claims has the identical
    // reg/vvvv/rm shape and is defined per 128-bit lane.
    bool DecodeAvxHadd(const VexInsn& v);


    // ---- AVX2 gather (decoder_avx_gather.cc) -----------------------------
    // Built from existing IR in the shape DecodeMaskmovdqu already uses: per
    // element TestNotZero on the mask msb -> NotGoto -> LoadMemory -> guarded
    // slot write. A CallLambda helper was rejected because its loads would go
    // through a raw host pointer: in the JIT the faulting host PC would sit
    // inside the helper and AddressSpace::LookupFault would miss it, killing
    // the HOST process instead of raising PageFatal.
    bool DecodeAvxGather(const VexInsn& v);
    void DecodeAvxGatherOp(const VexInsn& v, u32 element_bits, u32 index_bits,
                           u32 index_reg);
    ir::Value GatherSlotRead(u32 reg, u32 slot);
    void GatherSlotWrite(u32 reg, u32 slot, ir::Value value);

    // ---- VEX blend / extract / maskmov (decoder_avx_blend.cc) ------------
    // vblendps/pd, vblendvps/pd, vextractps, vinsertps and vmaskmovps/pd,
    // both widths. Same decline contract as DecodeAvxInt; the imm8 blends
    // reuse DecodeAvxIntBinary and vextractps reuses DecodeAvxFpExtract.
    // The masked moves are the one family here that touches memory per
    // element rather than per vector, so that a masked-off element cannot
    // fault -- see the file header.
    // ---- VEX round / dot product / vpermilpd-var (decoder_avx_misc.cc) ----
    bool DecodeAvxMisc(const VexInsn& v);
    void DecodeAvxRound(const VexInsn& v, u32 lane_bits, bool scalar);
    void DecodeAvxDotProduct(const VexInsn& v, u32 lane_bits);

    bool DecodeAvxBlend(const VexInsn& v);
    void DecodeAvxBlendVar(const VexInsn& v, u32 lane_bits);
    void DecodeAvxInsertPs(const VexInsn& v);
    void DecodeAvxMaskMov(const VexInsn& v, u32 lane_bits, bool store);

    // ---- FMA3 (decoder_avx_fma.cc) --------------------------------------
    bool DecodeAvxFma(const VexInsn& v);
    void DecodeAvxFmaPacked(const VexInsn& v, u32 order, u32 flags, u32 lane_bits);
    void DecodeAvxFmaScalar(const VexInsn& v, u32 order, u32 flags, u32 lane_bits);
    void DecodeAvxFmaAddSub(const VexInsn& v, u32 order, bool sub_even, u32 lane_bits);

    // ---- VEX.256 (decoder_avx.cc) ---------------------------------------
    // A 256-bit operation is two independent V128 operations (contract C1:
    // ARM64 V registers are 128 bits and RegAlloc maps one value onto one
    // register, so no V256 IR value can exist). These write BOTH halves of
    // the destination and therefore must NOT call ZeroYmmHigh.
    bool DecodeAvx256(_DInst& insn, const VexInfo& vex);
    VecHalves LoadAvx256Src(_DInst& insn, _Operand& op);
    void StoreAvx256Dst(_DInst& insn, _Operand& op, ir::Value lo, ir::Value hi);
    void WriteAvx256(u32 index, ir::Value lo, ir::Value hi);
    void DecodeAvx256Mov(_DInst& insn);
    void DecodeAvx256Bitwise(_DInst& insn, VecBitwiseOp op);
    void DecodeAvx256Int(_DInst& insn, VecIntOp op, u32 lane_bits);
    void DecodeAvx256MinMax(_DInst& insn, bool max, u32 lane_bits, bool is_signed);
    void DecodeAvx256Pmovmskb(_DInst& insn);
    void DecodeAvx256Pshufb(_DInst& insn);
    void DecodeAvx256BroadcastSS(_DInst& insn);

    // ---- x87 support ----------------------------------------------------
    // The architectural stack and extF80 arithmetic live in ThreadContext64
    // and are updated by the SoftFloat-backed stateful helper.
    void DecodeX87(_DInst& insn);
    void DecodeX87FreePop(u8 index);
    ir::Value CallX87(u64 command, ir::Value guest_address);
    void ApplyX87CompareFlags(ir::Value compact_flags);

    // ---- legacy SSE3 / SSSE3 / SSE4.1 / SSE4.2 (decoder_sse4.cc) ---------
    // Single entry point, called from DecodeSwitch's `default:` arm: these are
    // all distorm-decoded (non-VEX) opcodes, and routing them from `default`
    // rather than from sixty `case` labels keeps this family out of the way of
    // every other change to that switch.  Returns false for anything it does
    // not claim, which keeps the previous FALLBACK behaviour exactly.
    //
    // Unlike the VEX handlers, NOTHING here zeroes bits 255:128 of the
    // destination YMM: legacy SSE preserves them.  See decoder_sse4.cc.
    bool DecodeSse4(_DInst& insn);

    // Per-128-bit-lane callbacks, the legacy twins of AvxIntBinFn/AvxIntUnFn
    // (no `half` argument: a legacy SSE form has exactly one 128-bit lane).
    using SseBinFn = ir::Value (*)(ir::Assembler*, ir::Value, ir::Value, u32);
    using SseUnFn = ir::Value (*)(ir::Assembler*, ir::Value, u32);

    ir::Value SseNarrowSrc(_DInst& insn, _Operand& op, u32 bytes);
    void DecodeSseBinary(_DInst& insn, SseBinFn fn, u32 param);
    void DecodeSseUnary(_DInst& insn, SseUnFn fn, u32 param);
    void DecodeSseRound(_DInst& insn, u32 lane_bits, bool scalar);
    void DecodeSsePTest(_DInst& insn);
    void DecodeSseExtend(_DInst& insn, u32 src_bits, u32 dst_bits, bool is_signed);
    void DecodeSseBlendVar(_DInst& insn, u32 lane_bits);
    void DecodeSseInsertPs(_DInst& insn);
    void DecodeSseExtract(_DInst& insn, u32 element_bits);
    void DecodeSseInsert(_DInst& insn, u32 element_bits);
    void DecodeSseMpsadbw(_DInst& insn);
    void DecodeSsePhminposuw(_DInst& insn);
    // Shared by PTEST and VTESTPS/VTESTPD: the EFLAGS half of both.
    void DecodeSseTestFlags(ir::Value both, ir::Value notdest);
    ir::Value SseFoldToScalar(ir::Value vec);

    // The VEX forms of this family that no decoder_avx*.cc file claimed --
    // vaddsubps/pd, vpmulhrsw, vtestps/pd, vphminposuw, vmpsadbw. They live in
    // decoder_sse4.cc so they can share its lane functions with the legacy
    // forms instead of copying them a second time. Same contract as the other
    // VEX entry points: false for anything unmodelled, and every false return
    // happens before any IR is emitted.
    bool DecodeAvxSse4(const VexInsn& v);
    void DecodeAvxPhminposuw(const VexInsn& v);
    void DecodeAvxVTest(const VexInsn& v, u32 lane_bits);

    // ---- SSE4.2 string compare (decoder_sse42str.cc) --------------------
    // PCMPISTRI / PCMPISTRM / PCMPESTRI / PCMPESTRM and their VEX twins: the
    // whole of SSE4.2 beyond POPCNT and CRC32.  Two entry points, one per
    // encoding family, both with the usual contract -- false for anything
    // unmodelled, and every false return happens before any IR is emitted.
    bool DecodeSse42Str(_DInst& insn);
    bool DecodeSse42StrVex(const VexInsn& v);
    // Shared body.  `reg1` holds the first operand (always a register),
    // `src2` is the already-loaded second operand, `wide` selects RAX/RDX over
    // EAX/EDX for the explicit lengths, and `vex` selects contract C3 (zero
    // bits 255:128 of YMM0) over the legacy preserve-them contract.
    void DecodeSse42StrBody(_RegisterType reg1, ir::Value src2, u8 imm8, bool explicit_length,
                            bool wide, bool mask_form, bool vex);
    // |signed length| saturated at 8 or 16; the family's most error-prone step.
    ir::Value Sse42StrLength(ir::Value raw, u32 elements);
    // CF / ZF / SF / OF from the helper's packed result, AF and PF to zero.
    void Sse42StrFlags(ir::Value packed);

    VAddr start;
    VAddr pc;
    // First byte of the instruction currently in DecodeSwitch. Handlers that
    // must inspect the raw encoding (VEX prefix fields, see DecodeVex) read
    // through this; _DInst alone does not carry them.
    const u8* insn_bytes{nullptr};
    ir::Assembler* assembler;
    runtime::MemoryInterface* memory;
    bool end_decode{false};
    bool is_64bit{false};
    VAddr addr_mask{UINT64_MAX};
    CarryPolarity carry_{CarryPolarity::Unknown};
};

void FromHost(backend::State* state, ThreadContext64* ctx);
void ToHost(backend::State* state, ThreadContext64* ctx);

}  // namespace swift::x86
