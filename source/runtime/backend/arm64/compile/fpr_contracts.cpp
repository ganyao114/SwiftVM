#include <algorithm>
#include <unordered_set>
#include "fpr_contracts.h"
#include "runtime/backend/arm64/compile/constraints.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/backend/arm64/pshufd_direct.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/facts.h"
#include "runtime/compile/analysis/guest_state.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::backend::arm64 {

const FPRContract::Instruction* FPRContract::Find(u32 id) const {
    const auto found = std::lower_bound(
            instructions.begin(), instructions.end(), id, [](const auto& row, u32 value) {
                return row.id < value;
            });
    return found != instructions.end() && found->id == id ? &*found : nullptr;
}
void FPRContract::Certify(u32 id, FPRAction action) {
    ASSERT(id <= UINT16_MAX);
    if (!instructions.empty() && instructions.back().id == id)
        instructions.back().actions = static_cast<FPRAction>(
                static_cast<u8>(instructions.back().actions) | static_cast<u8>(action));
    else {
        ASSERT(instructions.empty() || instructions.back().id < id);
        instructions.push_back({static_cast<u16>(id), action});
    }
}
void FPRContract::SetControl(u32 id, u8 control) {
    Certify(id, FPRAction::Pshufd);
    instructions.back().control = control;
}
bool FPRContract::Has(u32 id, FPRAction action) const {
    const auto* instruction = Find(id);
    return instruction && (static_cast<u8>(instruction->actions) & static_cast<u8>(action));
}
std::optional<u8> FPRContract::Control(u32 id) const {
    const auto* instruction = Find(id);
    if (!instruction ||
        !(static_cast<u8>(instruction->actions) & static_cast<u8>(FPRAction::Pshufd)))
        return std::nullopt;
    return instruction->control;
}

