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

    // Data processing.
    void VisitAddSubImmediate(const Instruction* instr) override;
    void VisitAddSubShifted(const Instruction* instr) override;
    void VisitAddSubExtended(const Instruction* instr) override;
    void VisitAddSubWithCarry(const Instruction* instr) override;
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
