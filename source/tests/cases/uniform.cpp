#include "../support/case_support.h"
#include "../support/fp_environment.h"

TEST_CASE("Uniform elimination does not propagate conditional stores past labels") {
    using namespace swift::runtime::ir;

    UniformInfo info{.uniform_size = 64};

    Block conditional{0, Location{0x1000}};
    auto condition = conditional.LoadImm<BOOL>(Imm{1u});
    auto skip_store = conditional.NotGoto(condition);
    auto stored = conditional.LoadImm(Imm{0u}).SetType(ValueType::U8);
    conditional.StoreUniform(Uniform{32, ValueType::U8}, stored);
    conditional.BindLabel(skip_store);
    auto merged_load = conditional.LoadUniform(Uniform{32, ValueType::U8});

    UniformEliminationPass::Run(&conditional, info, FeatureSet{});
    REQUIRE(merged_load.Def()->GetOp() == OpCode::LoadUniform);

    Block straight_line{1, Location{0x2000}};
    auto straight_value = straight_line.LoadImm(Imm{0u}).SetType(ValueType::U8);
    straight_line.StoreUniform(Uniform{32, ValueType::U8}, straight_value);
    auto straight_load = straight_line.LoadUniform(Uniform{32, ValueType::U8});

    UniformEliminationPass::Run(&straight_line, info, FeatureSet{});
    REQUIRE(straight_load.Def()->GetOp() == OpCode::BitExtract);

    // A value established before the branch dominates both successors. W14's
    // path merge keeps it when the guarded region leaves that uniform byte
    // untouched, while SVM_UNIFORM_PATH_FWD=0 restores the old label barrier.
    Block dominated{2, Location{0x3000}};
    auto dominating_value = dominated.LoadImm(Imm{7u}).SetType(ValueType::U8);
    dominated.StoreUniform(Uniform{32, ValueType::U8}, dominating_value);
    auto dominated_cond = dominated.LoadImm<BOOL>(Imm{1u});
    auto dominated_skip = dominated.NotGoto(dominated_cond);
    dominated.LoadImm(Imm{99u});
    dominated.BindLabel(dominated_skip);
    auto dominated_load = dominated.LoadUniform(Uniform{32, ValueType::U8});

    UniformEliminationPass::Run(&dominated, info, FeatureSet{});
    const bool path_forward_off =
            !swift::runtime::GetSvmConfig().uniform_path_fwd;
    REQUIRE(dominated_load.Def()->GetOp() ==
            (path_forward_off ? OpCode::LoadUniform : OpCode::BitExtract));

    // A write on only the fallthrough edge must still prevent forwarding.
    Block differing{3, Location{0x4000}};
    auto before = differing.LoadImm(Imm{7u}).SetType(ValueType::U8);
    differing.StoreUniform(Uniform{32, ValueType::U8}, before);
    auto differing_cond = differing.LoadImm<BOOL>(Imm{1u});
    auto differing_skip = differing.NotGoto(differing_cond);
    auto guarded = differing.LoadImm(Imm{9u}).SetType(ValueType::U8);
    differing.StoreUniform(Uniform{32, ValueType::U8}, guarded);
    differing.BindLabel(differing_skip);
    auto differing_load = differing.LoadUniform(Uniform{32, ValueType::U8});

    UniformEliminationPass::Run(&differing, info, FeatureSet{});
    REQUIRE(differing_load.Def()->GetOp() == OpCode::LoadUniform);

    // The reverse walk uses the same path intersection. A guarded write is
    // dead when a later store overwrites it after the merge on both paths.
    Block guarded_dead{4, Location{0x5000}};
    auto dead_cond = guarded_dead.LoadImm<BOOL>(Imm{1u});
    auto dead_skip = guarded_dead.NotGoto(dead_cond);
    auto dead_value = guarded_dead.LoadImm(Imm{3u}).SetType(ValueType::U8);
    guarded_dead.StoreUniform(Uniform{32, ValueType::U8}, dead_value);
    guarded_dead.BindLabel(dead_skip);
    auto final_value = guarded_dead.LoadImm(Imm{4u}).SetType(ValueType::U8);
    guarded_dead.StoreUniform(Uniform{32, ValueType::U8}, final_value);

    UniformEliminationPass::Run(&guarded_dead, info, FeatureSet{});
    size_t guarded_store_count{};
    for (const auto& inst : guarded_dead.GetInstList()) {
        guarded_store_count += inst.GetOp() == OpCode::StoreUniform;
    }
    REQUIRE(guarded_store_count == (path_forward_off ? 2 : 1));
}

