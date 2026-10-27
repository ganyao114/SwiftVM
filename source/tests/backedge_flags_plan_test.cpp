#include <array>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/lower_edges.h"
#include "translator/x86/cpu.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("backedge static plans separate edge and observation contracts", "[backedge-plan]") {
    for (bool canonical : {false, true}) for (unsigned scenario = 0; scenario < 8; ++scenario) {
            IntrusivePtr<Block> block{new Block(0, Location{0xc000})};
            const auto left = block->LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
            const auto producer = block->Sub(ValueType::U8, left, Operand{Imm{1u}});
            block->SaveFlags(producer, scenario == 5 ? Flags::NZCV : Flags::All);
            if (scenario == 2) (void)block->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{1}}});
            Value polarity;
            if (canonical)
                block->InvertCarry();
            else {
                polarity = block->LoadImm(ValueType::U8, Imm{swift::u8{0}});
                block->StoreUniform(Uniform{offsetof(swift::x86::ThreadContext64, carry_inverted),
                                            ValueType::U8},
                                    polarity);
            }
        if (scenario != 3) block->AdvancePC(Imm{swift::u64{1}});
        if (scenario == 4) block->AdvancePC(Imm{swift::u64{1}});
        if (scenario == 6) block->StoreUniform(Uniform{8, ValueType::U8}, left);
        const auto condition = block->LocalCondSet(Cond::EQ);
        const auto then_target = scenario == 1 || scenario == 7 ? Location{0xc010} : block->GetStartLocation();
        block->SetTerminal(terminal::If{condition, terminal::LinkBlock{then_target}, terminal::LinkBlock{Location{0xc020}}});
        block->ReIdInstr();
        FeatureSet features{};
        arm64::BackedgeInputs inputs{features};
        inputs.canonical_carry = canonical;
        inputs.self_enabled = true;
        inputs.region_enabled = scenario == 7;
        const std::array<swift::u64, 1> dead_targets{then_target.Value()};
        if (scenario == 7) inputs.flags_dead_targets = dead_targets;
        const auto plan = arm64::LowerBackedge(*block, inputs);
        const bool accepted = scenario == 0 || scenario == 7;
        REQUIRE(plan.has_value() == accepted);
        if (plan) {
            REQUIRE(plan->action.canonical_carry == canonical);
            REQUIRE(plan->action.dead_successor == (scenario == 7));
            REQUIRE(plan->action.self_target == then_target);
            REQUIRE(plan->action.Required() == Flags::NZCV);
            REQUIRE(plan->pfaf.has_value() == (scenario == 7));
            REQUIRE(plan->action.omit_polarity_load == !canonical);
            REQUIRE(plan->action.omit_polarity_store == !canonical);
            if (!canonical) REQUIRE(plan->action.polarity_load == polarity.Id());
        }
    }
}

TEST_CASE("backedge terminal boundary follows the actual local condition",
          "[backedge-plan][terminal-boundary]") {
    for (bool canonical : {false, true}) for (auto type : {ValueType::U32, ValueType::U64})
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        CAPTURE(canonical, type, scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xc080})};
        const auto input = block->LoadUniform(type, Uniform{0, type});
        const auto producer = block->Sub(type, input, Operand{Imm{swift::u64{1}}});
        block->SaveFlags(producer, Flags::All);
        if (canonical) block->InvertCarry();
        else block->StoreUniform(
                Uniform{offsetof(swift::x86::ThreadContext64, carry_inverted), ValueType::U8},
                block->LoadImm(ValueType::U8, Imm{swift::u8{1}}));
        const Inst* primary = nullptr;
        if (scenario != 3) {
            block->AdvancePC(Imm{swift::u64{3}});
            primary = &block->GetInstList().back();
        }
        if (scenario == 2) block->AdvancePC(Imm{swift::u64{6}});
        const auto condition = block->LocalCondSet(Cond::NE);
        if (scenario == 6)
            (void)block->LoadMemory(ValueType::U64, Operand{Imm{swift::u64{0}}});
        if (scenario == 7) (void)block->TestFlags(Flags::Carry);
        if (scenario != 0 && scenario != 2) block->AdvancePC(Imm{swift::u64{6}});
        if (scenario == 4) block->AdvancePC(Imm{swift::u64{7}});
        if (scenario == 5) (void)block->LocalCondSet(Cond::EQ);
        block->SetTerminal(terminal::If{
                condition, terminal::LinkBlock{block->GetStartLocation()},
                terminal::LinkBlock{Location{0xc090}}});
        block->ReIdInstr();
        FeatureSet features{};
        arm64::BackedgeInputs inputs{features};
        inputs.self_enabled = true;
        inputs.canonical_carry = canonical;
        const auto selected = arm64::LowerBackedge(*block, inputs);
        // 0 is the original single producer boundary. 1 additionally has
        // exactly one terminal boundary after the condition, at list.back().
        // 2 has two before the condition; 3 has no producer boundary;
        // 4 has two after; 5 does not end at that boundary. 6/7 observe flags.
        REQUIRE(selected.has_value() == (scenario == 0 || scenario == 1));
        if (selected) {
            REQUIRE(primary);
            REQUIRE(selected->action.final_advance == primary->Id());
            REQUIRE(primary->Id() < condition.Id());
            REQUIRE(selected->action.Required() == Flags::NZCV);
            REQUIRE(selected->action.canonical_carry == canonical);
            REQUIRE_FALSE(selected->action.dead_successor);
            if (scenario == 1) {
                const auto& terminal_boundary = block->GetInstList().back();
                REQUIRE(terminal_boundary.GetOp() == OpCode::AdvancePC);
                REQUIRE(terminal_boundary.Id() > condition.Id());
                REQUIRE(terminal_boundary.Id() != selected->action.final_advance);
            }
        }
    }
}
