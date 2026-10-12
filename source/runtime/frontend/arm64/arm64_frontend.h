//
// Created by 甘尧 on 2024/1/4.
//

#pragma once

#include "aarch64/abi-aarch64.h"
#include "aarch64/cpu-features-auditor-aarch64.h"
#include "aarch64/disasm-aarch64.h"
#include "aarch64/instructions-aarch64.h"
#include "cpu-features.h"
#include "cpu.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/include/config.h"

namespace swift::arm64 {

using namespace vixl::aarch64;
using namespace runtime;

class A64Decoder : public DecoderVisitor {
public:
    explicit A64Decoder(VAddr start, runtime::MemoryInterface* memory, runtime::ir::Assembler* visitor);

    // Decodes one basic block starting at the constructor's start address.
    // Fetches guest instructions through the memory interface, dispatches
    // them through the VIXL decoder and emits IR until a block terminal
    // (branch / return / host call) is produced.
    void Decode();

    // Emits one complete guest instruction, including all local IR control
    // flow. Reads exactly four bytes through MemoryInterface::ReadCode and never
    // fetches the following instruction. Misalignment reports ILL_CODE and a
    // wrapping instruction boundary reports PAGE_FATAL before any fetch.
    // Visitor-produced terminals are preserved; ordinary fallthrough links to the
    // following guest PC. This constructs IR only: the caller must execute it
    // through a single-unit runtime boundary. Unsupported leaves the decoder,
    // assembler and dependencies unchanged; no unchecked fetch is attempted.
    // Fault denotes an emitted alignment/fetch trap. Success also covers an
    // already closed assembler, for which this method performs no work.
    CodeFetchResult DecodeSingle();

    // Data processing.
    void VisitAddSubImmediate(const Instruction* instr) override;
    void VisitAddSubShifted(const Instruction* instr) override;
    void VisitAddSubExtended(const Instruction* instr) override;
    void VisitAddSubWithCarry(const Instruction* instr) override;
    void VisitRotateRightIntoFlags(const Instruction* instr) override;
    void VisitEvaluateIntoFlags(const Instruction* instr) override;
    void VisitLogicalImmediate(const Instruction* instr) override;
    void VisitLogicalShifted(const Instruction* instr) override;
    void VisitMoveWideImmediate(const Instruction* instr) override;
    void VisitBitfield(const Instruction* instr) override;
    void VisitExtract(const Instruction* instr) override;
    void VisitDataProcessing2Source(const Instruction* instr) override;
    void VisitDataProcessing3Source(const Instruction* instr) override;
    void VisitConditionalSelect(const Instruction* instr) override;

    // PC relative.
    void VisitPCRelAddressing(const Instruction* instr) override;

    // Branches.
    void VisitUnconditionalBranch(const Instruction* instr) override;
    void VisitConditionalBranch(const Instruction* instr) override;
    void VisitCompareBranch(const Instruction* instr) override;
    void VisitTestBranch(const Instruction* instr) override;
    void VisitUnconditionalBranchToRegister(const Instruction* instr) override;

    // Loads / stores.
    void VisitLoadLiteral(const Instruction* instr) override;
    void VisitLoadStoreUnsignedOffset(const Instruction* instr) override;
    void VisitLoadStoreUnscaledOffset(const Instruction* instr) override;
    void VisitLoadStoreRCpcUnscaledOffset(const Instruction* instr) override;
    void VisitLoadStorePreIndex(const Instruction* instr) override;
    void VisitLoadStorePostIndex(const Instruction* instr) override;
    void VisitLoadStoreRegisterOffset(const Instruction* instr) override;
    void VisitLoadStorePairOffset(const Instruction* instr) override;
    void VisitLoadStorePairPreIndex(const Instruction* instr) override;
    void VisitLoadStorePairPostIndex(const Instruction* instr) override;
    void VisitLoadStorePairNonTemporal(const Instruction* instr) override;

    // System.
    void VisitSystem(const Instruction* instr) override;
    void VisitException(const Instruction* instr) override;

