#include <catch2/catch_test_macros.hpp>
#include <array>
#include <optional>
#include "runtime/backend/arm64/alloc/fpr_allocation_plan.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/backend/arm64/pshufd_direct.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("shared-index direct shuffle selection keeps component and retry contracts", "[shuffle-plan]") {
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xddc0})};
        IntrusivePtr<Block> external{new Block(0, Location{0xddd0})};
        const auto source = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto [low, high] = arm64::PshufdIndexMask(0x4e);
        if (scenario == 2) low ^= 1;
        const auto mask = block->VecLoadConst(Imm{low}, Imm{high}).SetType(ValueType::V128);
        std::array<Value, 2> shuffles;
        if (scenario != 4) for (unsigned i = 0; i < shuffles.size(); ++i) {
            shuffles[i] = block->VecShuffle32Indexed(scenario == 3 ? mask : source, mask).SetType(ValueType::V128);
            block->StoreUniform(Uniform{16 + i * 16, ValueType::V128}, shuffles[i]);
        }
        if (scenario == 1) block->VecTableLookup8(source, mask).SetType(ValueType::V128);
        if (scenario == 5) external->StoreUniform(Uniform{0, ValueType::V128}, mask);
        block->SetTerminal(terminal::ReturnToHost{});
        external->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        if (scenario == 6) {
            allocation.BeginTargetPlanning();
            allocation.DisableFPRCoalescing();
        }
        arm64::BlockAnalysisIndex index;
        index.Build(block.get());
        REQUIRE(index.HasVectorConstants());
        arm64::SelectPshufdDirect(index, allocation);
        REQUIRE(allocation.IsPshufdDirect(mask.Id()) == (scenario == 0));
        for (const auto shuffle : shuffles) if (shuffle.Defined())
            REQUIRE(allocation.IsPshufdDirect(shuffle.Id()) == (scenario == 0));
        allocation.ResetAllocations();
        REQUIRE_FALSE(allocation.IsPshufdDirect(mask.Id()));
        index.Build(external.get());
        REQUIRE_FALSE(index.HasVectorConstants());
        index.Build(block.get());
        arm64::SelectPshufdDirect(index, allocation);
        REQUIRE(allocation.IsPshufdDirect(mask.Id()) == (scenario == 0));
        if (scenario == 6) allocation.AbortTargetPlanning();
    }
}

TEST_CASE("direct shuffle plans validate every canonical control and exact consumer contract", "[shuffle-plan]") {
    for (unsigned control : arm64::kPshufdDirectControls)
        for (unsigned scenario = 0; scenario < 6; ++scenario) {
            CAPTURE(control, scenario);
            IntrusivePtr<Block> block{new Block(0, Location{0xdd80})};
            const auto source = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
            auto [low, high] = arm64::PshufdIndexMask(control);
            if (scenario == 1) low ^= 1;
            const auto mask = block->VecLoadConst(Imm{low}, Imm{high}).SetType(ValueType::V128);
            Value result;
            if (scenario != 5) {
                result = block->VecShuffle32Indexed(scenario == 4 ? mask : source, mask).SetType(ValueType::V128);
                block->StoreUniform(Uniform{16, ValueType::V128}, result);
            }
            if (scenario == 3) block->VecTableLookup8(source, mask).SetType(ValueType::V128);
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
            allocation.MarkPshufdDirect(mask.Id());
            if (result.Defined() && scenario != 2) allocation.MarkPshufdDirect(result.Id());
            arm64::BlockAnalysisIndex index;
            index.Build(block.get());
            const auto shared = arm64::BuildFPRAllocationPlan(block.get(), allocation, FeatureSet{}, false, false, &index);
            const auto own = arm64::BuildFPRAllocationPlan(block.get(), allocation, FeatureSet{}, false, false);
            REQUIRE(shared.valid == (scenario == 0));
            REQUIRE(shared.valid == own.valid);
            REQUIRE(shared.rejected == own.rejected);
            REQUIRE(shared.controls == own.controls);
            if (shared.valid) {
                REQUIRE(shared.Control(mask.Def()) == control);
                REQUIRE(shared.Control(result.Def()) == control);
            } else REQUIRE(shared.rejected == mask.Def());
        }
}

