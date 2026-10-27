#include "internal.h"
#include "controller.h"
#include "runtime/common/perf_stats.h"

namespace swift::runtime::compile::detail {
template <typename Unit> static void RunAttempts(Unit* unit,
                                                 AllocationWriter* reg_alloc,
                                                 AllocationState& state,
                                                 AllocationStatistics* test_result = nullptr) {
    const auto& features = state.inputs.features;
    const auto scalar_insert = state.scalar_insert;
    const auto intwidth_tie = state.intwidth_tie;
    const auto spill_evict = state.spill_evict;
    auto& workspace = reg_alloc->Workspace();
    auto& facts = state.facts;
    const bool single_block_fast_path = state.block_intervals;
    const auto finish_placement_check = [&] {
        if (features.ra_home_perm) reg_alloc->PermuteGPRHomes(14, 15);
        reg_alloc->BuildGPRPreservationActions();
    };
    auto record_shape = [&](const LinearScan& scan) {
        if (!RAShapeProfEnabled()) return;
        auto& shape = reg_alloc->RAShape();
        shape = {};
        shape.ra_valid = true;
        auto gprs = reg_alloc->GetGprs();
        auto fprs = reg_alloc->GetFprs();
        shape.gpr_pool = gprs.GetClearCount();
        shape.fpr_pool = fprs.GetClearCount();
        shape.max_live_gpr = scan.MaxLiveGPR();
        shape.max_live_fpr = scan.MaxLiveFPR();
        shape.scratch_gpr = scan.GPRReserve();
        shape.scratch_fpr = scan.FPRReserve();
        shape.spill_defs = scan.SpillCount();
        shape.spill_high_water = scan.SpillHighWater();
        if constexpr (std::is_same_v<std::remove_const_t<Unit>, Block>) {
            RAShapeAnalyzeFlags(unit, shape);
        } else {
            for (auto* hir_block : unit->GetHIRBlocks()) {
                RAShapeAnalyzeFlags(hir_block->GetBlock(), shape);
            }
        }
    };
    auto record_test_result =
            [&](const LinearScan& scan, u32 eviction_restarts, bool fell_back_to_ladder) {
                if (!test_result) return;
                test_result->semantic_use_blocks = static_cast<u32>(facts.PreparedBlockCount());
                test_result->spill_use_builds = facts.HasSpillUses() ? 1u : 0u;
                test_result->eviction_restarts = eviction_restarts;
                test_result->final_gpr_reserve = scan.GPRReserve();
                test_result->final_fpr_reserve = scan.FPRReserve();
                test_result->fell_back_to_ladder = fell_back_to_ladder;
            };
    const auto& pool_policy = state.inputs.PolicyFor(reg_alloc->GetGprs());
    const auto budget = pool_policy.budget;
    const u32 default_gpr_reserve = budget.gpr_reserve;
    // Attempt one uses the ordinary emitter reserve. Site requirements and
    // escalation maxima were prepared once with the target input tables.
    u32 eviction_restarts = 0;
    bool eviction_fallback = false;
    u32 census_attempts = 0;
    if (spill_evict) {
        Vector<u32> forced_spills{};
        u32 census_first_spills = 0;
        // Each retry walks the whole unit. Bound optional quality work before
        // using the existing verified reserve ladder, independent of unit size.
        constexpr u32 kMaxEvictionRestarts = 8;
        while (true) {
            if (eviction_restarts) {
                reg_alloc->ResetAllocations();
            }
            LinearScan scan{unit,
                            reg_alloc,
                            {.single_block_fast_path = single_block_fast_path,
                             .scalar_insert = scalar_insert,
                             .intwidth_tie = intwidth_tie},
                            {.gpr_reserve = default_gpr_reserve,
                             .fpr_reserve = budget.fpr_reserve,
                             .forced_spills = &forced_spills,
                             .select_eviction = true,
                             .finish_on_candidate = eviction_restarts > 0},
                            facts,
                            workspace,
                            state.inputs};
            ++census_attempts;
            scan.AllocateRegisters();
            if (eviction_restarts == 0) census_first_spills = scan.SpillCount();
            if (scan.HasEvictionCandidate()) {
                if (eviction_restarts == kMaxEvictionRestarts) {
                    eviction_fallback = true;
                    break;
                }
                if (eviction_restarts > 0) {
                    // A forced spill already failed to yield a
                    // candidate-free attempt. If this attempt also
                    // cannot verify at the current reserve, more
                    // evictions cannot fix that -- the reserve
                    // ladder is the only way forward, so stop
                    // spending full-unit scans here. (Census on
                    // CoreMark: every unit that restarted at all
                    // eventually fell back to the ladder.)
                    PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
                    const bool verified = scan.Verify();
                    perf_verify.Stop();
                    if (!verified) {
                        eviction_fallback = true;
                        break;
                    }
                }
                forced_spills.push_back(scan.EvictionCandidate());
                ++eviction_restarts;
                continue;
            }
            PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
            const bool verified = scan.Verify();
            perf_verify.Stop();
            if (verified) {
                if (GetSvmConfig().density_prof) {
                    std::fprintf(stderr,
                                 "[evict-census] restarts=%u first_spills=%u final_spills=%u "
                                 "attempts=%u path=evict\n",
                                 eviction_restarts,
                                 census_first_spills,
                                 scan.SpillCount(),
                                 census_attempts);
                }
                record_shape(scan);
                record_test_result(scan, eviction_restarts, false);
                finish_placement_check();
                return;
            }
            eviction_fallback = true;
            break;
        }
    } else {
        LinearScan scan{unit,
                        reg_alloc,
                        {.single_block_fast_path = single_block_fast_path,
                         .scalar_insert = scalar_insert,
                         .intwidth_tie = intwidth_tie},
                        {.gpr_reserve = default_gpr_reserve, .fpr_reserve = budget.fpr_reserve},
                        facts,
                        workspace,
                        state.inputs};
        ++census_attempts;
        scan.AllocateRegisters();
        PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
        const bool verified = scan.Verify();
        perf_verify.Stop();
        if (verified) {
            if (GetSvmConfig().density_prof) {
                std::fprintf(stderr,
                             "[evict-census] restarts=0 attempts=%u path=plain\n",
                             census_attempts);
            }
            record_shape(scan);
            record_test_result(scan, 0, false);
            finish_placement_check();
            return;
        }
    }
    const auto& maximum = state.inputs.PolicyFor(reg_alloc->GetGprs()).max_scratch;
    const u32 unit_gpr = maximum.gpr, unit_fpr = maximum.fpr;
    const u32 reload_bound = state.inputs.reload_bound;

    // A reserve at or above the pool size would leave the scan nothing to
    // allocate, so each rung is clamped to leave one register allocatable.
    // The terminal attempt below may reserve the full GPR pool if necessary.
    const auto pool_gpr = static_cast<u32>(backend::GPRSMask{reg_alloc->GetGprs()}.GetClearCount());
    const auto pool_fpr = static_cast<u32>(backend::FPRSMask{reg_alloc->GetFprs()}.GetClearCount());
    const auto clamp_gpr = [allow_zero = budget.allow_zero_gpr](u32 want, u32 pool) {
        if (!pool) {
            return 0u;
        }
        // A target may admit an all-spill rung: spilled definitions
        // are emitted into reload scratch, so correctness does not require a
        // value register to remain allocatable.
        return std::min(want, allow_zero ? pool : pool - 1);
    };
    const auto clamp_fpr = [](u32 want, u32 pool) { return pool ? std::min(want, pool - 1) : 0u; };
    const u32 base_gpr = std::max<u32>(unit_gpr, default_gpr_reserve);
    const u32 base_fpr = std::max<u32>(unit_fpr, budget.fpr_reserve);
    struct ReserveRung {
        u32 gpr;
        u32 fpr;
    };
    Vector<ReserveRung> ladder{};
    auto add_rung = [&](u32 gpr, u32 fpr) {
        ReserveRung rung{clamp_gpr(gpr, pool_gpr), clamp_fpr(fpr, pool_fpr)};
        if (ladder.empty() || ladder.back().gpr != rung.gpr || ladder.back().fpr != rung.fpr) {
            ladder.push_back(rung);
        }
    };
    add_rung(base_gpr, base_fpr);
    if (budget.incremental_rungs) {
        // A small dynamic pool needs intermediate balances. Jumping from the reserve
        // to the all-spill rung is counterproductive: it turns ordinary
        // three-operand ops into four-reload shapes. Walk the two intermediate
        // balances and stop at the first verified allocation.
        add_rung(base_gpr + 1, base_fpr + 1);
        add_rung(base_gpr + 2, base_fpr + 2);
    }
    add_rung(base_gpr + reload_bound, base_fpr + reload_bound);
    // Biased-memory configurations can also exhaust the dynamic GPR pool.
    // A spilled result uses reload scratch, so the terminal attempt need not
    // keep a separate value register. Try it only after the ordinary ladder.
    if (pool_gpr && ladder.back().gpr < pool_gpr) {
        ladder.push_back({pool_gpr, clamp_fpr(base_fpr + reload_bound, pool_fpr)});
    }
    for (auto& rung : ladder) {
        reg_alloc->ResetAllocations();
        LinearScan scan{unit,
                        reg_alloc,
                        {.single_block_fast_path = single_block_fast_path,
                         .scalar_insert = scalar_insert,
                         .intwidth_tie = intwidth_tie},
                        {.gpr_reserve = rung.gpr, .fpr_reserve = rung.fpr},
                        facts,
                        workspace,
                        state.inputs};
        ++census_attempts;
        scan.AllocateRegisters();
        PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
        const bool verified = scan.Verify();
        perf_verify.Stop();
        if (verified) {
            if (AllocationDiagnosticsEnabled()) {
                LOG_WARNING("Allocate: scratch reserve escalated to {}/{}", rung.gpr, rung.fpr);
            }
            if (GetSvmConfig().density_prof) {
                std::fprintf(stderr,
                             "[evict-census] restarts=%u attempts=%u path=ladder fallback=%u\n",
                             eviction_restarts,
                             census_attempts,
                             eviction_fallback ? 1u : 0u);
            }
            record_shape(scan);
            record_test_result(scan, eviction_restarts, eviction_fallback);
            finish_placement_check();
            return;
        }
    }
    // A rejected mapping must never reach an emitter, regardless of pin level.
    PANIC("Allocate: pool {}/{} cannot cover scratch demand {}/{} (+{} reloads)",
          pool_gpr,
          pool_fpr,
          base_gpr,
          base_fpr,
          reload_bound);
}

void AllocateVerified(const ir::Block* block,
                      AllocationWriter* allocation,
                      AllocationInputs inputs,
                      bool block_intervals,
                      bool scalar_insert,
                      bool intwidth_tie,
                      bool spill_evict,
                      AllocationStatistics* statistics) {
    auto& state = AllocationController::Begin(block,
                                              *allocation,
                                              std::move(inputs),
                                              block_intervals,
                                              scalar_insert,
                                              intwidth_tie,
                                              spill_evict);
    if (!block->IsEmptyBlock()) RunAttempts(block, allocation, state, statistics);
}
void AllocateVerified(const ir::HIRFunction* function,
                      AllocationWriter* allocation,
                      AllocationInputs inputs,
                      bool block_intervals,
                      bool scalar_insert,
                      bool intwidth_tie,
                      bool spill_evict,
                      AllocationStatistics* statistics) {
    auto& state = AllocationController::Begin(function,
                                              *allocation,
                                              std::move(inputs),
                                              block_intervals,
                                              scalar_insert,
                                              intwidth_tie,
                                              spill_evict);
    RunAttempts(function, allocation, state, statistics);
}
}  // namespace swift::runtime::compile::detail

