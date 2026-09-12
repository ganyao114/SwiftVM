#pragma once

#include "runtime/ir/opcodes.h"

namespace swift::runtime::backend {

// Destination capability only. RA and emission independently prove width,
// register ownership, liveness and observation boundaries.
inline bool IsGPRPublicationProducer(ir::OpCode op, bool width_chain = false) {
    switch (op) {
        case ir::OpCode::LoadImm:
        case ir::OpCode::LoadMemory:
        case ir::OpCode::LoadUniform:
        case ir::OpCode::GetHostFPR:
        case ir::OpCode::Zero:
        case ir::OpCode::Add:
        case ir::OpCode::Sub:
        case ir::OpCode::And:
        case ir::OpCode::AndNot:
        case ir::OpCode::Or:
        case ir::OpCode::Xor:
        case ir::OpCode::Adc:
        case ir::OpCode::Sbb:
        case ir::OpCode::Mul:
        case ir::OpCode::Div:
        case ir::OpCode::SignedDiv64:
        case ir::OpCode::Not:
        case ir::OpCode::Neg:
        case ir::OpCode::GetOperand:
        case ir::OpCode::ZeroExtend32:
        case ir::OpCode::SignExtend:
        case ir::OpCode::LslImm:
        case ir::OpCode::LslValue:
        case ir::OpCode::LsrImm:
        case ir::OpCode::LsrValue:
        case ir::OpCode::AsrImm:
        case ir::OpCode::AsrValue:
        case ir::OpCode::RorImm:
        case ir::OpCode::RorValue:
        case ir::OpCode::ByteSwap:
        case ir::OpCode::BitExtract:
        case ir::OpCode::BitClear:
        case ir::OpCode::Select:
        case ir::OpCode::SelectZero:
        case ir::OpCode::CondSelect:
        case ir::OpCode::MulHigh:
        case ir::OpCode::VecMovMask:
            return true;
        case ir::OpCode::GetHostGPR:
            return width_chain;
        default:
            return false;
    }
}

} // namespace swift::runtime::backend
