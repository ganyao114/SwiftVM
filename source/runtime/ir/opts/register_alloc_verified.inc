// Allocate, then check the result against what the emitters actually need
// (LinearScanAllocator::Verify). Escalating the reserve only on failure is
// what keeps the guarantee free: the first attempt reserves the ordinary
// emitter shape and, on this corpus, always passes.
//
// The escalation ladder is finite and its last rung is sufficient by
// construction -- reserving the unit's largest opcode budget plus its largest
// possible reload count leaves every instruction more free registers than it
// can ask for -- so the loop cannot spin.
static void CollectUnitBudget(Block* block,
                              u32& gpr,
                              u32& fpr,
                              u32& reload_bound,
                              const backend::GPRSMask& pool,
                              const FeatureSet& features) {
    for (auto& inst : block->GetInstList()) {
        auto need = backend::ScratchBudget(inst, features);
        const u32 scratch_only =
                LinearScanAllocator::ScratchOnlyGPRs(inst.GetOp(), pool, features);
        gpr = std::max<u32>(gpr, need.gpr > scratch_only ? need.gpr - scratch_only : 0);
        fpr = std::max<u32>(fpr, need.fpr);
        reload_bound = std::max<u32>(reload_bound, static_cast<u32>(inst.GetValues().size()) + 1);
    }
}

static void CollectUnitBudget(HIRFunction* function,
                              u32& gpr,
                              u32& fpr,
                              u32& reload_bound,
                              const backend::GPRSMask& pool,
                              const FeatureSet& features) {
    for (auto* hir_block : function->GetHIRBlocks()) {
        CollectUnitBudget(
                hir_block->GetBlock(), gpr, fpr, reload_bound, pool, features);
    }
}