namespace {
struct FPRRegister {
    u16 slot;
    u16 GetCode() const { return slot; }
};
class FPRAllocationView {
public:
    FPRAllocationView(compile::AllocationView allocation, const FeatureSet& features)
            : allocation(allocation), features(features) {}
    const FeatureSet& GetFeatures() const { return features; }
    FPRRegister V(ir::Value v) const {
        return {static_cast<u16>(v.Defined() && allocation.ValueType(v) ==
                                                         compile::AllocationView::FPR
                                         ? allocation.ValueFPR(v).id
                                         : UINT16_MAX)};
    }
    bool SharesFPR(ir::Value a, ir::Value b) const { return allocation.SharesFPR(a, b); }
    bool IsHostReadCoalesced(u32 id) const { return allocation.IsHostReadCoalesced(id); }
    bool IsHostWriteCoalesced(u32 id) const { return allocation.IsHostWriteCoalesced(id); }
    bool IsAesChainTied(u32 id) const { return allocation.IsAesChainTied(id); }
    bool IsPshufdDirect(u32 id) const { return allocation.IsPshufdDirect(id); }
    u16 AesChainTarget(u32 id) const { return allocation.AesChainTarget(id); }

private:
    compile::AllocationView allocation;
    const FeatureSet& features;
};

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

bool IsHostFPRCoalesceProducer(ir::OpCode op, bool scalar_insert, bool scalar_tie) {
    using O = ir::OpCode;
    switch (op) {
        case O::LoadUniform:
        case O::LoadMemory:
        case O::LoadMemoryTSO:
        case O::VecAnd:
        case O::VecAndNot:
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
            return IsHostScalarFPRBinaryProducer(op) && (!scalar_insert || scalar_tie);
    }
}

bool IsHostScalarUnaryProducer(const ir::Inst& inst, bool scalar_tie) {
    if (!scalar_tie || inst.GetOp() != ir::OpCode::VecFUnary ||
        inst.GetArg<ir::Imm>(3).Get() != 0 || inst.GetArg<ir::Imm>(4).Get() == 0) {
        return false;
    }
    const u32 lane_bits = inst.GetArg<ir::Imm>(2).Get();
    return lane_bits == 32 || lane_bits == 64;
}

bool IsHostCoalesceObserver(const ir::Inst& inst, const FeatureSet& features) {
    return IsFixedGPRPublicationObserver(inst, features);
}

class FPRContractBuilder {
public:
    FPRContractBuilder(compile::AllocationView allocation,
                       const FeatureSet& features,
                       bool scalar_insert,
                       bool scalar_tie,
                       const compile::BlockAnalysisIndex& analysis,
                       const compile::GuestStateRegisterAccesses* registers,
                       std::optional<std::span<const ir::Inst* const>> observations,
                       std::span<const compile::LoweringCandidate> candidates,
                       std::span<const detail::FPRProofOwner> owners,
                       compile::CompileWorkspace& workspace)
            : context(allocation, features)
            , sse_scalar_insert(scalar_insert)
            , sse_scalar_tie(scalar_tie)
            , index(&analysis)
            , registers(registers)
            , observations(observations)
            , candidates(candidates)
            , owners(owners)
            , workspace(workspace) {}
    FPRContract Build();

private:
    enum class Kind { Read, AesTie, AesWrite, Write, ScalarTie, Count };
    // Unseen is the existing row buffer's all-ones fill. Five proof kinds
    // occupy ten bits; recursive visits to another kind preserve its fields.
    enum class State : u32 { Visiting = 0, Rejected = 1, Accepted = 2, Unseen = 3 };
    static_assert(2 * static_cast<u32>(Kind::Count) <= 32);
    std::optional<compile::InstructionRowScope> cache;
    template <class Compute> bool Cached(Kind kind, const ir::Inst* node, Compute compute) {
        const auto position = index->InstructionPosition(node);
        if (!position) return false;
        if (!cache) cache.emplace(workspace, index->Instructions().size());
        const u32 shift = 2 * static_cast<u32>(kind);
        const u32 mask = u32{3} << shift;
        const auto state = static_cast<State>((cache->Find(*position) >> shift) & 3u);
        if (state != State::Unseen) return state == State::Accepted;
        auto& row = (*cache)[*position];
        row = (row & ~mask) | (static_cast<u32>(State::Visiting) << shift);
        const bool accepted = compute();
        row = (row & ~mask) |
              (static_cast<u32>(accepted ? State::Accepted : State::Rejected) << shift);
        return accepted;
    }
    bool ReproveCoalescedHostFPRRead(const ir::Inst* node) {
        return Cached(Kind::Read, node, [&] { return ComputeCoalescedHostFPRRead(node); });
    }
    bool ComputeCoalescedHostFPRRead(const ir::Inst* inst);
    bool ReproveAesChainTie(const ir::Inst* node) {
        return Cached(Kind::AesTie, node, [&] { return ComputeAesChainTie(node); });
    }
    bool ComputeAesChainTie(const ir::Inst* inst);
    bool ReproveAesChainHostWrite(const ir::Inst* node) {
        return Cached(Kind::AesWrite, node, [&] { return ComputeAesChainHostWrite(node); });
    }
    bool ComputeAesChainHostWrite(const ir::Inst* inst);
    bool ReproveCoalescedHostFPRWrite(const ir::Inst* node) {
        return Cached(Kind::Write, node, [&] { return ComputeCoalescedHostFPRWrite(node); });
    }
    bool ComputeCoalescedHostFPRWrite(const ir::Inst* inst);
    bool ReproveScalarFPRTie(const ir::Inst* node) {
        return Cached(Kind::ScalarTie, node, [&] { return ComputeScalarFPRTie(node); });
    }
    bool ComputeScalarFPRTie(const ir::Inst* inst);
    std::span<const detail::FPRProofOwner> Owners(u32 target) const {
        const auto first = std::lower_bound(
                owners.begin(), owners.end(), target, [](const auto& owner, u32 home) {
                    return owner.home < home;
                });
        const auto last =
                std::upper_bound(first, owners.end(), target, [](u32 home, const auto& owner) {
                    return home < owner.home;
                });
        return {first, last};
    }
    FPRAllocationView context;
    bool sse_scalar_insert, sse_scalar_tie;
    const compile::BlockAnalysisIndex* index;
    const compile::GuestStateRegisterAccesses* registers;
    std::optional<std::span<const ir::Inst* const>> observations;
    bool HasAccess(u32 home, bool reads, u32 after, u32 before) const {
        if (!reads && !index->HasFPRWrites()) return false;
        ASSERT(registers);
        const auto next = registers->FirstAccess(compile::GuestStateRegisterFile::FPR,
                                                 home, reads, after);
        return next && *next < before;
    }
    bool HasAccess(u32 home, u32 after, u32 before) const {
        return HasAccess(home, false, after, before) || HasAccess(home, true, after, before);
    }
    std::span<const ir::Inst* const> Observations(u32 after, u32 before) const {
        ASSERT(observations);
        return compile::GuestStateObservationWindow(*observations, after, before);
    }
    std::span<const compile::LoweringCandidate> candidates;
    std::span<const detail::FPRProofOwner> owners;
    compile::CompileWorkspace& workspace;
};

bool FPRContractBuilder::ComputeCoalescedHostFPRRead(const ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::GetHostFPR || inst->GetArg<ir::Imm>(1).Get() != 0 ||
        !ir::IsFloatValueType(inst->ReturnType())) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(0).Get();
    if (target >= 32 || context.V(ir::Value{inst}).GetCode() != target) {
        return false;
    }
    const u32 end = index->LastInstructionUse(inst);
    return !HasAccess(target, false, inst->Id(), end + 1);
}

