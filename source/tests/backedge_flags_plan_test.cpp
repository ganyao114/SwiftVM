#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/alloc/backedge_flags_plan.h"
#include "translator/x86/cpu.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("backedge static plans separate edge and observation contracts", "[backedge-plan]") {
    for (bool canonical : {false, true}) for (unsigned scenario = 0; scenario < 8; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xc000})};
        const auto left = block->LoadUniform(Uniform{0, ValueType::U8}).SetType(ValueType::U8);
        const auto producer = block->Sub(left, Operand{Imm{1u}}).SetType(ValueType::U8);
        block->SaveFlags(producer, scenario == 5 ? Flags::NZCV : Flags::All);
        const auto* save = &block->GetInstList().back();
        if (scenario == 2) (void)block->CallLambda(Lambda{Imm{swift::u64{1}}});
        Value polarity;
        if (canonical) block->InvertCarry();
        else {
            polarity = block->LoadImm(Imm{swift::u8{0}}).SetType(ValueType::U8);
            block->StoreUniform(Uniform{offsetof(swift::x86::ThreadContext64, carry_inverted), ValueType::U8}, polarity);
        }
        if (scenario != 3) block->AdvancePC(Imm{swift::u64{1}});
        if (scenario == 4) block->AdvancePC(Imm{swift::u64{1}});
        if (scenario == 6) block->StoreUniform(Uniform{8, ValueType::U8}, left);
        const auto condition = block->LocalCondSet(Cond::EQ);
        const auto then_target = scenario == 1 || scenario == 7 ? Location{0xc010} : block->GetStartLocation();
        block->SetTerminal(terminal::If{condition, terminal::LinkBlock{then_target}, terminal::LinkBlock{Location{0xc020}}});
        block->ReIdInstr();
        FeatureSet features{};
        arm64::BackedgeFlagsInputs inputs{features};
        inputs.canonical_carry = canonical;
        inputs.self_enabled = true;
        inputs.region_enabled = scenario == 7;
        if (scenario == 7) inputs.flags_dead_targets.insert(then_target.Value());
        const auto plan = arm64::BuildBackedgeFlagsPlan(*block, inputs);
        const bool accepted = scenario == 0 || scenario == 7;
        REQUIRE(plan.has_value() == accepted);
        if (plan) {
            REQUIRE(plan->final_save == save);
            REQUIRE(plan->canonical_carry == canonical);
            REQUIRE(plan->dead_successor == (scenario == 7));
            REQUIRE(plan->self_target == then_target);
            REQUIRE(plan->requested == Flags::NZCV);
            REQUIRE(plan->pfaf.has_value() == (scenario == 7));
            if (!canonical) REQUIRE(plan->polarity_load == polarity.Def());
        }
    }
}
