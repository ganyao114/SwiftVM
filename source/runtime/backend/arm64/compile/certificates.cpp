#include "certificates.h"
#include "register_contract.h"
#include "runtime/common/perf_stats.h"
#include "runtime/compile/analysis/block_index.h"

#include <unordered_map>

namespace swift::runtime::backend::arm64 {
static_assert(sizeof(EmissionCertificateInput) == 32);
namespace {
void VerifyFinalGPRConstraints(const EmissionCertificateInput& input,
                               compile::AllocationView allocation,
                               const compile::FixedValidationInputs& fixed) {
    ASSERT(input.block && input.analysis);
    const auto* range = fixed.BlockFor(input.block);
    ASSERT((range && range->index == input.analysis) || input.block->GetInstList().empty());
    PerfScope2 perf_fixed{GetPerfStats2().verify_fixed};
    compile::VerifyFixedConstraints(input.block, allocation, fixed, input.writes);
}

void CollectFlagsOnlySpillUse(const ir::Inst& definition,
                              const EmissionCertificateInput& input,
                              compile::AllocationView allocation,
                              EmissionCertificates& certificates) {
    if (!input.analysis || !definition.HasValue() || !definition.HasPseudoOperations() ||
        definition.ReturnType() < ir::ValueType::U8 ||
        definition.ReturnType() > ir::ValueType::S64 || !input.analysis->Contains(&definition))
        return;
    const ir::Value value{&definition};
    if (value.Id() >= allocation.MapCount() ||
        allocation.ValueType(value) != compile::AllocationView::MEM)
        return;
    // Raw records group all occurrences for one consumer, including Lambda/Params.
    input.analysis->PrepareUses();
    const auto uses = input.analysis->Uses(&definition);
    if (uses.size() != 1) return;
    const auto& use = uses.front();
    if (!input.analysis->Contains(use.consumer) ||
        (use.consumer->GetOp() != ir::OpCode::SaveFlags &&
         use.consumer->GetOp() != ir::OpCode::BranchOnlyFlags) ||
        definition.GetUses(false) != use.count)
        return;
    if (allocation
                .GPRLocationAt(value, definition.Id(), compile::AllocationView::GPRView::WholeValue)
                .kind != compile::AllocationView::MEM)
        return;
    certificates.flags_only_spills.push_back({use.consumer->Id(), definition.Id()});
}
}  // namespace

EmissionCertificates PrepareEmissionCertificates(std::span<const EmissionCertificateInput> inputs,
                                                 compile::AllocationView allocation,
                                                 const UnitActions& actions,
                                                 const compile::FixedValidationInputs& fixed,
                                                 const FeatureSet& features,
                                                 const GPRSMask& uniforms) {
    ASSERT(actions.MatchesPreparation(allocation) || actions.MatchesAllocation(allocation));
    EmissionCertificates certificates;
    for (const auto& input : inputs) {
        ASSERT(input.block && input.analysis && actions.ForBlock(input.block));
        VerifyFinalGPRConstraints(input, allocation, fixed);
        PerfScope2 perf_width{GetPerfStats2().verify_width};
        auto widths = PrepareWidthCertificates(
                input.block, allocation, features, uniforms, *input.analysis);
        perf_width.Stop();
        certificates.widths.width.insert(
                certificates.widths.width.end(), widths.width.begin(), widths.width.end());
        certificates.widths.low32.insert(
                certificates.widths.low32.end(), widths.low32.begin(), widths.low32.end());
        certificates.widths.reads.insert(
                certificates.widths.reads.end(), widths.reads.begin(), widths.reads.end());
        for (const auto& inst : input.block->GetInstList())
            CollectFlagsOnlySpillUse(inst, input, allocation, certificates);
    }
    return certificates;
}
}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {

namespace {
class ZeroStoreProof {
public:
    ZeroStoreProof(compile::BlockAnalysisIndex& a, compile::AllocationView r)
            : analysis(a), allocation(r) {}
    std::vector<u32> Verify(std::vector<const ir::Inst*>* incoming) {
        std::vector<u32> verified;
        const auto first_incoming = incoming ? incoming->size() : 0;
        const auto candidate = [&](const ir::Inst* inst) {
            if (!inst || !inst->HasValue()) return false;
            const auto op = inst->GetOp();
            if (op != ir::OpCode::LoadImm && !Extension(inst)) return false;
            if (op == ir::OpCode::LoadImm && inst->GetArg<ir::Imm>(0).Get() != 0) return false;
            return allocation.ValueType(ir::Value{inst}) != compile::AllocationView::MEM &&
                   Zero(inst);
        };
        for (auto* inst : analysis.Instructions()) {
            // Incoming roots are operands of this block, never implicit roots.
            // Seed them during the existing visit; a nonzero block needs no
            // reverse-use graph merely to enumerate arbitrary definitions.
            if (incoming)
                inst->ForEachValue([&](ir::Value value) {
                    const auto* definition = value.Def();
                    if (!definition || analysis.Contains(definition) || !candidate(definition) ||
                        !Stores(definition))
                        return;
                    if (std::find(incoming->begin() + first_incoming,
                                  incoming->end(),
                                  definition) == incoming->end())
                        incoming->push_back(definition);
                });
            if (candidate(inst) && Stores(inst)) verified.push_back(inst->Id());
        }
        return verified;
    }

private:
    enum class State { Visiting, Accepted, Rejected };
    static bool Extension(const ir::Inst* inst) {
        return inst->GetOp() == ir::OpCode::ZeroExtend32 ||
               inst->GetOp() == ir::OpCode::ZeroExtend32To64;
    }
    bool Zero(const ir::Inst* inst) {
        if (const auto found = zero_memo.find(inst); found != zero_memo.end())
            return found->second == State::Accepted;
        zero_memo.emplace(inst, State::Visiting);
        bool accepted = inst->GetOp() == ir::OpCode::LoadImm && inst->GetArg<ir::Imm>(0).Get() == 0;
        if (Extension(inst)) {
            const auto source = inst->GetArg<ir::Value>(0);
            accepted =
                    source.Defined() && !ir::IsFloatValueType(source.Type()) && Zero(source.Def());
        }
        zero_memo[inst] = accepted ? State::Accepted : State::Rejected;
        return accepted;
    }
    bool Stores(const ir::Inst* inst) {
        if (const auto found = uses_memo.find(inst); found != uses_memo.end())
            return found->second == State::Accepted;
        uses_memo.emplace(inst, State::Visiting);
        u32 compatible = 0;
        analysis.PrepareUses();
        for (const auto& use : analysis.Uses(inst)) {
            auto* user = use.consumer;
            const auto op = user->GetOp();
            if ((op == ir::OpCode::StoreUniform || op == ir::OpCode::StoreMemory) &&
                user->GetArg<ir::Value>(1).Def() == inst)
                ++compatible;
            else if (op == ir::OpCode::SetHostFPR && user->GetArg<ir::Value>(0).Def() == inst)
                ++compatible;
            else if (Extension(user) && user->GetArg<ir::Value>(0).Def() == inst &&
                     allocation.ValueType(ir::Value{user}) != compile::AllocationView::MEM &&
                     Zero(user) && Stores(user))
                ++compatible;
        }
        // Count once per consumer. An address/data duplicate must reject.
        const bool accepted = compatible != 0 && compatible == inst->GetUses(false);
        uses_memo[inst] = accepted ? State::Accepted : State::Rejected;
        return accepted;
    }
    compile::BlockAnalysisIndex& analysis;
    compile::AllocationView allocation;
    std::unordered_map<const ir::Inst*, State> zero_memo, uses_memo;
};
}  // namespace
std::vector<u32> ProveZeroStores(compile::BlockAnalysisIndex& analysis,
                                 compile::AllocationView allocation,
                                 std::vector<const ir::Inst*>* incoming) {
    return ZeroStoreProof(analysis, allocation).Verify(incoming);
}
}  // namespace swift::runtime::backend::arm64