    // NEON / SIMD and scalar floating-point.
    void VisitCryptoAES(const Instruction* instr) override;
    void VisitCrypto2RegSHA(const Instruction* instr) override;
    void VisitCrypto3RegSHA(const Instruction* instr) override;
    void VisitNEONCopy(const Instruction* instr) override;
    void VisitNEONModifiedImmediate(const Instruction* instr) override;
    void VisitNEON2RegMisc(const Instruction* instr) override;
    void VisitNEON2RegMiscFP16(const Instruction* instr) override;
    void VisitNEON3Same(const Instruction* instr) override;
    void VisitNEON3SameExtra(const Instruction* instr) override;
    void VisitNEONByIndexedElement(const Instruction* instr) override;
    void VisitNEONScalar3Same(const Instruction* instr) override;
    void VisitNEONScalar3SameExtra(const Instruction* instr) override;
    void VisitNEON3SameFP16(const Instruction* instr) override;
    void VisitNEONScalar3SameFP16(const Instruction* instr) override;
    void VisitNEONScalar2RegMiscFP16(const Instruction* instr) override;
    void VisitNEONScalar3Diff(const Instruction* instr) override;
    void VisitNEONScalarByIndexedElement(const Instruction* instr) override;
    void VisitNEONScalar2RegMisc(const Instruction* instr) override;
    void VisitNEONScalarPairwise(const Instruction* instr) override;
    void VisitNEONScalarShiftImmediate(const Instruction* instr) override;
    void VisitNEONPerm(const Instruction* instr) override;
    void VisitNEONTable(const Instruction* instr) override;
    void VisitNEONAcrossLanes(const Instruction* instr) override;
    void VisitNEON3Different(const Instruction* instr) override;
    void VisitNEONExtract(const Instruction* instr) override;
    void VisitNEONShiftImmediate(const Instruction* instr) override;
    void VisitNEONLoadStoreMultiStruct(const Instruction* instr) override;
    void VisitNEONLoadStoreMultiStructPostIndex(const Instruction* instr) override;
    void VisitNEONLoadStoreSingleStruct(const Instruction* instr) override;
    void VisitNEONLoadStoreSingleStructPostIndex(const Instruction* instr) override;
    void VisitFPIntegerConvert(const Instruction* instr) override;
    void VisitFPFixedPointConvert(const Instruction* instr) override;
    void VisitFPImmediate(const Instruction* instr) override;
    void VisitFPDataProcessing1Source(const Instruction* instr) override;
    void VisitFPDataProcessing2Source(const Instruction* instr) override;
    void VisitFPDataProcessing3Source(const Instruction* instr) override;
    void VisitFPCompare(const Instruction* instr) override;
    void VisitFPConditionalCompare(const Instruction* instr) override;
    void VisitFPConditionalSelect(const Instruction* instr) override;
    void VisitNEONScalarCopy(const Instruction* instr) override;

    // Conditional compare.
    void VisitConditionalCompareImmediate(const Instruction* instr) override;
    void VisitConditionalCompareRegister(const Instruction* instr) override;

    // Data processing (1 source): RBIT, REV, CLZ, CLS, etc.
    void VisitDataProcessing1Source(const Instruction* instr) override;

    // Load/store exclusive (LDXR/STXR/LDAXR/STLXR/CAS).
    void VisitLoadStoreExclusive(const Instruction* instr) override;

    // Atomic memory (LDADD, SWP, LDSET, LDCLR, etc.).
    void VisitAtomicMemory(const Instruction* instr) override;

    // Fallback for anything not implemented yet.
    void VisitUnimplemented(const Instruction* instr) override;
    void VisitUnallocated(const Instruction* instr) override;
    void VisitReserved(const Instruction* instr) override;

private:
    enum class AddrMode { Offset, PreIndex, PostIndex };

    // --- Register access -------------------------------------------------
    // Uniform offset of GPR `code` inside ThreadContext64. code 31 maps to
    // sp; callers must intercept XZR/WZR before calling this.
    static u32 GPROffset(u8 code);

    ir::Value ReadRegister(u8 code, ir::ValueType size, Reg31Mode r31mode = Reg31IsZeroRegister);

    ir::Value ReadXRegister(u8 code, Reg31Mode r31mode = Reg31IsZeroRegister) {
        return ReadRegister(code, ir::ValueType::U64, r31mode);
    }

    ir::Value ReadWRegister(u8 code, Reg31Mode r31mode = Reg31IsZeroRegister) {
        return ReadRegister(code, ir::ValueType::U32, r31mode);
    }

    ir::Value ReadVRegister(u8 code, ir::ValueType type);

    ir::Value ReadVHalf(u8 code, bool high);

    void WriteVHalves(u8 code, ir::Value low, ir::Value high);

    ir::Value ZeroByteMask(ir::Value value);

    void WritePC(ir::Lambda new_pc);

    void WriteXRegister(u8 code, ir::Value value, Reg31Mode r31mode = Reg31IsZeroRegister);

    // W writes zero the top 32 bits of the matching X register (AArch64 semantics).
    void WriteWRegister(u8 code, ir::Value value, Reg31Mode r31mode = Reg31IsZeroRegister);

    void WriteVRegister(u8 code, ir::Value value);

    void WriteSRegister(u8 code, ir::Value value) { WriteVRegister(code, value); }

    void WriteDRegister(u8 code, ir::Value value) { WriteVRegister(code, value); }

    void WriteQRegister(u8 code, ir::Value value) { WriteVRegister(code, value); }

    // --- Immediates / scalars -------------------------------------------
    ir::Value ImmValue(u64 imm, ir::ValueType type);

    // Zero extend a narrower value to 64 bits.
    ir::Value Widen(ir::Value value);

    // Sign extend the low `from_bits` of value to 64 bits.
    ir::Value SignExtendValue(ir::Value value, u32 from_bits);

    // Shifted register operand (LSL/LSR/ASR/ROR by immediate).
    ir::Value ShiftOperand(bool is64, ir::Value value, Shift shift, u32 amount);

