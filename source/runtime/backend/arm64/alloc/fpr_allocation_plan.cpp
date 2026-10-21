#include "fpr_allocation_plan.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/backend/arm64/alloc/fixed_gpr_constraints.h"
#include "runtime/backend/arm64/pshufd_direct.h"
#include <algorithm>
#include <array>
#include <unordered_set>

namespace swift::runtime::backend::arm64 {
namespace {
struct FPRRegister { u16 slot; u16 GetCode() const { return slot; } };
class FPRAllocationView {
public:
    FPRAllocationView(const RegAlloc& allocation, const FeatureSet& features)
            : allocation(allocation), features(features) {}
    const FeatureSet& GetFeatures() const { return features; }
    FPRRegister V(ir::Value v) const {
        return {static_cast<u16>(v.Defined() && allocation.ValueType(v) == RegAlloc::FPR
                               ? allocation.ValueFPR(v).id : UINT16_MAX)};
    }
    bool SharesFPR(ir::Value a, ir::Value b) const {
        return allocation.SharesFPR(a, b);
    }
    bool IsHostReadCoalesced(u32 id) const { return allocation.IsHostReadCoalesced(id); }
    bool IsHostWriteCoalesced(u32 id) const { return allocation.IsHostWriteCoalesced(id); }
    bool IsAesChainTied(u32 id) const { return allocation.IsAesChainTied(id); }
    bool IsPshufdDirect(u32 id) const { return allocation.IsPshufdDirect(id); }
    u16 AesChainTarget(u32 id) const { return allocation.AesChainTarget(id); }
private:
    const RegAlloc& allocation;
    const FeatureSet& features;
};

ir::Value ResolveHostCoalesceBitCast(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<ir::Value>(0);
    }
    return value;
}

bool IsHostScalarFPRBinaryProducer(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::VecFAddScalar32:
        case O::VecFSubScalar32:
        case O::VecFMulScalar32:
        case O::VecFDivScalar32:
        case O::VecFAddScalar64:
        case O::VecFSubScalar64:
        case O::VecFMulScalar64:
        case O::VecFDivScalar64:
            return true;
        default:
            return false;
    }
}

bool IsHostFPRCoalesceProducer(ir::OpCode op, bool scalar_insert,
                              bool scalar_tie) {
    using O = ir::OpCode;
    switch (op) {
        case O::LoadUniform:
        case O::LoadMemory:
        case O::VecAnd:
        case O::VecOr:
        case O::VecXor:
        case O::VecAdd:
        case O::VecSub:
        case O::VecCmpEq:
        case O::VecCmpGt:
        case O::VecMul:
        case O::VecShuffle32Indexed:
        case O::VecExtractBytes:
        case O::VecZip:
        case O::VecFAdd:
        case O::VecFSub:
        case O::VecFMul:
        case O::VecFDiv:
            return true;
        default:
            return IsHostScalarFPRBinaryProducer(op) &&
                   (!scalar_insert || scalar_tie);
    }
}

bool IsHostScalarUnaryProducer(const ir::Inst& inst,
                               bool scalar_tie) {
    if (!scalar_tie || inst.GetOp() != ir::OpCode::VecFUnary ||
        inst.GetArg<ir::Imm>(3).Get() != 0 ||
        inst.GetArg<ir::Imm>(4).Get() == 0) {
        return false;
    }
    const u32 lane_bits = inst.GetArg<ir::Imm>(2).Get();
    return lane_bits == 32 || lane_bits == 64;
}

bool IsHostCoalesceObserver(const ir::Inst& inst, const FeatureSet& features) {
    return IsFixedGPRPublicationObserver(inst, features);
}

