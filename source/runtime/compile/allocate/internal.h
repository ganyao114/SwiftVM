#pragma once

#include "runtime/compile/analysis/semantic_facts.h"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <map>
#include <vector>

#include "allocation.h"
#include "inputs.h"
#include "runtime/compile/allocate/writer.h"
#include "runtime/compile/analysis/facts.h"
#include "runtime/compile/workspace.h"
#include "runtime/ir/hir_builder.h"
#include "scratch.h"

namespace swift::runtime::ir {
class RegisterAllocTestSupport;
}

namespace swift::runtime::compile::detail {
using namespace ir;
inline bool AllocationDiagnosticsEnabled() { return GetSvmConfig().ra_diag; }

struct LiveInterval {
    const Inst* inst{};
    u32 start{};
    u32 end{};
    // Allocation outcome cached for eviction evaluation. A mapping
    // never changes while its interval is active (SpillAtInterval
    // only ever spills the incoming interval), so candidate checks
    // read these instead of re-querying Allocation for every active
    // interval at every pressure point.
    AllocationWriter::Type alloc_kind{};
    u16 alloc_code{};

    bool operator<(const LiveInterval& other) const {
        if (start == other.start) {
            return end < other.end;
        } else {
            return start < other.start;
        }
    }
};

struct AllocationOptions {
    bool single_block_fast_path{};
    bool scalar_insert{};
    bool intwidth_tie{};
    bool xmm_resident{GetSvmConfig().xmm_resident};
};

struct AllocationAttempt {
    u32 gpr_reserve{};
    u32 fpr_reserve{};
    const Vector<u32>* forced_spills{};
    bool select_eviction{};
    // Complete the scan even after an eviction candidate is found,
    // so the caller can Verify() the finished allocation before
    // deciding whether another eviction restart is worthwhile.
    bool finish_on_candidate{};
};

class LinearScan {
public:
    explicit LinearScan(const HIRFunction* function,
                        AllocationWriter* alloc,
                        const AllocationOptions& options,
                        const AllocationAttempt& attempt,
                        compile::SemanticFacts& facts,
                        compile::CompileWorkspace& workspace,
                        const AllocationInputs& inputs);

    explicit LinearScan(const Block* block,
                        AllocationWriter* alloc,
                        const AllocationOptions& options,
                        const AllocationAttempt& attempt,
                        compile::SemanticFacts& facts,
                        compile::CompileWorkspace& workspace,
                        const AllocationInputs& inputs);

private:
    LinearScan(const HIRFunction* function,
               const Block* block,
               AllocationWriter* alloc,
               const AllocationOptions& options,
               const AllocationAttempt& attempt,
               compile::SemanticFacts& facts,
               compile::CompileWorkspace& workspace,
               const AllocationInputs& inputs);

public:
    [[nodiscard]] u32 SpillCount() const { return spill_count; }
    [[nodiscard]] u32 SpillHighWater() const { return spill_count ? max_spill_slot + 1 : 0; }
    [[nodiscard]] u32 MaxLiveGPR() const { return max_live_gpr; }
    [[nodiscard]] u32 MaxLiveFPR() const { return max_live_fpr; }
    [[nodiscard]] u32 GPRReserve() const { return gpr_reserve; }
    [[nodiscard]] u32 FPRReserve() const { return fpr_reserve; }
    [[nodiscard]] bool HasEvictionCandidate() const { return has_eviction_candidate; }
    [[nodiscard]] u32 EvictionCandidate() const {
        ASSERT(has_eviction_candidate);
        return eviction_candidate;
    }
    friend class ir::RegisterAllocTestSupport;

    // Re-reads the masks this scan recorded and confirms each instruction is
    // left enough free registers for its prepared scratch requirements plus
    // one reload register per distinct spilled value it names. This is exactly
    // the condition JitContext::GetTmpX/GetTmpV depend on, checked against the
    // data the JIT will actually consume rather than against a model of it.
    [[nodiscard]] bool Verify();

    void AllocateRegisters();

private:
    bool TryAllocateCallABIGPR(LiveInterval& interval);

    bool MapPreallocatedGPR(u32 id, bool single_home = false);

    void FinalizeSingleHomePreallocation();

    void MapFixedRead(u32 id, u16 target);

    [[nodiscard]] bool ValueUsesStayInBlock(const Block* lir_block, Value value) const;

    [[nodiscard]] bool CoalescingValueUsesStayInBlock(const Block* lir_block, Value value) const;

    void FinalizeFixedGPRReadConstraints();

    [[nodiscard]] bool FixedConstraintConflict(const Block* lir_block,
                                               u16 target,
                                               u32 begin,
                                               u32 end) const;

    bool IsForcedSpill(const LiveInterval& interval) const {
        return forced_spills &&
               std::find(forced_spills->begin(), forced_spills->end(), interval.inst->Id()) !=
                       forced_spills->end();
    }

    void CollectSpillUses();

    void FillAllocFacts(LiveInterval& interval) const;

    void SelectEvictionCandidate(const LiveInterval& current, bool is_float);

    void ReuseFixedPublications();

    void CoalesceWidthChains();
    void CoalescePinnedWViewInputs(const Block*);
    void RecipeWidthComponentOwners(const Block*);
    bool ValidateWidthComponentTransaction(const Block*);
    Vector<u16> CollectLongWidthChainBridges(const Block*);
    void CoalesceWidthChainBridges(const Block*, bool enabled, const Vector<u16>&);

