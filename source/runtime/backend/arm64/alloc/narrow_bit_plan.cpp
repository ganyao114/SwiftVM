#include "narrow_bit_plan.h"

#include <iterator>

namespace swift::runtime::backend::arm64 {

namespace {
bool Covered(const NarrowBitInputs& inputs, const ir::Inst* inst) {
    return inst && inputs.earlier_coverage && inst->Id() < inputs.earlier_coverage->size() &&
           inputs.earlier_coverage->test(inst->Id());
}

// Producers whose host emission writes the W form for a <=32-bit result,
// zeroing bits [63:32] of the destination register. The ALU set mirrors
// the width verifier's IsPinnedWViewProducer; loads and zero extensions
// have the same property by their emission contract.
bool ProducesZeroUpper32(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::LoadUniform:
        case ir::OpCode::ZeroExtend32:
        case ir::OpCode::ZeroExtend32To64:
        case ir::OpCode::Add:
        case ir::OpCode::Sub:
        case ir::OpCode::And:
        case ir::OpCode::AndNot:
        case ir::OpCode::Or:
        case ir::OpCode::Xor:
        case ir::OpCode::Mul:
        case ir::OpCode::LslImm:
        case ir::OpCode::LsrImm:
        case ir::OpCode::AsrImm:
        case ir::OpCode::RorImm:
        case ir::OpCode::RorValue:
        case ir::OpCode::Neg:
        case ir::OpCode::LoadImm:
        case ir::OpCode::Select:
        case ir::OpCode::SelectZero:
        case ir::OpCode::CondSelect:
        case ir::OpCode::PopCount:
            return true;
        default:
            return false;
    }
}

bool IsNarrowLoadZeroExtended(ir::Value value, u32 width) {
    for (u32 depth = 0; depth != 8 && value.Defined(); ++depth) {
        auto* definition = value.Def();
        while (definition && definition->IsBitCastOperation()) {
            value = definition->GetArg<ir::Value>(0);
            definition = value.Def();
        }
        if (!definition) {
            return false;
        }
        if (definition->GetOp() == ir::OpCode::LoadMemory ||
            definition->GetOp() == ir::OpCode::LoadMemoryTSO ||
            definition->GetOp() == ir::OpCode::LoadUniform) {
            return ir::GetValueSizeByte(definition->ReturnType()) <= width;
        }
        if (definition->GetOp() != ir::OpCode::ZeroExtend32 &&
            definition->GetOp() != ir::OpCode::ZeroExtend32To64) {
            return false;
        }
        value = definition->GetArg<ir::Value>(0);
    }
    return false;
}

}  // namespace

class NarrowBitBuilder {
public:
    NarrowBitBuilder(ir::Block* block, const RegAlloc& allocation, const NarrowBitInputs& inputs)
            : cur_block(block), allocation(allocation), inputs(inputs) {}
    NarrowBitPlan Build();
private:
    std::optional<NarrowExtractExtension> MatchNarrowExtractExtension(ir::Inst*) const;
    std::optional<NarrowMaskedInput> MatchNarrowMaskedInput(ir::Inst*) const;
    std::optional<ShiftMaskedInput> MatchShiftMaskedInput(ir::Inst*);
    ir::Block* cur_block;
    const RegAlloc& allocation;
    const NarrowBitInputs& inputs;
    NarrowBitPlan plan;
};

