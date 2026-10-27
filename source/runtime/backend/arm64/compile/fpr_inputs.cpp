#include "fpr_inputs.h"

#include <algorithm>

#include "constraints.h"
#include "runtime/common/svm_config.h"
#include "runtime/compile/analysis/facts.h"
#include "runtime/compile/analysis/semantic_facts.h"

namespace swift::runtime::backend::arm64 {
namespace {
using namespace ir;
using Tables = compile::FPRInputs;
using Rule = Tables::MergeRule;

bool IsResidentFPRTarget(u32 reg) {
    // The actual enabled subset is determined by the allocator's reserved
    // FPR mask below.  Keeping one architectural range avoids a parallel
    // high-XMM coalescer and naturally supports the P1 partial map.
    return reg >= 16 && reg <= 31;
}

bool IsScalarFPRBinaryProducer(OpCode op) {
    switch (op) {
        case OpCode::VecFAddScalar32:
        case OpCode::VecFSubScalar32:
        case OpCode::VecFMulScalar32:
        case OpCode::VecFDivScalar32:
        case OpCode::VecFAddScalar64:
        case OpCode::VecFSubScalar64:
        case OpCode::VecFMulScalar64:
        case OpCode::VecFDivScalar64:
            return true;
        default:
            return false;
    }
}

bool IsPublicationProducer(OpCode op) {
    switch (op) {
        // These emitters either write a fresh destination or use a single
        // A64 three-register SIMD instruction. Their source/destination
        // aliasing is architectural; the input-liveness guard below still
        // rejects an input that must survive the publication point.
        case OpCode::LoadUniform:
        case OpCode::LoadMemory:
        case OpCode::LoadMemoryTSO:
        case OpCode::VecAnd:
        case OpCode::VecAndNot:
        case OpCode::VecOr:
        case OpCode::VecXor:
        case OpCode::VecAdd:
        case OpCode::VecSub:
        case OpCode::VecCmpEq:
        case OpCode::VecCmpGt:
        case OpCode::VecMul:
        case OpCode::VecShuffle32Indexed:
        case OpCode::VecExtractBytes:
        case OpCode::VecZip:
        case OpCode::VecFAdd:
        case OpCode::VecFSub:
        case OpCode::VecFMul:
        case OpCode::VecFDiv:
            return true;
        default:
            return IsScalarFPRBinaryProducer(op);
    }
}

bool IsResidentScalarUnaryProducer(const Inst& inst, bool scalar_tie) {
    if (!scalar_tie || inst.GetOp() != OpCode::VecFUnary || inst.GetArg<Imm>(3).Get() != 0 ||
        inst.GetArg<Imm>(4).Get() == 0) {
        return false;
    }
    const u32 lane_bits = inst.GetArg<Imm>(2).Get();
    return lane_bits == 32 || lane_bits == 64;
}

bool IsAesEncChainProducer(OpCode op) {
    return op == OpCode::VecAesEncFast || op == OpCode::VecAesEncLastFast;
}

Value DestructiveTieSource(const Inst* inst, const FeatureSet& features, bool& insert_only) {
    // FEX-style AES rounds are destructive on the state operand.  When
    // that SSA value dies at this instruction, give the result its host
    // register and eliminate the otherwise mandatory vector copy.  The
    // fast opcodes are emitted only under SVM_AES_ZERO_REUSE, so =0
    // restores both the old IR and the old allocation.
    switch (inst->GetOp()) {
        case OpCode::VecAesEncFast:
        case OpCode::VecAesEncLastFast:
        case OpCode::VecAesDecFast:
        case OpCode::VecAesDecLastFast:
            return inst->GetArg<Value>(0);
        default:
            break;
    }
    if (features.sse_shufps_imm && inst->GetOp() == OpCode::VecShuffle32TwoSrc) {
        const u32 control = inst->GetArg<Imm>(2).Get() & 0xffu;
        if (control == 0xe4 || ((control == 0xe0 || control == 0xe5) &&
                                inst->GetArg<Value>(0).Id() == inst->GetArg<Value>(1).Id())) {
            return inst->GetArg<Value>(0);
        }
    }
    insert_only = true;
    switch (inst->GetOp()) {
        case OpCode::VecFAddScalar32:
        case OpCode::VecFSubScalar32:
        case OpCode::VecFMulScalar32:
        case OpCode::VecFDivScalar32:
        case OpCode::VecFAddScalar64:
        case OpCode::VecFSubScalar64:
        case OpCode::VecFMulScalar64:
        case OpCode::VecFDivScalar64:
            return inst->GetArg<Value>(0);
        case OpCode::VecFMinMax:
            if (inst->GetArg<Imm>(4).Get() != 0) {
                return inst->GetArg<Value>(0);
            }
            break;
        case OpCode::VecFUnary:
            if (inst->GetArg<Imm>(4).Get() != 0) {
                return inst->GetArg<Value>(1);
            }
            break;
        default:
            break;
    }
    return {};
}

u32 OperandEnd(const compile::BlockAnalysisIndex& graph, Value value, u32 count) {
    value = ResolveBitCastSource(value);
    if (!value.Defined() || value.Id() >= count) return 0;
    const auto uses = graph.ResolvedUses(value.Def());
    return uses.empty() ? 0 : uses.back().consumer->Id();
}

bool IsFixedMerge(Value value, u32 home) {
    return value.Defined() && value.Def()->GetOp() == OpCode::GetHostFPR &&
           value.Def()->GetArg<Imm>(0).Get() == home && value.Def()->GetArg<Imm>(1).Get() == 0;
}

void AddNode(Tables& tables, const Inst* inst, std::span<const u16> inputs) {
    tables.nodes.push_back(
            {static_cast<u32>(tables.inputs.size()), inst->Id(), static_cast<u16>(inputs.size())});
    tables.inputs.insert(tables.inputs.end(), inputs.begin(), inputs.end());
}

bool DescribeAES(const Block* block, compile::SemanticFacts& facts, u32 count,
                 const FeatureSet& features,
                 const compile::BlockAnalysisIndex& graph,
                 const Inst& store,
                 Value produced,
                 u16 home,
                 Tables& tables) {
    if (!features.ra_aes_chain_tie || !produced.Defined() ||
        !IsAesEncChainProducer(produced.Def()->GetOp()))
        return false;
    Vector<const Inst*> chain;
    Value head{};
    for (auto* node = produced.Def(); node && IsAesEncChainProducer(node->GetOp());) {
        chain.push_back(node);
        auto data = ResolveBitCastSource(node->GetArg<Value>(0));
        if (!data.Defined()) return false;
        if (!IsAesEncChainProducer(data.Def()->GetOp())) {
            head = data;
            break;
        }
        node = data.Def();
    }
    if (chain.size() < 2 || !IsFixedMerge(head, home) || head.Id() >= count) return false;
    std::reverse(chain.begin(), chain.end());
    const auto in_chain = [&](const Inst* value) {
        return std::find(chain.begin(), chain.end(), value) != chain.end();
    };
    for (size_t i = 0; i < chain.size(); ++i) {
        const auto* node = chain[i];
        const auto data = ResolveBitCastSource(node->GetArg<Value>(0));
        const auto expected = i ? chain[i - 1] : head.Def();
        if (data.Def() != expected || data.Id() >= count ||
            OperandEnd(graph, data, count) != node->Id() || node->ReturnType() != ValueType::V128)
            return false;
        const auto* consumer = i + 1 < chain.size() ? chain[i + 1] : &store;
        u32 semantic_uses = 0;
        for (const auto& use : graph.ResolvedUses(node)) {
            if (use.consumer->IsBitCastOperation()) continue;
            if (use.consumer != consumer) return false;
            semantic_uses += use.count;
        }
        if (semantic_uses != 1) return false;
    }
    if (produced.Id() >= count || OperandEnd(graph, produced, count) != store.Id()) return false;
    const auto& accesses = facts.PrepareGuestStateRegisterAccesses(block);
    for (const bool reads : {false, true}) {
        const auto access = accesses.FirstAccess(compile::GuestStateRegisterFile::FPR,
                                                home, reads, chain.front()->Id());
        if (access && *access < store.Id()) return false;
    }
    const auto observers = facts.PrepareGuestStateObservations(block);
    for (const auto* scan : compile::GuestStateObservationWindow(
                 observers, chain.front()->Id(), store.Id())) {
        if (in_chain(scan) || scan->IsBitCastOperation()) continue;
        // A committed AES home can be saved across an ordinary memory fault.
        const bool committed_safe = scan->GetOp() == OpCode::LoadMemory ||
                                    scan->GetOp() == OpCode::StoreMemory ||
                                    scan->GetOp() == OpCode::SaveFlags;
        if (IsFixedGPRPublicationObserver(scan->GetOp()) && !committed_safe) return false;
    }
    const auto first = static_cast<u32>(tables.nodes.size());
    for (const auto* node : chain) {
        StackVector<u16, 2> operands;
        for (size_t at = 1; at < 3; ++at) {
            auto input = ResolveBitCastSource(node->GetArg<Value>(at));
            if (input.Defined() && input.Id() < count) operands.push_back(input.Id());
        }
        AddNode(tables, node, {operands.data(), operands.size()});
    }
    tables.publications.push_back({.first_node = first,
                                   .node_count = static_cast<u16>(chain.size()),
                                   .store = store.Id(),
                                   .home = home,
                                   .head = head.Id(),
                                   .end = store.Id(),
                                   .recolor_safe = true});
    return true;
}

void DescribePublication(const Block* block, compile::SemanticFacts& facts, u32 count,
                         const FeatureSet& features,
                         const compile::BlockAnalysisIndex& graph,
                         const Inst& store,
                         Value produced,
                         u16 home,
                         Tables& tables) {
    if (DescribeAES(block, facts, count, features, graph, store, produced, home, tables)) return;
    const auto* producer = produced.Def();
    if (!produced.Defined() || produced.Id() >= count || produced.Type() != ValueType::V128 ||
        OperandEnd(graph, produced, count) < store.Id() ||
        (!IsPublicationProducer(producer->GetOp()) &&
         !IsResidentScalarUnaryProducer(*producer, features.sse_scalar_tie)))
        return;
    Tables::Publication publication{.first_node = static_cast<u32>(tables.nodes.size()),
                                    .node_count = 1,
                                    .store = store.Id(),
                                    .home = home,
                                    .head = UINT16_MAX,
                                    .end = static_cast<u16>(OperandEnd(graph, produced, count)),
                                    .recolor_safe = true};
    if (IsScalarFPRBinaryProducer(producer->GetOp())) {
        auto left = ResolveBitCastSource(producer->GetArg<Value>(0));
        publication.head = left.Defined() && left.Id() < count ? left.Id() : UINT16_MAX;
        const bool last = left.Defined() && left.Id() < count &&
                          OperandEnd(graph, left, count) == producer->Id();
        publication.merge[0] =
                last && IsFixedMerge(left, home) ? Rule::ReadIfMapped : Rule::RejectIfMapped;
        if (!features.sse_scalar_tie || !last)
            publication.modes &= ~u8{2};
        else
            publication.merge[1] = IsScalarFPRBinaryProducer(left.Def()->GetOp()) ||
                                                   IsResidentScalarUnaryProducer(
                                                           *left.Def(), features.sse_scalar_tie)
                                           ? Rule::Mapped
                                           : Rule::ReadMapped;
    } else if (IsResidentScalarUnaryProducer(*producer, features.sse_scalar_tie)) {
        auto merge = ResolveBitCastSource(producer->GetArg<Value>(1));
        if (!IsFixedMerge(merge, home) || merge.Id() >= count ||
            OperandEnd(graph, merge, count) != producer->Id())
            return;
        publication.head = merge.Id();
        publication.merge = {Rule::ReadMapped, Rule::ReadMapped};
    }
    // A result already in its target home needs no recolor. Keep the
    // publication even if these static recolor windows fail.
    const auto& accesses = facts.PrepareGuestStateRegisterAccesses(block);
    const auto has_access = [&](bool reads, u32 after, u32 before) {
        const auto next = accesses.FirstAccess(compile::GuestStateRegisterFile::FPR,
                                               home, reads, after);
        return next && *next < before;
    };
    if (has_access(false, store.Id(), publication.end + 1)) publication.recolor_safe = false;
    if (graph.Contains(producer)) {
        if (has_access(false, producer->Id(), store.Id()) ||
            has_access(true, producer->Id(), store.Id())) publication.recolor_safe = false;
        const auto observers = facts.PrepareGuestStateObservations(block);
        for (const auto* scan : compile::GuestStateObservationWindow(
                     observers, producer->Id(), store.Id())) {
            if (IsFixedGPRPublicationObserver(scan->GetOp())) publication.recolor_safe = false;
        }
    }
    StackVector<u16, 4> operands;
    producer->ForEachValue([&](Value value) {
        value = ResolveBitCastSource(value);
        if (value.Defined() && value.Id() < count &&
            OperandEnd(graph, value, count) > producer->Id())
            operands.push_back(value.Id());
    });
    AddNode(tables, producer, {operands.data(), operands.size()});
    tables.publications.push_back(publication);
}
}  // namespace

void DescribeFPRInputs(const ir::Block* block,
                       compile::SemanticFacts& facts,
                       u32 count,
                       FPRSMask fprs,
                       const FeatureSet& features,
                       compile::BlockAnalysisIndex& graph,
                       std::unique_ptr<compile::FPRInputs>& output) {
    if (!graph.HasFPRInstructions()) return;
    const auto ensure = [&]() -> Tables& {
        if (!output) {
            output = std::make_unique<Tables>();
            output->resident_homes = 0xffff0000u;
        }
        return *output;
    };
    const auto first_publication = output ? output->publications.size() : 0;
    bool has_ties = false;
    for (const auto* inst : graph.Instructions()) {
        bool insert_only = false;
        auto source = ResolveBitCastSource(DestructiveTieSource(inst, features, insert_only));
        if (source.Defined()) {
            ensure().ties.push_back({inst->Id(), source.Id(), insert_only});
            has_ties = true;
        }
        if (inst->GetOp() != OpCode::SetHostFPR || inst->GetArg<Imm>(2).Get() != 0) continue;
        const auto home = inst->GetArg<Imm>(1).Get();
        if (!IsResidentFPRTarget(home) || !fprs.Get(home)) continue;
        graph.PrepareResolvedUses();
        DescribePublication(block,
                            facts,
                            count,
                            features,
                            graph,
                            *inst,
                            ResolveBitCastSource(inst->GetArg<Value>(0)),
                            home,
                            ensure());
    }
    if (has_ties && features.sse_scalar_tie) {
        u32 mask = 0;
        const auto first_range = output->writes.size();
        const auto nodes = graph.Instructions();
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            const auto* inst = *it;
            if (IsFixedGPRPublicationObserver(inst->GetOp()))
                mask = 0;
            else if (inst->GetOp() == OpCode::SetHostFPR && inst->GetArg<Imm>(2).Get() == 0) {
                const auto home = inst->GetArg<Imm>(1).Get();
                if (home < 32) mask |= u32{1} << home;
            }
            if (!mask) continue;
            auto& ranges = output->writes;
            if (ranges.size() > first_range && ranges.back().mask == mask &&
                ranges.back().begin == inst->Id() + 1)
                ranges.back().begin = inst->Id();
            else
                ranges.push_back({mask, inst->Id(), inst->Id()});
        }
    }
    if (!output || output->publications.size() == first_publication) return;
    const auto first_value = output->values.size();
    for (const auto* inst : graph.Instructions()) {
        if (!inst->HasValue() || inst->IsBitCastOperation()) continue;
        const auto end = OperandEnd(graph, Value{inst}, count);
        if (end) output->values.push_back({inst->Id(), static_cast<u16>(end)});
    }
    output->blocks.push_back({static_cast<u32>(first_value),
                              static_cast<u32>(output->values.size() - first_value),
                              static_cast<u32>(first_publication),
                              static_cast<u32>(output->publications.size() - first_publication)});
}

}  // namespace swift::runtime::backend::arm64
