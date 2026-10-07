#include "jit_context.h"

#include "runtime/backend/context.h"
#include "runtime/backend/riscv64/defines.h"
#include "runtime/ir/instr.h"
#include "runtime/ir/block.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;

void JitContext::EnsureSpace() {
    auto& buffer = masm.GetCodeBuffer();
    if (buffer.GetRemainingBytes() < 8192)
        buffer.Grow(buffer.GetSizeInBytes() + 16384);
}

void JitContext::Address(GPR result, GPR base, s64 offset) {
    if (offset >= -2048 && offset <= 2047) {
        masm.ADDI(result, base, static_cast<s32>(offset));
    } else {
        // t6 is reserved for addresses, never an SSA or operand register.
        ASSERT(base != t6);
        masm.LI(t6, static_cast<u64>(offset));
        masm.ADD(result, base, t6);
    }
}

void JitContext::Load(GPR result, GPR base, s64 offset, u32 size) {
    if (offset < -2048 || offset > 2047) {
        Address(t6, base, offset);
        base = t6;
        offset = 0;
    }
    const auto immediate = static_cast<s32>(offset);
    switch (size) {
        case 1: masm.LBU(result, immediate, base); break;
        case 2: masm.LHU(result, immediate, base); break;
        case 4: masm.LWU(result, immediate, base); break;
        case 8: masm.LD(result, immediate, base); break;
        default: PANIC("unsupported RV64 scalar load size {}", size);
    }
}

void JitContext::Store(GPR value, GPR base, s64 offset, u32 size) {
    ASSERT(value != t6);
    if (offset < -2048 || offset > 2047) {
        Address(t6, base, offset);
        base = t6;
        offset = 0;
    }
    const auto immediate = static_cast<s32>(offset);
    switch (size) {
        case 1: masm.SB(value, immediate, base); break;
        case 2: masm.SH(value, immediate, base); break;
        case 4: masm.SW(value, immediate, base); break;
        case 8: masm.SD(value, immediate, base); break;
        default: PANIC("unsupported RV64 scalar store size {}", size);
    }
}

void JitContext::Read(GPR result, ir::Value value) { ReadPart(result, value, 0); }

void JitContext::ReadPart(GPR result, ir::Value value, u32 part) {
    const auto source = SourcePart(value, part, result);
    if (source != result) masm.MV(result, source);
}

GPR JitContext::SourceRegister(ir::Value value, GPR scratch) { return SourcePart(value, 0, scratch); }

bool JitContext::ZeroHigh(ir::Inst* inst) {
    if (inst->GetOp() == ir::OpCode::BitCast || inst->GetOp() == ir::OpCode::GetResult)
        return ZeroHigh(inst->GetArg<ir::Value>(0).Def());
    return !ir::IsFloatValueType(inst->ReturnType());
}

GPR JitContext::SourcePart(ir::Value value, u32 part, GPR scratch) {
    ASSERT(part < 2);
    // Reads never allocate: all internal select arms share one mapping.
    for (size_t i = 0; i < cached_values.size(); ++i) {
        if (cached_values[i] == value.Def() && cached_parts[i] == part) {
            ++value_stats.cache_hits;
            return scalar_registers[i];
        }
    }
    for (size_t i = 0; i < cached_vectors.size(); ++i) {
        if (cached_vectors[i] == value.Def()) {
            SetVectorType(64, 2);
            const auto source = Vec{static_cast<u32>(i + 8)};
            if (part) { masm.VSLIDEDOWN(v7, source, 1u); masm.VMV_XS(scratch, v7); }
            else masm.VMV_XS(scratch, source);
            ++value_stats.cache_hits;
            return scratch;
        }
    }
    if (part && ZeroHigh(value.Def())) return x0;
    Load(scratch, values, static_cast<s64>(value.Def()->Id()) * kValueStride + part * 8);
    ++value_stats.loads;
    return scratch;
}

void JitContext::Data(GPR result, const ir::DataClass& data) {
    if (data.IsValue()) Read(result, data.value);
    else if (data.IsImm()) masm.LI(result, data.imm.Get());
    else masm.MV(result, x0);
}

void JitContext::Operand(GPR result, const ir::Operand& operand) {
    ASSERT(result != t3 && result != t4 && result != t6);
    Data(result, operand.GetLeft());
    if (operand.GetRight().Null() || operand.GetOp().type == ir::OperandOp::None) return;
    Data(t3, operand.GetRight());
    switch (operand.GetOp().type) {
        case ir::OperandOp::Plus: masm.ADD(result, result, t3); break;
        case ir::OperandOp::PlusExt:
            masm.SLLI(t3, t3, operand.GetOp().shift_ext);
            masm.ADD(result, result, t3);
            break;
        case ir::OperandOp::LSL: masm.SLL(result, result, t3); break;
        case ir::OperandOp::LSR:
            if (operand.GetLeft().IsValue())
                Mask(result, ir::GetValueSizeByte(operand.GetLeft().value.Type()) * 8);
            masm.SRL(result, result, t3);
            break;
        default: PANIC("unsupported RV64 operand");
    }
}