std::optional<NarrowExtractExtension>
NarrowBitBuilder::MatchNarrowExtractExtension(ir::Inst* wrapper) const {
    if (!wrapper || wrapper->GetOp() != ir::OpCode::ZeroExtend32) {
        return std::nullopt;
    }
    auto value = wrapper->GetArg<ir::Value>(0);
    auto* extract = value.Def();
    if (!extract || extract->GetOp() != ir::OpCode::BitExtract ||
        extract->GetArg<ir::Imm>(1).Get() != 0 ||
        extract->GetUses() != 1 || extract->GetUses(false) != 1 ||
        extract->GetArg<ir::Value>(0).Def() == nullptr ||
        inputs.blocked_extracts.contains(extract) || allocation.IsWidthChainCoalesced(wrapper->Id())) {
        return std::nullopt;
    }
    const u32 width = ir::GetValueSizeByte(extract->ReturnType());
    if ((width != sizeof(u8) && width != sizeof(u16)) ||
        extract->GetArg<ir::Imm>(2).Get() != width * 8 ||
        ir::GetValueSizeByte(value.Type()) != width ||
        allocation.ValueType(extract->GetArg<ir::Value>(0)) == RegAlloc::MEM) {
        // A spill reload region may end at the extract and never write its
        // slot. A later wrapper cannot extend that region in the emitter.
        // The IR width pass handles this fusion before allocation instead.
        return std::nullopt;
    }
    auto& list = cur_block->GetInstList();
    auto next = std::next(list.iterator_to(*extract));
    if (next == list.end() || next.operator->() != wrapper) {
        return std::nullopt;
    }
    ir::Inst* shift{};
    auto after_wrapper = std::next(list.iterator_to(*wrapper));
    if (wrapper->GetUses() == 1 && wrapper->GetUses(false) == 1 &&
        after_wrapper != list.end() &&
        after_wrapper->GetOp() == ir::OpCode::LsrImm &&
        after_wrapper->GetArg<ir::Value>(0).Def() == wrapper) {
        const u64 amount = after_wrapper->GetArg<ir::Imm>(1).Get();
        if (amount > 0 && amount < width * 8) {
            shift = after_wrapper.operator->();
        }
    }
    return NarrowExtractExtension{
            .extract = extract,
            .source = extract->GetArg<ir::Value>(0),
            .shift = shift,
            .width = static_cast<u8>(width),
            .source_high_zero = IsNarrowLoadZeroExtended(
                    extract->GetArg<ir::Value>(0), width),
    };
}

std::optional<NarrowMaskedInput>
NarrowBitBuilder::MatchNarrowMaskedInput(ir::Inst* consumer) const {
    if (!consumer || consumer->GetOp() != ir::OpCode::And) {
        return std::nullopt;
    }
    auto left = consumer->GetArg<ir::Value>(0);
    auto* extract = left.Def();
    const u32 width = ir::GetValueSizeByte(consumer->ReturnType());
    if (!extract || extract->GetOp() != ir::OpCode::BitExtract ||
        (width != sizeof(u8) && width != sizeof(u16)) ||
        ir::GetValueSizeByte(left.Type()) != width ||
        extract->GetArg<ir::Imm>(1).Get() != 0 ||
        extract->GetArg<ir::Imm>(2).Get() != width * 8 ||
        extract->GetUses() != 1 || extract->GetUses(false) != 1 ||
        !extract->GetArg<ir::Value>(0).Defined() ||
        inputs.blocked_extracts.contains(extract) ||
        allocation.ValueType(extract->GetArg<ir::Value>(0)) == RegAlloc::MEM) {
        return std::nullopt;
    }
    const auto right = consumer->GetArg<ir::Operand>(1);
    u64 mask{};
    if (right.IsImm()) {
        mask = right.GetLeft().imm.Get();
    } else if (right.GetLeft().IsValue() && right.GetRight().Null()) {
        auto* definition = right.GetLeft().value.Def();
        if (!definition || definition->GetOp() != ir::OpCode::LoadImm) {
            return std::nullopt;
        }
        mask = definition->GetArg<ir::Imm>(0).Get();
    } else {
        return std::nullopt;
    }
    if ((mask >> (width * 8)) != 0) {
        return std::nullopt;
    }
    // Allocation only keeps the parent live through the extract. Moving its
    // read past another IR instruction can consume a register that has already
    // been reused, even while the extracted value itself remains live.
    const auto next = std::next(cur_block->GetInstList().iterator_to(*extract));
    if (next == cur_block->GetInstList().end() || &*next != consumer) {
        return std::nullopt;
    }
    return NarrowMaskedInput{
            .extract = extract,
            .source = extract->GetArg<ir::Value>(0),
            .width = static_cast<u8>(width),
    };
}