TEST_CASE("Uniform range mode keeps mapped GPR, XMM, and ordinary byte facts local") {
    using namespace swift::runtime::ir;

    UniformInfo info{.uniform_size = 96};
    auto map_gpr = [&](std::uint32_t begin, std::uint16_t host_id) {
        UniformRegister reg{.uniform = Uniform{begin, ValueType::U64}};
        reg.host_reg.gpr = HostGPR{host_id};
        reg.host_reg.is_fpr = false;
        info.uniform_regs_map.Map(begin, begin + sizeof(std::uint64_t), reg);
    };
    map_gpr(16, 20);
    map_gpr(32, 21);
    info.xmm_uniform_ranges.push_back({48, 64});

    const bool range_on = swift::runtime::GetSvmConfig().ir_uniform_range;

    Block block{0, Location{0x6000}};
    auto ordinary = block.LoadImm(Imm{std::uint64_t(0x1111111111111111ull)}).SetType(ValueType::U64);
    block.StoreUniform(Uniform{0, ValueType::U64}, ordinary);
    auto other_gpr = block.LoadImm(Imm{std::uint64_t(0x2222222222222222ull)}).SetType(ValueType::U64);
    block.StoreUniform(Uniform{32, ValueType::U64}, other_gpr);
    auto xmm = block.LoadImm(Imm{0u}).SetType(ValueType::V128);
    block.StoreUniform(Uniform{48, ValueType::V128}, xmm);
    auto target_gpr = block.LoadImm(Imm{std::uint64_t(0x3333333333333333ull)}).SetType(ValueType::U64);
    block.StoreUniform(Uniform{16, ValueType::U64}, target_gpr);
    auto target_patch =
            block.LoadImm(Imm{std::uint16_t{0x4444}}).SetType(ValueType::U16);
    block.StoreUniform(Uniform{18, ValueType::U16}, target_patch);

    auto untouched_target = block.LoadUniform(Uniform{16, ValueType::U16});
    auto written_target = block.LoadUniform(Uniform{18, ValueType::U16});
    auto preserved_gpr = block.LoadUniform(Uniform{32, ValueType::U64});
    auto preserved_xmm = block.LoadUniform(Uniform{48, ValueType::V128});
    auto preserved_ordinary = block.LoadUniform(Uniform{0, ValueType::U64});

    UniformEliminationPass::Run(&block, info, FeatureSet{});
    REQUIRE(untouched_target.Def()->GetOp() ==
            (range_on ? OpCode::BitExtract : OpCode::GetHostGPR));
    REQUIRE(written_target.Def()->GetOp() ==
            (range_on ? OpCode::BitExtract : OpCode::GetHostGPR));
    REQUIRE(preserved_gpr.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::GetHostGPR));
    REQUIRE(preserved_xmm.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::LoadUniform));
    REQUIRE(preserved_ordinary.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::LoadUniform));
}

TEST_CASE("Uniform helper effect sets preserve only unaffected facts") {
    using namespace swift::runtime::ir;

    UniformInfo info{.uniform_size = 32};
    static constexpr std::array touched_ranges{
            UniformEffectRange{0, sizeof(std::uint64_t)},
    };
    static constexpr UniformEffectSet touched_effects{
            touched_ranges.data(), touched_ranges.size()};
    const auto touched_id = RegisterUniformEffectSet(&touched_effects);
    const bool range_on = swift::runtime::GetSvmConfig().ir_uniform_range;

    auto seed = [](Block& block) {
        auto low = block.LoadImm(Imm{std::uint64_t(0x1111111111111111ull)}).SetType(ValueType::U64);
        auto high = block.LoadImm(Imm{std::uint64_t(0x2222222222222222ull)}).SetType(ValueType::U64);
        block.StoreUniform(Uniform{0, ValueType::U64}, low);
        block.StoreUniform(Uniform{16, ValueType::U64}, high);
    };

    Block pure{0, Location{0x7000}};
    seed(pure);
    pure.CallLambda(Lambda{DataClass{Imm{std::uint64_t(1ull)}}, UniformEffectId::None});
    auto pure_low = pure.LoadUniform(Uniform{0, ValueType::U64});
    auto pure_high = pure.LoadUniform(Uniform{16, ValueType::U64});
    UniformEliminationPass::Run(&pure, info, FeatureSet{});
    REQUIRE(pure_low.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::LoadUniform));
    REQUIRE(pure_high.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::LoadUniform));

    Block ranged{1, Location{0x8000}};
    seed(ranged);
    ranged.CallLambda(Lambda{DataClass{Imm{std::uint64_t(1ull)}}, touched_id});
    auto ranged_low = ranged.LoadUniform(Uniform{0, ValueType::U64});
    auto ranged_high = ranged.LoadUniform(Uniform{16, ValueType::U64});
    UniformEliminationPass::Run(&ranged, info, FeatureSet{});
    REQUIRE(ranged_low.Def()->GetOp() == OpCode::LoadUniform);
    REQUIRE(ranged_high.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::LoadUniform));

    Block unknown{2, Location{0x9000}};
    seed(unknown);
    unknown.CallLambda(Lambda{Imm{std::uint64_t(1ull)}});
    auto unknown_low = unknown.LoadUniform(Uniform{0, ValueType::U64});
    auto unknown_high = unknown.LoadUniform(Uniform{16, ValueType::U64});
    UniformEliminationPass::Run(&unknown, info, FeatureSet{});
    REQUIRE(unknown_low.Def()->GetOp() == OpCode::LoadUniform);
    REQUIRE(unknown_high.Def()->GetOp() == OpCode::LoadUniform);

    Block dead_store{3, Location{0xa000}};
    auto old_value =
            dead_store.LoadImm(Imm{std::uint64_t(0x1111111111111111ull)}).SetType(ValueType::U64);
    auto new_value =
            dead_store.LoadImm(Imm{std::uint64_t(0x2222222222222222ull)}).SetType(ValueType::U64);
    dead_store.StoreUniform(Uniform{0, ValueType::U64}, old_value);
    dead_store.CallLambda(Lambda{DataClass{Imm{std::uint64_t(1ull)}}, UniformEffectId::None});
    dead_store.StoreUniform(Uniform{0, ValueType::U64}, new_value);
    UniformEliminationPass::Run(&dead_store, info, FeatureSet{});
    size_t stores{};
    for (const auto& inst : dead_store.GetInstList()) {
        stores += inst.GetOp() == OpCode::StoreUniform;
    }
    // Effect sets describe helper writes for forward fact invalidation. They
    // do not claim the helper cannot read uniform state, so reverse DSE keeps
    // every helper as an observation barrier.
    REQUIRE(stores == 2);
}

