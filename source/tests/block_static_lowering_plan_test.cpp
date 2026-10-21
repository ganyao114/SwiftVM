#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "runtime/backend/arm64/alloc/gpr_block_plan.h"
#include "runtime/backend/arm64/alloc/region_pfaf_plan.h"
#include "runtime/backend/arm64/alloc/flags_token_plan.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("block-local scalar plans reject incoming producers", "[flags-token-plan]") {
    IntrusivePtr<Block> predecessor{new Block(0, Location{0xbcf0})};
    const auto source = predecessor->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
    const auto constant = predecessor->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
    const auto sum = predecessor->Add(source, Operand{constant}).SetType(ValueType::U64);
    predecessor->SaveFlags(sum, Flags::All);
    predecessor->ReIdInstr();
    IntrusivePtr<Block> block{new Block(0, Location{0xbcf8})};
    block->SetHostGPR(sum, HostRegIndex(20), Imm{0u});
    auto* publication = &block->GetInstList().back();
    publication->SetId(4);
    RegAlloc allocation{6, GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.MapRegister(source.Id(), HostGPR{20});
    allocation.MapRegister(constant.Id(), HostGPR{14});
    allocation.MapRegister(sum.Id(), HostGPR{20});
    allocation.MarkHostWriteCoalesced(publication->Id());
    arm64::BlockAnalysisIndex analysis;
    analysis.Build(block.get());
    REQUIRE(analysis.Contains(publication));
    REQUIRE_FALSE(analysis.Contains(sum.Def()));
    const auto flags = arm64::BuildFlagsTokenPlan(analysis, allocation, FeatureSet{});
    REQUIRE(flags.homes.empty());
    REQUIRE(flags.point_homes.empty());
}

TEST_CASE("region PF AF plans preserve cold reread inputs", "[region-pfaf-plan]") {
    for (unsigned scenario = 0; scenario < 11; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbd10})};
        const bool uniform = scenario == 1 || scenario == 3 || scenario == 7 || scenario == 8;
        const auto type = scenario == 10 ? ValueType::U32 : ValueType::U16;
        const auto left = uniform ? block->LoadUniform(Uniform{0, type}).SetType(type)
                : block->GetHostGPR(HostRegIndex(20), Imm{scenario == 9 ? 7u : 0u}).SetType(type);
        auto right = Operand{Imm{swift::u64{1}}};
        Value sum;
        if (scenario == 6) {
            const auto computed = block->Add(left, right).SetType(type);
            sum = block->Sub(computed, right).SetType(type);
        } else sum = block->Sub(left, right).SetType(type);
        block->SaveFlags(sum, scenario == 4 ? Flags::NZCV : Flags::All);
        const auto* save = &block->GetInstList().back();
        if (scenario == 2) block->SetHostGPR(left, HostRegIndex(20), Imm{0u});
        if (scenario == 3 || scenario == 7 || scenario == 8) {
            const auto offset = scenario == 7 ? 2u : scenario == 8 ? 1u : 0u;
            block->StoreUniform(Uniform{offset, ValueType::U8}, Value{left}.SetCastType(ValueType::U8));
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        const auto plan = arm64::BuildRegionPFAFPlan(*block, scenario != 5, sum.Def(), save);
        const bool accepted = scenario == 0 || scenario == 1 || scenario == 7;
        REQUIRE(plan.has_value() == accepted);
        if (plan) {
            REQUIRE(plan->width == 2);
            REQUIRE(plan->producer == sum.Def());
            REQUIRE(plan->right.kind == arm64::RegionPFAFPlan::Operand::Kind::Imm);
            REQUIRE(plan->left.kind == (uniform ? arm64::RegionPFAFPlan::Operand::Kind::Uniform
                                               : arm64::RegionPFAFPlan::Operand::Kind::HostGPR));
        }
    }
}

TEST_CASE("region PF AF plans are independent per producer and reject foreign saves", "[region-pfaf-plan]") {
    using Kind = arm64::RegionPFAFPlan::Operand::Kind;
    for (bool interfere : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbd60})};
        const auto left_a = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U16);
        const auto a = block->Sub(left_a, Operand{Imm{swift::u64{1}}}).SetType(ValueType::U16);
        block->SaveFlags(a, Flags::All);
        const auto* save_a = &block->GetInstList().back();
        if (interfere) block->SetHostGPR(left_a, HostRegIndex(20), Imm{0u});
        const auto left_b = block->GetHostGPR(HostRegIndex(21), Imm{0u}).SetType(ValueType::U16);
        const auto b = block->Sub(left_b, Operand{Imm{swift::u64{1}}}).SetType(ValueType::U16);
        block->SaveFlags(b, Flags::All);
        const auto* save_b = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        const auto plan_a = arm64::BuildRegionPFAFPlan(*block, true, a.Def(), save_a);
        const auto plan_b = arm64::BuildRegionPFAFPlan(*block, true, b.Def(), save_b);
        REQUIRE(plan_a.has_value() == !interfere);
        REQUIRE(plan_b.has_value());
        REQUIRE(plan_b->producer == b.Def());
        REQUIRE(plan_b->width == 2);
        REQUIRE(plan_b->left.kind == Kind::HostGPR);
        REQUIRE(plan_b->left.value == 21);
        REQUIRE(plan_b->right.kind == Kind::Imm);
        REQUIRE(plan_b->right.value == 1);
        if (plan_a) {
            REQUIRE(plan_a->producer == a.Def());
            REQUIRE(plan_a->left.value == 20);
            const auto again = arm64::BuildRegionPFAFPlan(*block, true, a.Def(), save_a);
            REQUIRE(again.has_value());
            REQUIRE(again->width == plan_a->width);
            REQUIRE(again->left.kind == plan_a->left.kind);
            REQUIRE(again->left.value == plan_a->left.value);
            REQUIRE(again->right.kind == plan_a->right.kind);
            REQUIRE(again->right.value == plan_a->right.value);
        }
        REQUIRE_FALSE(arm64::BuildRegionPFAFPlan(*block, true, a.Def(), save_b));
        REQUIRE_FALSE(arm64::BuildRegionPFAFPlan(*block, false, b.Def(), save_b));
        REQUIRE_FALSE(arm64::BuildRegionPFAFPlan(*block, true, nullptr, save_b));
        REQUIRE_FALSE(arm64::BuildRegionPFAFPlan(*block, true, b.Def(), nullptr));
    }
    IntrusivePtr<Block> foreign{new Block(0, Location{0xbd68})};
    const auto foreign_left = foreign->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U16);
    const auto foreign_sub = foreign->Sub(foreign_left, Operand{Imm{swift::u64{1}}}).SetType(ValueType::U16);
    foreign->SaveFlags(foreign_sub, Flags::All);
    const auto* foreign_save = &foreign->GetInstList().back();
    foreign->SetTerminal(terminal::ReturnToHost{});
    foreign->ReIdInstr();
    IntrusivePtr<Block> block{new Block(0, Location{0xbd70})};
    const auto left = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U16);
    const auto sub = block->Sub(left, Operand{Imm{swift::u64{1}}}).SetType(ValueType::U16);
    block->SaveFlags(sub, Flags::All);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    REQUIRE_FALSE(arm64::BuildRegionPFAFPlan(*block, true, sub.Def(), foreign_save));
    REQUIRE_FALSE(arm64::BuildRegionPFAFPlan(*foreign, true, sub.Def(), foreign_save));
}

