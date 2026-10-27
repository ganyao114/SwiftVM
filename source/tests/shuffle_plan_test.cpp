#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <array>
#include <optional>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/fpr_preparation.h"
#include "runtime/backend/arm64/pshufd_direct.h"
#include "runtime/compile/analysis/block_index.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("shared-index direct shuffle selection keeps component and retry contracts", "[shuffle-plan]") {
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xddc0})};
        IntrusivePtr<Block> external{new Block(0, Location{0xddd0})};
        const auto source = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
        auto [low, high] = arm64::PshufdIndexMask(0x4e);
        if (scenario == 2) low ^= 1;
        const auto mask = block->VecLoadConst(ValueType::V128, Imm{low}, Imm{high});
        std::array<Value, 2> shuffles;
        if (scenario != 4)
            for (unsigned i = 0; i < shuffles.size(); ++i) {
                shuffles[i] = block->VecShuffle32Indexed(
                        ValueType::V128, scenario == 3 ? mask : source, mask);
                block->StoreUniform(Uniform{16 + i * 16, ValueType::V128}, shuffles[i]);
            }
        if (scenario == 1) block->VecTableLookup8(ValueType::V128, source, mask);
        if (scenario == 5) external->StoreUniform(Uniform{0, ValueType::V128}, mask);
        block->SetTerminal(terminal::ReturnToHost{});
        external->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        if (scenario == 6) {
            allocation.BeginTargetPlanning();
            allocation.DisableFPRCoalescing();
        }
        compile::BlockAnalysisIndex index;
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
            const auto source = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
            auto [low, high] = arm64::PshufdIndexMask(control);
            if (scenario == 1) low ^= 1;
            const auto mask = block->VecLoadConst(ValueType::V128, Imm{low}, Imm{high});
            Value result;
            if (scenario != 5) {
                result = block->VecShuffle32Indexed(
                        ValueType::V128, scenario == 4 ? mask : source, mask);
                block->StoreUniform(Uniform{16, ValueType::V128}, result);
            }
            if (scenario == 3) block->VecTableLookup8(ValueType::V128, source, mask);
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
            allocation.MarkPshufdDirect(mask.Id());
            if (result.Defined() && scenario != 2) allocation.MarkPshufdDirect(result.Id());
            compile::BlockAnalysisIndex index;
            index.Build(block.get());
            const auto shared = arm64::PrepareFPRContract(block.get(),
                                                          allocation,
                                                          FeatureSet{},
                                                          false,
                                                          false,
                                                          index,
                                                          allocation.Workspace());
            compile::BlockAnalysisIndex independent;
            independent.Build(block.get());
            const auto own = arm64::PrepareFPRContract(block.get(),
                                                       allocation,
                                                       FeatureSet{},
                                                       false,
                                                       false,
                                                       independent,
                                                       allocation.Workspace());
            REQUIRE(shared.valid == (scenario == 0));
            REQUIRE(shared.valid == own.valid);
            REQUIRE(shared.rejected == own.rejected);
            REQUIRE(shared.instructions == own.instructions);
            if (shared.valid) {
                REQUIRE(shared.Control(mask.Id()) == control);
                REQUIRE(shared.Control(result.Id()) == control);
                allocation.BeginTargetPlanning();
                allocation.CertifyFPR(shared);
                REQUIRE(allocation.Actions().PshufdControl(result.Id()) == control);
                allocation.EndTargetPlanning();
                REQUIRE(allocation.Actions().Ready());
                REQUIRE(allocation.Actions().FPRVerified(mask.Id(), arm64::FPRAction::Pshufd));
                REQUIRE(allocation.Actions().PshufdControl(result.Id()) == control);
                allocation.ResetAllocations();
                REQUIRE_FALSE(
                        allocation.Actions().FPRVerified(mask.Id(), arm64::FPRAction::Pshufd));
                REQUIRE_FALSE(allocation.Actions().PshufdControl(result.Id()));
            } else REQUIRE(shared.rejected == mask.Def());
        }
}