class FPRPlanBuilder {
public:
    FPRPlanBuilder(ir::Block* block, const RegAlloc& allocation, const FeatureSet& features,
                   bool scalar_insert, bool scalar_tie, const BlockAnalysisIndex* analysis)
            : cur_block(block), context(allocation, features), sse_scalar_insert(scalar_insert),
              sse_scalar_tie(scalar_tie) {
        if (analysis) index = analysis;
        else { owned_index.Build(block, &compact_uses); index = &owned_index; }
        const auto instructions = index->Instructions();
        first_id = instructions.empty() ? 0 : instructions.front()->Id();
        count = instructions.empty() ? 0 : instructions.back()->Id() - first_id + 1;
    }
    FPRAllocationPlan Build();
private:
    enum class Kind { Read, AesTie, AesWrite, Write, ScalarTie, Count };
    enum class State : u8 { Unseen, Visiting, Rejected, Accepted };
    std::vector<std::array<State, static_cast<size_t>(Kind::Count)>> cache;
    u32 first_id{};
    u32 count{};
    template<class Compute> bool Cached(Kind kind, ir::Inst* node, Compute compute) {
        if (!node) return false;
        if (node->Id() < first_id || node->Id() - first_id >= count) return false;
        if (cache.empty()) cache.resize(count);
        auto& state = cache[node->Id() - first_id][static_cast<size_t>(kind)];
        if (state != State::Unseen) return state == State::Accepted;
        state = State::Visiting;
        const bool accepted = compute();
        state = accepted ? State::Accepted : State::Rejected;
        return accepted;
    }
    bool ReproveCoalescedHostFPRRead(ir::Inst* node) { return Cached(Kind::Read, node, [&] { return ComputeCoalescedHostFPRRead(node); }); }
    bool ComputeCoalescedHostFPRRead(ir::Inst* inst);
    bool ReproveAesChainTie(ir::Inst* node) { return Cached(Kind::AesTie, node, [&] { return ComputeAesChainTie(node); }); }
    bool ComputeAesChainTie(ir::Inst* inst);
    bool ReproveAesChainHostWrite(ir::Inst* node) { return Cached(Kind::AesWrite, node, [&] { return ComputeAesChainHostWrite(node); }); }
    bool ComputeAesChainHostWrite(ir::Inst* inst);
    bool ReproveCoalescedHostFPRWrite(ir::Inst* node) { return Cached(Kind::Write, node, [&] { return ComputeCoalescedHostFPRWrite(node); }); }
    bool ComputeCoalescedHostFPRWrite(ir::Inst* inst);
    bool ReproveScalarFPRTie(ir::Inst* node) { return Cached(Kind::ScalarTie, node, [&] { return ComputeScalarFPRTie(node); }); }
    bool ComputeScalarFPRTie(ir::Inst* inst);
    const std::vector<ir::Inst*>& Owners(u32 target) {
        if (!owners_ready[target]) {
            for (auto* inst : index->Instructions())
                if (inst->HasValue() && !inst->IsBitCastOperation() &&
                    context.V(ir::Value{inst}).GetCode() == target) owners[target].push_back(inst);
            owners_ready[target] = true;
        }
        return owners[target];
    }
    ir::Block* cur_block;
    FPRAllocationView context;
    bool sse_scalar_insert, sse_scalar_tie;
    BlockAnalysisIndex owned_index;
    const BlockAnalysisIndex* index{};
    BlockAnalysisIndex::CompactUses compact_uses;
    std::array<std::vector<ir::Inst*>, 32> owners;
    std::array<bool, 32> owners_ready{};
};

bool FPRPlanBuilder::ComputeCoalescedHostFPRRead(ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::GetHostFPR ||
        inst->GetArg<ir::Imm>(1).Get() != 0 ||
        !ir::IsFloatValueType(inst->ReturnType())) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(0).Get();
    if (target >= 32 || context.V(ir::Value{inst}).GetCode() != target) {
        return false;
    }
    const u32 end = index->LastInstructionUse(inst);
    for (auto* instruction : index->Between(inst->Id(), end + 1)) {
        auto& scan = *instruction;
        if (scan.GetOp() == ir::OpCode::SetHostFPR &&
            scan.GetArg<ir::Imm>(1).Get() == target) {
            return false;
        }
    }
    return true;
}