TEST_CASE("XMM uniform forwarding covers V128 and scalar views") {
    using namespace swift::runtime::ir;

    // The pass is runtime-generic, so describe a synthetic XMM slot exactly
    // as the x86 frontend does: both the full V128 access and the two U64
    // architectural views live in one byte range.
    UniformInfo info{.uniform_size = 64};
    info.xmm_uniform_ranges.push_back({16, 32});
    const auto& svm_config = swift::runtime::GetSvmConfig();
    const bool xmm_forward_off = !svm_config.xmm_uniform_fwd;
    const bool xmm_ssa_fwd2_off = !svm_config.xmm_ssa_fwd2;
    const bool xmm_narrow_fwd_off = !svm_config.xmm_narrow_fwd;

    Block straight{0, Location{0x1000}};
    auto vector_value = straight.LoadImm(Imm{0u}).SetType(ValueType::V128);
    straight.StoreUniform(Uniform{16, ValueType::V128}, vector_value);
    auto vector_load = straight.LoadUniform(Uniform{16, ValueType::V128});
    auto narrow64_load = straight.LoadUniform(Uniform{16, ValueType::V64});
    auto narrow32_load = straight.LoadUniform(Uniform{16, ValueType::V32});
    auto scalar_value = straight.LoadImm(Imm{std::uint64_t(0x1122334455667788ull)}).SetType(ValueType::U64);
    straight.StoreUniform(Uniform{24, ValueType::U64}, scalar_value);
    auto scalar_load = straight.LoadUniform(Uniform{24, ValueType::U64});

    UniformEliminationPass::Run(&straight, info, FeatureSet{});
    REQUIRE(vector_load.Def()->GetOp() ==
            (xmm_forward_off ? OpCode::LoadUniform : OpCode::BitCast));
    const auto narrow_expected =
            (!xmm_forward_off && !xmm_narrow_fwd_off) ? OpCode::BitCast
                                                      : OpCode::LoadUniform;
    REQUIRE(narrow64_load.Def()->GetOp() == narrow_expected);
    REQUIRE(narrow64_load.Type() == ValueType::V64);
    REQUIRE(narrow32_load.Def()->GetOp() == narrow_expected);
    REQUIRE(narrow32_load.Type() == ValueType::V32);
    REQUIRE(scalar_load.Def()->GetOp() ==
            (xmm_forward_off ? OpCode::LoadUniform : OpCode::BitCast));

    // A fact established before the guarded region dominates both paths. The
    // same byte-for-byte intersection used by W14's GPR path must preserve it
    // for a V128 value too.
    Block guarded{1, Location{0x2000}};
    auto guarded_value = guarded.LoadImm(Imm{1u}).SetType(ValueType::V128);
    guarded.StoreUniform(Uniform{16, ValueType::V128}, guarded_value);
    auto condition = guarded.LoadImm<BOOL>(Imm{1u});
    auto skip = guarded.NotGoto(condition);
    guarded.LoadImm(Imm{7u});
    guarded.BindLabel(skip);
    auto merged_load = guarded.LoadUniform(Uniform{16, ValueType::V128});
    UniformEliminationPass::Run(&guarded, info, FeatureSet{});
    REQUIRE(merged_load.Def()->GetOp() ==
            (xmm_forward_off ? OpCode::LoadUniform : OpCode::BitCast));

    // With no read between them, the first whole-XMM store is dead only while
    // this forwarding family is enabled.  The off mode is intentionally a
    // precise rollback: neither vector forwarding nor its dead-store sweep
    // touches the XMM range.
    Block dead_store{2, Location{0x3000}};
    auto old_value = dead_store.LoadImm(Imm{2u}).SetType(ValueType::V128);
    auto new_value = dead_store.LoadImm(Imm{3u}).SetType(ValueType::V128);
    dead_store.StoreUniform(Uniform{16, ValueType::V128}, old_value);
    dead_store.StoreUniform(Uniform{16, ValueType::V128}, new_value);
    UniformEliminationPass::Run(&dead_store, info, FeatureSet{});
    size_t stores{};
    for (const auto& inst : dead_store.GetInstList()) {
        stores += inst.GetOp() == OpCode::StoreUniform;
    }
    REQUIRE(stores == (xmm_forward_off ? 2 : 1));

    // Phase 2 seeds the byte table from a computed XMM load as well as a
    // store. This is the hot AES/GHASH shape: a live-in key/state register is
    // read repeatedly without an intervening architectural write.
    Block repeated_load{3, Location{0x4000}};
    auto first_vector_load = repeated_load.LoadUniform(Uniform{16, ValueType::V128});
    auto low_lane_load = repeated_load.LoadUniform(Uniform{16, ValueType::V64});
    auto second_vector_load = repeated_load.LoadUniform(Uniform{16, ValueType::V128});
    auto first_lane_load = repeated_load.LoadUniform(Uniform{24, ValueType::U64});
    auto second_lane_load = repeated_load.LoadUniform(Uniform{24, ValueType::U64});
    UniformEliminationPass::Run(&repeated_load, info, FeatureSet{});
    REQUIRE(first_vector_load.Def()->GetOp() == OpCode::LoadUniform);
    REQUIRE(low_lane_load.Def()->GetOp() ==
            (!xmm_forward_off && !xmm_ssa_fwd2_off && !xmm_narrow_fwd_off
                     ? OpCode::BitCast
                     : OpCode::LoadUniform));
    REQUIRE(first_lane_load.Def()->GetOp() == OpCode::LoadUniform);
    const auto repeated_vector_expected =
            (!xmm_forward_off && !xmm_ssa_fwd2_off && !xmm_narrow_fwd_off)
                    ? OpCode::BitCast
                    : OpCode::LoadUniform;
    const auto repeated_lane_expected =
            (!xmm_forward_off && !xmm_ssa_fwd2_off) ? OpCode::BitCast
                                                    : OpCode::LoadUniform;
    REQUIRE(second_vector_load.Def()->GetOp() == repeated_vector_expected);
    REQUIRE(second_lane_load.Def()->GetOp() == repeated_lane_expected);

    // An intervening write must still replace the load fact byte-for-byte.
    Block invalidated_load{4, Location{0x5000}};
    invalidated_load.LoadUniform(Uniform{16, ValueType::V128});
    auto replacement = invalidated_load.LoadImm(Imm{4u}).SetType(ValueType::V128);
    invalidated_load.StoreUniform(Uniform{16, ValueType::V128}, replacement);
    auto after_store = invalidated_load.LoadUniform(Uniform{16, ValueType::V128});
    UniformEliminationPass::Run(&invalidated_load, info, FeatureSet{});
    REQUIRE(after_store.Def()->GetOp() ==
            (xmm_forward_off ? OpCode::LoadUniform : OpCode::BitCast));

    // PIN_EXT performs its generic DSE before mapped GPR stores become
    // SetHostGPR. A narrow XMM load is still computed at that point, so a
    // second XMM-only sweep must collect the old store after the load folds;
    // the disjoint mapped GPR write is not an XMM observation boundary.
    UniformInfo pinned_info{.uniform_size = 128};
    auto map_pin = [&](std::uint32_t offset, std::uint16_t host_id) {
        UniformRegister pin{.uniform = Uniform{offset, ValueType::U64}};
        pin.host_reg.gpr = HostGPR{host_id};
        pin.host_reg.is_fpr = false;
        pinned_info.uniform_regs_map.Map(offset, offset + 8, pin);
        pinned_info.uni_gprs.Mark(host_id);
    };
    map_pin(0, 22);
    map_pin(8, 23);
    map_pin(16, 29);
    pinned_info.xmm_uniform_ranges.push_back({64, 80});
    Block pinned{6, Location{0x7000}};
    auto old_xmm = pinned.LoadImm(Imm{5u}).SetType(ValueType::V128);
    pinned.StoreUniform(Uniform{64, ValueType::V128}, old_xmm);
    auto pinned_lane = pinned.LoadUniform(Uniform{64, ValueType::V64});
    auto pin_value = pinned.LoadImm(Imm{std::uint64_t{7}}).SetType(ValueType::U64);
    pinned.StoreUniform(Uniform{0, ValueType::U64}, pin_value);
    auto new_xmm = pinned.LoadImm(Imm{6u}).SetType(ValueType::V128);
    pinned.StoreUniform(Uniform{64, ValueType::V128}, new_xmm);
    UniformEliminationPass::Run(&pinned, pinned_info, FeatureSet{});
    REQUIRE(pinned_lane.Def()->GetOp() ==
            (xmm_forward_off || xmm_narrow_fwd_off ? OpCode::LoadUniform
                                                   : OpCode::BitCast));
    size_t pinned_xmm_stores{};
    for (const auto& inst : pinned.GetInstList()) {
        if (inst.GetOp() != OpCode::StoreUniform) continue;
        const auto uniform = inst.GetArg<Uniform>(0);
        pinned_xmm_stores += uniform.GetOffset() == 64;
    }
    REQUIRE(pinned_xmm_stores ==
            (xmm_forward_off || xmm_narrow_fwd_off ? 2 : 1));
}

