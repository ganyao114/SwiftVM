//
// Created by 甘尧 on 2024/1/4.
//

#pragma once

#include "aarch64/abi-aarch64.h"
#include "aarch64/cpu-features-auditor-aarch64.h"
#include "aarch64/disasm-aarch64.h"
#include "aarch64/instructions-aarch64.h"
#include "cpu-features.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/include/config.h"

namespace swift::arm64 {

using namespace vixl::aarch64;
using namespace runtime;

class A64Decoder : public DecoderVisitor {
public:
    explicit A64Decoder(VAddr start, runtime::MemoryInterface* memory, runtime::ir::Assembler* visitor);

    void Decode();

    void VisitMoveWideImmediate(const Instruction* instr) override;

    void VisitBitfield(const Instruction* instr) override;

    void VisitSystem(const Instruction* instr) override;

    void VisitLoadLiteral(const Instruction* instr) override;

private:
    ir::Value ReadRegister(u8 code, ir::ValueType size, Reg31Mode r31mode = Reg31IsZeroRegister);

    ir::Value ReadXRegister(u8 code, Reg31Mode r31mode = Reg31IsZeroRegister) {
        return ReadRegister(code, ir::ValueType::U64, r31mode);
    }

    ir::Value ReadWRegister(u8 code, Reg31Mode r31mode = Reg31IsZeroRegister) {
        return ReadRegister(code, ir::ValueType::U32, r31mode);
    }

    void WritePC(ir::Lambda new_pc);

    void WriteXRegister(u8 code, ir::Value value, Reg31Mode r31mode = Reg31IsZeroRegister);

    void WriteWRegister(u8 code, ir::Value value, Reg31Mode r31mode = Reg31IsZeroRegister);

    void WriteVRegister(u8 code, ir::Value value);

    void WriteSRegister(u8 code, ir::Value value) { WriteVRegister(code, value); }

    void WriteDRegister(u8 code, ir::Value value) { WriteVRegister(code, value); }

    void WriteQRegister(u8 code, ir::Value value) { WriteVRegister(code, value); }

    [[nodiscard]] VAddr CurrentPC() const;

    ir::Value ReadMemory(ir::Lambda address, ir::ValueType type);

    ir::Assembler* assembler;
    runtime::MemoryInterface* memory;
    VAddr current_pc;
};

}  // namespace swift::arm64