void JitContext::Mask(GPR value, u32 bits) {
    ASSERT(bits > 0 && bits <= 64);
    if (bits == 8) {
        masm.ANDI(value, value, 255);
    } else if (bits < 64) {
        // Biscuit::ZEXTW emits Zba ADD.UW; shifts require only RV64I.
        masm.SLLI(value, value, 64 - bits);
        masm.SRLI(value, value, 64 - bits);
    }
}

void JitContext::SignExtend(GPR value, u32 bits) {
    ASSERT(bits > 0 && bits <= 64);
    if (bits < 64) {
        masm.SLLI(value, value, 64 - bits);
        masm.SRAI(value, value, 64 - bits);
    }
}

GPR JitContext::ResultRegister(ir::Inst* inst) {
    if (!cache_scalars || inst->ReturnType() == ir::ValueType::VOID ||
        ir::IsFloatValueType(inst->ReturnType())) return t0;
    return ReserveRegister();
}

GPR JitContext::ReserveRegister(GPR excluded) {
    const auto count = cached_values.size() - size_t(flags_enabled);
    auto index = next_register;
    if (scalar_registers[index] == excluded) index = (index + 1) % count;
    for (size_t scanned = 0; scanned < count; ++scanned) {
        const auto candidate = (next_register + scanned) % count;
        if (!cached_values[candidate] && scalar_registers[candidate] != excluded) { index = candidate; break; }
    }
    next_register = (index + 1) % (cached_values.size() - size_t(flags_enabled));
    if (auto* previous = cached_values[index]) {
        Store(scalar_registers[index], values, static_cast<s64>(previous->Id()) * kValueStride + cached_parts[index] * 8);
        ++value_stats.stores;
        ++value_stats.spills;
    }
    // Evict before operand reads: the selected destination may hold an input
    // to this instruction. Publish the new mapping only after its definition.
    cached_values[index] = nullptr;
    return scalar_registers[index];
}

void JitContext::Write(ir::Inst* inst, GPR value, bool normalized) {
    if (inst->ReturnType() == ir::ValueType::VOID) return;
    if (!normalized) Mask(value, ir::GetValueSizeByte(inst->ReturnType()) * 8);
    if (cache_scalars) {
        for (size_t i = 0; i < scalar_registers.size(); ++i) {
            if (value == scalar_registers[i]) {
                ASSERT(cached_values[i] == nullptr);
                cached_values[i] = inst;
                cached_parts[i] = 0;
                return;
            }
        }
        PANIC("RV64 scalar result has no reserved register");
    }
    Store(value, values, static_cast<s64>(inst->Id()) * kValueStride);
    ++value_stats.stores;
}

void JitContext::ConfigureLiveness(ir::Block* block) {
    last_uses.clear();
    // A backward local edge can read an input again on its next iteration.
    // Until CFG liveness is available, retain canonical homes for such blocks.
    for (auto& inst : block->GetInstList())
        if (inst.GetOp() == ir::OpCode::Goto || inst.GetOp() == ir::OpCode::NotGoto) return;
    u32 position{};
    const auto mark = [&](ir::Value value, u32 use) { last_uses[value.Def()] = use; };
    const auto operands = [&](ir::Inst* inst, u32 use) {
        // Walk physical slots: Params can contain more than Inst::max_args
        // values, so Inst::GetValues()'s small StackVector is insufficient.
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst->ArgAt(slot);
            if (arg.IsValue()) mark(arg.Get<ir::Value>(), use);
            else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) mark(arg.Get<ir::Lambda>().GetValue(), use);
            else if (arg.IsParams())
                for (auto& param : arg.Get<ir::Params>()) if (param.data.IsValue()) mark(param.data.value, use);
        }
    };
    for (auto& inst : block->GetInstList()) {
        last_uses.try_emplace(&inst, position);
        operands(&inst, position);
        if (inst.GetOp() == ir::OpCode::SaveFlags || inst.GetOp() == ir::OpCode::BranchOnlyFlags)
            operands(inst.GetArg<ir::Value>(0).Def(), position);
        ++position;
    }
    const auto terminal = [&](const auto& recurse, const ir::Terminal& value) -> void {
        VisitVariant<void>(value, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) {
                mark(term.cond, position); recurse(recurse, term.then_); recurse(recurse, term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                recurse(recurse, term.then_); recurse(recurse, term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                mark(term.value, position); for (const auto& item : term.cases) recurse(recurse, item.then);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) recurse(recurse, term.else_);
        });
    };
    terminal(terminal, block->GetTerminal());
}

