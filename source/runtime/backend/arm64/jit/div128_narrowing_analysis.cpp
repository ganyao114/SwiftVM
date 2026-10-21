#include "div128_narrowing_analysis.h"
#include "block_analysis_index.h"

#include <algorithm>

namespace swift::runtime::backend::arm64 {

namespace {

constexpr u8 kMaxResolveDepth = 16;

bool Overlaps(u32 left_offset, u32 left_size, u32 right_offset, u32 right_size) {
    return left_offset < right_offset + right_size &&
           right_offset < left_offset + left_size;
}

bool IsUniformBarrier(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::UniformBarrier:
        case O::Goto:
        case O::NotGoto:
        case O::BindLabel:
        case O::CallLambda:
        case O::CallLocation:
        case O::CallDynamic:
        case O::X87Op:
        case O::Sse42Str:
            return true;
        default:
            return false;
    }
}

}  // namespace

std::optional<ir::Value> Div128NarrowingAnalysis::ResolveUniformLoad(
        const ir::Inst* load) const {
    if (!block || !load || (analysis && !analysis->Contains(load))) return std::nullopt;
    auto& instructions = block->GetInstList();
    // The unit index proves local membership. Manual callers must find the
    // node before using an intrusive iterator; incoming loads are not local.
    auto position = analysis ? instructions.iterator_to(*load) :
            std::find_if(instructions.begin(), instructions.end(),
                         [&](const ir::Inst& inst) { return &inst == load; });
    if (position == instructions.end()) return std::nullopt;
    const auto load_uniform = load->GetArg<ir::Uniform>(0);
    const u32 load_offset = load_uniform.GetOffset();
    const u32 load_size = ir::GetValueSizeByte(load_uniform.GetType());
    while (position != instructions.begin()) {
        auto& candidate = *--position;
        if (IsUniformBarrier(candidate.GetOp())) {
            return std::nullopt;
        }
        if (candidate.GetOp() != ir::OpCode::StoreUniform) {
            continue;
        }
        const auto store_uniform = candidate.GetArg<ir::Uniform>(0);
        const u32 store_offset = store_uniform.GetOffset();
        const u32 store_size = ir::GetValueSizeByte(store_uniform.GetType());
        if (!Overlaps(load_offset, load_size, store_offset, store_size)) {
            continue;
        }
        if (load_offset != store_offset || load_size != store_size) {
            return std::nullopt;
        }
        return candidate.GetArg<ir::Value>(1);
    }
    return std::nullopt;
}

ir::Value Div128NarrowingAnalysis::Resolve(ir::Value value, u8 depth) const {
    while (value.Def() && depth++ < kMaxResolveDepth) {
        auto* definition = value.Def();
        if (definition->GetOp() == ir::OpCode::BitCast) {
            value = definition->GetArg<ir::Value>(0);
            continue;
        }
        if (definition->GetOp() == ir::OpCode::GetOperand) {
            const auto operand = definition->GetArg<ir::Operand>(0);
            if (operand.GetOp().type != ir::OperandOp::None ||
                !operand.GetRight().Null() || !operand.GetLeft().IsValue()) {
                break;
            }
            value = operand.GetLeft().value;
            continue;
        }
        if (definition->GetOp() == ir::OpCode::LoadUniform) {
            const auto forwarded = ResolveUniformLoad(definition);
            if (!forwarded) {
                break;
            }
            value = *forwarded;
            continue;
        }
        break;
    }
    return value;
}

bool Div128NarrowingAnalysis::CanLowerNatively(const ir::Inst* inst) const {
    if (!inst || inst->GetOp() != ir::OpCode::Div128) {
        return false;
    }
    const bool sign = inst->GetArg<ir::Imm>(3).Get() != 0;
    const auto high = Resolve(inst->GetArg<ir::Value>(0));
    auto* high_definition = high.Def();
    if (!high_definition) {
        return false;
    }
    if (!sign) {
        return high_definition->GetOp() == ir::OpCode::LoadImm &&
               high_definition->GetArg<ir::Imm>(0).Get() == 0;
    }
    if (high_definition->GetOp() != ir::OpCode::AsrImm ||
        high_definition->GetArg<ir::Imm>(1).Get() != 63) {
        return false;
    }
    const auto low = Resolve(inst->GetArg<ir::Value>(1));
    const auto sign_source = Resolve(high_definition->GetArg<ir::Value>(0));
    return sign_source.Def() && sign_source.Def() == low.Def();
}

}  // namespace swift::runtime::backend::arm64