TEST_CASE("XMM fault captures retain the latest SSA value and remove only safe duplicates") {
    using namespace swift::runtime::ir;

    UniformInfo info{.uniform_size = 32};
    info.xmm_uniform_ranges.push_back({0, 16});

    struct Shape {
        std::unique_ptr<Block> block;
        Inst* observation{};
        Value raw{};
        Value product{};
    };

    auto make_triad = [] {
        auto block = std::make_unique<Block>(0, Location{0x3410});
        auto input_address = block->LoadImm(Imm{swift::u64{0x1000}})
                                     .SetType(ValueType::U64);
        auto raw = block->LoadMemory(Operand{input_address}).SetType(ValueType::V128);
        block->StoreUniform(Uniform{0, ValueType::V128}, raw);
        auto scalar = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::V128);
        auto product = block->VecFMul(raw, scalar, Imm{64u}).SetType(ValueType::V128);
        block->StoreUniform(Uniform{0, ValueType::V128}, product);
        auto rhs_address = block->LoadImm(Imm{swift::u64{0x2000}})
                                   .SetType(ValueType::U64);
        auto rhs = block->LoadMemory(Operand{rhs_address}).SetType(ValueType::V128);
        auto sum = block->VecFAdd(product, rhs, Imm{64u}).SetType(ValueType::V128);
        block->StoreUniform(Uniform{0, ValueType::V128}, sum);
        return Shape{std::move(block), rhs.Def(), raw, product};
    };

    auto run_triad = [&] {
        auto shape = make_triad();
        auto features = FeatureSet{};
        UniformEliminationPass::Run(shape.block.get(), info, features);
        UniformStoreSinkPass::Run(shape.block.get(), info, features);

        Inst* previous{};
        std::size_t stores{};
        for (auto& inst : shape.block->GetInstList()) {
            if (&inst == shape.observation) {
                REQUIRE(previous != nullptr);
                REQUIRE(previous->GetOp() == OpCode::StoreUniform);
                const auto capture = previous->GetArg<Value>(1);
                REQUIRE(capture == shape.product);
            }
            stores += inst.GetOp() == OpCode::StoreUniform;
            previous = &inst;
        }
        REQUIRE(stores == 2);
    };
    run_triad();

    auto scale_store_count = [&] {
        Block block{0, Location{0x3420}};
        auto input_address = block.LoadImm(Imm{swift::u64{0x1000}})
                                     .SetType(ValueType::U64);
        auto raw = block.LoadMemory(Operand{input_address}).SetType(ValueType::V128);
        block.StoreUniform(Uniform{0, ValueType::V128}, raw);
        auto scalar = block.LoadImm(Imm{swift::u64{0}}).SetType(ValueType::V128);
        auto product = block.VecFMul(raw, scalar, Imm{64u}).SetType(ValueType::V128);
        block.StoreUniform(Uniform{0, ValueType::V128}, product);
        auto output_address = block.LoadImm(Imm{swift::u64{0x3000}})
                                      .SetType(ValueType::U64);
        block.StoreMemory(Operand{output_address}, product);

        auto features = FeatureSet{};
        UniformEliminationPass::Run(&block, info, features);
        UniformStoreSinkPass::Run(&block, info, features);
        return std::count_if(block.GetInstList().begin(), block.GetInstList().end(),
                             [](const Inst& inst) {
                                 return inst.GetOp() == OpCode::StoreUniform;
                             });
    };
    REQUIRE(scale_store_count() == 1);

    auto add_capture_is_raw = [&] {
        Block block{0, Location{0x3430}};
        auto input_address = block.LoadImm(Imm{swift::u64{0x1000}})
                                     .SetType(ValueType::U64);
        auto raw = block.LoadMemory(Operand{input_address}).SetType(ValueType::V128);
        block.StoreUniform(Uniform{0, ValueType::V128}, raw);
        auto rhs_address = block.LoadImm(Imm{swift::u64{0x2000}})
                                   .SetType(ValueType::U64);
        auto rhs = block.LoadMemory(Operand{rhs_address}).SetType(ValueType::V128);
        auto sum = block.VecFAdd(raw, rhs, Imm{64u}).SetType(ValueType::V128);
        block.StoreUniform(Uniform{0, ValueType::V128}, sum);

        auto features = FeatureSet{};
        UniformEliminationPass::Run(&block, info, features);
        UniformStoreSinkPass::Run(&block, info, features);
        Inst* previous{};
        for (auto& inst : block.GetInstList()) {
            if (&inst == rhs.Def()) {
                REQUIRE(previous != nullptr);
                REQUIRE(previous->GetOp() == OpCode::StoreUniform);
                return previous->GetArg<Value>(1) == raw;
            }
            previous = &inst;
        }
        return false;
    };
    REQUIRE(add_capture_is_raw());

    // Uniform forwarding can turn a disjoint GPR store into SetHostGPR between
    // the latest XMM producer and the original fault boundary. The sink must
    // consume the recipe at that newly introduced observation and retarget the
    // existing carrier; it must not add a second store at the later load.
    UniformInfo mapped_info = info;
    UniformRegister mapped_gpr{.uniform = Uniform{16, ValueType::U64}};
    mapped_gpr.host_reg.gpr = HostGPR{22};
    mapped_gpr.host_reg.is_fpr = false;
    mapped_info.uniform_regs_map.Map(16, 24, mapped_gpr);
    mapped_info.uni_gprs.Mark(22);
    Block forwarded_boundary{0, Location{0x3438}};
    auto forwarded_address = forwarded_boundary.LoadImm(Imm{swift::u64{0x1000}})
                                     .SetType(ValueType::U64);
    auto forwarded_raw = forwarded_boundary.LoadMemory(Operand{forwarded_address})
                                 .SetType(ValueType::V128);
    forwarded_boundary.StoreUniform(Uniform{0, ValueType::V128}, forwarded_raw);
    auto forwarded_scalar = forwarded_boundary.LoadImm(Imm{swift::u64{0}})
                                    .SetType(ValueType::V128);
    auto forwarded_product = forwarded_boundary
            .VecFMul(forwarded_raw, forwarded_scalar, Imm{64u})
            .SetType(ValueType::V128);
    forwarded_boundary.StoreUniform(Uniform{0, ValueType::V128}, forwarded_product);
    auto mapped_value = forwarded_boundary.LoadImm(Imm{swift::u64{7}})
                                .SetType(ValueType::U64);
    forwarded_boundary.StoreUniform(Uniform{16, ValueType::U64}, mapped_value);
    auto* mapped_write = &forwarded_boundary.GetInstList().back();
    auto forwarded_rhs_address = forwarded_boundary.LoadImm(Imm{swift::u64{0x2000}})
                                         .SetType(ValueType::U64);
    [[maybe_unused]] auto forwarded_rhs =
            forwarded_boundary.LoadMemory(Operand{forwarded_rhs_address})
                    .SetType(ValueType::V128);
    auto forwarded_features = FeatureSet{};
    UniformEliminationPass::Run(&forwarded_boundary, mapped_info,
                                forwarded_features);
    REQUIRE(mapped_write->GetOp() == OpCode::SetHostGPR);
    UniformStoreSinkPass::Run(&forwarded_boundary, mapped_info,
                              forwarded_features);
    Inst* before_mapped_write{};
    for (auto& inst : forwarded_boundary.GetInstList()) {
        if (&inst == mapped_write) break;
        before_mapped_write = &inst;
    }
    REQUIRE(before_mapped_write != nullptr);
    REQUIRE(before_mapped_write->GetOp() == OpCode::StoreUniform);
    REQUIRE(before_mapped_write->GetArg<Value>(1) == forwarded_product);

    // Keep the last pre-fault carrier so register allocation can commit it
    // directly into the resident home before the faulting instruction.
    UniformInfo resident_info = info;
    UniformRegister resident{.uniform = Uniform{0, ValueType::V128}};
    resident.host_reg.fpr = HostFPR{17};
    resident.host_reg.is_fpr = true;
    resident_info.uniform_regs_map.Map(0, 16, resident);
    resident_info.uni_fprs.Mark(17);
    auto resident_shape = make_triad();
    UniformStoreSinkPass::CaptureLatestCaptures(resident_shape.block.get(),
                                                  resident_info);
    const auto& resident_recipes = resident_shape.block->GetUniformCaptureRecipes();
    REQUIRE(resident_recipes.size() == 1);
    REQUIRE(resident_recipes.front().boundary == resident_shape.observation);
    REQUIRE(resident_recipes.front().value == resident_shape.product);

    const std::array affected_producers{
            OpCode::VecFAdd, OpCode::VecFSub, OpCode::VecFMul,
            OpCode::VecFDiv, OpCode::VecAdd,  OpCode::VecSub,
            OpCode::VecXor,
    };
    for (const auto op : affected_producers) {
        Block block{0, Location{swift::u64{0x3440u + static_cast<swift::u32>(op)}}};
        auto input_address = block.LoadImm(Imm{swift::u64{0x1000}})
                                     .SetType(ValueType::U64);
        auto raw = block.LoadMemory(Operand{input_address}).SetType(ValueType::V128);
        block.StoreUniform(Uniform{0, ValueType::V128}, raw);
        auto right = block.LoadImm(Imm{swift::u64{0}}).SetType(ValueType::V128);
        Value produced;
        switch (op) {
            case OpCode::VecFAdd: produced = block.VecFAdd(raw, right, Imm{64u}); break;
            case OpCode::VecFSub: produced = block.VecFSub(raw, right, Imm{64u}); break;
            case OpCode::VecFMul: produced = block.VecFMul(raw, right, Imm{64u}); break;
            case OpCode::VecFDiv: produced = block.VecFDiv(raw, right, Imm{64u}); break;
            case OpCode::VecAdd: produced = block.VecAdd(raw, right, Imm{64u}); break;
            case OpCode::VecSub: produced = block.VecSub(raw, right, Imm{64u}); break;
            case OpCode::VecXor: produced = block.VecXor(raw, right); break;
            default: FAIL("missing fault-capture producer case");
        }
        produced = produced.SetType(ValueType::V128);
        block.StoreUniform(Uniform{0, ValueType::V128}, produced);
        auto rhs_address = block.LoadImm(Imm{swift::u64{0x2000}})
                                   .SetType(ValueType::U64);
        auto rhs = block.LoadMemory(Operand{rhs_address}).SetType(ValueType::V128);
        auto final_value = block.VecXor(produced, rhs).SetType(ValueType::V128);
        block.StoreUniform(Uniform{0, ValueType::V128}, final_value);

        auto features = FeatureSet{};
        UniformEliminationPass::Run(&block, info, features);
        UniformStoreSinkPass::Run(&block, info, features);
        Inst* previous{};
        bool checked{};
        for (auto& inst : block.GetInstList()) {
            if (&inst == rhs.Def()) {
                REQUIRE(previous != nullptr);
                REQUIRE(previous->GetOp() == OpCode::StoreUniform);
                REQUIRE(previous->GetArg<Value>(1) == produced);
                checked = true;
            }
            previous = &inst;
        }
        REQUIRE(checked);
    }

    // Two stores are not proof that the faulting producer has another live
    // consumer: both stores may be DSE victims in the same batch. Keep at
    // least one use unless a non-uniform-store path reaches an observer.
    Block stranded{0, Location{0x3490}};
    auto address = stranded.LoadImm(Imm{swift::u64{0x1000}}).SetType(ValueType::U64);
    auto loaded = stranded.LoadMemory(Operand{address}).SetType(ValueType::V128);
    stranded.StoreUniform(Uniform{0, ValueType::V128}, loaded);
    stranded.StoreUniform(Uniform{0, ValueType::V128}, loaded);
    auto replacement = stranded.LoadImm(Imm{swift::u64{0}}).SetType(ValueType::V128);
    stranded.StoreUniform(Uniform{0, ValueType::V128}, replacement);
    auto precise = FeatureSet{};
    UniformEliminationPass::Run(&stranded, info, precise);
    UniformStoreSinkPass::Run(&stranded, info, precise);
    const auto retained_uses = std::count_if(
            stranded.GetInstList().begin(), stranded.GetInstList().end(),
            [&](const Inst& inst) {
                return inst.GetOp() == OpCode::StoreUniform &&
                       inst.GetArg<Value>(1) == loaded;
            });
    REQUIRE(retained_uses == 1);
}