bool FPRPlanBuilder::ComputeAesChainTie(ir::Inst* inst) {
    if (!context.GetFeatures().ra_aes_chain_tie || !inst ||
        (inst->GetOp() != ir::OpCode::VecAesEncFast &&
         inst->GetOp() != ir::OpCode::VecAesEncLastFast) ||
        !context.IsAesChainTied(inst->Id())) {
        return false;
    }
    const u32 target = context.AesChainTarget(inst->Id());
    auto data = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(0));
    auto key = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(1));
    auto zero = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(2));
    if (!data.Defined() || !data.Def() || target < 16 || target > 31 ||
        context.V(ir::Value{inst}).GetCode() != target ||
        context.V(data).GetCode() != target ||
        context.V(key).GetCode() == target ||
        context.V(zero).GetCode() == target) {
        return false;
    }

    const u32 data_end = index->LastInstructionUse(data.Def());
    ir::Inst* semantic_consumer = nullptr;
    u32 semantic_uses = 0;
    for (const auto& use : index->ResolvedUses(inst)) {
        if (!use.consumer->IsBitCastOperation()) {
            semantic_consumer = use.consumer;
            semantic_uses += use.count;
        }
    }
    if (data_end != inst->Id() || semantic_uses != 1 || !semantic_consumer) {
        return false;
    }

    const bool predecessor_is_chain =
            (data.Def()->GetOp() == ir::OpCode::VecAesEncFast ||
             data.Def()->GetOp() == ir::OpCode::VecAesEncLastFast) &&
            context.IsAesChainTied(data.Id()) &&
            context.AesChainTarget(data.Id()) == target;
    const bool predecessor_is_home =
            data.Def()->GetOp() == ir::OpCode::GetHostFPR &&
            data.Def()->GetArg<ir::Imm>(0).Get() == target &&
            data.Def()->GetArg<ir::Imm>(1).Get() == 0 &&
            context.IsHostReadCoalesced(data.Id());
    if (!predecessor_is_chain && !predecessor_is_home) {
        return false;
    }

    const bool successor_is_chain =
            (semantic_consumer->GetOp() == ir::OpCode::VecAesEncFast ||
             semantic_consumer->GetOp() == ir::OpCode::VecAesEncLastFast) &&
            context.IsAesChainTied(semantic_consumer->Id()) &&
            context.AesChainTarget(semantic_consumer->Id()) == target &&
            ResolveHostCoalesceBitCast(
                    semantic_consumer->GetArg<ir::Value>(0)).Def() == inst;
    const bool successor_is_store =
            semantic_consumer->GetOp() == ir::OpCode::SetHostFPR &&
            semantic_consumer->GetArg<ir::Imm>(1).Get() == target &&
            semantic_consumer->GetArg<ir::Imm>(2).Get() == 0 &&
            context.IsHostWriteCoalesced(semantic_consumer->Id());
    // A single producer is deliberately outside this spike: the component
    // must own both the initial read and final publication, not just move one
    // of the two copies to the other side of the AES instruction.
    return (predecessor_is_home && successor_is_chain) ||
           (predecessor_is_chain && (successor_is_chain || successor_is_store));
}

bool FPRPlanBuilder::ComputeAesChainHostWrite(ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::SetHostFPR ||
        inst->GetArg<ir::Imm>(2).Get() != 0) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    if (target >= 32) return false;
    auto produced = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(0));
    if (!produced.Defined() || !produced.Def() ||
        !context.IsAesChainTied(produced.Id()) ||
        context.AesChainTarget(produced.Id()) != target ||
        context.V(produced).GetCode() != target) {
        return false;
    }

    std::vector<ir::Inst*> reverse_chain;
    auto data = produced;
    while (data.Defined() && data.Def() && context.IsAesChainTied(data.Id())) {
        auto* node = data.Def();
        if (!ReproveAesChainTie(node)) {
            return false;
        }
        reverse_chain.push_back(node);
        data = ResolveHostCoalesceBitCast(node->GetArg<ir::Value>(0));
    }
    if (reverse_chain.size() < 2 || !data.Defined() || !data.Def() ||
        data.Def()->GetOp() != ir::OpCode::GetHostFPR ||
        data.Def()->GetArg<ir::Imm>(0).Get() != target ||
        data.Def()->GetArg<ir::Imm>(1).Get() != 0 ||
        !context.IsHostReadCoalesced(data.Id()) ||
        context.V(data).GetCode() != target) {
        return false;
    }
    std::reverse(reverse_chain.begin(), reverse_chain.end());
    std::unordered_set<const ir::Inst*> chain_nodes;
    if (reverse_chain.size() > 8) chain_nodes.insert(reverse_chain.begin(), reverse_chain.end());
    auto is_chain_node = [&](const ir::Inst* candidate) {
        return reverse_chain.size() <= 8
                ? std::find(reverse_chain.begin(), reverse_chain.end(), candidate) != reverse_chain.end()
                : chain_nodes.contains(candidate);
    };

    const auto last_use = [&](ir::Inst* definition) { return index->LastInstructionUse(definition); };
    if (last_use(produced.Def()) != inst->Id()) {
        return false;
    }
    const u32 begin = reverse_chain.front()->Id();
    for (auto* owner : Owners(target)) {
        auto& other = *owner;
        if (&other == inst || &other == data.Def() || is_chain_node(&other) ||
            !other.HasValue() || other.IsBitCastOperation() || other.Id() >= inst->Id()) {
            continue;
        }
        ir::Value value{&other};
        if (context.SharesFPR(value, produced) && last_use(&other) > begin) {
            return false;
        }
    }
    for (auto* instruction : index->Between(begin, inst->Id())) {
        auto& scan = *instruction;
        if (is_chain_node(&scan) || scan.IsBitCastOperation()) {
            continue;
        }
        if ((scan.GetOp() == ir::OpCode::GetHostFPR &&
             scan.GetArg<ir::Imm>(0).Get() == target) ||
            (scan.GetOp() == ir::OpCode::SetHostFPR &&
             scan.GetArg<ir::Imm>(1).Get() == target)) {
            return false;
        }
        const bool committed_home_safe =
                scan.GetOp() == ir::OpCode::LoadMemory ||
                scan.GetOp() == ir::OpCode::StoreMemory ||
                scan.GetOp() == ir::OpCode::SaveFlags;
        if (IsHostCoalesceObserver(scan, context.GetFeatures()) && !committed_home_safe) {
            return false;
        }
    }
    return true;
}