template <typename Unit>
static void RunVerified(Unit* unit,
                        backend::RegAlloc* reg_alloc,
                        const FeatureSet& features,
                        bool single_block_fast_path = false,
                        bool scalar_insert = false,
                        bool intwidth_tie = false,
                        bool induct_tie = false,
                        bool spill_evict = true,
                        RegisterAllocPass::SpillEvictTestResult* test_result = nullptr) {
    const auto finish_placement_probe = [&] {
        if (features.ra_home_perm) {
            reg_alloc->PermutePlacementProbeGPRHomes();
        }
    };
    auto rerun_with_conditional_spill_scratch = [&] {
#if defined(__linux__) && !defined(__ANDROID__)
        const u32 spills = reg_alloc->SpillCount();
        if (spills != 0 && !reg_alloc->GetGprs().Get(18)) {
            // The first pass answers the only question that justifies paying
            // for x18: does this unit spill at all? If yes, rerun with x18 in
            // this unit's pool baseline. Reusing the first allocation would
            // be unsafe because it may already have assigned a live value to
            // x18, which the spill reload path must be free to overwrite.
            reg_alloc->ReserveGPRForUnit(18);
            reg_alloc->ResetAllocations();
            RunVerified(unit, reg_alloc, features, single_block_fast_path, scalar_insert,
                        intwidth_tie, induct_tie, spill_evict, test_result);
            if (RaDiagEnabled()) {
                LOG_WARNING("RegisterAllocPass: {} initial spill(s); x18 reserved for this unit",
                            spills);
            }
            return true;
        }
#endif
        return false;
    };
    auto record_shape = [&](const LinearScanAllocator& scan) {
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
        if constexpr (std::is_same_v<Unit, Block>) {
            RAShapeAnalyzeFlags(unit, shape);
        } else {
            for (auto* hir_block : unit->GetHIRBlocks()) {
                RAShapeAnalyzeFlags(hir_block->GetBlock(), shape);
            }
        }
    };
    auto record_test_result = [&](const LinearScanAllocator& scan,
                                  u32 eviction_restarts,
                                  bool fell_back_to_ladder) {
        if (!test_result) return;
        test_result->eviction_restarts = eviction_restarts;
        test_result->final_gpr_reserve = scan.GPRReserve();
        test_result->final_fpr_reserve = scan.FPRReserve();
        test_result->fell_back_to_ladder = fell_back_to_ladder;
    };
    const bool scratch_only_enabled =
            backend::X86PinExtScratchOnlyEnabled(reg_alloc->GetGprs(), features);
    const u32 scratch_only = scratch_only_enabled ? 2u : 0u;
    const u32 default_gpr_reserve = backend::kDefaultScratchGPR > scratch_only
            ? backend::kDefaultScratchGPR - scratch_only
            : 0u;
    // Attempt one: the ordinary emitter shape. Every unit in the corpus stops
    // here, so nothing above this point may walk the instruction list -- the
    // whole-unit budget scan below is deliberately deferred until an escalation
    // is known to be necessary (func_tests is dominated by translation cost and
    // notices a redundant pass).
    u32 eviction_restarts = 0;
    bool eviction_fallback = false;
    if (spill_evict) {
        Vector<u32> forced_spills{};
        while (true) {
            if (eviction_restarts) {
                reg_alloc->ResetAllocations();
            }
            LinearScanAllocator scan{unit, reg_alloc, default_gpr_reserve,
                                     backend::kDefaultScratchFPR,
                                     single_block_fast_path, scalar_insert,
                                     intwidth_tie, induct_tie, &forced_spills,
                                     true, features};
            scan.AllocateRegisters();
            if (scan.HasEvictionCandidate()) {
                forced_spills.push_back(scan.EvictionCandidate());
                ++eviction_restarts;
                continue;
            }
            PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
            const bool verified = scan.Verify();
            perf_verify.Stop();
            if (verified) {
                if (rerun_with_conditional_spill_scratch()) return;
                record_shape(scan);
                record_test_result(scan, eviction_restarts, false);
                finish_placement_probe();
                return;
            }
            eviction_fallback = true;
            break;
        }
    } else {
        LinearScanAllocator scan{unit, reg_alloc, default_gpr_reserve,
                                 backend::kDefaultScratchFPR, single_block_fast_path,
                                 scalar_insert, intwidth_tie, induct_tie,
                                 nullptr, false, features};
        scan.AllocateRegisters();
        PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
        const bool verified = scan.Verify();
        perf_verify.Stop();
        if (verified) {
            if (rerun_with_conditional_spill_scratch()) return;
            record_shape(scan);
            record_test_result(scan, 0, false);
            finish_placement_probe();
            return;
        }
    }
    u32 unit_gpr = 0, unit_fpr = 0, reload_bound = 0;
    CollectUnitBudget(unit,
                      unit_gpr,
                      unit_fpr,
                      reload_bound,
                      reg_alloc->GetGprs(),
                      features);

    // A reserve at or above the pool size would leave the scan nothing to
    // allocate, so each rung is clamped to leave one register allocatable.
    // On the ARM64 pools this never binds (the pool is far larger than any
    // opcode budget); it only matters for the artificially small masks unit
    // tests use to force the spill path.
    const auto pool_gpr = static_cast<u32>(backend::GPRSMask{reg_alloc->GetGprs()}.GetClearCount());
    const auto pool_fpr = static_cast<u32>(backend::FPRSMask{reg_alloc->GetFprs()}.GetClearCount());
    const bool level3 = backend::X86PinExtLevel3Enabled(reg_alloc->GetGprs());
    const auto clamp_gpr = [level3](u32 want, u32 pool) {
        if (!pool) {
            return 0u;
        }
        // Level 3 may need the all-spill terminal rung: spilled definitions
        // are emitted into reload scratch, so correctness does not require a
        // value register to remain allocatable.
        return std::min(want, level3 ? pool : pool - 1);
    };
    const auto clamp_fpr = [](u32 want, u32 pool) {
        return pool ? std::min(want, pool - 1) : 0u;
    };
    const u32 base_gpr = std::max<u32>(unit_gpr, default_gpr_reserve);
    const u32 base_fpr = std::max<u32>(unit_fpr, backend::kDefaultScratchFPR);
    struct ReserveRung {
        u32 gpr;
        u32 fpr;
    };
    Vector<ReserveRung> ladder{};
    auto add_rung = [&](u32 gpr, u32 fpr) {
        ReserveRung rung{clamp_gpr(gpr, pool_gpr), clamp_fpr(fpr, pool_fpr)};
        if (ladder.empty() || ladder.back().gpr != rung.gpr ||
            ladder.back().fpr != rung.fpr) {
            ladder.push_back(rung);
        }
    };
    add_rung(base_gpr, base_fpr);
    if (level3) {
        // With seven dynamic GPRs, jumping straight from the emitter reserve
        // to the all-spill rung is counterproductive: it turns ordinary
        // three-operand ops into four-reload shapes. Walk the two intermediate
        // balances and stop at the first verified allocation.
        add_rung(base_gpr + 1, base_fpr + 1);
        add_rung(base_gpr + 2, base_fpr + 2);
    }
    add_rung(base_gpr + reload_bound, base_fpr + reload_bound);
    for (auto& rung : ladder) {
        reg_alloc->ResetAllocations();
        LinearScanAllocator scan{unit, reg_alloc, rung.gpr, rung.fpr, single_block_fast_path,
                                 scalar_insert, intwidth_tie, induct_tie,
                                 nullptr, false, features};
        scan.AllocateRegisters();
        PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
        const bool verified = scan.Verify();
        perf_verify.Stop();
        if (verified) {
            if (RaDiagEnabled()) {
                LOG_WARNING("RegisterAllocPass: scratch reserve escalated to {}/{}", rung.gpr,
                            rung.fpr);
            }
            if (rerun_with_conditional_spill_scratch()) return;
            record_shape(scan);
            record_test_result(scan, eviction_restarts, eviction_fallback);
            finish_placement_probe();
            return;
        }
    }
    // Only reachable when the register pool itself is smaller than one
    // instruction's scratch demand, which no ARM64 configuration produces. Say
    // so here, where the unit is still identifiable, instead of letting the
    // JIT walk into "No free temporary GPR" halfway through an instruction.
    LOG_WARNING("RegisterAllocPass: pool {}/{} cannot cover scratch demand {}/{} (+{} reloads); "
                "emitting with the largest workable reserve",
                pool_gpr, pool_fpr, base_gpr, base_fpr, reload_bound);
}