TEST_CASE("Uniform fast path is instruction-identical to the legacy pass") {
    using namespace swift::runtime::ir;

    UniformInfo info{.uniform_size = 64};
    auto make_uniform_block = [] {
        auto block = std::make_unique<Block>(0, Location{0x3000});

        // Full overwrite: the first store is dead.
        auto first = block->LoadImm(Imm{std::uint64_t(0x1111111111111111ull)}).SetType(ValueType::U64);
        auto latest = block->LoadImm(Imm{std::uint64_t(0x2222222222222222ull)}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{0, ValueType::U64}, first);
        block->StoreUniform(Uniform{0, ValueType::U64}, latest);

        // Narrow forwarding must stay a BitExtract with the same source/type.
        block->LoadUniform(Uniform{2, ValueType::U16});

        // The opaque call invalidates forwarding facts. The following load
        // must remain a LoadUniform in both implementations.
        Params params{};
        block->CallDynamic(Lambda(Imm{1u}), params);
        block->LoadUniform(Uniform{2, ValueType::U16});

        // Two overlapping later stores jointly cover the earlier U64 store.
        // This pins byte-range aliasing rather than just exact-offset DSE.
        auto wide = block->LoadImm(Imm{std::uint64_t(0x3333333333333333ull)}).SetType(ValueType::U64);
        auto low = block->LoadImm(Imm{0x44444444u}).SetType(ValueType::U32);
        auto high = block->LoadImm(Imm{0x55555555u}).SetType(ValueType::U32);
        block->StoreUniform(Uniform{16, ValueType::U64}, wide);
        block->StoreUniform(Uniform{16, ValueType::U32}, low);
        block->StoreUniform(Uniform{20, ValueType::U32}, high);
        // Spans two different stored values, so it must not forward.
        block->LoadUniform(Uniform{18, ValueType::U32});
        return block;
    };

    auto legacy = make_uniform_block();
    auto fast = make_uniform_block();
    UniformEliminationPass::Run(legacy.get(), info, false, FeatureSet{});
    UniformEliminationPass::Run(fast.get(), info, true, FeatureSet{});

    // ToString includes every opcode, result type, argument and defining id;
    // equality is therefore a per-instruction IR comparison, not just counts.
    REQUIRE(fast->ToString() == legacy->ToString());
    size_t stores = 0;
    size_t loads = 0;
    size_t extracts = 0;
    for (const auto& inst : fast->GetInstList()) {
        stores += inst.GetOp() == OpCode::StoreUniform;
        loads += inst.GetOp() == OpCode::LoadUniform;
        extracts += inst.GetOp() == OpCode::BitExtract;
    }
    REQUIRE(stores == 3);
    REQUIRE(loads == 2);
    REQUIRE(extracts == 1);

    // No-uniform early return must preserve even an otherwise trivial block.
    Block legacy_basic{1, Location{0x4000}};
    Block fast_basic{1, Location{0x4000}};
    legacy_basic.LoadImm(Imm{7u}).SetType(ValueType::U32);
    fast_basic.LoadImm(Imm{7u}).SetType(ValueType::U32);
    UniformEliminationPass::Run(&legacy_basic, info, false, FeatureSet{});
    UniformEliminationPass::Run(&fast_basic, info, true, FeatureSet{});
    REQUIRE(fast_basic.ToString() == legacy_basic.ToString());
}