bool FPRContractBuilder::ComputeAesChainTie(const ir::Inst* inst) {
    if (!context.GetFeatures().ra_aes_chain_tie || !inst ||
        (inst->GetOp() != ir::OpCode::VecAesEncFast &&
         inst->GetOp() != ir::OpCode::VecAesEncLastFast) ||
        !context.IsAesChainTied(inst->Id())) {
        return false;
    }
    const u32 target = context.AesChainTarget(inst->Id());
    auto data = ir::ResolveBitCastSource(inst->GetArg<ir::Value>(0));
    auto key = ir::ResolveBitCastSource(inst->GetArg<ir::Value>(1));
    auto zero = ir::ResolveBitCastSource(inst->GetArg<ir::Value>(2));
    if (!data.Defined() || !data.Def() || target < 16 || target > 31 ||
        context.V(ir::Value{inst}).GetCode() != target || context.V(data).GetCode() != target ||
        context.V(key).GetCode() == target || context.V(zero).GetCode() == target) {
        return false;
    }

    const u32 data_end = index->LastInstructionUse(data.Def());
    const ir::Inst* semantic_consumer = nullptr;
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

    const bool predecessor_is_chain = (data.Def()->GetOp() == ir::OpCode::VecAesEncFast ||
                                       data.Def()->GetOp() == ir::OpCode::VecAesEncLastFast) &&
                                      context.IsAesChainTied(data.Id()) &&
                                      context.AesChainTarget(data.Id()) == target;
    const bool predecessor_is_home = data.Def()->GetOp() == ir::OpCode::GetHostFPR &&
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
            ir::ResolveBitCastSource(semantic_consumer->GetArg<ir::Value>(0)).Def() == inst;
    const bool successor_is_store = semantic_consumer->GetOp() == ir::OpCode::SetHostFPR &&
                                    semantic_consumer->GetArg<ir::Imm>(1).Get() == target &&
                                    semantic_consumer->GetArg<ir::Imm>(2).Get() == 0 &&
                                    context.IsHostWriteCoalesced(semantic_consumer->Id());
    // A single producer is deliberately outside this spike: the component
    // must own both the initial read and final publication, not just move one
    // of the two copies to the other side of the AES instruction.
    return (predecessor_is_home && successor_is_chain) ||
           (predecessor_is_chain && (successor_is_chain || successor_is_store));
}