TEST_CASE("shuffle selection resets and physical ties resolve aliases without accepting spills", "[shuffle-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xdda0})};
    const auto left = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    const auto same = block->LoadUniform(Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    const auto other = block->LoadUniform(Uniform{32, ValueType::V128}).SetType(ValueType::V128);
    const auto alias = block->LoadUniform(Uniform{48, ValueType::V128}).SetType(ValueType::V128);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    for (const auto value : {left, same, other, alias}) REQUIRE_FALSE(allocation.IsPshufdDirect(value.Id()));
    REQUIRE_FALSE(allocation.IsPshufdDirect(block->MaxInstrId() + 1));
    allocation.MarkPshufdDirect(same.Id());
    REQUIRE(allocation.IsPshufdDirect(same.Id()));
    REQUIRE_FALSE(allocation.IsPshufdDirect(left.Id()));
    allocation.MapRegister(left.Id(), HostFPR{17});
    allocation.MapRegister(same.Id(), HostFPR{17});
    allocation.MapRegister(other.Id(), HostFPR{18});
    allocation.MapReference(left.Id(), alias.Id());
    REQUIRE(allocation.SharesFPR(left, same));
    REQUIRE(allocation.SharesFPR(alias, same));
    REQUIRE_FALSE(allocation.SharesFPR(left, other));
    REQUIRE_FALSE(allocation.SharesFPR(left, Value{}));
    allocation.MapRegister(other.Id(), HostGPR{17});
    REQUIRE_FALSE(allocation.SharesFPR(left, other));
    allocation.MapMemSpill(other.Id(), SpillSlot{17});
    REQUIRE_FALSE(allocation.SharesFPR(left, other));
    REQUIRE_FALSE(allocation.SharesFPR(other, left));
    allocation.MapRegister(left.Id(), HostFPR{32});
    allocation.MapRegister(same.Id(), HostFPR{32});
    REQUIRE_FALSE(allocation.SharesFPR(left, same));
    allocation.ResetAllocations();
    REQUIRE_FALSE(allocation.IsPshufdDirect(same.Id()));
    REQUIRE_FALSE(allocation.SharesFPR(left, same));
    allocation.MarkPshufdDirect(left.Id());
    REQUIRE(allocation.IsPshufdDirect(left.Id()));
    REQUIRE_FALSE(allocation.IsPshufdDirect(same.Id()));
}

TEST_CASE("shuffle ties reject live old values and resident guest reads", "[shuffle-plan]") {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xdd90})};
        const auto left = scenario == 2
                ? block->GetHostFPR(HostRegIndex(17), Imm{0u}).SetType(ValueType::V128)
                : block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        const auto result = block->VecShuffle32TwoSrc(left, left, Imm{scenario == 3 ? 0x4eu : 0xe0u}).SetType(ValueType::V128);
        block->StoreUniform(Uniform{16, ValueType::V128}, result);
        if (scenario == 1) block->StoreUniform(Uniform{32, ValueType::V128}, left);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        FeatureSet features{};
        features.sse_shufps_imm = true;
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, features};
        allocation.MapRegister(left.Id(), HostFPR{17});
        allocation.MapRegister(result.Id(), HostFPR{static_cast<swift::u16>(scenario == 4 ? 18 : 17)});
        if (scenario == 2) allocation.MarkHostReadCoalesced(left.Id());
        arm64::BlockAnalysisIndex index;
        index.Build(block.get());
        const auto plan = arm64::BuildFPRAllocationPlan(block.get(), allocation, features, false, false, &index);
        REQUIRE(plan.valid == (scenario != 1 && scenario != 2));
        REQUIRE(plan.Has(result.Def(), arm64::FPRAllocationPlan::ShuffleTie) == (scenario == 0));
        if (!plan.valid) REQUIRE(plan.rejected == result.Def());
        if (scenario == 4) {
            REQUIRE(plan.actions.empty());
            REQUIRE(plan.controls.empty());
        }
    }
}