namespace {

struct IRBuildFixture {
    explicit IRBuildFixture(bool fast)
            : builder(1, true, fast) {
        using namespace swift::runtime::ir;

        function = builder.AppendFunction(Location{0x1000}, Location{0x1200});
        const Local local{.id = 7, .type = ValueType::U64};
        function->DefineLocal(local);

        auto imm = function->LoadImm(Imm{std::uint64_t(0x1122334455667788ull)})
                           .SetType(ValueType::U64);
        first_inst = imm.Def();
        auto uniform =
                function->LoadUniform(Uniform{24, ValueType::U64}).SetType(ValueType::U64);
        auto operand =
                function->Add(imm, Operand{uniform, imm, OperandLsl}).SetType(ValueType::U64);
        auto selected =
                function->CondSelect(Cond::EQ, operand, uniform).SetType(ValueType::U64);
        function->SaveFlags(selected, Flags::All);
        function->StoreLocal(local, selected);
        auto local_value = function->LoadLocal(local).SetType(ValueType::U64);

        // Lambda-as-Value plus three ordinary Value slots.
        [[maybe_unused]] auto lambda_result =
                function->CallLambda(Lambda{selected}, imm, uniform, operand)
                        .SetType(ValueType::U64);

        // Params deliberately repeats `imm`: order and duplicates are part of
        // the HIR use-list contract, not a set.
        Params params{};
        params.Push(imm);
        params.Push(Imm{0xa5a5u});
        params.Push(uniform);
        params.Push(imm);
        [[maybe_unused]] auto dynamic_result =
                function->CallDynamic(Lambda{Imm{std::uint64_t(0x1234ull)}}, params)
                        .SetType(ValueType::U64);
        function->StoreUniform(Uniform{40, ValueType::U64}, local_value);

        // More than one 64 KiB Inst arena chunk even at the minimum possible
        // slot size. All instructions stay live until both fixtures have been
        // compared, pinning pointer stability across growth.
        for (unsigned i = 0; i < 1536; ++i) {
            function->Nop();
        }

        auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x1100}});
        builder.SetCurBlock(next);
        auto tail = function->Add(selected, Operand{imm}).SetType(ValueType::U64);
        function->StoreUniform(Uniform{48, ValueType::U64}, tail);
        function->EndBlock(terminal::ReturnToDispatch{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
    }

    swift::runtime::ir::HIRBuilder builder{1, false, FeatureSet{}};
    swift::runtime::ir::HIRFunction* function{};
    swift::runtime::ir::Inst* first_inst{};
};