void JitContext::ReleaseDeadValues(u32 position) {
    if (last_uses.empty()) return;
    for (auto& value : cached_values) {
        if (value && last_uses.at(value) <= position) { value = nullptr; ++value_stats.dead_values; }
    }
    for (auto& value : cached_vectors) {
        if (value && last_uses.at(value) <= position) { value = nullptr; ++value_stats.dead_values; }
    }
}

void JitContext::PublishFlags() {
    if (flags_enabled && flags_dirty) {
        Store(flags, state, state_offset_host_flags);
        flags_dirty = false;
    }
}

void JitContext::ReloadFlags() {
    if (flags_enabled) Load(flags, state, state_offset_host_flags);
    flags_dirty = false;
}

void JitContext::FlushValues() {
    for (size_t i = 0; i < cached_values.size(); ++i) {
        if (auto* inst = cached_values[i]) {
            Store(scalar_registers[i], values, static_cast<s64>(inst->Id()) * kValueStride + cached_parts[i] * 8);
            ++value_stats.stores;
        }
    }
    SaveVectorsForCall();
    DiscardValues();
}

void JitContext::DiscardValues() {
    cached_values.fill(nullptr);
    cached_vectors.fill(nullptr);
    next_register = next_vector = 0;
    ResetVectorType();
}

void JitContext::Jump(Label& label) {
    // PC-relative pair avoids JAL's +/-1 MiB limit and survives copying to RX.
    masm.LILabel(t5, &label);
    masm.JR(t5);
}

void JitContext::BranchZero(GPR value, Label& label, bool zero) {
    // Only this local skip uses the +/-4 KiB conditional branch encoding.
    Label skip;
    if (zero) masm.BNE(value, x0, &skip);
    else masm.BEQ(value, x0, &skip);
    Jump(label);
    masm.Bind(&skip);
}

void JitContext::Condition(GPR result, ir::Cond condition) {
    ASSERT(result != t1 && result != t2 && result != t3 && result != t4);
    if (condition == ir::Cond::AL || condition == ir::Cond::NV) {
        masm.LI(result, 1);
        return;
    }
    ASSERT(flags_enabled);
    const auto extract = [&](GPR reg, u32 bit) {
        masm.SRLI(reg, flags, bit);
        masm.ANDI(reg, reg, 1);
    };
    // Most guest conditions need only one flag; avoid extracting all NZCV.
    switch (condition) {
        case ir::Cond::EQ: case ir::Cond::NE:
            extract(result, kZeroBit);
            if (condition == ir::Cond::NE) masm.XORI(result, result, 1);
            break;
        case ir::Cond::CS: case ir::Cond::CC:
            extract(result, kCarryBit);
            if (condition == ir::Cond::CC) masm.XORI(result, result, 1);
            break;
        case ir::Cond::MI: case ir::Cond::PL:
            extract(result, kNegateBit);
            if (condition == ir::Cond::PL) masm.XORI(result, result, 1);
            break;
        case ir::Cond::VS: case ir::Cond::VC:
            extract(result, kOverflowBit);
            if (condition == ir::Cond::VC) masm.XORI(result, result, 1);
            break;
        case ir::Cond::HI: case ir::Cond::LS:
            extract(t2, kZeroBit);
            extract(t3, kCarryBit);
            if (condition == ir::Cond::HI) {
                masm.XORI(t2, t2, 1); masm.AND(result, t3, t2);
            } else {
                masm.XORI(t3, t3, 1); masm.OR(result, t3, t2);
            }
            break;
        case ir::Cond::GE: case ir::Cond::LT:
            extract(t1, kNegateBit);
            extract(t4, kOverflowBit);
            masm.XOR(result, t1, t4);
            if (condition == ir::Cond::GE) masm.XORI(result, result, 1);
            break;
        case ir::Cond::GT: case ir::Cond::LE:
            extract(t1, kNegateBit);
            extract(t2, kZeroBit);
            extract(t4, kOverflowBit);
            masm.XOR(result, t1, t4); masm.OR(result, result, t2);
            if (condition == ir::Cond::GT) masm.XORI(result, result, 1);
            break;
        case ir::Cond::AL: case ir::Cond::NV: break;
    }
}

u32 JitContext::CurrentBufferSize() {
    return static_cast<u32>(masm.GetCodeBuffer().GetSizeInBytes());
}

void JitContext::Flush(const CodeBuffer& buffer) {
    ASSERT(buffer.size >= CurrentBufferSize());
    std::memcpy(buffer.rw_data, masm.GetCodeBuffer().GetOffsetPointer(0), CurrentBufferSize());
    buffer.Flush();
}

}  // namespace swift::runtime::backend::riscv64