namespace swift::runtime::compile {
template <class Unit> AllocationState& AllocationController::Begin(Unit* unit,
                                                                   AllocationWriter& allocation,
                                                                   AllocationInputs inputs,
                                                                   bool block_intervals,
                                                                   bool scalar_insert,
                                                                   bool intwidth_tie,
                                                                   bool spill_evict) {
    if constexpr (std::is_same_v<std::remove_const_t<Unit>, ir::Block>) {
        inputs.Validate(unit, nullptr, allocation.GetGprs(), allocation.GetFprs());
        ASSERT(allocation.PrepareFacts().MatchesInput(unit, nullptr));
        ASSERT(allocation.InputBlock() == unit && !allocation.InputFunction());
    } else {
        inputs.Validate(nullptr, unit, allocation.GetGprs(), allocation.GetFprs());
        ASSERT(allocation.PrepareFacts().MatchesInput(nullptr, unit));
        ASSERT(allocation.InputFunction() == unit && !allocation.InputBlock());
    }
    allocation.allocation_state = std::make_unique<AllocationState>(std::move(inputs),
                                                                    allocation.PrepareFacts(),
                                                                    block_intervals,
                                                                    scalar_insert,
                                                                    intwidth_tie,
                                                                    spill_evict);
    return *allocation.allocation_state;
}

void AllocationController::RetryWithoutFPRCoalescing(AllocationWriter& allocation) {
    ASSERT_MSG(allocation.allocation_state && !allocation.FPRCoalescingDisabled(),
               "FPR contract rejected without a physical allocation input");
    ASSERT(!allocation.locations_editing && !allocation.GPRLocationsFrozen());
    allocation.DisableFPRCoalescing();
    ++allocation.fpr_contract_retries;
    PerfScope2 retry{GetPerfStats2().fpr_contract_retry};
    allocation.ResetAllocations();
    auto& state = *allocation.allocation_state;
    if (allocation.allocation_block)
        detail::RunAttempts(allocation.allocation_block, &allocation, state);
    else
        detail::RunAttempts(allocation.allocation_function, &allocation, state);
}

void AllocationController::Finish(AllocationWriter& allocation) {
    ASSERT(allocation.GPRLocationsFrozen() && !allocation.allocation_edit &&
           !allocation.locations_editing);
    allocation.allocation_state.reset();
    allocation.input_pending = false;
}

void AllocateBlock(const ir::Block* block,
                   AllocationWriter* allocation,
                   AllocationInputs inputs,
                   bool scalar_insert) {
    PerfScope2 perf{GetPerfStats2().allocate_block};
    const auto& features = inputs.features;
    detail::AllocateVerified(block,
                             allocation,
                             std::move(inputs),
                             false,
                             scalar_insert,
                             features.ra_intwidth_tie,
                             features.ra_spill_evict);
}
void AllocateFunction(const ir::HIRFunction* function,
                      AllocationWriter* allocation,
                      AllocationInputs inputs,
                      FunctionAllocationOptions options) {
    PerfScope2 perf{GetPerfStats2().allocate_function};
    const auto& features = inputs.features;
    const bool local = options.block_intervals.value_or(features.ra_1blk) &&
                       function->GetHIRBlocksRPO().size() == 1;
    detail::AllocateVerified(function,
                             allocation,
                             std::move(inputs),
                             local,
                             options.scalar_insert,
                             features.ra_intwidth_tie,
                             features.ra_spill_evict);
}
}  // namespace swift::runtime::compile