    // Extended register operand (UXTB..SXTX by 0..4).
    ir::Value ExtendOperand(ir::Value value, Extend extend, u32 shift);

    // --- Flags ------------------------------------------------------------
    ir::BOOL BoolAnd(ir::BOOL a, ir::BOOL b);
    ir::BOOL BoolOr(ir::BOOL a, ir::BOOL b);
    ir::BOOL BoolXor(ir::BOOL a, ir::BOOL b);

    // Evaluates an AArch64 condition code against the virtual NZCV state.
    ir::BOOL CondPassed(Condition cond);

    // --- Memory -----------------------------------------------------------
    ir::Value AddressAdd(ir::Value base, s64 offset);

    ir::Value ReadMemory(ir::Lambda address, ir::ValueType type);

    void WriteMemory(ir::Value address, ir::Value value);

    // --- Helpers for whole instruction classes ---------------------------
    void AddSubHelper(const Instruction* instr, const ir::DataClass& op2);

    void LoadStoreHelper(const Instruction* instr, s64 offset, AddrMode mode);

    void LoadStoreHelper(const Instruction* instr, const ir::DataClass& offset, AddrMode mode);

    void LoadStorePairHelper(const Instruction* instr, AddrMode mode);

    void ConditionalCompareHelper(const Instruction* instr, const ir::DataClass& op2);
    void ScalarFPHelper(const Instruction* instr, bool comparison = false);
    ir::Value ScalarDoublingMultiply(ir::Value lhs, ir::Value rhs, ir::Value accumulator,
                                    u32 bits, bool accumulate, bool subtract);
    bool TryNEONSaturatingMultiplyHigh(const Instruction* instr, bool scalar, bool indexed, bool extra);
    bool TryScalarPairwiseFP(const Instruction* instr);
    bool TryNEONFP16Unary(const Instruction* instr);
    bool TryNEONVectorFPBinary(const Instruction* instr);
    bool TryNEONVectorFPUnary(const Instruction* instr);
    bool TryNEONFPCompare(const Instruction* instr, bool half, bool zero);
    bool TryNEONFPInteger(const Instruction* instr, bool half);
    bool TryNEONFPFixedSIMD(const Instruction* instr);
    bool TryFRINTRange(const Instruction* instr);
    bool TryNEONPolynomial(const Instruction* instr,bool widening);
    bool TryNEONMixedSaturating(const Instruction* instr,bool scalar);
    bool TryNEONPairwiseWiden(const Instruction* instr);
    bool TryNEONHighNarrow(const Instruction* instr);
    bool TryNEONAbsoluteLong(const Instruction* instr);
    bool TryNEONAbsoluteNarrow(const Instruction* instr);
    bool TryNEONHalving(const Instruction* instr);
    bool TryNEONFPPrecision(const Instruction* instr);
    bool TryNEONFPEstimate(const Instruction* instr, bool half);
    bool TryNEONUnsignedEstimate(const Instruction* instr);
    bool TryBaseMemoryExtension(const Instruction* instr);
    bool TryNEONFPReduction(const Instruction* instr, bool half, bool across);
    bool TryNEONVectorFMA(const Instruction* instr, bool fp16);
    bool TryNEONExtendedFP(const Instruction* instr, bool fp16);
    bool TryNEONIndexedFP(const Instruction* instr);
    bool TryNEONVectorDoublingLong(const Instruction* instr, bool indexed);
    void WriteNZCV(ir::Value source);
    bool TryNEONInteger(const Instruction* instr, bool misc);
    bool TryNEONDotProduct(const Instruction* instr, bool indexed);
    bool TryNEONBitMisc(const Instruction* instr);
    bool TryNEONShiftImmediate(const Instruction* instr);
    bool TryNEONVectorSaturatingShift(const Instruction* instr);
    bool TryNEONVariableShift(const Instruction* instr, bool scalar = false);
    bool TryNEONSaturatingArithmetic(const Instruction* instr);
    void NEONMultiMemory(const Instruction* instr, bool post_index);
    void NEONSingleMemory(const Instruction* instr, bool post_index);

    // --- Block control -----------------------------------------------------
    [[nodiscard]] VAddr CurrentPC() const;

    // Address of the instruction after the one being decoded.
    [[nodiscard]] VAddr NextPC() const { return current_pc + kInstructionSize; }

    // Unconditional direct branch: links the successor block and ends decode.
    void BranchImm(VAddr target);

    // Stops translation, stores the exit reason and returns to the host.
    void Interrupt(InterruptReason reason, VAddr resume_pc);

    ir::Assembler* assembler;
    runtime::MemoryInterface* memory;
    VAddr current_pc;
    // Set once a block terminal (branch / return / host exit) was emitted.
    // Tracked locally because Assembler::EndCommit() is only updated for
    // some terminals depending on the assembler mode (Block vs HIRBuilder).
    bool end_decode_{false};
};

}  // namespace swift::arm64