TEST_CASE("shuffle plan decisions are order-independent with prefix certification", "[shuffle-plan]") {
    struct Outcome {
        bool valid{};
        const Inst* rejected{};
        std::array<std::optional<swift::u8>, 2> control{};
        std::array<const Inst*, 2> masks{};
    };
    const auto build = [](bool second_first, bool corrupt_second) {
        IntrusivePtr<Block> block{new Block(0, Location{0xddb0})};
        const auto source = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        const std::array<unsigned, 2> controls{0x4e, 0x39};
        std::array<Value, 2> masks, results;
        const auto make = [&](unsigned i) {
            auto [low, high] = arm64::PshufdIndexMask(static_cast<swift::u8>(controls[i]));
            if (i == 1 && corrupt_second) low ^= 1;
            masks[i] = block->VecLoadConst(Imm{low}, Imm{high}).SetType(ValueType::V128);
            results[i] = block->VecShuffle32Indexed(source, masks[i]).SetType(ValueType::V128);
            block->StoreUniform(Uniform{16 + i * 16, ValueType::V128}, results[i]);
        };
        if (second_first) { make(1); make(0); } else { make(0); make(1); }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        for (unsigned i = 0; i < 2; ++i) {
            allocation.MarkPshufdDirect(masks[i].Id());
            allocation.MarkPshufdDirect(results[i].Id());
        }
        arm64::BlockAnalysisIndex index;
        index.Build(block.get());
        const auto shared = arm64::BuildFPRAllocationPlan(block.get(), allocation, FeatureSet{}, false, false, &index);
        const auto own = arm64::BuildFPRAllocationPlan(block.get(), allocation, FeatureSet{}, false, false);
        REQUIRE(shared.valid == own.valid);
        REQUIRE(shared.rejected == own.rejected);
        REQUIRE(shared.actions == own.actions);
        REQUIRE(shared.controls == own.controls);
        Outcome outcome;
        outcome.valid = shared.valid;
        outcome.rejected = shared.rejected;
        for (unsigned i = 0; i < 2; ++i) {
            outcome.masks[i] = masks[i].Def();
            outcome.control[i] = shared.Control(results[i].Def());
        }
        return outcome;
    };
    const auto forward = build(false, false);
    const auto swapped = build(true, false);
    REQUIRE(forward.valid);
    REQUIRE(swapped.valid);
    REQUIRE(forward.control[0].has_value());
    REQUIRE(forward.control[1].has_value());
    REQUIRE(*forward.control[0] == 0x4e);
    REQUIRE(*forward.control[1] == 0x39);
    REQUIRE(forward.control == swapped.control);
    const auto corrupt_late = build(false, true);
    REQUIRE_FALSE(corrupt_late.valid);
    REQUIRE(corrupt_late.rejected == corrupt_late.masks[1]);
    REQUIRE(corrupt_late.control[0].has_value());
    REQUIRE(*corrupt_late.control[0] == 0x4e);
    REQUIRE_FALSE(corrupt_late.control[1].has_value());
    const auto corrupt_early = build(true, true);
    REQUIRE_FALSE(corrupt_early.valid);
    REQUIRE(corrupt_early.rejected == corrupt_early.masks[1]);
    REQUIRE_FALSE(corrupt_early.control[0].has_value());
    REQUIRE_FALSE(corrupt_early.control[1].has_value());
}

TEST_CASE("shuffle plans stay empty for stale indexes and cleared selections", "[shuffle-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xddc8})};
    const auto source = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    const auto [low, high] = arm64::PshufdIndexMask(0x4e);
    const auto mask = block->VecLoadConst(Imm{low}, Imm{high}).SetType(ValueType::V128);
    const auto result = block->VecShuffle32Indexed(source, mask).SetType(ValueType::V128);
    block->StoreUniform(Uniform{16, ValueType::V128}, result);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.MarkPshufdDirect(mask.Id());
    allocation.MarkPshufdDirect(result.Id());
    IntrusivePtr<Block> other{new Block(0, Location{0xddd8})};
    other->LoadUniform(Uniform{0, ValueType::U64});
    other->SetTerminal(terminal::ReturnToHost{});
    other->ReIdInstr();
    arm64::BlockAnalysisIndex stale;
    stale.Build(other.get());
    const auto foreign = arm64::BuildFPRAllocationPlan(block.get(), allocation, FeatureSet{}, false, false, &stale);
    REQUIRE(foreign.valid);
    REQUIRE(foreign.actions.empty());
    REQUIRE(foreign.controls.empty());
    REQUIRE_FALSE(foreign.Control(mask.Def()).has_value());
    REQUIRE_FALSE(foreign.Has(result.Def(), arm64::FPRAllocationPlan::Pshufd));
    stale.Build(block.get());
    const auto fresh = arm64::BuildFPRAllocationPlan(block.get(), allocation, FeatureSet{}, false, false, &stale);
    REQUIRE(fresh.valid);
    REQUIRE(fresh.Control(result.Def()) == std::optional<swift::u8>{0x4e});
    allocation.ResetAllocations();
    const auto cleared = arm64::BuildFPRAllocationPlan(block.get(), allocation, FeatureSet{}, false, false, &stale);
    REQUIRE(cleared.valid);
    REQUIRE(cleared.actions.empty());
    REQUIRE(cleared.controls.empty());
    REQUIRE_FALSE(cleared.Control(result.Def()).has_value());
    allocation.MarkPshufdDirect(mask.Id());
    allocation.MarkPshufdDirect(result.Id());
    const auto restored = arm64::BuildFPRAllocationPlan(block.get(), allocation, FeatureSet{}, false, false, &stale);
    REQUIRE(restored.valid);
    REQUIRE(restored.actions == fresh.actions);
    REQUIRE(restored.controls == fresh.controls);
    REQUIRE(restored.selected == fresh.selected);
}