TEST_CASE("shuffle selection resets and physical ties resolve aliases without accepting spills", "[shuffle-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xdda0})};
    const auto left = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
    const auto same = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
    const auto other = block->LoadUniform(ValueType::V128, Uniform{32, ValueType::V128});
    const auto alias = block->LoadUniform(ValueType::V128, Uniform{48, ValueType::V128});
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
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
        const auto left =
                scenario == 2 ? block->GetHostFPR(ValueType::V128, HostRegIndex(17), Imm{0u})
                              : block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
        const auto result = block->VecShuffle32TwoSrc(
                ValueType::V128, left, left, Imm{scenario == 3 ? 0x4eu : 0xe0u});
        block->StoreUniform(Uniform{16, ValueType::V128}, result);
        if (scenario == 1) block->StoreUniform(Uniform{32, ValueType::V128}, left);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        FeatureSet features{};
        features.sse_shufps_imm = true;
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, features};
        allocation.MapRegister(left.Id(), HostFPR{17});
        allocation.MapRegister(result.Id(), HostFPR{static_cast<swift::u16>(scenario == 4 ? 18 : 17)});
        if (scenario == 2) allocation.MarkHostReadCoalesced(left.Id());
        compile::BlockAnalysisIndex index;
        index.Build(block.get());
        const auto plan = arm64::PrepareFPRContract(
                block.get(), allocation, features, false, false, index, allocation.Workspace());
        REQUIRE(plan.valid == (scenario != 1 && scenario != 2));
        REQUIRE(plan.Has(result.Id(), arm64::FPRAction::ShuffleTie) == (scenario == 0));
        if (!plan.valid) REQUIRE(plan.rejected == result.Def());
        if (scenario == 4) {
            REQUIRE(plan.instructions.empty());
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
        const auto source = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
        const std::array<unsigned, 2> controls{0x4e, 0x39};
        std::array<Value, 2> masks, results;
        const auto make = [&](unsigned i) {
            auto [low, high] = arm64::PshufdIndexMask(static_cast<swift::u8>(controls[i]));
            if (i == 1 && corrupt_second) low ^= 1;
            masks[i] = block->VecLoadConst(ValueType::V128, Imm{low}, Imm{high});
            results[i] = block->VecShuffle32Indexed(ValueType::V128, source, masks[i]);
            block->StoreUniform(Uniform{16 + i * 16, ValueType::V128}, results[i]);
        };
        if (second_first) {
            make(1);
            make(0);
        } else {
            make(0);
            make(1);
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        for (unsigned i = 0; i < 2; ++i) {
            allocation.MarkPshufdDirect(masks[i].Id());
            allocation.MarkPshufdDirect(results[i].Id());
        }
        compile::BlockAnalysisIndex index;
        index.Build(block.get());
        const auto shared = arm64::PrepareFPRContract(
                block.get(), allocation, FeatureSet{}, false, false, index, allocation.Workspace());
        compile::BlockAnalysisIndex independent;
        independent.Build(block.get());
        const auto own = arm64::PrepareFPRContract(block.get(),
                                                   allocation,
                                                   FeatureSet{},
                                                   false,
                                                   false,
                                                   independent,
                                                   allocation.Workspace());
        REQUIRE(shared.valid == own.valid);
        REQUIRE(shared.rejected == own.rejected);
        REQUIRE(shared.instructions == own.instructions);
        Outcome outcome;
        outcome.valid = shared.valid;
        outcome.rejected = shared.rejected;
        for (unsigned i = 0; i < 2; ++i) {
            outcome.masks[i] = masks[i].Def();
            outcome.control[i] = shared.Control(results[i].Id());
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

TEST_CASE("shuffle plans reject stale indexes and clear old selections", "[shuffle-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xddc8})};
    const auto source = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
    const auto [low, high] = arm64::PshufdIndexMask(0x4e);
    const auto mask = block->VecLoadConst(ValueType::V128, Imm{low}, Imm{high});
    const auto result = block->VecShuffle32Indexed(ValueType::V128, source, mask);
    block->StoreUniform(Uniform{16, ValueType::V128}, result);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.MarkPshufdDirect(mask.Id());
    allocation.MarkPshufdDirect(result.Id());
    IntrusivePtr<Block> other{new Block(0, Location{0xddd8})};
    other->LoadUniform(Uniform{0, ValueType::U64});
    other->SetTerminal(terminal::ReturnToHost{});
    other->ReIdInstr();
    compile::BlockAnalysisIndex stale;
    stale.Build(other.get());
    const auto foreign = arm64::PrepareFPRContract(
            block.get(), allocation, FeatureSet{}, false, false, stale, allocation.Workspace());
    REQUIRE_FALSE(foreign.valid);
    REQUIRE(foreign.rejected == &block->GetInstList().front());
    REQUIRE(foreign.instructions.empty());
    REQUIRE_FALSE(foreign.Control(mask.Id()).has_value());
    REQUIRE_FALSE(foreign.Has(result.Id(), arm64::FPRAction::Pshufd));
    stale.Build(block.get());
    const auto fresh = arm64::PrepareFPRContract(
            block.get(), allocation, FeatureSet{}, false, false, stale, allocation.Workspace());
    REQUIRE(fresh.valid);
    REQUIRE(fresh.Control(result.Id()) == std::optional<swift::u8>{0x4e});
    allocation.ResetAllocations();
    const auto cleared = arm64::PrepareFPRContract(
            block.get(), allocation, FeatureSet{}, false, false, stale, allocation.Workspace());
    REQUIRE(cleared.valid);
    REQUIRE(cleared.instructions.empty());
    REQUIRE_FALSE(cleared.Control(result.Id()).has_value());
    allocation.MarkPshufdDirect(mask.Id());
    allocation.MarkPshufdDirect(result.Id());
    const auto restored = arm64::PrepareFPRContract(
            block.get(), allocation, FeatureSet{}, false, false, stale, allocation.Workspace());
    REQUIRE(restored.valid);
    REQUIRE(restored.instructions == fresh.instructions);
}