bool FPRContractBuilder::ComputeAesChainHostWrite(const ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::SetHostFPR || inst->GetArg<ir::Imm>(2).Get() != 0) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    if (target >= 32) return false;
    auto produced = ir::ResolveBitCastSource(inst->GetArg<ir::Value>(0));
    if (!produced.Defined() || !produced.Def() || !context.IsAesChainTied(produced.Id()) ||
        context.AesChainTarget(produced.Id()) != target ||
        context.V(produced).GetCode() != target) {
        return false;
    }

    std::vector<const ir::Inst*> reverse_chain;
    auto data = produced;
    while (data.Defined() && data.Def() && context.IsAesChainTied(data.Id())) {
        auto* node = data.Def();
        if (!ReproveAesChainTie(node)) {
            return false;
        }
        reverse_chain.push_back(node);
        data = ir::ResolveBitCastSource(node->GetArg<ir::Value>(0));
    }
    if (reverse_chain.size() < 2 || !data.Defined() || !data.Def() ||
        data.Def()->GetOp() != ir::OpCode::GetHostFPR ||
        data.Def()->GetArg<ir::Imm>(0).Get() != target ||
        data.Def()->GetArg<ir::Imm>(1).Get() != 0 || !context.IsHostReadCoalesced(data.Id()) ||
        context.V(data).GetCode() != target) {
        return false;
    }
    std::reverse(reverse_chain.begin(), reverse_chain.end());
    std::unordered_set<const ir::Inst*> chain_nodes;
    if (reverse_chain.size() > 8) chain_nodes.insert(reverse_chain.begin(), reverse_chain.end());
    auto is_chain_node = [&](const ir::Inst* candidate) {
        return reverse_chain.size() <= 8
                       ? std::find(reverse_chain.begin(), reverse_chain.end(), candidate) !=
                                 reverse_chain.end()
                       : chain_nodes.contains(candidate);
    };

    const auto last_use = [&](const ir::Inst* definition) {
        return index->LastInstructionUse(definition);
    };
    if (last_use(produced.Def()) != inst->Id()) {
        return false;
    }
    const u32 begin = reverse_chain.front()->Id();
    for (const auto owner : Owners(target)) {
        auto& other = *index->Instructions()[owner.position];
        if (&other == inst || &other == data.Def() || is_chain_node(&other) || !other.HasValue() ||
            other.IsBitCastOperation() || other.Id() >= inst->Id()) {
            continue;
        }
        ir::Value value{&other};
        if (context.SharesFPR(value, produced) && last_use(&other) > begin) {
            return false;
        }
    }
    if (HasAccess(target, begin, inst->Id())) return false;
    for (auto* instruction : Observations(begin, inst->Id())) {
        auto& scan = *instruction;
        if (is_chain_node(&scan) || scan.IsBitCastOperation()) continue;
        const bool committed_home_safe = scan.GetOp() == ir::OpCode::LoadMemory ||
                                         scan.GetOp() == ir::OpCode::StoreMemory ||
                                         scan.GetOp() == ir::OpCode::SaveFlags;
        if (IsHostCoalesceObserver(scan, context.GetFeatures()) && !committed_home_safe) {
            return false;
        }
    }
    return true;
}

