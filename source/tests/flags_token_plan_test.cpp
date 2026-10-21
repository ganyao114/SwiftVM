#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/alloc/flags_token_plan.h"
#include "runtime/backend/arm64/alloc/gpr_block_plan.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("flags token plans preserve publication and suffix ownership contracts", "[flags-token-plan]") {
    for (const bool narrow : {false, true}) {
        for (const unsigned scenario : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
            IntrusivePtr<Block> block{new Block(0, Location{0xbc00})};
            const auto producer = block->LoadImm(Imm{swift::u64{42}})
                    .SetType(narrow ? ValueType::U32 : ValueType::U64);
            auto stored = producer;
            if (narrow) stored = block->ZeroExtend32To64(producer).SetType(ValueType::U64);
            if (scenario != 6) block->SaveFlags(producer, Flags::Parity);
            block->SetHostGPR(stored, HostRegIndex(20), Imm{0u});
            auto* publication = &block->GetInstList().back();
            Value other;
            Inst* second_publication = nullptr;
            if (scenario == 1) {
                // A second publication to another home is independently safe.
                block->SetHostGPR(stored, HostRegIndex(22), Imm{0u});
                second_publication = &block->GetInstList().back();
            } else if (scenario == 2) {
                block->SetHostGPR(stored, HostRegIndex(20), Imm{0u});
                second_publication = &block->GetInstList().back();
            } else if (scenario == 3) {
                other = block->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
            } else if (scenario == 4) {
                block->SetHostGPR(stored, HostRegIndex(20), Imm{8u});
            } else if (scenario == 5) {
                // An uncoalesced first publication cannot retain the home.
            }
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
            allocation.MapRegister(producer.Id(), HostGPR{20});
            if (narrow) allocation.MapRegister(stored.Id(), HostGPR{20});
            if (other.Defined()) allocation.MapRegister(other.Id(), HostGPR{20});
            if (scenario != 5) allocation.MarkHostWriteCoalesced(publication->Id());
            if (second_publication) allocation.MarkHostWriteCoalesced(second_publication->Id());
            arm64::BlockAnalysisIndex analysis;
            analysis.Build(block.get(), nullptr, true);
            const auto plan = arm64::BuildFlagsTokenPlan(analysis, allocation, FeatureSet{});
            std::map<Inst*, arm64::GPRPublicationAction> publications;
            for (auto& node : block->GetInstList())
                if (node.GetOp() == OpCode::SetHostGPR) publications.emplace(&node, arm64::GPRPublicationAction{});
            const auto prepared = arm64::BuildFlagsTokenPlan(analysis, allocation, FeatureSet{}, &publications, &analysis.PseudoFlags());
            REQUIRE(plan.Retains(producer.Def(), 20, producer.Id()) == (scenario <= 1));
            REQUIRE(plan.Retains(producer.Def(), 22, producer.Id()) == (scenario == 1));
            REQUIRE(prepared.Retains(producer.Def(), 20, producer.Id()) == plan.Retains(producer.Def(), 20, producer.Id()));
            REQUIRE(prepared.Retains(producer.Def(), 22, producer.Id()) == plan.Retains(producer.Def(), 22, producer.Id()));
            REQUIRE(prepared.homes == plan.homes);
            REQUIRE(prepared.point_homes == plan.point_homes);
            if (scenario == 6) REQUIRE(plan.homes.empty());
            REQUIRE_FALSE(plan.Retains(producer.Def(), 31, producer.Id()));
            REQUIRE_FALSE(plan.Retains(nullptr, 20, producer.Id()));
        }
    }
}