    void CoalesceLow32Copies();
    void CoalesceGuestGPRReads(const Block*);
    void CoalesceGuestGPRWrites(const Block*, bool width_enabled);

    void CoalesceGuestRegisterAccesses();

    void CoalesceGuestRegisterAccessesAndWidthChains();

    void CoalesceFPRPublications(bool enabled = GetSvmConfig().xmm_resident);

    void InitializeFixedClobbers();

    void RecordActiveRegs(u32 id);

    [[nodiscard]] u32 IntervalFixedClobbers(const LiveInterval& interval) const;

    // Queries a precomputed local-consumer summary; no const lazy construction.
    bool DirectlyFeedsMemory(const Inst* inst) const;

    bool TryTieGPR(LiveInterval& current, u32 source, bool allow_fixed_owner = false);

    bool TryTieFPR(LiveInterval& current);

    // True when a value ends up in State::spill_area, so every access to it
    // costs a scratch register.

    void CountTerminalReloads(const Terminal& term, u32& gpr, u32& fpr);

    bool CheckInstr(const Inst* inst, u32 extra_gpr, u32 extra_fpr);

    // Number of Allocation map entries (matches how the caller sized it).
    u32 InstrCount() {
        return function ? static_cast<u32>(function->MaxInstrCount()) : block->MaxInstrId();
    }

    void CollectLiveIntervals(const HIRFunction* hir_function);

    void CollectLiveIntervalsSingleBlock(const HIRFunction* hir_function);

    void CollectLiveIntervals(const Block* lir_block);

    void ExpireOldIntervals(LiveInterval& current);

    void ReleaseInterval(const LiveInterval& interval);

    // Appends new_item_size free slots. Callers must take the new index from
    // spill_slots.size() *before* calling this, never from a cached cursor.
    void GrowSpillStack(u32 new_item_size);

    static bool IsFloatValue(const Inst* inst) {
        auto value_type = inst->ReturnType();
        return value_type >= ValueType::V8 && value_type <= ValueType::V256;
    }

    // The mask this pass records for an instruction is exactly `active_*` at
    // the moment that instruction is reached, so refusing to allocate below
    // the reserve here is what guarantees GetTmpX/GetTmpV find a register at
    // every instruction of the unit.
    int AllocGPR(const LiveInterval& interval);

    int AllocFPR();

    void SpillAtInterval(LiveInterval& interval);

    void FreeGPR(u32 id);

    void FreeFPR(u32 id);

    void FreeSpill(u32 slot);

    void InitializeTargetInputs();
    void InstallInputConstraints();
    void PrepareFixedPreferences();
    void FinishCoalescing();
    void VerifyFixedConstraints();
    void BuildSpillReloads();
    bool TryTieTargetGPR(LiveInterval& interval);
    bool MapFixedInput(u32 id, u32 end, u32 local_end, u32 owner_end);

    const HIRFunction* function;
    const Block* block;
    AllocationWriter* reg_alloc;
    Vector<LiveInterval> live_interval;
    List<LiveInterval> active_lives;
    Vector<LiveInterval> fast_active_lives;
    compile::SemanticFacts& facts;
    const AllocationInputs& inputs;
    const ValueLiveness& value_liveness;
    const Vector<Vector<u32>>* spill_uses{};
    compile::CompileWorkspace& workspace;
    bool eviction_facts_ready{};
    backend::GPRSMask active_gprs;
    backend::FPRSMask active_fprs;
    Map<u32, u32> fixed_fpr_read_ends;
    const u32 gpr_reserve{0};
    const u32 fpr_reserve{0};
    const bool single_block_fast_path{false};
    const bool scalar_insert{false};
    const bool intwidth_tie{false};
    const Vector<u32>* forced_spills{};
    const bool select_eviction{false};
    const bool finish_on_candidate{false};
    const FeatureSet& features;
    const bool xmm_resident{false};
    bool fixed_class{false};
    bool has_eviction_candidate{false};
    u32 eviction_candidate{0};
    Vector<u32> fixed_gpr_alias_end{};
    u32 fixed_read_cursor{};
    Vector<bool> spill_slots{};
    Vector<u32> fixed_gpr_clobbers{};
    u32 fixed_gpr_clobber_codes{};
    std::array<Vector<u32>, 32> fixed_gpr_clobber_points{};
    // Spill telemetry (reported at the end of AllocateRegisters): spilling
    // has never triggered on current workloads, so any hit is worth a log
    // line — it means the JIT's defensive MEM path is being exercised.
    u32 spill_count{0};
    u32 max_spill_slot{0};
    u32 active_spill_gpr{0};
    u32 active_spill_fpr{0};
    u32 max_live_gpr{0};
    u32 max_live_fpr{0};
};

void AllocateVerified(const ir::Block*,
                      AllocationWriter*,
                      AllocationInputs,
                      bool single_block_fast_path = false,
                      bool scalar_insert = false,
                      bool intwidth_tie = false,
                      bool spill_evict = true,
                      AllocationStatistics* statistics = nullptr);
void AllocateVerified(const ir::HIRFunction*,
                      AllocationWriter*,
                      AllocationInputs,
                      bool single_block_fast_path = false,
                      bool scalar_insert = false,
                      bool intwidth_tie = false,
                      bool spill_evict = true,
                      AllocationStatistics* statistics = nullptr);
}  // namespace swift::runtime::compile::detail
