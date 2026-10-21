#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "runtime/ir/hir_builder.h"
#include "runtime/ir/opts/flags_elimination_pass.h"

namespace swift::runtime::ir {

namespace {

void AddFlagsOverwrite(HIRFunction* function) {
    const auto left = function->LoadImm(Imm{1u}).SetType(ValueType::U32);
    const auto right = function->LoadImm(Imm{2u}).SetType(ValueType::U32);
    const auto result = function->Add(left, Operand{right}).SetType(ValueType::U32);
    function->SaveFlags(result, Flags::All);
    function->EndBlock(terminal::ReturnToHost{});
}

}  // namespace

TEST_CASE("branch-only flags discard dead carry normalization") {
    FeatureSet features{};
    HIRBuilder builder{1, true, features};
    constexpr Location entry{0x9100};
    constexpr Location then_target{0x9200};
    constexpr Location else_target{0x9300};
    auto* function = builder.AppendFunction(entry, Location{0x9400});
    const auto address = function->LoadImm(Imm{0x1000u})
                                 .SetType(ValueType::U64);
    const auto right = function->LoadMemory(Operand{address})
                               .SetType(ValueType::U16);
    const auto left = function->LoadUniform(Uniform{0, ValueType::U16});
    const auto result = function->Sub(left, Operand{right})
                                .SetType(ValueType::U16);
    function->SaveFlags(result, Flags::All);
    function->InvertCarry();
    function->AdvancePC(Imm{5u});
    const auto condition = function->LocalCondSet(Cond::NE)
                                   .SetType(ValueType::U8);
    auto [then_block, else_block] = builder.If(terminal::If{
            condition,
            terminal::LinkBlock{then_target},
            terminal::LinkBlock{else_target},
    });

    builder.SetCurBlock(then_block);
    AddFlagsOverwrite(function);
    builder.SetCurBlock(else_block);
    AddFlagsOverwrite(function);
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();

    FlagsEliminationPass::Run(function, features);

    const auto pseudos = CollectPseudoOperations(
            *function->GetHIRBlocksRPO().front().GetBlock(), result.Def(),
            OpCode::BranchOnlyFlags);
    REQUIRE(pseudos.size() == 1);
    REQUIRE(pseudos.front()->GetArg<Flags>(1) == Flags::Zero);
    const auto& instructions =
            function->GetHIRBlocksRPO().front().GetBlock()->GetInstList();
    REQUIRE(std::none_of(instructions.begin(), instructions.end(),
                         [](const Inst& inst) {
                             return inst.GetOp() == OpCode::InvertCarry;
                         }));
}

TEST_CASE("branch-only flags discard full-width carry normalization") {
    for (const auto type : {ValueType::U32, ValueType::U64}) {
        FeatureSet features{};
        HIRBuilder builder{1, true, features};
        auto* function = builder.AppendFunction(Location{0x9410}, Location{0x9450});
        auto* compare_block = builder.LinkBlock(
                terminal::LinkBlock{Location{0x9420}});
        builder.SetCurBlock(compare_block);
        const auto left = function->LoadImm(Imm{9u}).SetType(type);
        const auto right = function->LoadImm(Imm{4u}).SetType(type);
        const auto result = function->Sub(left, Operand{right}).SetType(type);
        function->SaveFlags(result, Flags::All);
        function->InvertCarry();
        function->AdvancePC(Imm{3u});
        const auto condition = function->LocalCondSet(Cond::NE)
                                       .SetType(ValueType::U8);
        auto [then_block, else_block] = builder.If(terminal::If{
                condition,
                terminal::LinkBlock{Location{0x9430}},
                terminal::LinkBlock{Location{0x9440}},
        });

        builder.SetCurBlock(then_block);
        AddFlagsOverwrite(function);
        builder.SetCurBlock(else_block);
        AddFlagsOverwrite(function);
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();

        FlagsEliminationPass::Run(function, features);

        const auto pseudos = CollectPseudoOperations(
                *compare_block->GetBlock(), result.Def(), OpCode::BranchOnlyFlags);
        REQUIRE(pseudos.size() == 1);
        REQUIRE(pseudos.front()->GetArg<Flags>(1) == Flags::Zero);
        REQUIRE(std::none_of(compare_block->GetInstList().begin(),
                             compare_block->GetInstList().end(),
                             [](const Inst& inst) {
                                 return inst.GetOp() == OpCode::InvertCarry;
                             }));
    }
}

TEST_CASE("function flags liveness removes only internally dead publications") {
    FeatureSet features{};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0x9500}, Location{0x9700});
    const auto left = function->LoadImm(Imm{1u}).SetType(ValueType::U32);
    const auto right = function->LoadImm(Imm{2u}).SetType(ValueType::U32);
    const auto dead_result = function->Add(left, Operand{right}).SetType(ValueType::U32);
    function->SaveFlags(dead_result, Flags::All);
    auto* successor = builder.LinkBlock(terminal::LinkBlock{Location{0x9600}});
    builder.SetCurBlock(successor);
    AddFlagsOverwrite(function);
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();

    FlagsEliminationPass::Run(function, features);
    REQUIRE(CollectPseudoOperations(*function->GetHIRBlocksRPO().front().GetBlock(),
                                    dead_result.Def(), OpCode::SaveFlags)
                    .empty());

    HIRBuilder external_builder{1, true, features};
    auto* external = external_builder.AppendFunction(Location{0x9800}, Location{0x9900});
    const auto external_left = external->LoadImm(Imm{1u}).SetType(ValueType::U32);
    const auto external_right = external->LoadImm(Imm{2u}).SetType(ValueType::U32);
    const auto live_result = external->Add(external_left, Operand{external_right})
                                     .SetType(ValueType::U32);
    external->SaveFlags(live_result, Flags::All);
    external->EndBlock(terminal::ReturnToHost{});
    external->EndFunction();
    external->ComputeRPO();
    external->IdByRPO();

    FlagsEliminationPass::Run(external, features);
    REQUIRE(CollectPseudoOperations(*external->GetHIRBlocksRPO().front().GetBlock(),
                                    live_result.Def(), OpCode::SaveFlags)
                    .size() == 1);
}

}  // namespace swift::runtime::ir
