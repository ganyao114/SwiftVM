#pragma once

#include <memory>
#include <optional>

#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/cfg.h"
#include "runtime/compile/analysis/facts.h"
#include "runtime/compile/analysis/guest_state.h"
#include "runtime/compile/analysis/liveness.h"
#include "runtime/compile/analysis/register_reads.h"
#include "runtime/compile/analysis/value_flow.h"

namespace swift::runtime::compile {

// One numbered immutable IR input. Allocation attempts and target preparation
// borrow this same owner; summaries do not force construction of consumer or
// spill-position indexes. Every preparation operation is explicitly mutable.
class SemanticFacts final {
public:
    SemanticFacts() = default;
    explicit SemanticFacts(const ir::Block* block);
    explicit SemanticFacts(const ir::HIRFunction* function);
    ~SemanticFacts() { ASSERT(!writer_active); }
    SemanticFacts(const SemanticFacts&) = delete;
    SemanticFacts& operator=(const SemanticFacts&) = delete;

    // A fresh mutable input explicitly invalidates its old facts. A sealed
    // input can only be requested again with the same identity and reuses data.
    void BeginInput(const ir::Block* block, const ir::HIRFunction* function);
    [[nodiscard]] bool HasInput() const { return block || function; }
    [[nodiscard]] bool MatchesInput(const ir::Block* input_block,
                                    const ir::HIRFunction* input_function) const {
        return HasInput() && block == input_block && function == input_function;
    }
    [[nodiscard]] bool Contains(const ir::Block* input) const;

    [[nodiscard]] const BlockUseEnds& PrepareBlockUses(const ir::Block* block);
    [[nodiscard]] const BlockSummary& PrepareSummary(const ir::Block* block);
    [[nodiscard]] const BlockSummary* FindSummary(const ir::Block* block) const;
    // Prepared with the base summaries, across executable members only.
    // Raw narrow GetHostGPR/SetHostGPR views still declare their home.
    [[nodiscard]] u32 GPRHomes() const {
        ASSERT(flags_ready);
        return gpr_homes;
    }
    [[nodiscard]] const Vector<FlagsDemand>& PrepareFlags();
    [[nodiscard]] const ValueLiveness& PrepareValueLiveness();
    [[nodiscard]] const ValueLiveness* FindValueLiveness() const { return value_liveness.get(); }
    [[nodiscard]] const RegisterReadUses& PrepareRegisterReads();
    [[nodiscard]] const RegisterReadUses* FindRegisterReads() const { return register_reads.get(); }
    [[nodiscard]] std::span<const ir::Inst* const> PrepareGuestStateObservations(
            const ir::Block* block);
    [[nodiscard]] std::optional<std::span<const ir::Inst* const>> FindGuestStateObservations(
            const ir::Block* block) const;
    [[nodiscard]] std::span<const ir::Inst* const> PrepareGuestStateGPRDefinitions(
            const ir::Block* block);
    [[nodiscard]] std::optional<std::span<const ir::Inst* const>> FindGuestStateGPRDefinitions(
            const ir::Block* block) const;
    [[nodiscard]] const GuestStateRegisterAccesses& PrepareGuestStateRegisterAccesses(const ir::Block* block);
    [[nodiscard]] const GuestStateRegisterAccesses* FindGuestStateRegisterAccesses(
            const ir::Block* block) const;
    // Prove the requested logical bytes immediately before an instruction.
    // Mutable preparation may build/cache a proof. Find never scans/builds;
    // nullopt and a prepared negative answer are deliberately distinct.
    [[nodiscard]] bool PrepareGuestStateGPRMatch(
            const ir::Block* block, u32 home, u32 offset, const ir::Inst& before,
            ir::Value expected, GuestStateEntryPolicy policy);
    [[nodiscard]] std::optional<bool> FindGuestStateGPRMatch(
            const ir::Block* block, u32 home, u32 offset, const ir::Inst& before,
            ir::Value expected, GuestStateEntryPolicy policy) const;
    [[nodiscard]] const FunctionCFG& PrepareFunctionCFG();
    [[nodiscard]] const FunctionCFG* FindFunctionCFG() const { return function_cfg.get(); }
    [[nodiscard]] bool PrepareSSAValidation();
    [[nodiscard]] std::optional<bool> FindSSAValidation() const { return ssa_valid; }
    [[nodiscard]] bool RequiresIncomingValues(const ir::Block* block);
    [[nodiscard]] const Vector<u32>& PrepareCFGUseEnds();
    [[nodiscard]] const Vector<Vector<u32>>& PrepareSpillUses();
    [[nodiscard]] BlockAnalysisIndex& PrepareIndex(const ir::Block* block);
    [[nodiscard]] const BlockAnalysisIndex* FindIndex(const ir::Block* block) const;
    [[nodiscard]] BlockAnalysisIndex& PrepareUses(const ir::Block* block);
    [[nodiscard]] const BlockAnalysisIndex* FindUses(const ir::Block* block) const;
    [[nodiscard]] bool CanUseBlockIntervals() const;
    [[nodiscard]] size_t PreparedBlockCount() const;
    [[nodiscard]] bool HasSpillUses() const { return spill_ready; }

private:
    friend class AllocationWriter;
    friend class OptimizedUnit;
    friend class SSAValidation;
    bool writer_active{};
    std::optional<bool> ssa_valid;
    struct BlockRecord {
        const ir::Block* block;
        std::optional<BlockUseEnds> ends;
        std::unique_ptr<BlockAnalysisIndex> index;
        PseudoFlagsPlan pseudo_flags;
        BlockSummary summary;
    };
    BlockRecord& PrepareBlockRecord(const ir::Block* block);
    [[nodiscard]] bool InputSealed() const;
    [[nodiscard]] bool PrepareGuestStateGPRMatchCold(
            const ir::Block* block, u32 home, u32 offset, const ir::Inst& before,
            ir::Value expected, GuestStateEntryPolicy policy);
    FunctionValueFlow& PrepareFunctionValues();
    void PrepareBaseFacts(bool values);
    const ir::Block* block{};
    const ir::HIRFunction* function{};
    u32 instruction_count{};
    // Reserve the complete member count before publishing any inline summary.
    // Consumer payloads have independent stable addresses and are created only
    // when a family actually requires arbitrary reverse uses.
    StackVector<BlockRecord, 4> blocks;
    Vector<FlagsDemand> flags;
    std::unique_ptr<FunctionValueFlow> function_values;
    Vector<Vector<u32>> spill_uses;
    std::unique_ptr<ValueLiveness> value_liveness;
    std::unique_ptr<RegisterReadUses> register_reads;
    std::unique_ptr<GuestStateFacts> guest_state;
    std::unique_ptr<FunctionCFG> function_cfg;
    bool flags_ready{}, spill_ready{};
    u32 gpr_homes{};
};

}  // namespace swift::runtime::compile