std::string IRCaptureArg(swift::runtime::ir::Arg& arg) {
    using namespace swift::runtime::ir;
    switch (arg.GetType()) {
        case ArgType::Void:
            return "void";
        case ArgType::Value: {
            const auto value = arg.Get<Value>();
            return fmt::format("value:{}:{}", value.Id(), static_cast<unsigned>(value.Type()));
        }
        case ArgType::Imm: {
            const auto imm = arg.Get<Imm>();
            return fmt::format("imm:{}:{}", static_cast<unsigned>(imm.GetType()), imm.Get());
        }
        case ArgType::Cond:
            return fmt::format("cond:{}", static_cast<unsigned>(arg.Get<Cond>()));
        case ArgType::Flags:
            return fmt::format("flags:{}", static_cast<std::uint64_t>(arg.Get<Flags>()));
        case ArgType::Operand: {
            const auto op = arg.Get<Operand::Op>();
            return fmt::format(
                    "operand:{}:{}", static_cast<unsigned>(op.type), op.shift_ext);
        }
        case ArgType::Local: {
            const auto local = arg.Get<Local>();
            return fmt::format(
                    "local:{}:{}", local.id, static_cast<unsigned>(local.type));
        }
        case ArgType::Uniform: {
            const auto uniform = arg.Get<Uniform>();
            return fmt::format("uniform:{}:{}",
                               uniform.GetOffset(),
                               static_cast<unsigned>(uniform.GetType()));
        }
        case ArgType::Lambda: {
            const auto lambda = arg.Get<Lambda>();
            if (lambda.IsValue()) {
                const auto value = lambda.GetValue();
                return fmt::format(
                        "lambda-value:{}:{}", value.Id(), static_cast<unsigned>(value.Type()));
            }
            const auto imm = lambda.GetImm();
            return fmt::format(
                    "lambda-imm:{}:{}", static_cast<unsigned>(imm.GetType()), imm.Get());
        }
        case ArgType::Params: {
            std::string out{"params"};
            for (const auto& param : arg.Get<Params>()) {
                if (param.data.IsValue()) {
                    out += fmt::format(
                            ":v{}:{}", param.data.value.Id(),
                            static_cast<unsigned>(param.data.value.Type()));
                } else {
                    out += fmt::format(
                            ":i{}:{}", static_cast<unsigned>(param.data.imm.GetType()),
                            param.data.imm.Get());
                }
            }
            return out;
        }
    }
    return "invalid";
}

}  // namespace