bool FPRPlanBuilder::ComputeCoalescedHostFPRWrite(ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::SetHostFPR ||
        inst->GetArg<ir::Imm>(2).Get() != 0) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    if (target >= 32) return false;
    auto produced = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(0));
    auto* producer = produced.Def();
    const bool scalar_unary = producer &&
            IsHostScalarUnaryProducer(*producer, sse_scalar_tie);
    if (producer && context.IsAesChainTied(produced.Id())) {
        return ReproveAesChainHostWrite(inst);
    }
    if (!producer || produced.Type() != ir::ValueType::V128 ||
        (!IsHostFPRCoalesceProducer(producer->GetOp(), sse_scalar_insert,
                                    sse_scalar_tie) &&
         !scalar_unary) ||
        context.V(produced).GetCode() != target) {
        return false;
    }

    const auto last_use = [&](ir::Inst* definition) { return index->LastInstructionUse(definition); };
    const u32 produced_end = last_use(producer);
    if (produced_end < inst->Id()) {
        return false;
    }
    if (IsHostScalarFPRBinaryProducer(producer->GetOp())) {
        auto left = ResolveHostCoalesceBitCast(producer->GetArg<ir::Value>(0));
        if (sse_scalar_insert) {
            if (!left.Defined() || context.V(left).GetCode() != target ||
                !ReproveScalarFPRTie(producer)) {
                return false;
            }
        } else if (left.Defined() && context.SharesFPR(left, produced)) {
            if (!left.Def() ||
                left.Def()->GetOp() != ir::OpCode::GetHostFPR ||
                left.Def()->GetArg<ir::Imm>(0).Get() != target ||
                left.Def()->GetArg<ir::Imm>(1).Get() != 0 ||
                !context.IsHostReadCoalesced(left.Id()) ||
                last_use(left.Def()) != producer->Id()) {
                return false;
            }
        }
    } else if (scalar_unary) {
        auto merge = ResolveHostCoalesceBitCast(
                producer->GetArg<ir::Value>(1));
        if (!merge.Defined() || !merge.Def() ||
            merge.Def()->GetOp() != ir::OpCode::GetHostFPR ||
            merge.Def()->GetArg<ir::Imm>(0).Get() != target ||
            merge.Def()->GetArg<ir::Imm>(1).Get() != 0 ||
            !context.IsHostReadCoalesced(merge.Id()) ||
            context.V(merge).GetCode() != target ||
            last_use(merge.Def()) != producer->Id()) {
            return false;
        }
    }
    for (auto input : producer->GetValues()) {
        auto root = ResolveHostCoalesceBitCast(input);
        if (root.Defined() && context.SharesFPR(root, produced) &&
            last_use(root.Def()) > producer->Id()) {
            return false;
        }
    }
    auto is_in_place_successor = [&](ir::Inst& successor) {
        if (successor.Id() <= producer->Id() || successor.Id() != produced_end) {
            return false;
        }
        const auto successor_values = successor.GetValues();
        const bool consumes_produced = std::any_of(
                successor_values.begin(), successor_values.end(), [producer](ir::Value value) {
                    return ResolveHostCoalesceBitCast(value).Def() == producer;
                });
        if (!consumes_produced) {
            return false;
        }
        for (const auto& use : index->ResolvedUses(&successor)) {
            auto& scan = *use.consumer;
            if (scan.Id() <= successor.Id() || scan.GetOp() != ir::OpCode::SetHostFPR ||
                scan.GetArg<ir::Imm>(1).Get() != target || scan.GetArg<ir::Imm>(2).Get() != 0 ||
                !context.IsHostWriteCoalesced(scan.Id())) {
                continue;
            }
            if (ResolveHostCoalesceBitCast(scan.GetArg<ir::Value>(0)).Def() == &successor) {
                return ReproveCoalescedHostFPRWrite(&scan);
            }
        }
        return false;
    };
    for (auto* owner : Owners(target)) {
        auto& other = *owner;
        if (&other == producer || &other == inst || !other.HasValue() ||
            other.IsBitCastOperation() || other.Id() > produced_end) {
            continue;
        }
        ir::Value value{&other};
        if (context.SharesFPR(value, produced) && last_use(&other) > producer->Id() &&
            !is_in_place_successor(other)) {
            return false;
        }
    }
    for (auto* instruction : index->Between(inst->Id(), produced_end + 1)) {
        auto& scan = *instruction;
        if (scan.GetOp() == ir::OpCode::SetHostFPR &&
            scan.GetArg<ir::Imm>(1).Get() == target) {
            return false;
        }
    }
    for (auto* instruction : index->Between(producer->Id(), inst->Id())) {
        auto& scan = *instruction;
        if (IsHostCoalesceObserver(scan, context.GetFeatures()) ||
            (scan.GetOp() == ir::OpCode::GetHostFPR &&
             scan.GetArg<ir::Imm>(0).Get() == target) ||
            (scan.GetOp() == ir::OpCode::SetHostFPR &&
             scan.GetArg<ir::Imm>(1).Get() == target)) {
            return false;
        }
    }
    return true;
}