TEST_CASE("flags token plan rejects fixed home clobbers after publication", "[flags-token-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbc08})};
    const auto producer = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
    block->SaveFlags(producer, Flags::Parity);
    block->SetHostGPR(producer, HostRegIndex(20), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->XchgBarrier(Imm{3u});
    auto* call = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    FeatureSet features{};
    REQUIRE((FixedGPRClobbers(*call, features) & (1u << 20)) != 0);
    RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, features};
    allocation.MapRegister(producer.Id(), HostGPR{20});
    allocation.MarkHostWriteCoalesced(publication->Id());
    arm64::BlockAnalysisIndex analysis;
    analysis.Build(block.get(), nullptr, true);
    const auto plan = arm64::BuildFlagsTokenPlan(analysis, allocation, features);
    const auto cached = arm64::BuildFlagsTokenPlan(analysis, allocation, features, nullptr, &analysis.PseudoFlags());
    REQUIRE(cached.homes == plan.homes);
    REQUIRE(cached.point_homes == plan.point_homes);
    REQUIRE_FALSE(plan.Retains(producer.Def(), 20, producer.Id()));
}

TEST_CASE("cached token demand distinguishes saved parity from branch-only parity", "[flags-token-plan][pseudo-flags-plan]") {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xbc18})};
        const auto producer = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
        if (scenario == 1 || scenario == 3) block->SaveFlags(producer, Flags::Parity);
        if (scenario == 2 || scenario == 3) block->BranchOnlyFlags(producer, Flags::Parity);
        if (scenario == 4) {
            block->SaveFlags(producer, Flags::NZCV);
            block->BranchOnlyFlags(producer, Flags::Parity);
        }
        if (scenario == 5) (void)block->GetFlags(producer, Flags::Parity);
        block->SetHostGPR(producer, HostRegIndex(20), Imm{0u});
        auto* publication = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(producer.Id(), HostGPR{20});
        allocation.MarkHostWriteCoalesced(publication->Id());
        arm64::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        const auto& demand = analysis.PseudoFlags();
        REQUIRE(demand.SavesParity(producer.Def()) == (scenario == 1 || scenario == 3));
        REQUIRE_FALSE(demand.SavesParity(nullptr));
        const auto legacy = arm64::BuildFlagsTokenPlan(analysis, allocation, FeatureSet{});
        const auto cached = arm64::BuildFlagsTokenPlan(analysis, allocation, FeatureSet{}, nullptr, &demand);
        REQUIRE(cached.homes == legacy.homes);
        REQUIRE(cached.point_homes == legacy.point_homes);
        REQUIRE(cached.Retains(producer.Def(), 20, producer.Id()) == (scenario == 1 || scenario == 3));
    }
}

TEST_CASE("forwarded flags captures use their planned preservation point", "[flags-token-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbc10})};
    const auto input = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
    const auto producer = block->Or(input, Operand{Imm{swift::u64{0}}}).SetType(ValueType::U64);
    const auto other = block->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
    const auto capture = block->Or(producer, Operand{Imm{swift::u64{0}}}).SetType(ValueType::U64);
    block->SaveFlags(capture, Flags::Parity);
    block->SetHostGPR(producer, HostRegIndex(22), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    GPRSMask pool{};
    pool.Mark(20);
    pool.Mark(22);
    RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, FeatureSet{}};
    allocation.MapRegister(producer.Id(), HostGPR{22});
    allocation.MapRegister(other.Id(), HostGPR{20});
    allocation.PlanFixedGPRDefinition(producer.Id(), {.home = 20, .end = capture.Id(),
            .transfer = capture.Id(), .last_use = publication->Id(),
            .owner = producer.Id(), .width = 8, .preserved = true});
    allocation.MarkHostWriteCoalesced(publication->Id());
    arm64::BlockAnalysisIndex analysis;
    analysis.Build(block.get(), nullptr, true);
    const auto plan = arm64::BuildFlagsTokenPlan(analysis, allocation, FeatureSet{});
    const auto cached = arm64::BuildFlagsTokenPlan(analysis, allocation, FeatureSet{}, nullptr, &analysis.PseudoFlags());
    REQUIRE(cached.homes == plan.homes);
    REQUIRE(cached.point_homes == plan.point_homes);
    REQUIRE(cached.point_dependent == plan.point_dependent);
    REQUIRE(plan.point_dependent);
    REQUIRE_FALSE(plan.Retains(producer.Def(), 22, producer.Id()));
    REQUIRE(plan.Retains(producer.Def(), 22, capture.Id()));
    REQUIRE_FALSE(plan.Retains(producer.Def(), 20, capture.Id()));
}