bool FPRContractBuilder::ComputeCoalescedHostFPRWrite(const ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::SetHostFPR || inst->GetArg<ir::Imm>(2).Get() != 0) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    if (target >= 32) return false;
    auto produced = ir::ResolveBitCastSource(inst->GetArg<ir::Value>(0));
    auto* producer = produced.Def();
    const bool scalar_unary = producer && IsHostScalarUnaryProducer(*producer, sse_scalar_tie);
    if (producer && context.IsAesChainTied(produced.Id())) {
        return ReproveAesChainHostWrite(inst);
    }
    if (!producer || produced.Type() != ir::ValueType::V128 ||
        (!IsHostFPRCoalesceProducer(producer->GetOp(), sse_scalar_insert, sse_scalar_tie) &&
         !scalar_unary) ||
        context.V(produced).GetCode() != target) {
        return false;
    }

    const auto last_use = [&](const ir::Inst* definition) {
        return index->LastInstructionUse(definition);
    };
    const u32 produced_end = last_use(producer);
    if (produced_end < inst->Id()) {
        return false;
    }
    if (IsHostScalarFPRBinaryProducer(producer->GetOp())) {
        auto left = ir::ResolveBitCastSource(producer->GetArg<ir::Value>(0));
        if (sse_scalar_insert) {
            if (!left.Defined() || context.V(left).GetCode() != target ||
                !ReproveScalarFPRTie(producer)) {
                return false;
            }
        } else if (left.Defined() && context.SharesFPR(left, produced)) {
            if (!left.Def() || left.Def()->GetOp() != ir::OpCode::GetHostFPR ||
                left.Def()->GetArg<ir::Imm>(0).Get() != target ||
                left.Def()->GetArg<ir::Imm>(1).Get() != 0 ||
                !context.IsHostReadCoalesced(left.Id()) || last_use(left.Def()) != producer->Id()) {
                return false;
            }
        }
    } else if (scalar_unary) {
        auto merge = ir::ResolveBitCastSource(producer->GetArg<ir::Value>(1));
        if (!merge.Defined() || !merge.Def() || merge.Def()->GetOp() != ir::OpCode::GetHostFPR ||
            merge.Def()->GetArg<ir::Imm>(0).Get() != target ||
            merge.Def()->GetArg<ir::Imm>(1).Get() != 0 ||
            !context.IsHostReadCoalesced(merge.Id()) || context.V(merge).GetCode() != target ||
            last_use(merge.Def()) != producer->Id()) {
            return false;
        }
    }
    for (auto input : producer->GetValues()) {
        auto root = ir::ResolveBitCastSource(input);
        if (root.Defined() && context.SharesFPR(root, produced) &&
            last_use(root.Def()) > producer->Id()) {
            return false;
        }
    }
    auto is_in_place_successor = [&](const ir::Inst& successor) {
        if (successor.Id() <= producer->Id() || successor.Id() != produced_end) {
            return false;
        }
        const auto successor_values = successor.GetValues();
        const bool consumes_produced = std::any_of(
                successor_values.begin(), successor_values.end(), [producer](ir::Value value) {
                    return ir::ResolveBitCastSource(value).Def() == producer;
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
            if (ir::ResolveBitCastSource(scan.GetArg<ir::Value>(0)).Def() == &successor) {
                return ReproveCoalescedHostFPRWrite(&scan);
            }
        }
        return false;
    };
    for (const auto owner : Owners(target)) {
        auto& other = *index->Instructions()[owner.position];
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
    if (HasAccess(target, false, inst->Id(), produced_end + 1) ||
        HasAccess(target, producer->Id(), inst->Id())) return false;
    for (auto* instruction : Observations(producer->Id(), inst->Id())) {
        if (IsHostCoalesceObserver(*instruction, context.GetFeatures())) return false;
    }
    return true;
}

bool FPRContractBuilder::ComputeScalarFPRTie(const ir::Inst* inst) {
    if (!sse_scalar_tie || !sse_scalar_insert || !inst ||
        !IsHostScalarFPRBinaryProducer(inst->GetOp())) {
        return false;
    }
    const u32 target = context.V(ir::Value{inst}).GetCode();
    if (target < 16 || target > 31) {
        return false;
    }
    const auto last_use = [&](const ir::Inst* definition) {
        return index->LastInstructionUse(definition);
    };
    auto crosses_write = [&](u32 begin, u32 end, const ir::Inst* owner) {
        if (!index->HasFPRWrites()) return false;
        ASSERT(registers);
        for (const auto id : registers->Positions(compile::GuestStateRegisterFile::FPR,
                                                   target, false, begin, end)) {
            const auto* instruction = index->FindInstruction(id);
            ASSERT(instruction);
            if (ir::ResolveBitCastSource(instruction->GetArg<ir::Value>(0)).Def() != owner)
                return true;
        }
        return false;
    };
    const auto result_end = last_use(inst);
    if (crosses_write(inst->Id(), result_end, inst)) {
        return false;
    }
    for (const auto owner : Owners(target)) {
        auto& scan = *index->Instructions()[owner.position];
        if (scan.GetOp() == ir::OpCode::GetHostFPR && context.IsHostReadCoalesced(scan.Id()) &&
            scan.GetArg<ir::Imm>(0).Get() == target && scan.Id() < result_end &&
            last_use(&scan) > inst->Id()) {
            return false;
        }
    }
    auto tie_source = [&](const ir::Inst* node) -> ir::Value {
        if (IsHostScalarFPRBinaryProducer(node->GetOp())) {
            return node->GetArg<ir::Value>(0);
        }
        if (IsHostScalarUnaryProducer(*node, sse_scalar_tie)) {
            return node->GetArg<ir::Value>(1);
        }
        return {};
    };
    for (auto* node = inst;;) {
        auto left = ir::ResolveBitCastSource(tie_source(node));
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
                auto value = ir::ResolveBitCastSource(scan.GetArg<ir::Value>(0));
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

FPRContract FPRContractBuilder::Build() {
    FPRContract plan;
    for (const auto candidate : candidates) {
        const auto& inst = *index->Instructions()[candidate.position];
        const auto value = ir::Value{&inst};
        const auto reject = [&] {
            plan.valid = false;
            plan.rejected = &inst;
        };
        if (inst.GetOp() == ir::OpCode::VecLoadConst && context.IsPshufdDirect(inst.Id())) {
            const auto control = DecodePshufdDirectControl(inst.GetArg<ir::Imm>(0).Get(),
                                                           inst.GetArg<ir::Imm>(1).Get());
            u32 uses = 0;
            bool exact = control.has_value();
            for (const auto& use : index->Uses(&inst)) {
                const auto* consumer = use.consumer;
                uses += use.count;
                exact &= use.count == 1 && consumer->GetOp() == ir::OpCode::VecShuffle32Indexed &&
                         consumer->GetArg<ir::Value>(1).Def() == &inst &&
                         consumer->GetArg<ir::Value>(0).Def() != &inst &&
                         context.IsPshufdDirect(consumer->Id());
            }
            if (!exact || uses == 0 || uses != inst.GetUses()) {
                reject();
                return plan;
            }
            plan.SetControl(inst.Id(), *control);
        }
        if (inst.GetOp() == ir::OpCode::VecShuffle32Indexed && context.IsPshufdDirect(inst.Id())) {
            const auto control = plan.Control(inst.GetArg<ir::Value>(1).Id());
            if (!control) {
                reject();
                return plan;
            }
            plan.SetControl(inst.Id(), *control);
        }
        if (inst.GetOp() == ir::OpCode::VecShuffle32TwoSrc &&
            context.GetFeatures().sse_shufps_imm &&
            context.SharesFPR(value, inst.GetArg<ir::Value>(0))) {
            const auto control = inst.GetArg<ir::Imm>(2).Get() & 0xffu;
            const bool same_source =
                    inst.GetArg<ir::Value>(0).Id() == inst.GetArg<ir::Value>(1).Id();
            if (control == 0xe4 || ((control == 0xe0 || control == 0xe5) && same_source)) {
                const auto left = ir::ResolveBitCastSource(inst.GetArg<ir::Value>(0));
                if (!left.Defined() || context.IsHostReadCoalesced(left.Id()) ||
                    index->LastInstructionUse(left.Def()) != inst.Id()) {
                    reject();
                    return plan;
                }
                plan.Certify(inst.Id(), FPRAction::ShuffleTie);
            }
        }
        if (inst.GetOp() == ir::OpCode::GetHostFPR && context.IsHostReadCoalesced(inst.Id())) {
            if (!ReproveCoalescedHostFPRRead(&inst)) {
                reject();
                return plan;
            }
            plan.Certify(inst.Id(), FPRAction::Read);
        }
        if ((inst.GetOp() == ir::OpCode::VecAesEncFast ||
             inst.GetOp() == ir::OpCode::VecAesEncLastFast) &&
            context.IsAesChainTied(inst.Id())) {
            if (!ReproveAesChainTie(&inst)) {
                reject();
                return plan;
            }
            plan.Certify(inst.Id(), FPRAction::AesTie);
        }
        if (sse_scalar_insert && sse_scalar_tie && IsHostScalarFPRBinaryProducer(inst.GetOp()) &&
            context.V(value).GetCode() >= 16 && context.V(value).GetCode() < 32 &&
            context.SharesFPR(value, inst.GetArg<ir::Value>(0))) {
            if (!ReproveScalarFPRTie(&inst)) {
                reject();
                return plan;
            }
            plan.Certify(inst.Id(), FPRAction::ScalarTie);
        }
        if (inst.GetOp() == ir::OpCode::SetHostFPR && context.IsHostWriteCoalesced(inst.Id())) {
            if (!ReproveCoalescedHostFPRWrite(&inst)) {
                reject();
                return plan;
            }
            plan.Certify(inst.Id(), FPRAction::Write);
        }
    }
    return plan;
}
}  // namespace
namespace detail {
u16 SelectFPRProof(const ir::Inst& inst,
                   compile::AllocationView allocation,
                   const FeatureSet& features,
                   bool scalar_insert,
                   bool scalar_tie) {
    const FPRAllocationView context{allocation, features};
    using O = ir::OpCode;
    switch (inst.GetOp()) {
        case O::GetHostFPR:
            return allocation.IsHostReadCoalesced(inst.Id())
                           ? FPRProofCandidate | FPRProofResolved | FPRProofRegisters : 0;
        case O::SetHostFPR:
            return allocation.IsHostWriteCoalesced(inst.Id())
                           ? FPRProofCandidate | FPRProofResolved | FPRProofOwners |
                                     FPRProofRegisters | FPRProofObservations
                           : 0;
        case O::VecAesEncFast:
        case O::VecAesEncLastFast:
            return allocation.IsAesChainTied(inst.Id())
                           ? FPRProofCandidate | FPRProofResolved |
                                     FPRProofRegisters | FPRProofObservations : 0;
        case O::VecLoadConst:
            return allocation.IsPshufdDirect(inst.Id()) ? FPRProofCandidate | FPRProofDirect : 0;
        case O::VecShuffle32Indexed:
            return allocation.IsPshufdDirect(inst.Id()) ? FPRProofCandidate : 0;
        case O::VecShuffle32TwoSrc: {
            if (!features.sse_shufps_imm ||
                !context.SharesFPR(ir::Value{&inst}, inst.GetArg<ir::Value>(0)))
                return 0;
            const auto control = inst.GetArg<ir::Imm>(2).Get() & 0xffu;
            const bool same_source =
                    inst.GetArg<ir::Value>(0).Id() == inst.GetArg<ir::Value>(1).Id();
            return control == 0xe4 || ((control == 0xe0 || control == 0xe5) && same_source)
                           ? FPRProofCandidate | FPRProofResolved
                           : 0;
        }
        default:
            return scalar_insert && scalar_tie && IsHostScalarFPRBinaryProducer(inst.GetOp()) &&
                                   context.V(ir::Value{&inst}).GetCode() >= 16 &&
                                   context.V(ir::Value{&inst}).GetCode() < 32 &&
                                   context.SharesFPR(ir::Value{&inst}, inst.GetArg<ir::Value>(0))
                           ? FPRProofCandidate | FPRProofResolved | FPRProofOwners |
                                     FPRProofRegisters | FPRProofObservations
                           : 0;
    }
}

FPRContract VerifyFPRContract(compile::AllocationView allocation,
                              const FeatureSet& features,
                              bool scalar_insert,
                              bool scalar_tie,
                              const compile::BlockAnalysisIndex& analysis,
                              const compile::GuestStateRegisterAccesses* registers,
                              std::optional<std::span<const ir::Inst* const>> observations,
                              std::span<const compile::LoweringCandidate> candidates,
                              std::span<const FPRProofOwner> owners,
                              compile::CompileWorkspace& workspace) {
    return FPRContractBuilder{allocation,
                              features,
                              scalar_insert,
                              scalar_tie,
                              analysis,
                              registers,
                              observations,
                              candidates,
                              owners,
                              workspace}
            .Build();
}
}  // namespace detail
}  // namespace swift::runtime::backend::arm64
