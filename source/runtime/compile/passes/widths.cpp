#include "passes.h"
#include "runtime/compile/workspace.h"
#include "runtime/ir/hir_builder.h"
#include "value_worklist.h"

namespace swift::runtime::compile {
using namespace ir;

namespace {

constexpr u32 kMaxSourceDistance = 128;

bool ReplaceSingleUse(RewriteContext& rewrite,
                      Block* block,
                      const Inst* consumer,
                      const Inst* definition,
                      Value replacement) {
    for (u32 index = 0; index < Inst::max_args; ++index) {
        auto& arg = consumer->ArgAt(index);
        if (arg.IsValue() && arg.Get<Value>().Def() == definition) {
            rewrite.SetArg(block, consumer, index, arg.Get<Value>().WithDefinition(replacement.Def()));
            return true;
        }
        if (arg.IsLambda() && arg.Get<Lambda>().IsValue() &&
            arg.Get<Lambda>().GetValue().Def() == definition) {
            auto lambda = arg.Get<Lambda>();
            lambda.GetValue() = lambda.GetValue().WithDefinition(replacement.Def());
            rewrite.SetArg(block, consumer, index, lambda);
            return true;
        }
    }
    return false;
}

Value DirectSource(const Inst* inst) {
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

bool IsU32Consumer(const Inst* consumer, const Inst* definition) {
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

void FoldDirectExtract(RewriteContext& rewrite,
                       ValueWorklist& work,
                       Block* block,
                       const Inst& inst) {
    if (inst.GetArg<Imm>(1).Get() != 0 ||
        inst.GetArg<Imm>(2).Get() != GetValueSizeByte(inst.ReturnType()) * 8 ||
        inst.GetUses(false) != 1)
        return;
    const auto source = DirectSource(&inst);
    if (!source.Defined() || !work.Nearby(source.Def(), &inst, kMaxSourceDistance)) return;
    const auto* consumer = work.SingleConsumer(&inst);
    if (consumer && IsU32Consumer(consumer, &inst))
        ReplaceSingleUse(rewrite, block, consumer, &inst, source);
}

void FoldNarrowExtension(RewriteContext& rewrite, Block* block, const Inst& inst) {
    const auto input = inst.GetArg<Value>(0);
    const auto* extract = input.Def();
    if (!extract || extract->GetOp() != OpCode::BitExtract || extract->GetArg<Imm>(1).Get() != 0 ||
        extract->GetUses(false) != 1 || extract->GetUses() != 1)
        return;
    const auto width = GetValueSizeByte(input.Type());
    if ((width != 1 && width != 2) || GetValueSizeByte(extract->ReturnType()) != width ||
        extract->GetArg<Imm>(2).Get() != width * 8 ||
        GetValueSizeByte(inst.ReturnType()) > sizeof(u32))
        return;
    const auto source = extract->GetArg<Value>(0);
    const auto result_type = inst.ReturnType();
    // Rewrite the actual read before allocation, so the source's live range
    // and spill backing include it instead of deferring this to the emitter.
    rewrite.Change(block, &inst, [&](Inst& writer) {
        writer.Reset();
        writer.BitExtract(source, Imm{0u}, Imm{width * 8u}).SetReturn(result_type);
    });
}

void FoldNarrowMaskedExtract(RewriteContext& rewrite,
                             ValueWorklist& work,
                             Block* block,
                             const Inst& inst) {
    if (inst.HasPseudoOperations()) return;
    const auto input = inst.GetArg<Value>(0);
    const auto* extract = input.Def();
    const auto width = GetValueSizeByte(inst.ReturnType());
    if ((width != 1 && width != 2) || !extract || extract->GetOp() != OpCode::BitExtract ||
        extract->GetUses() != 1 || extract->GetUses(false) != 1 ||
        GetValueSizeByte(input.Type()) != width || extract->GetArg<Imm>(1).Get() != 0 ||
        extract->GetArg<Imm>(2).Get() != width * 8 ||
        !work.Nearby(extract, &inst, kMaxSourceDistance))
        return;
    const auto source = extract->GetArg<Value>(0);
    if (!source.Defined()) return;
    const auto source_width = GetValueSizeByte(source.Type());
    if (source_width != 4 && source_width != 8) return;
    const auto right = inst.GetArg<Operand>(1);
    u64 mask{};
    if (right.IsImm()) {
        mask = right.GetLeft().imm.Get();
    } else if (right.GetRight().Null() && right.GetLeft().IsValue() &&
               right.GetLeft().value.Defined() &&
               right.GetLeft().value.Def()->GetOp() == OpCode::LoadImm) {
        mask = right.GetLeft().value.Def()->GetArg<Imm>(0).Get();
    } else
        return;
    if ((mask >> (width * 8)) != 0) return;
    rewrite.SetArg(block, &inst, 0, Value{source}.SetCastType(ValueType::U32));
}

}  // namespace

void SimplifyWidth(RewriteContext& rewrite, ValueWorklist& work, Block* block, const Inst* inst) {
    if (inst->GetOp() == OpCode::ZeroExtend32) FoldNarrowExtension(rewrite, block, *inst);
    if (inst->GetOp() == OpCode::And) FoldNarrowMaskedExtract(rewrite, work, block, *inst);
    if (inst->GetOp() == OpCode::BitExtract) FoldDirectExtract(rewrite, work, block, *inst);
}

void NormalizeWidths(RewriteContext& rewrite) {
    const FeatureSet features{};
    const BuilderCapabilities target{};
    std::optional<ValueWorklist> cleanup;
    BeginValueCleanup(cleanup, rewrite, features, target, ValueCleanup::Widths);
}

}  // namespace swift::runtime::compile
