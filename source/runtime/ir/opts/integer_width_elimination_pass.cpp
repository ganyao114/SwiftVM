#include "integer_width_elimination_pass.h"

namespace swift::runtime::ir {

namespace {

constexpr u32 kMaxSourceDistance = 128;

bool IsNearbyDefinition(Block* block, Inst* definition, Inst* use) {
    auto& list = block->GetInstList();
    auto current = list.iterator_to(*use);
    for (u32 distance = 0; current != list.begin() && distance < kMaxSourceDistance; ++distance) {
        --current;
        if (&*current == definition) {
            return true;
        }
    }
    return false;
}

bool ReplaceSingleUse(Inst* consumer, Inst* definition, Value replacement) {
    for (u32 index = 0; index < Inst::max_args; ++index) {
        auto& arg = consumer->ArgAt(index);
        if (arg.IsValue() && arg.Get<Value>().Def() == definition) {
            consumer->SetArg(index, replacement);
            return true;
        }
        if (arg.IsLambda() && arg.Get<Lambda>().IsValue() &&
            arg.Get<Lambda>().GetValue().Def() == definition) {
            consumer->SetArg(index, Lambda{replacement});
            return true;
        }
    }
    return false;
}

Inst* FindSingleConsumer(Block* block, Inst* definition) {
    Inst* consumer = nullptr;
    u32 edges = 0;
    for (auto& candidate : block->GetInstList()) {
        auto values = candidate.GetValues();
        for (auto value : values) {
            if (value.Def() == definition) {
                consumer = &candidate;
                ++edges;
            }
        }
    }
    return edges == 1 ? consumer : nullptr;
}

Value DirectSource(Inst* inst) {
    auto source = inst->GetArg<Value>(0);
    if (inst->ReturnType() != ValueType::U32) {
        return {};
    }
    if (GetValueSizeByte(source.Type()) == sizeof(u32)) {
        return source;
    }
    if (!source.Def() ||
        (source.Def()->GetOp() != OpCode::ZeroExtend32To64 &&
         source.Def()->GetOp() != OpCode::SignExtend)) {
        return {};
    }
    auto narrow = source.Def()->GetArg<Value>(0);
    return GetValueSizeByte(narrow.Type()) == sizeof(u32) ? narrow : Value{};
}

bool IsU32Consumer(Inst* consumer, Inst* definition) {
    switch (consumer->GetOp()) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::And:
        case OpCode::AndNot:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::Mul:
        case OpCode::Neg:
        case OpCode::LslImm:
        case OpCode::LslValue:
        case OpCode::LsrImm:
        case OpCode::LsrValue:
        case OpCode::AsrImm:
        case OpCode::AsrValue:
        case OpCode::RorImm:
        case OpCode::RorValue:
        case OpCode::ByteSwap:
        case OpCode::BitClear:
        case OpCode::Select:
        case OpCode::SelectZero:
        case OpCode::CondSelect:
            return consumer->ReturnType() == ValueType::U32;
        case OpCode::ZeroExtend32:
        case OpCode::ZeroExtend32To64:
        case OpCode::ZeroExtend64:
        case OpCode::SignExtend:
            return consumer->GetArg<Value>(0).Def() == definition;
        case OpCode::VecFCvtIntToFloat:
            return consumer->GetArg<Value>(0).Def() == definition &&
                   consumer->GetArg<Imm>(1).Get() == 32;
        case OpCode::StoreMemory:
        case OpCode::StoreMemoryTSO:
            return consumer->GetArg<Value>(1).Def() == definition;
        case OpCode::StoreUniform:
            return consumer->GetArg<Value>(1).Def() == definition;
        case OpCode::SetHostGPR:
            return consumer->GetArg<Value>(0).Def() == definition;
        default:
            return false;
    }
}

void FoldDirectExtracts(Block* block) {
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != OpCode::BitExtract || inst.GetArg<Imm>(1).Get() != 0 ||
            inst.GetArg<Imm>(2).Get() != GetValueSizeByte(inst.ReturnType()) * 8 ||
            inst.GetUses(false) != 1) {
            continue;
        }

        auto source = DirectSource(&inst);
        if (!source.Defined() || !IsNearbyDefinition(block, source.Def(), &inst)) {
            continue;
        }

        auto* consumer = FindSingleConsumer(block, &inst);
        if (consumer && IsU32Consumer(consumer, &inst)) {
            ReplaceSingleUse(consumer, &inst, source);
        }
    }
}

void FoldNarrowExtensions(Block* block) {
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != OpCode::ZeroExtend32) continue;
        const auto input = inst.GetArg<Value>(0);
        auto* extract = input.Def();
        if (!extract || extract->GetOp() != OpCode::BitExtract ||
            extract->GetArg<Imm>(1).Get() != 0 || extract->GetUses(false) != 1 ||
            extract->GetUses() != 1) {
            continue;
        }
        const auto width = GetValueSizeByte(input.Type());
        if ((width != 1 && width != 2) ||
            GetValueSizeByte(extract->ReturnType()) != width ||
            extract->GetArg<Imm>(2).Get() != width * 8 ||
            GetValueSizeByte(inst.ReturnType()) > sizeof(u32)) {
            continue;
        }
        const auto source = extract->GetArg<Value>(0);
        const auto result_type = inst.ReturnType();
        // Move the source use before allocation so its live range and spill
        // backing include the actual read. Delaying it only in the emitter
        // can read an uninitialized spill slot after a register-only region.
        inst.Reset();
        inst.BitExtract(source, Imm{0u}, Imm{width * 8u}).SetReturn(result_type);
    }
}

}  // namespace

void IntegerWidthEliminationPass::Run(HIRBuilder* hir_builder) {
    for (auto& function : hir_builder->GetHIRFunctions()) {
        Run(&function);
    }
}

void IntegerWidthEliminationPass::Run(HIRFunction* hir_function) {
    for (auto& hir_block : hir_function->GetHIRBlocksRPO()) {
        FoldNarrowExtensions(hir_block.GetBlock());
        FoldDirectExtracts(hir_block.GetBlock());
    }
}

void IntegerWidthEliminationPass::Run(Block* block) {
    FoldNarrowExtensions(block);
    FoldDirectExtracts(block);
}

}  // namespace swift::runtime::ir