bool FPRPlanBuilder::ComputeScalarFPRTie(ir::Inst* inst) {
    if (!sse_scalar_tie || !sse_scalar_insert || !inst ||
        !IsHostScalarFPRBinaryProducer(inst->GetOp())) {
        return false;
    }
    const u32 target = context.V(ir::Value{inst}).GetCode();
    if (target < 16 || target > 31) {
        return false;
    }
    const auto last_use = [&](ir::Inst* definition) { return index->LastInstructionUse(definition); };
    auto crosses_write = [&](u32 begin, u32 end, ir::Inst* owner) {
        for (auto* instruction : index->Between(begin, end)) {
            auto& scan = *instruction;
            if (scan.GetOp() == ir::OpCode::SetHostFPR &&
                scan.GetArg<ir::Imm>(1).Get() == target &&
                ResolveHostCoalesceBitCast(scan.GetArg<ir::Value>(0)).Def() != owner) {
                return true;
            }
        }
        return false;
    };
    const auto result_end = last_use(inst);
    if (crosses_write(inst->Id(), result_end, inst)) {
        return false;
    }
    for (auto* owner : Owners(target)) {
        auto& scan = *owner;
        if (scan.GetOp() == ir::OpCode::GetHostFPR &&
            context.IsHostReadCoalesced(scan.Id()) &&
            scan.GetArg<ir::Imm>(0).Get() == target &&
            scan.Id() < result_end && last_use(&scan) > inst->Id()) {
            return false;
        }
    }
    auto tie_source = [&](ir::Inst* node) -> ir::Value {
        if (IsHostScalarFPRBinaryProducer(node->GetOp())) {
            return node->GetArg<ir::Value>(0);
        }
        if (IsHostScalarUnaryProducer(*node, sse_scalar_tie)) {
            return node->GetArg<ir::Value>(1);
        }
        return {};
    };
    for (auto* node = inst;;) {
        auto left = ResolveHostCoalesceBitCast(tie_source(node));
        if (!left.Defined() || !left.Def() || context.V(left).GetCode() != target ||
            last_use(left.Def()) != node->Id() ||
            crosses_write(left.Id(), node->Id(), left.Def())) {
            return false;
        }
        if (left.Def()->GetOp() == ir::OpCode::GetHostFPR) {
            return left.Def()->GetArg<ir::Imm>(0).Get() == target &&
                   left.Def()->GetArg<ir::Imm>(1).Get() == 0 &&
                   context.IsHostReadCoalesced(left.Id());
        }
        if (!IsHostScalarFPRBinaryProducer(left.Def()->GetOp()) &&
            !IsHostScalarUnaryProducer(*left.Def(), sse_scalar_tie)) {
            for (const auto& use : index->ResolvedUses(left.Def())) {
                auto& scan = *use.consumer;
                if (scan.Id() <= left.Id() || scan.Id() >= node->Id() ||
                    scan.GetOp() != ir::OpCode::SetHostFPR ||
                    scan.GetArg<ir::Imm>(1).Get() != target ||
                    !context.IsHostWriteCoalesced(scan.Id())) {
                    continue;
                }
                auto value = ResolveHostCoalesceBitCast(scan.GetArg<ir::Value>(0));
                if (value.Def() == left.Def()) {
                    return ReproveCoalescedHostFPRWrite(&scan);
                }
            }
            return false;
        }
        if (IsHostScalarFPRBinaryProducer(left.Def()->GetOp()))
            return ReproveScalarFPRTie(left.Def());
        node = left.Def();
    }
}