std::optional<ShiftMaskedInput>
NarrowBitBuilder::MatchShiftMaskedInput(ir::Inst* consumer) {
    if (!consumer || consumer->GetOp() != ir::OpCode::And ||
        inputs.flagged.contains(consumer)) {
        return std::nullopt;
    }
    const auto right = consumer->GetArg<ir::Operand>(1);
    if (!right.IsImm() || right.GetLeft().imm.Get() != 1) {
        return std::nullopt;
    }
    const auto value = consumer->GetArg<ir::Value>(0);
    auto* shift = value.Def();
    if (!shift ||
        (shift->GetOp() != ir::OpCode::LsrImm &&
         shift->GetOp() != ir::OpCode::AsrImm) ||
        inputs.flagged.contains(shift) || shift->GetUses() != 1 ||
        shift->GetUses(false) != 1 ||
        ir::GetValueSizeByte(value.Type()) !=
                ir::GetValueSizeByte(consumer->ReturnType()) ||
        plan.extract_shifts.contains(shift) ||
        inputs.discarded.contains(shift)) {
        return std::nullopt;
    }
    const auto source = shift->GetArg<ir::Value>(0);
    const u32 width = ir::GetValueSizeByte(source.Type()) * 8;
    const u64 offset = shift->GetArg<ir::Imm>(1).Get();
    if (!source.Defined() || width != ir::GetValueSizeByte(value.Type()) * 8 ||
        offset >= width || allocation.ValueType(source) == RegAlloc::NONE ||
        allocation.ValueType(source) == RegAlloc::MEM) {
        return std::nullopt;
    }
    auto& list = cur_block->GetInstList();
    auto current = list.iterator_to(*consumer);
    if (current == list.begin() || std::prev(current).operator->() != shift) {
        return std::nullopt;
    }
    return ShiftMaskedInput{
            .shift = shift,
            .source = source,
            .offset = static_cast<u8>(offset),
    };
}

NarrowBitPlan NarrowBitBuilder::Build() {
    auto* block = cur_block;
    plan.extensions.clear();
    plan.extracts.clear();
    plan.extract_shifts.clear();
    plan.masks.clear();
    plan.masked_extracts.clear();
    plan.shift_masks.clear();
    plan.masked_shifts.clear();
    plan.tied_load_extensions.clear();
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchNarrowExtractExtension(&inst);
        if (!candidate) {
            continue;
        }
        if (Covered(inputs, &inst) || Covered(inputs, candidate->extract) || Covered(inputs, candidate->shift)) continue;
        plan.extracts.emplace(candidate->extract, &inst);
        plan.extensions.emplace(&inst, *candidate);
        if (candidate->shift) {
            plan.extract_shifts.emplace(candidate->shift, &inst);
        }
    }
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchNarrowMaskedInput(&inst);
        if (!candidate) {
            continue;
        }
        if (Covered(inputs, &inst) || Covered(inputs, candidate->extract)) continue;
        plan.masks.emplace(&inst, *candidate);
        plan.masked_extracts.emplace(candidate->extract, &inst);
    }
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchShiftMaskedInput(&inst);
        if (!candidate) {
            continue;
        }
        if (Covered(inputs, &inst) || Covered(inputs, candidate->shift)) continue;
        plan.shift_masks.emplace(&inst, *candidate);
        plan.masked_shifts.emplace(candidate->shift, &inst);
    }
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != ir::OpCode::ZeroExtend32 &&
            inst.GetOp() != ir::OpCode::ZeroExtend32To64) continue;
        if (allocation.IsWidthChainCoalesced(inst.Id())) continue;
        auto source = inst.GetArg<ir::Value>(0);
        auto* definition = source.Def();
        while (definition && definition->IsBitCastOperation()) {
            source = definition->GetArg<ir::Value>(0);
            definition = source.Def();
        }
        if (!definition || !ProducesZeroUpper32(definition->GetOp())) continue;
        // A width-chain producer may emit the wide form, and a preserved
        // fixed definition is only a low view of a wider live home value;
        // neither guarantees zeroed upper bits in the register.
        if (allocation.IsWidthChainCoalesced(definition->Id()) ||
            allocation.PreservedGPRDefinitionAt(definition->Id()) ||
            plan.masked_shifts.contains(definition)) continue;
        if (ir::GetValueSizeByte(definition->ReturnType()) >
            ir::GetValueSizeByte(source.Type())) continue;
        if (allocation.ValueType(source) != RegAlloc::GPR ||
            allocation.ValueType(ir::Value{&inst}) != RegAlloc::GPR ||
            allocation.ValueGPR(source).id != allocation.ValueGPR(ir::Value{&inst}).id ||
            allocation.SpillReloadAt(source, inst.Id())) continue;
        plan.tied_load_extensions.insert(&inst);
    }
    return std::move(plan);
}

NarrowBitPlan BuildNarrowBitPlan(ir::Block* block, const RegAlloc& allocation, const NarrowBitInputs& inputs) {
    return NarrowBitBuilder{block, allocation, inputs}.Build();
}
}  // namespace swift::runtime::backend::arm64