TEST_CASE("IR build fast path is field-identical across all argument shapes") {
    using namespace swift::runtime::ir;

    IRBuildFixture legacy{false};
    IRBuildFixture fast{true};
    auto* lhs = legacy.function;
    auto* rhs = fast.function;

    REQUIRE(lhs->MaxBlockCount() == rhs->MaxBlockCount());
    REQUIRE(lhs->MaxInstrCount() == rhs->MaxInstrCount());
    REQUIRE(lhs->MaxLocalCount() == rhs->MaxLocalCount());
    REQUIRE(lhs->GetHIRBlocks().size() == rhs->GetHIRBlocks().size());
    REQUIRE(lhs->GetHIRBlocksRPO().size() == rhs->GetHIRBlocksRPO().size());

    // The first instruction was allocated before >1536 later live objects.
    // Its address and initialized fields must remain valid after arena growth.
    REQUIRE(legacy.first_inst->GetOp() == OpCode::LoadImm);
    REQUIRE(fast.first_inst->GetOp() == OpCode::LoadImm);
    REQUIRE(legacy.first_inst->GetArg<Imm>(0).Get() == 0x1122334455667788ull);
    REQUIRE(fast.first_inst->GetArg<Imm>(0).Get() == 0x1122334455667788ull);

    for (std::size_t block_index = 0; block_index < lhs->GetHIRBlocks().size();
         ++block_index) {
        auto* left_hir = lhs->GetHIRBlocks()[block_index];
        auto* right_hir = rhs->GetHIRBlocks()[block_index];
        INFO("block " << block_index);
        REQUIRE(left_hir->GetOrderId() == right_hir->GetOrderId());
        REQUIRE(left_hir->GetPredecessors().size() == right_hir->GetPredecessors().size());
        REQUIRE(left_hir->GetSuccessors().size() == right_hir->GetSuccessors().size());
        for (std::size_t i = 0; i < left_hir->GetPredecessors().size(); ++i) {
            REQUIRE(left_hir->GetPredecessors()[i]->GetOrderId() ==
                    right_hir->GetPredecessors()[i]->GetOrderId());
        }
        for (std::size_t i = 0; i < left_hir->GetSuccessors().size(); ++i) {
            REQUIRE(left_hir->GetSuccessors()[i]->GetOrderId() ==
                    right_hir->GetSuccessors()[i]->GetOrderId());
        }

        auto* left_block = left_hir->GetBlock();
        auto* right_block = right_hir->GetBlock();
        REQUIRE(left_block->GetStartLocation() == right_block->GetStartLocation());
        REQUIRE(left_block->GetEndLocation() == right_block->GetEndLocation());
        REQUIRE(fmt::format("{}", left_block->GetTerminal()) ==
                fmt::format("{}", right_block->GetTerminal()));

        auto left_it = left_block->GetInstList().begin();
        auto right_it = right_block->GetInstList().begin();
        for (; left_it != left_block->GetInstList().end() &&
               right_it != right_block->GetInstList().end();
             ++left_it, ++right_it) {
            const auto& left_inst = *left_it;
            const auto& right_inst = *right_it;
            INFO("instruction id " << left_inst.Id());
            REQUIRE(left_inst.GetOp() == right_inst.GetOp());
            REQUIRE(left_inst.Id() == right_inst.Id());
            REQUIRE(left_inst.ReturnType() == right_inst.ReturnType());
            REQUIRE(left_inst.VirRegID() == right_inst.VirRegID());
            REQUIRE(const_cast<Inst&>(left_inst).GetUses(false) ==
                    const_cast<Inst&>(right_inst).GetUses(false));
            for (unsigned slot = 0; slot < Inst::max_args; ++slot) {
                INFO("physical argument slot " << slot);
                REQUIRE(IRCaptureArg(left_inst.ArgAt(slot)) ==
                        IRCaptureArg(right_inst.ArgAt(slot)));
            }
            auto left_pseudo = const_cast<Inst&>(left_inst).GetPseudoOperations();
            auto right_pseudo = const_cast<Inst&>(right_inst).GetPseudoOperations();
            REQUIRE(left_pseudo.size() == right_pseudo.size());
            for (std::size_t i = 0; i < left_pseudo.size(); ++i) {
                REQUIRE(left_pseudo[i]->Id() == right_pseudo[i]->Id());
                REQUIRE(left_pseudo[i]->GetOp() == right_pseudo[i]->GetOp());
            }
        }
        REQUIRE(left_it == left_block->GetInstList().end());
        REQUIRE(right_it == right_block->GetInstList().end());
    }

    const auto& left_values = lhs->GetHIRValues();
    const auto& right_values = rhs->GetHIRValues();
    REQUIRE(left_values.size() == right_values.size());
    for (std::size_t id = 0; id < left_values.size(); ++id) {
        auto* left_value = left_values[id];
        auto* right_value = right_values[id];
        INFO("HIR value id " << id);
        REQUIRE((left_value == nullptr) == (right_value == nullptr));
        if (!left_value) {
            continue;
        }
        REQUIRE(left_value->value.Id() == right_value->value.Id());
        REQUIRE(left_value->value.Type() == right_value->value.Type());
        REQUIRE(left_value->block->GetOrderId() == right_value->block->GetOrderId());
        REQUIRE(left_value->allocated.type == right_value->allocated.type);

        auto left_use = left_value->uses.begin();
        auto right_use = right_value->uses.begin();
        for (; left_use != left_value->uses.end() && right_use != right_value->uses.end();
             ++left_use, ++right_use) {
            REQUIRE(left_use->inst->Id() == right_use->inst->Id());
            REQUIRE(left_use->arg_idx == right_use->arg_idx);
        }
        REQUIRE(left_use == left_value->uses.end());
        REQUIRE(right_use == right_value->uses.end());
    }
}