FPRAllocationPlan FPRPlanBuilder::Build() {
    FPRAllocationPlan plan;
    plan.first_id = first_id;
    plan.actions.resize(count);
    for (auto& inst : cur_block->GetInstList()) {
        const auto value = ir::Value{&inst};
        const auto reject = [&] { plan.valid = false; plan.rejected = &inst; };
        if (inst.GetOp() == ir::OpCode::VecLoadConst && context.IsPshufdDirect(inst.Id())) {
            const auto control = DecodePshufdDirectControl(inst.GetArg<ir::Imm>(0).Get(), inst.GetArg<ir::Imm>(1).Get());
            u32 uses = 0;
            bool exact = control.has_value();
            for (const auto& use : index->Uses(&inst)) {
                const auto* consumer = use.consumer;
                uses += use.count;
                exact &= use.count == 1 && consumer->GetOp() == ir::OpCode::VecShuffle32Indexed &&
                        consumer->GetArg<ir::Value>(1).Def() == &inst &&
                        consumer->GetArg<ir::Value>(0).Def() != &inst && context.IsPshufdDirect(consumer->Id());
            }
            if (!exact || uses == 0 || uses != inst.GetUses()) { reject(); return plan; }
            plan.SetControl(&inst, *control);
        }
        if (inst.GetOp() == ir::OpCode::VecShuffle32Indexed && context.IsPshufdDirect(inst.Id())) {
            const auto control = plan.Control(inst.GetArg<ir::Value>(1).Def());
            if (!control) { reject(); return plan; }
            plan.SetControl(&inst, *control);
        }
        if (inst.GetOp() == ir::OpCode::VecShuffle32TwoSrc && context.GetFeatures().sse_shufps_imm &&
            context.SharesFPR(value, inst.GetArg<ir::Value>(0))) {
            const auto control = inst.GetArg<ir::Imm>(2).Get() & 0xffu;
            const bool same_source = inst.GetArg<ir::Value>(0).Id() == inst.GetArg<ir::Value>(1).Id();
            if (control == 0xe4 || ((control == 0xe0 || control == 0xe5) && same_source)) {
                const auto left = ResolveHostCoalesceBitCast(inst.GetArg<ir::Value>(0));
                if (!left.Defined() || context.IsHostReadCoalesced(left.Id()) ||
                    index->LastInstructionUse(left.Def()) != inst.Id()) { reject(); return plan; }
                plan.Certify(&inst, FPRAllocationPlan::ShuffleTie);
            }
        }
        if (inst.GetOp() == ir::OpCode::GetHostFPR && context.IsHostReadCoalesced(inst.Id())) {
            if (!ReproveCoalescedHostFPRRead(&inst)) { reject(); return plan; }
            plan.Certify(&inst, FPRAllocationPlan::Read);
        }
        if ((inst.GetOp() == ir::OpCode::VecAesEncFast || inst.GetOp() == ir::OpCode::VecAesEncLastFast) &&
            context.IsAesChainTied(inst.Id())) {
            if (!ReproveAesChainTie(&inst)) { reject(); return plan; }
            plan.Certify(&inst, FPRAllocationPlan::AesTie);
        }
        if (sse_scalar_insert && sse_scalar_tie && IsHostScalarFPRBinaryProducer(inst.GetOp()) &&
            context.V(value).GetCode() >= 16 && context.V(value).GetCode() < 32 &&
            context.SharesFPR(value, inst.GetArg<ir::Value>(0))) {
            if (!ReproveScalarFPRTie(&inst)) { reject(); return plan; }
            plan.Certify(&inst, FPRAllocationPlan::ScalarTie);
        }
        if (inst.GetOp() == ir::OpCode::SetHostFPR && context.IsHostWriteCoalesced(inst.Id())) {
            if (!ReproveCoalescedHostFPRWrite(&inst)) { reject(); return plan; }
            plan.Certify(&inst, FPRAllocationPlan::Write);
        }
    }
    return plan;
}
}
FPRAllocationPlan BuildFPRAllocationPlan(ir::Block* block, const RegAlloc& allocation,
                                         const FeatureSet& features, bool scalar_insert, bool scalar_tie,
                                         const BlockAnalysisIndex* analysis) {
    const FPRAllocationView context{allocation, features};
    const auto selected = [&](ir::Inst& inst) {
        const auto op = inst.GetOp();
        switch (op) {
            case ir::OpCode::GetHostFPR:
                return allocation.IsHostReadCoalesced(inst.Id());
            case ir::OpCode::SetHostFPR:
                return allocation.IsHostWriteCoalesced(inst.Id());
            case ir::OpCode::VecAesEncFast:
            case ir::OpCode::VecAesEncLastFast:
                return allocation.IsAesChainTied(inst.Id());
            case ir::OpCode::VecLoadConst:
            case ir::OpCode::VecShuffle32Indexed:
                return allocation.IsPshufdDirect(inst.Id());
            case ir::OpCode::VecShuffle32TwoSrc:
                return features.sse_shufps_imm &&
                       context.SharesFPR(ir::Value{&inst}, inst.GetArg<ir::Value>(0));
            default:
                return scalar_insert && scalar_tie && IsHostScalarFPRBinaryProducer(op);
        }
    };
    bool needed = false;
    if (analysis) {
        needed = std::ranges::any_of(analysis->Instructions(), [&](ir::Inst* inst) { return selected(*inst); });
    } else for (auto& inst : block->GetInstList()) {
        if (selected(inst)) { needed = true; break; }
    }
    return needed ? FPRPlanBuilder{block, allocation, features, scalar_insert, scalar_tie, analysis}.Build()
                  : FPRAllocationPlan{};
}

void SelectPshufdDirect(const BlockAnalysisIndex& analysis, RegAlloc& allocation) {
    if (allocation.FPRCoalescingDisabled()) return;
    for (auto* constant : analysis.Instructions()) {
        if (constant->GetOp() != ir::OpCode::VecLoadConst ||
            !DecodePshufdDirectControl(constant->GetArg<ir::Imm>(0).Get(), constant->GetArg<ir::Imm>(1).Get())) continue;
        const auto uses = analysis.Uses(constant);
        u32 local_uses = 0;
        bool exact = !uses.empty();
        for (const auto& use : uses) {
            const auto* consumer = use.consumer;
            local_uses += use.count;
            exact &= use.count == 1 && consumer->GetOp() == ir::OpCode::VecShuffle32Indexed &&
                     consumer->GetArg<ir::Value>(1).Def() == constant &&
                     consumer->GetArg<ir::Value>(0).Def() != constant;
        }
        if (!exact || local_uses != constant->GetUses()) continue;
        allocation.MarkPshufdDirect(constant->Id());
        for (const auto& use : uses) allocation.MarkPshufdDirect(use.consumer->Id());
    }
}
}

