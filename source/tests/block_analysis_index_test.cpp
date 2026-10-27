#include <utility>
#include <optional>
#include <array>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/backend/arm64/compile/write_successors.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/arm64/compile/register_state.h"
#include "runtime/backend/arm64/compile/effects.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/workspace.h"
#include "runtime/compile/unit.h"
#include "tests/support/allocation_fixture.h"
#include "tests/support/rewrite_fixture.h"
using swift::runtime::test::AllocationFixture;
using swift::runtime::test::RewriteFixture;
#include "runtime/compile/passes/passes.h"

using namespace swift::runtime;
using namespace swift::runtime::ir;
using swift::runtime::compile::BlockAnalysisIndex;

TEST_CASE("logical guest-state versions respect byte writes and entry paths",
          "[block-analysis][guest-state]") {
    using Policy = compile::GuestStateEntryPolicy;
    SECTION("partial writes preserve unspecified bytes and never widen a raw view") {
        IntrusivePtr<Block> block{new Block{Location{0x2100}}};
        const auto original = block->LoadImm(ValueType::U64, Imm{swift::u64{0x8877665544332211}});
        const auto low = block->LoadImm(ValueType::U32, Imm{swift::u32{0xaabbccdd}});
        const auto byte = block->LoadImm(ValueType::U8, Imm{swift::u8{0x99}});
        const auto wanted = block->LoadImm(ValueType::U64, Imm{swift::u64{0x88779955aabbccdd}});
        const auto zero_high = block->ZeroExtend32To64(low);
        block->SetHostGPR(original, HostRegIndex(20), Imm{0u});
        block->SetHostGPR(low, HostRegIndex(20), Imm{0u});
        block->SetHostGPR(byte, HostRegIndex(20), Imm{5u});
        block->Nop();
        const auto* point = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::CompileUnit unit{block}; unit.SealIR();
        compile::SemanticFacts facts{block.get()};
        CHECK_FALSE(facts.FindGuestStateGPRMatch(block.get(), 20, 0, *point, wanted, Policy::Block));
        CHECK(facts.PrepareGuestStateGPRMatch(block.get(), 20, 0, *point, wanted, Policy::Block));
        CHECK(facts.FindGuestStateGPRMatch(block.get(), 20, 0, *point, wanted, Policy::Block) == true);
        CHECK_FALSE(facts.PrepareGuestStateGPRMatch(block.get(), 20, 0, *point, zero_high, Policy::Block));
        CHECK(facts.FindGuestStateGPRMatch(block.get(), 20, 0, *point, zero_high, Policy::Block) == false);
        CHECK(facts.PrepareGuestStateGPRMatch(block.get(), 20, 0, *point, low, Policy::Block));
        CHECK_FALSE(facts.PrepareGuestStateGPRMatch(block.get(), 20, 1, *point, low, Policy::Block));
        const auto widened = Value{low}.SetCastType(ValueType::U64);
        CHECK_FALSE(facts.PrepareGuestStateGPRMatch(
                block.get(), 20, 0, *point, widened, Policy::Block));
        CHECK(facts.FindGuestStateGPRMatch(block.get(), 20, 0, *point, low, Policy::Block) == true);
        CHECK_FALSE(facts.PrepareGuestStateGPRMatch(block.get(), 20, 5, *point, low, Policy::Block));
        CHECK_FALSE(facts.PrepareGuestStateGPRMatch(block.get(), 20, 0, *point, wanted, Policy::Function));
        CHECK_FALSE(facts.FindFunctionCFG());
        CHECK_FALSE(facts.FindIndex(block.get()));
    }
    SECTION("conversion origins describe logical sign, truncation and extraction bytes") {
        IntrusivePtr<Block> block{new Block{Location{0x2180}}};
        const auto source = block->LoadImm(ValueType::U64, Imm{swift::u64{0x1122334480ff0080}});
        const auto narrow = block->LoadImm(ValueType::U8, Imm{swift::u8{0x80}});
        const std::array<Value, 4> expressions{
            block->ZeroExtend32To64(source), block->SignExtend(ValueType::S64, narrow),
            block->BitExtract(source, Imm{16u}, Imm{16u}), block->ZeroExtend64(narrow)};
        const std::array<swift::u64, 4> bits{0x80ff0080, 0xffffffffffffff80, 0x80ff, 0x80};
        for (unsigned i = 0; i < bits.size(); ++i) {
            const auto literal = block->LoadImm(ValueType::U64, Imm{bits[i]});
            block->SetHostGPR(literal, HostRegIndex(22 + i), Imm{0u});
        }
        block->Nop(); const auto* point = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
        compile::CompileUnit unit{block}; unit.SealIR();
        compile::SemanticFacts facts{block.get()};
        for (unsigned i = 0; i < expressions.size(); ++i)
            CHECK(facts.PrepareGuestStateGPRMatch(block.get(), 22 + i, 0, *point,
                                                 expressions[i], Policy::Block));
    }
    SECTION("read-only helpers preserve logical versions and reentry invalidates them") {
        IntrusivePtr<Block> block{new Block{Location{0x2200}}};
        const auto read = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
        const auto alias = block->BitCast(ValueType::U64, read);
        const HelperCallTraits traits{.guest_state = HelperGuestStateEffect::ReadOnly,
                                       .reentry = HelperReentryEffect::NoReentry};
        block->CallLambda(ValueType::VOID, Lambda{DataClass{Imm{swift::u64{1}}}, traits});
        block->Nop();
        const auto* safe = &block->GetInstList().back();
        block->SetHostGPR(alias, HostRegIndex(20), Imm{0u});
        const auto* publication = &block->GetInstList().back();
        block->CallLambda(ValueType::VOID, Lambda{DataClass{Imm{swift::u64{1}}},
                HelperCallTraits{.guest_state = HelperGuestStateEffect::ReadOnly}});
        block->Nop();
        const auto* unsafe = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::CompileUnit unit{block}; unit.SealIR();
        compile::SemanticFacts facts{block.get()};
        CHECK(facts.PrepareGuestStateGPRMatch(block.get(), 20, 0, *safe, alias, Policy::Block));
        CHECK_FALSE(facts.PrepareGuestStateGPRMatch(block.get(), 20, 0, *unsafe, alias, Policy::Block));
        backend::arm64::RegisterStatePreparation state;
        state.BeginBlock(block.get(), FeatureSet{}, facts);
        CHECK(state.PreparePublicationWindow(20, alias, alias.Id(), publication->Id()));
        CHECK_FALSE(facts.FindGuestStateGPRDefinitions(block.get()));
        CHECK_FALSE(facts.FindFunctionCFG());
    }
    SECTION("a local label meets taken and fallthrough byte versions") {
        for (bool agrees : {false, true}) {
            IntrusivePtr<Block> block{new Block{Location{0x2300}}};
            const auto wanted = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
            const auto other = block->LoadImm(ValueType::U64, Imm{swift::u64{agrees ? 7u : 8u}});
            const auto condition = block->GetHostGPR(ValueType::U8, HostRegIndex(21), Imm{0u});
            block->SetHostGPR(wanted, HostRegIndex(20), Imm{0u});
            const auto branch = block->Goto(BOOL{condition});
            block->SetHostGPR(other, HostRegIndex(20), Imm{0u});
            block->BindLabel(branch);
            block->Nop();
            const auto* point = &block->GetInstList().back();
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            compile::CompileUnit unit{block}; unit.SealIR();
            compile::SemanticFacts facts{block.get()};
            REQUIRE(facts.PrepareSSAValidation());
            CHECK(facts.PrepareGuestStateGPRMatch(block.get(), 20, 0, *point, wanted, Policy::Block) == agrees);
        }
    }
    SECTION("function propagation distinguishes ordinary, external and call-return entries") {
        for (unsigned root = 0; root < 3; ++root) {
            HIRBuilder builder{1, false, FeatureSet{}};
            auto* function = builder.AppendFunction(Location{0x2400});
            const auto wanted = function->LoadImm(ValueType::U64, Imm{swift::u64{0x8877665544332211}});
            function->SetHostGPR(wanted, HostRegIndex(20), Imm{0u});
            const auto condition = function->GetHostGPR(ValueType::U8, HostRegIndex(21), Imm{0u});
            auto [left, right] = builder.If(terminal::If{BOOL{condition},
                    terminal::LinkBlock{Location{0x2410}}, terminal::LinkBlock{Location{0x2420}}});
            builder.SetCurBlock(left);
            auto* join = builder.LinkBlock(terminal::LinkBlock{Location{0x2430}});
            builder.SetCurBlock(right);
            builder.LinkBlock(terminal::LinkBlock{Location{0x2430}});
            builder.SetCurBlock(join);
            function->Nop();
            const auto* point = &function->GetCurrentBlock()->GetInstList().back();
            if (root == 1) function->RegisterExternalEntryRoot(join);
            if (root == 2) builder.RegisterCallReturn(Location{0x2430});
            // A preserved self-edge exercises a cyclic proof as well as the
            // two incoming arms, without seeding the loop with a made-up value.
            builder.LinkBlock(terminal::LinkBlock{Location{0x2430}});
            function->EndFunction();
            function->ComputeRPO(); function->IdByRPO();
            compile::CompileUnit unit{function}; unit.SealIR();
            compile::SemanticFacts facts{function};
            REQUIRE(facts.PrepareSSAValidation());
            auto* block = join->GetBlock();
            CHECK_FALSE(facts.PrepareGuestStateGPRMatch(block, 20, 0, *point, wanted, Policy::Block));
            CHECK(facts.PrepareGuestStateGPRMatch(block, 20, 0, *point, wanted, Policy::Function) == (root == 0));
            CHECK(facts.FindGuestStateGPRMatch(block, 20, 0, *point, wanted, Policy::Function) == (root == 0));
            REQUIRE(facts.FindFunctionCFG());
            CHECK_FALSE(facts.FindIndex(block));
        }
    }
    SECTION("a loop never identifies different dynamic instances of an SSA origin") {
        HIRBuilder builder{1, false, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{0x2500});
        const auto initial = function->LoadImm(ValueType::U64, Imm{swift::u64{7}});
        function->SetHostGPR(initial, HostRegIndex(20), Imm{0u});
        auto* loop = builder.LinkBlock(terminal::LinkBlock{Location{0x2510}});
        builder.SetCurBlock(loop);
        const auto read = function->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
        const auto next = function->Add(ValueType::U64, read, Operand{Imm{swift::u64{1}}});
        function->Nop();
        const auto* point = &function->GetCurrentBlock()->GetInstList().back();
        function->SetHostGPR(next, HostRegIndex(20), Imm{0u});
        builder.LinkBlock(terminal::LinkBlock{Location{0x2510}});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        compile::CompileUnit unit{function}; unit.SealIR();
        compile::SemanticFacts facts{function};
        REQUIRE(facts.PrepareSSAValidation());
        CHECK_FALSE(facts.PrepareGuestStateGPRMatch(loop->GetBlock(), 20, 0, *point, next, Policy::Function));
        CHECK(facts.PrepareGuestStateGPRMatch(loop->GetBlock(), 20, 0, *point, read, Policy::Function));
    }
    SECTION("function-local proofs defer CFG and preserve answers across later input growth") {
        HIRBuilder builder{1, false, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{0x2600});
        auto* first = function->GetCurrentBlock()->GetBlock();
        const auto first_value = function->LoadImm(ValueType::U64, Imm{swift::u64{7}});
        function->SetHostGPR(first_value, HostRegIndex(20), Imm{0u});
        function->Nop(); const auto* first_point = &first->GetInstList().back();
        builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x2610}}));
        auto* second = function->GetCurrentBlock()->GetBlock();
        const auto second_value = function->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
        function->Nop(); const auto* second_point = &second->GetInstList().back();
        builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x2620}}));
        auto* third = function->GetCurrentBlock()->GetBlock();
        const auto third_value = function->LoadImm(ValueType::U64, Imm{swift::u64{9}});
        function->SetHostGPR(third_value, HostRegIndex(20), Imm{0u});
        function->Nop(); const auto* third_point = &third->GetInstList().back();
        builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x2630}}));
        auto* incoming = function->GetCurrentBlock()->GetBlock();
        function->Nop(); const auto* incoming_point = &incoming->GetInstList().back();
        builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x2640}}));
        auto* untouched = function->GetCurrentBlock()->GetBlock();
        function->UniformBarrier();
        builder.ReturnToHost(); function->ComputeRPO(); function->IdByRPO();
        compile::CompileUnit unit{function}; unit.SealIR();
        compile::SemanticFacts facts{function};
        REQUIRE(facts.PrepareSSAValidation());
        CHECK_FALSE(facts.FindFunctionCFG());
        CHECK(facts.PrepareGuestStateGPRMatch(first, 20, 0, *first_point, first_value, Policy::Function));
        CHECK_FALSE(facts.FindFunctionCFG());
        CHECK_FALSE(facts.FindGuestStateObservations(second));
        CHECK(facts.PrepareGuestStateGPRMatch(second, 20, 0, *second_point, second_value, Policy::Function));
        CHECK_FALSE(facts.FindFunctionCFG());
        CHECK_FALSE(facts.FindGuestStateObservations(third));
        CHECK(facts.FindGuestStateGPRMatch(first, 20, 0, *first_point, first_value, Policy::Function) == true);
        CHECK(facts.PrepareGuestStateGPRMatch(third, 20, 0, *third_point, third_value, Policy::Function));
        CHECK_FALSE(facts.FindFunctionCFG());
        CHECK_FALSE(facts.FindGuestStateObservations(incoming));
        CHECK(facts.PrepareGuestStateGPRMatch(incoming, 20, 0, *incoming_point,
                                             third_value, Policy::Function));
        CHECK(facts.FindFunctionCFG());
        CHECK_FALSE(facts.FindGuestStateObservations(untouched));
        CHECK(facts.FindGuestStateGPRMatch(first, 20, 0, *first_point, first_value, Policy::Function) == true);
        CHECK(facts.FindGuestStateGPRMatch(second, 20, 0, *second_point, second_value, Policy::Function) == true);
        CHECK(facts.FindGuestStateGPRMatch(third, 20, 0, *third_point, third_value, Policy::Function) == true);
        CHECK_FALSE(facts.FindGuestStateGPRMatch(untouched, 20, 0, *first_point,
                                               first_value, Policy::Function));
    }
}

TEST_CASE("canonical FPR sharing follows aliases and keeps banks distinct", "[block-analysis][register-sharing]") {
    using namespace swift::runtime::backend;
    IntrusivePtr<Block> block{new Block(0, Location{0x10a0})};
    const auto root = block->LoadImm(ValueType::U64, Imm{swift::u64{1}});
    const auto alias = block->BitCast(ValueType::U64, root);
    const auto nested = block->BitCast(ValueType::U64, alias);
    const auto other = block->LoadImm(ValueType::U64, Imm{swift::u64{2}});
    const auto floating = block->LoadImm(ValueType::U64, Imm{swift::u64{3}});
    block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.MapRegister(root.Id(), HostFPR{14});
    allocation.MapReference(root.Id(), alias.Id());
    allocation.MapReference(alias.Id(), nested.Id());
    allocation.MapRegister(other.Id(), HostFPR{15});
    allocation.MapRegister(floating.Id(), HostGPR{14});
    REQUIRE(allocation.SharesFPR(root, nested));
    REQUIRE(allocation.SharesFPR(nested, Value{root}.SetCastType(ValueType::U8)));
    REQUIRE_FALSE(allocation.SharesFPR(root, other));
    REQUIRE_FALSE(allocation.SharesFPR(root, floating));
    REQUIRE_FALSE(allocation.SharesFPR(floating, root));
    REQUIRE_FALSE(allocation.SharesFPR(Value{}, root));
    REQUIRE_FALSE(allocation.SharesFPR(root, Value{}));
}

TEST_CASE("block membership preserves sparse spans and incoming pointer identity", "[block-analysis]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x1090})};
    IntrusivePtr<Block> parent{new Block(0, Location{0x1098})};
    const auto first = block->LoadImm(ValueType::U64, Imm{swift::u64{1}});
    const auto middle = block->LoadImm(ValueType::U64, Imm{swift::u64{2}});
    const auto last = block->LoadImm(ValueType::U64, Imm{swift::u64{3}});
    const auto incoming = parent->LoadImm(ValueType::U64, Imm{swift::u64{4}});
    BlockAnalysisIndex index;
    const auto set_id = [](Block* owner, Value value, swift::u16 id) {
        ::swift::runtime::compile::CompileWorkspace compilation_workspace;
        compile::RewriteContext{owner, compilation_workspace, nullptr, false}.Change(
                owner, value.Def(), [id](Inst& target) { target.SetId(id); });
    };
    for (unsigned spacing : {1u, 3u})
        for (unsigned base : {0u, 1000u}) {
            set_id(block.get(), first, base);
            set_id(block.get(), middle, base + spacing);
            set_id(block.get(), last, base + 2 * spacing);
            index.Build(block.get());
            index.PrepareResolvedUses();
            REQUIRE(index.Summary().InstructionCount() == 3);
            REQUIRE_FALSE(index.Summary().HasConsumerDecisions());
            REQUIRE_FALSE(index.Summary().HasReturnAddressPush());
            REQUIRE(index.Contains(first.Def()));
            REQUIRE(index.Contains(middle.Def()));
            REQUIRE(index.Contains(last.Def()));
            REQUIRE_FALSE(index.Contains(nullptr));
            set_id(parent.get(), incoming, middle.Id());
            REQUIRE_FALSE(index.Contains(incoming.Def()));
            set_id(parent.get(), incoming, last.Id() + 1);
            REQUIRE_FALSE(index.Contains(incoming.Def()));
            set_id(parent.get(), incoming, base + 1);
            REQUIRE_FALSE(index.Contains(incoming.Def()));
        }
}

TEST_CASE("value visitation preserves order multiplicity and operand views", "[block-analysis][value-visitor]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x1080})};
    auto first = block->LoadImm(ValueType::U64, Imm{swift::u64{3}});
    auto second = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
    auto byte = Value{first}.SetCastType(ValueType::U8);
    auto word = Value{second}.SetCastType(ValueType::U32);
    auto direct = block->CallLambda(ValueType::VOID, Lambda{first}, byte, word, byte);
    Params params;
    params.Push(word);
    params.Push(Imm{swift::u64{99}});
    params.Push(byte);
    params.Push(word);
    auto nested = block->CallDynamic(ValueType::VOID, Lambda{first}, std::move(params));
    auto immediate = block->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{1}}}, byte);
    const auto check = [](const Inst* inst, std::initializer_list<Value> expected) {
        std::vector<Value> seen;
        inst->ForEachValue([&](Value value) { seen.push_back(value); });
        REQUIRE(seen.size() == expected.size());
        size_t at = 0;
        for (auto value : expected) {
            REQUIRE(seen[at].Def() == value.Def());
            REQUIRE(seen[at].Type() == value.Type());
            ++at;
        }
        // Changing the callback's value must not change an operand's view.
        inst->ForEachValue([](Value value) { value = value.SetCastType(ValueType::U16); });
        const auto copies = inst->GetValues();
        REQUIRE(copies.size() == expected.size());
        at = 0;
        for (auto value : expected) REQUIRE(copies[at++].Type() == value.Type());
    };
    check(direct.Def(), {first, byte, word, byte});
    check(nested.Def(), {first, word, byte, word});
    check(immediate.Def(), {byte});
    check(first.Def(), {});
    block->ReIdInstr();
    for (bool compact : {false, true}) {
        BlockAnalysisIndex::CompactUses scratch;
        BlockAnalysisIndex index;
        index.Build(block.get(), compact ? &scratch : nullptr);
        index.PrepareResolvedUses();
        REQUIRE(index.Uses(first.Def()).size() == 3);
        REQUIRE(index.Uses(first.Def())[0].count == 3);
        REQUIRE(index.Uses(first.Def())[1].count == 2);
        REQUIRE(index.Uses(first.Def())[2].count == 1);
        REQUIRE(index.Uses(second.Def()).size() == 2);
        REQUIRE(index.Uses(second.Def())[0].count == 1);
        REQUIRE(index.Uses(second.Def())[1].count == 2);
    }
}

TEST_CASE("resolved consumers combine direct and nested cast operands", "[block-analysis]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x1000})};
    auto source = block->LoadImm(ValueType::U64, Imm{swift::u64{3}});
    auto first = block->BitCast(ValueType::U64, source);
    auto second = block->BitCast(ValueType::U64, first);
    auto sum = block->Add(ValueType::U64, source, Operand{second});
    block->ReIdInstr();
    for (bool compact : {false, true}) {
        BlockAnalysisIndex::CompactUses scratch;
        BlockAnalysisIndex index; index.Build(block.get(), compact ? &scratch : nullptr);
        const auto& readonly = index;
        REQUIRE_FALSE(readonly.ResolvedUsesReady());
        REQUIRE_THROWS_AS(readonly.ResolvedUses(source.Def()), std::logic_error);
        index.PrepareResolvedUses();
        const auto uses = index.ResolvedUses(source.Def());
        REQUIRE(uses.size() == 3);
        REQUIRE(uses.back().consumer == sum.Def()); REQUIRE(uses.back().count == 2);
        REQUIRE(index.LastInstructionUse(source.Def()) == sum.Id());
        REQUIRE(index.ResolvedUses(first.Def()).empty());
    }
}

TEST_CASE("indexed last use agrees with resolved operand scans at proof boundaries", "[block-analysis][publication]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> parent{new Block(0, Location{0x1100})};
        IntrusivePtr<Block> block{new Block(0, Location{0x1108})};
        auto root = (scenario & 1 ? parent.get() : block.get())
                            ->LoadImm(ValueType::U64, Imm{swift::u64{3}});
        auto alias = root;
        if (scenario & 2) {
            alias = block->BitCast(ValueType::U64, root);
            alias = block->BitCast(ValueType::U64, alias);
        }
        const auto consumer = block->Add(ValueType::U64, root, Operand{alias});
        if (scenario & 4) block->SaveFlags(alias, Flags::NZCV);
        block->SetHostGPR(consumer, HostRegIndex(20), Imm{0u});
        parent->ReIdInstr();
        block->ReIdInstr();
        if (scenario & 1)
            compile::RewriteContext{parent.get(), compilation_workspace, nullptr, false}.Change(
                    parent.get(), root.Def(), [](Inst& target) {
                        target.SetId(500);
                    });  // incoming IDs can exceed local uses
        const auto resolve = [](Value value) {
            while (value.Def() && value.Def()->IsBitCastOperation())
                value = value.Def()->GetArg<Value>(0);
            return value.Def();
        };
        unsigned expected = root.Id();
        for (auto& node : block->GetInstList()) for (auto use : node.GetValues())
            if (resolve(use) == root.Def()) expected = std::max<unsigned>(expected, node.Id());
        for (bool compact : {false, true}) {
            BlockAnalysisIndex::CompactUses scratch;
            BlockAnalysisIndex index;
            index.Build(block.get(), compact ? &scratch : nullptr);
            index.PrepareResolvedUses();
            REQUIRE(index.LastInstructionUse(root.Def()) == expected);
            const auto window = index.Between(consumer.Id(), block->GetInstList().back().Id());
            REQUIRE(window.size() == ((scenario & 4) ? 1 : 0));
            if (!window.empty()) REQUIRE(window.front()->GetOp() == OpCode::SaveFlags);
        }
    }
}

TEST_CASE("resolved block uses follow incoming bitcasts and reset to direct operands", "[block-analysis]") {
    IntrusivePtr<Block> first{new Block(0, Location{0x1000})};
    auto source = first->LoadImm(ValueType::U64, Imm{swift::u64{3}});
    auto cast = first->BitCast(ValueType::U64, source);
    first->ReIdInstr();
    IntrusivePtr<Block> second{new Block(0, Location{0x2000})};
    auto sum = second->Add(ValueType::U64, cast, Operand{cast});
    second->ReIdInstr();
    BlockAnalysisIndex index; index.Build(second.get());
    index.PrepareResolvedUses();
    REQUIRE(index.ResolvedUses(source.Def()).size() == 1);
    REQUIRE(index.ResolvedUses(source.Def()).front().consumer == sum.Def());
    REQUIRE(index.ResolvedUses(source.Def()).front().count == 2);
    REQUIRE(index.Uses(source.Def()).empty());
    IntrusivePtr<Block> direct{new Block(0, Location{0x3000})};
    auto value = direct->LoadImm(ValueType::U64, Imm{swift::u64{5}});
    auto result = direct->Add(ValueType::U64, value, Operand{value});
    direct->ReIdInstr();
    index.Build(direct.get());
    index.PrepareResolvedUses();
    REQUIRE(index.ResolvedUses(source.Def()).empty());
    REQUIRE(index.ResolvedUses(value.Def()).front().consumer == result.Def());
    REQUIRE(index.ResolvedUses(value.Def()).front().count == 2);
}

TEST_CASE("block use lists retain operand multiplicity and discard previous contents", "[block-analysis]") {
    IntrusivePtr<Block> first{new Block(0, Location{0x1000})};
    auto value = first->LoadImm(ValueType::U64, Imm{swift::u64{3}});
    auto sum = first->Add(ValueType::U64, value, Operand{value});
    first->SetHostGPR(sum, HostRegIndex(19), Imm{0u});
    BlockAnalysisIndex index;
    index.Build(first.get());
    index.PrepareResolvedUses();
    const auto uses = index.Uses(value.Def());
    REQUIRE(uses.size() == 1);
    CHECK(uses[0].consumer == sum.Def());
    CHECK(uses[0].count == 2);
    IntrusivePtr<Block> second{new Block(0, Location{0x2000})};
    index.Build(second.get());
    index.PrepareResolvedUses();
    CHECK(index.Uses(value.Def()).empty());
    CHECK(index.Instructions().empty());
}

TEST_CASE("compact consumers retain multiplicity through capacity fallback", "[block-analysis]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x1000})};
    auto left = block->LoadImm(ValueType::U64, Imm{swift::u64{3}});
    auto right = block->LoadImm(ValueType::U64, Imm{swift::u64{4}});
    for (unsigned i = 0; i < 64; ++i) {
        auto sum = block->Add(ValueType::U64, left, Operand{left});
        block->Add(ValueType::U64, right, Operand{sum});
    }
    block->ReIdInstr();
    BlockAnalysisIndex::CompactUses scratch;
    BlockAnalysisIndex index; index.Build(block.get(), &scratch);
    index.PrepareResolvedUses();
    REQUIRE(index.DirectUseCount(left.Def()) == 128);
    REQUIRE(index.DirectUseCount(right.Def()) == 64);
    REQUIRE(index.ResolvedUses(left.Def()).size() == 64);
    for (const auto& use : index.ResolvedUses(left.Def())) REQUIRE(use.count == 2);
    index.Build(block.get(), &scratch);
    index.PrepareResolvedUses();
    REQUIRE(index.DirectUseCount(left.Def()) == 128);
}

TEST_CASE("reverse write search agrees with forward search at reads and partial writes", "[block-analysis]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<BlockAnalysisIndex>);
    // 64 tiny combinations; no guest execution or timing loops.
    for (unsigned scenario = 0; scenario < 64; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0x1000})};
        auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{3}});
        for (unsigned i = 0; i < 6; ++i) {
            const unsigned home = 19 + (i & 1);
            block->SetHostGPR(value, HostRegIndex(home), Imm{0u});
            if (scenario & (1u << i)) {
                if (i % 3 == 0) block->GetHostGPR(ValueType::U64, HostRegIndex(home), Imm{0u});
                if (i % 3 == 1) block->SetHostGPR(value, HostRegIndex(home), Imm{1u});
                if (i % 3 == 2) block->UniformBarrier();
            }
        }
        BlockAnalysisIndex index;
        index.Build(block.get());
        index.PrepareResolvedUses();
        auto observes = [](const Inst& inst) { return inst.GetOp() == OpCode::UniformBarrier; };
        auto full = [](const Inst& inst) { return inst.GetArg<Imm>(2).Get() == 0; };
        swift::runtime::backend::arm64::GPRWriteSuccessors successors;
        successors.Build(index, observes, full);
        // A distinct observer policy must not overwrite an existing projection
        // or require copying the canonical use graph.
        swift::runtime::backend::arm64::GPRWriteSuccessors conservative;
        conservative.Build(index, [](const Inst&) { return true; }, full);
        const auto instructions = index.Instructions();
        for (size_t i = 0; i < instructions.size(); ++i) {
            auto* write = instructions[i];
            if (write->GetOp() != OpCode::SetHostGPR) continue;
            const auto home = write->GetArg<Imm>(1).Get();
            const Inst* expected{};
            for (size_t j = i + 1; j < instructions.size(); ++j) {
                auto* later = instructions[j];
                if (observes(*later)) break;
                if (later->GetOp() == OpCode::GetHostGPR && later->GetArg<Imm>(0).Get() == home) break;
                if (later->GetOp() == OpCode::SetHostGPR && later->GetArg<Imm>(1).Get() == home) {
                    if (full(*later)) expected = later;
                    break;
                }
            }
            CHECK(successors.FollowingWrite(write) == expected);
            CHECK(conservative.FollowingWrite(write) == nullptr);
        }
    }
}

TEST_CASE("fixed home intervals preserve partial writes, exchanges and exact helper barriers",
          "[block-analysis][pinned-gpr]") {
    using swift::runtime::backend::arm64::RegisterStatePreparation;
    using swift::runtime::backend::arm64::HelperCallContract;
    IntrusivePtr<Block> block{new Block(0, Location{0x3000})};
    auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{3}});
    for (unsigned i = 0; i < 80; ++i) block->Nop();
    auto* full = block->AppendInst(OpCode::SetHostGPR, value, HostRegIndex(20), Imm{0u});
    const auto* read = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u}).Def();
    auto* partial = block->AppendInst(OpCode::SetHostGPR, value, HostRegIndex(20), Imm{1u});
    auto* call = block->AppendInst(OpCode::CallLambda, ValueType::VOID, Lambda{Imm{swift::u64{1}}});
    auto* resident = block->AppendInst(OpCode::CallLambda, ValueType::VOID,
            Lambda{DataClass{Imm{swift::u64{1}}},
                   HelperCallTraits{.host_registers = HostRegisterEffect::PreservesPinnedState}});
    auto* leaf = block->AppendInst(OpCode::CallLambda, ValueType::VOID,
            Lambda{DataClass{Imm{swift::u64{1}}},
                   HelperCallTraits{.abi = HelperABI::PreserveAllLeaf}});
    auto left = block->LoadUniform(Uniform{0, ValueType::V128});
    auto right = block->LoadUniform(Uniform{16, ValueType::V128});
    auto* sse = block->Sse42Str(ValueType::U64, left, right, Imm{swift::u64{0x02}}).Def();
    auto* exchange = block->AppendInst(OpCode::XchgBarrier, Imm{swift::u32{0x10}});
    block->ReIdInstr();
    std::vector<swift::u32> boundaries{0, 1, block->MaxInstrId()};
    for (const Inst* inst :
         std::initializer_list<const Inst*>{full, partial, call, resident, leaf, sse, exchange}) {
        boundaries.push_back(inst->Id() - 1);
        boundaries.push_back(inst->Id());
        boundaries.push_back(inst->Id() + 1);
    }

    compile::SemanticFacts facts{block.get()};
    CHECK_FALSE(facts.FindGuestStateObservations(block.get()).has_value());
    const Inst* const* shared_events{};
    const compile::GuestStateRegisterAccesses* shared_accesses{};
    bool access_input_reused = true;
    CHECK_FALSE(facts.FindGuestStateRegisterAccesses(block.get()));
    RegisterStatePreparation state;
    for (bool leaf_abi : {false, true}) {
        FeatureSet features{}; features.helper_leaf_abi = leaf_abi;
        state.BeginBlock(block.get(), features, facts);
        const auto clobbers = [&](const Inst& inst, swift::u32 home) {
            return inst.GetOp() == OpCode::XchgBarrier
                    ? (swift::runtime::backend::FixedGPRClobbers(inst, features) & (1u << home)) != 0
                    : (inst.GetOp() == OpCode::SetHostGPR && inst.GetArg<Imm>(1).Get() == home) ||
                              (home <= 9 && HelperCallContract::InstructionClobbersGPR(inst, home, features));
        };
        // Three successful scans precede the indexed preparation request.
        // The independent oracle walks the original instruction sequence.
        for (unsigned i = 0; i < 4; ++i)
            REQUIRE(state.PrepareHomeSurvival(31, 0, block->MaxInstrId()));
        if (!leaf_abi) CHECK_FALSE(facts.FindGuestStateObservations(block.get()).has_value());
        const auto* accesses = facts.FindGuestStateRegisterAccesses(block.get());
        if (!accesses) access_input_reused = false;
        else if (!shared_accesses) shared_accesses = accesses;
        else access_input_reused &= accesses == shared_accesses;
        for (swift::u32 home : {0u, 2u, 7u, 9u, 19u, 20u, 22u, 23u, 29u, 31u}) {
            for (auto after : boundaries) for (auto before : boundaries) {
                bool expected = true;
                for (auto& inst : block->GetInstList()) {
                    if (inst.Id() <= after || inst.Id() >= before) continue;
                    if (clobbers(inst, home)) { expected = false; break; }
                }
                CHECK(state.PrepareHomeSurvival(home, after, before) == expected);
            }
            for (auto after : boundaries) {
                std::optional<swift::u32> expected;
                for (const auto& inst : block->GetInstList()) {
                    if (inst.Id() > after && clobbers(inst, home)) {
                        expected = inst.Id();
                        break;
                    }
                }
                CHECK(state.PrepareFirstClobber(home, after) == expected);
            }
        }
        const std::array<std::pair<swift::u32, swift::u32>, 8> windows{{
                {full->Id() - 1, partial->Id() + 1},
                {full->Id(), partial->Id()},
                {read->Id(), partial->Id() + 1},
                {partial->Id(), call->Id() + 1},
                {call->Id(), resident->Id() + 1},
                {leaf->Id(), sse->Id() + 1},
                {block->MaxInstrId(), 0},
                {full->Id(), full->Id() + 1},
        }};
        for (swift::u32 home : {9u, 20u, 21u})
        for (const auto [after, before] : windows)
        for (const Inst* ignored : std::array<const Inst*, 4>{nullptr, read, partial, call}) {
            bool expected = true;
            for (const auto& inst : block->GetInstList()) {
                if (inst.Id() <= after || inst.Id() >= before || &inst == ignored) continue;
                const bool reads = inst.GetOp() == OpCode::GetHostGPR &&
                                   inst.GetArg<Imm>(0).Get() == home;
                const bool helper_op = inst.GetOp() == OpCode::CallLambda ||
                                       inst.GetOp() == OpCode::Sse42Str;
                const auto helper = HelperCallContract::Resolve(inst, features);
                const bool observes = helper_op && (!helper || helper->RequiresGuestStatePublication());
                if (reads || clobbers(inst, home) || observes) {
                    expected = false;
                    break;
                }
            }
            CHECK(state.PreparePublicationWindow(home, value, after, before, ignored) == expected);
        }
        const auto observations = facts.FindGuestStateObservations(block.get());
        REQUIRE(observations.has_value());
        if (!leaf_abi) shared_events = observations->data();
        else CHECK(observations->data() == shared_events);
        CHECK_FALSE(state.PrepareHomeSurvival(20, partial->Id() - 1, partial->Id() + 1));
        CHECK(state.PrepareHomeSurvival(20, partial->Id(), partial->Id() + 1));
        // Reanalysis can also change the helper ABI between queries.
        features.helper_leaf_abi = !leaf_abi;
        state.BeginBlock(block.get(), features, facts);
        for (unsigned i = 0; i < 3; ++i)
            REQUIRE(state.PrepareHomeSurvival(31, 0, block->MaxInstrId()));
        CHECK(state.PrepareHomeSurvival(9, leaf->Id() - 1, leaf->Id() + 1) ==
              !HelperCallContract::InstructionClobbersGPR(*leaf, 9, features));
    }
    REQUIRE(shared_accesses);
    CHECK(access_input_reused);
    CHECK_FALSE(facts.FindGuestStateGPRDefinitions(block.get()).has_value());
    const std::array<const Inst*, 5> expected_barriers{{call, resident, leaf, sse, exchange}};
    CHECK(std::equal(shared_accesses->GPRBarriers().begin(), shared_accesses->GPRBarriers().end(),
                     expected_barriers.begin(), expected_barriers.end()));
    // Compare the shared numbered inputs against the original sequence, without
    // using its packed keys or either physical clobber projection.
    bool static_accesses_match = true;
    for (swift::u32 home : {0u, 9u, 20u, 31u})
    for (bool reads : {false, true})
    for (swift::u32 after : boundaries)
    for (const Inst* ignored : std::array<const Inst*, 4>{nullptr, read, partial, full}) {
        std::optional<swift::u32> expected;
        for (const auto& inst : block->GetInstList()) {
            if (inst.Id() <= after || (ignored && inst.Id() == ignored->Id())) continue;
            if (inst.GetOp() == (reads ? OpCode::GetHostGPR : OpCode::SetHostGPR) &&
                inst.GetArg<Imm>(reads ? 0 : 1).Get() == home) {
                expected = inst.Id();
                break;
            }
        }
        const auto ignored_id = ignored ? std::optional<swift::u32>{ignored->Id()} : std::nullopt;
        static_accesses_match &=
                shared_accesses->FirstAccess(compile::GuestStateRegisterFile::GPR, home, reads, after, ignored_id) == expected;
    }
    CHECK(static_accesses_match);

    IntrusivePtr<Block> transitions{new Block(0, Location{0x3800})};
    const auto target = transitions->LoadImm<U64>(Imm{swift::u64{0x3810}});
    transitions->AppendInst(OpCode::CallLocation, ValueType::VOID, Lambda{Imm{swift::u64{0x3810}}}, Params{});
    transitions->AppendInst(OpCode::CallDynamic, ValueType::VOID, Lambda{target}, Params{});
    transitions->AppendInst(OpCode::X87Op, ValueType::U64, target, Imm{0u}, target);
    transitions->ReIdInstr();
    compile::SemanticFacts transition_facts{transitions.get()};
    state.BeginBlock(transitions.get(), FeatureSet{}, transition_facts);
    bool other_barriers_match = true;
    for (swift::u32 home : {0u, 9u, 20u})
    for (swift::u32 after = 0; after <= transitions->MaxInstrId(); ++after) {
        std::optional<swift::u32> expected;
        for (const auto& inst : transitions->GetInstList()) {
            if (inst.Id() > after && home <= 9 &&
                HelperCallContract::InstructionClobbersGPR(inst, home, FeatureSet{})) {
                expected = inst.Id();
                break;
            }
        }
        other_barriers_match &= state.PrepareFirstClobber(home, after) == expected;
    }
    CHECK(other_barriers_match);
    REQUIRE(transition_facts.FindGuestStateRegisterAccesses(transitions.get()));
    CHECK(transition_facts.FindGuestStateRegisterAccesses(transitions.get())->GPRBarriers().size() == 3);
    CHECK_FALSE(transition_facts.FindGuestStateObservations(transitions.get()).has_value());
    CHECK_FALSE(transition_facts.FindIndex(transitions.get()));
    CHECK_FALSE(facts.FindIndex(block.get()));
    CHECK_FALSE(facts.FindUses(block.get()));
    CHECK_FALSE(facts.FindFunctionCFG());
    bool all_target_candidates_present = true;
    for (unsigned raw = 1; raw < static_cast<unsigned>(OpCode::COUNT); ++raw) {
        const auto op = static_cast<OpCode>(raw);
        if (swift::runtime::backend::arm64::TargetMayFaultOrObserve(op) ||
            op == OpCode::GetUniformAddress || op == OpCode::UniformBarrier)
            all_target_candidates_present &= compile::IsGuestStateObservationCandidate(op);
    }
    CHECK(all_target_candidates_present);
    IntrusivePtr<Block> next{new Block(0, Location{0x4000})};
    for (unsigned i = 0; i < 100; ++i) next->Nop();
    next->ReIdInstr();
    facts.BeginInput(next.get(), nullptr);
    CHECK_FALSE(facts.FindGuestStateObservations(next.get()).has_value());
    CHECK_FALSE(facts.FindGuestStateRegisterAccesses(block.get()));
    CHECK_FALSE(facts.FindGuestStateRegisterAccesses(next.get()));
    const auto& empty_summary = facts.PrepareSummary(next.get());
    CHECK_FALSE(empty_summary.HasGPRWindowCandidates());
    state.BeginBlock(next.get(), FeatureSet{}, facts);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(state.PrepareHomeSurvival(20, 0, next->MaxInstrId()));
    CHECK_FALSE(facts.FindGuestStateObservations(next.get()).has_value());
    CHECK_FALSE(facts.FindGuestStateRegisterAccesses(next.get()));
    CHECK(facts.PrepareGuestStateObservations(next.get()).empty());
    CHECK(facts.FindGuestStateObservations(next.get()).has_value());
    CHECK_FALSE(facts.FindGuestStateGPRDefinitions(next.get()).has_value());
    state.PrepareValueVersions({}, {}, false);
    CHECK_FALSE(facts.FindGuestStateGPRDefinitions(next.get()).has_value());
    CHECK(facts.PrepareGuestStateGPRDefinitions(next.get()).empty());
    CHECK(facts.FindGuestStateGPRDefinitions(next.get()).has_value());
    const auto& empty_accesses = facts.PrepareGuestStateRegisterAccesses(next.get());
    CHECK(empty_accesses.GPRBarriers().empty());
    CHECK_FALSE(empty_accesses.FirstAccess(compile::GuestStateRegisterFile::GPR, 20, false, 0).has_value());
    CHECK(facts.FindGuestStateRegisterAccesses(next.get()) == &empty_accesses);
    CHECK_FALSE(facts.FindIndex(next.get()));

    // Preparing another Function member must not move an already-published
    // observation span, and this logical query must not force a CFG/use index.
    HIRBuilder hir{1, false, FeatureSet{}};
    auto* function = hir.AppendFunction(Location{0x5000});
    auto* first = function->GetCurrentBlock()->GetBlock();
    function->UniformBarrier();
    const auto* first_event = &first->GetInstList().back();
    const auto first_definition = function->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
    for (unsigned i = 1; i < 12; ++i) {
        hir.SetCurBlock(hir.LinkBlock(terminal::LinkBlock{Location{0x5000u + i * 16u}}));
        function->UniformBarrier();
        function->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
    }
    hir.ReturnToHost();
    function->ComputeRPO();
    function->IdByRPO();
    compile::SemanticFacts function_facts{function};
    const auto first_events = function_facts.PrepareGuestStateObservations(first);
    CHECK_FALSE(function_facts.FindGuestStateGPRDefinitions(first).has_value());
    const auto first_definitions = function_facts.PrepareGuestStateGPRDefinitions(first);
    REQUIRE(first_definitions.size() == 1);
    CHECK(first_definitions.front() == first_definition.Def());
    CHECK_FALSE(function_facts.FindGuestStateRegisterAccesses(first));
    const auto& first_accesses = function_facts.PrepareGuestStateRegisterAccesses(first);
    CHECK(first_accesses.FirstAccess(compile::GuestStateRegisterFile::GPR, 20, true, first_event->Id()) == first_definition.Id());
    CHECK(first_accesses.GPRBarriers().empty());
    for (const auto& member : function->GetHIRBlocksRPO()) {
        (void)function_facts.PrepareGuestStateObservations(member.GetBlock());
        (void)function_facts.PrepareGuestStateGPRDefinitions(member.GetBlock());
        (void)function_facts.PrepareGuestStateRegisterAccesses(member.GetBlock());
    }
    CHECK(function_facts.FindGuestStateRegisterAccesses(first) == &first_accesses);
    CHECK(first_accesses.FirstAccess(compile::GuestStateRegisterFile::GPR, 20, true, first_event->Id()) == first_definition.Id());
    CHECK(function_facts.FindGuestStateGPRDefinitions(first)->data() == first_definitions.data());
    CHECK(first_definitions.front() == first_definition.Def());
    CHECK(function_facts.FindGuestStateObservations(first)->data() == first_events.data());
    CHECK(first_events.front() == first_event);
    CHECK_FALSE(function_facts.FindFunctionCFG());
    CHECK_FALSE(function_facts.FindIndex(first));

    // A shared universe includes extension aliases and foreign definitions
    // even when their IDs collide. Distinct physical policies reuse only the
    // catalog; ignoring a publication must still change that view's result.
    Block external{Location{0x6100}};
    const auto foreign_raw = external.LoadImm(ValueType::U16, Imm{swift::u64{7}});
    const auto foreign_zero = external.ZeroExtend32(ValueType::U32, foreign_raw);
    const auto foreign_value = external.SignExtend(ValueType::U64, foreign_zero);
    external.ReIdInstr();
    IntrusivePtr<Block> catalog{new Block(0, Location{0x6000})};
    const auto local = catalog->LoadImm(ValueType::U64, Imm{swift::u64{3}});
    catalog->SetHostGPR(local, HostRegIndex(20), Imm{0u});
    for (unsigned i = 0; i < 200; ++i) catalog->Nop();
    catalog->SetHostGPR(foreign_value, HostRegIndex(21), Imm{0u});
    const auto* foreign_publication = &catalog->GetInstList().back();
    const auto narrow = catalog->GetHostGPR(ValueType::U16, HostRegIndex(22), Imm{0u});
    const auto zero = catalog->ZeroExtend32(ValueType::U32, narrow);
    const auto wide = catalog->SignExtend(ValueType::U64, zero);
    catalog->SetHostGPR(wide, HostRegIndex(23), Imm{0u});
    const auto local_user = catalog->Add(ValueType::U64, local, Operand{Imm{swift::u64{1}}});
    const auto foreign_user = catalog->Add(ValueType::U64, foreign_value, Operand{Imm{swift::u64{1}}});
    const auto alias_user = catalog->Or(ValueType::U32, zero, Operand{Imm{swift::u32{1}}});
    catalog->ReIdInstr();
    compile::SemanticFacts catalog_facts{catalog.get()};
    RegisterStatePreparation full_view;
    full_view.BeginBlock(catalog.get(), FeatureSet{}, catalog_facts);
    CHECK_FALSE(catalog_facts.FindGuestStateGPRDefinitions(catalog.get()).has_value());
    full_view.PrepareValueVersions({}, {}, false);
    CHECK_FALSE(catalog_facts.FindGuestStateGPRDefinitions(catalog.get()).has_value());
    const auto definitions = catalog_facts.PrepareGuestStateGPRDefinitions(catalog.get());
    std::array<const Inst*, 7> expected_definitions{
            local.Def(), foreign_raw.Def(), foreign_zero.Def(), foreign_value.Def(),
            narrow.Def(), zero.Def(), wide.Def()};
    std::sort(expected_definitions.begin(), expected_definitions.end(), std::less<const Inst*>{});
    REQUIRE(definitions.size() == expected_definitions.size());
    CHECK(std::ranges::equal(definitions, expected_definitions));
    CHECK_FALSE(catalog_facts.FindGuestStateObservations(catalog.get()).has_value());
    CHECK_FALSE(catalog_facts.FindIndex(catalog.get()));
    CHECK_FALSE(catalog_facts.FindUses(catalog.get()));
    CHECK_FALSE(catalog_facts.FindFunctionCFG());
    full_view.PrepareValueVersions({}, {}, true);
    CHECK(catalog_facts.FindGuestStateGPRDefinitions(catalog.get())->data() == definitions.data());
    const auto local_home = full_view.FixedHomeForUse(local, local_user.Def());
    const auto foreign_home = full_view.FixedHomeForUse(foreign_value, foreign_user.Def());
    const auto alias_home = full_view.FixedHomeForUse(zero, alias_user.Def());
    REQUIRE(local_home.has_value());
    REQUIRE(foreign_home.has_value());
    REQUIRE(alias_home.has_value());
    CHECK(local_home->home == 20);
    CHECK(foreign_home->home == 21);
    CHECK(alias_home->home == 23);
    RegisterStatePreparation ignored_view;
    ignored_view.BeginBlock(catalog.get(), FeatureSet{}, catalog_facts);
    const std::array ignored{foreign_publication};
    ignored_view.PrepareValueVersions(ignored, {}, true);
    CHECK_FALSE(ignored_view.FixedHomeForUse(foreign_value, foreign_user.Def()).has_value());
    CHECK(ignored_view.FixedHomeForUse(local, local_user.Def())->home == 20);
    CHECK(ignored_view.FixedHomeForUse(zero, alias_user.Def())->home == 23);
    CHECK(catalog_facts.FindGuestStateGPRDefinitions(catalog.get())->data() == definitions.data());

    // Compare both banks and every interval with an independent IR walk. Sparse
    // IDs and the last representable ID must not alias another bank/home.
    for (const bool sparse : {false, true}) {
        CAPTURE(sparse);
        IntrusivePtr<Block> mixed{new Block(0, Location{0x7000})};
        const auto integer = mixed->LoadImm(ValueType::U64, Imm{swift::u64{17}});
        const auto vector = mixed->VecLoadConst(ValueType::V128,
                                                Imm{swift::u64{1}}, Imm{swift::u64{2}});
        mixed->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
        mixed->SetHostFPR(vector, HostRegIndex(20), Imm{0u});
        mixed->GetHostFPR(ValueType::U64, HostRegIndex(20), Imm{8u});
        mixed->SetHostGPR(integer, HostRegIndex(20), Imm{0u});
        mixed->UniformBarrier();
        mixed->SetHostFPR(Value{vector}.SetCastType(ValueType::V64), HostRegIndex(20), Imm{0u});
        mixed->GetHostGPR(ValueType::U32, HostRegIndex(20), Imm{4u});
        mixed->GetHostFPR(ValueType::V128, HostRegIndex(31), Imm{0u});
        mixed->SetHostGPR(integer, HostRegIndex(31), Imm{0u});
        mixed->SetHostFPR(vector, HostRegIndex(31), Imm{0u});
        mixed->ReIdInstr();
        if (sparse) {
            swift::u32 ordinal{};
            RewriteFixture rewrite{mixed.get(), false};
            for (auto& inst : mixed->GetInstList()) {
                const auto id = &inst == &mixed->GetInstList().back()
                        ? UINT16_MAX : ordinal++ * 4096u;
                rewrite.Get().Change(mixed.get(), &inst,
                                     [&](Inst& target) { target.SetId(static_cast<swift::u16>(id)); });
            }
        }
        compile::SemanticFacts shared{mixed.get()};
        const auto& accesses = shared.PrepareGuestStateRegisterAccesses(mixed.get());
        CHECK_FALSE(shared.FindGuestStateObservations(mixed.get()).has_value());
        const auto observations = shared.PrepareGuestStateObservations(mixed.get());
        std::vector<swift::u32> points{0u, 65535u, 65536u, UINT32_MAX};
        for (const auto& inst : mixed->GetInstList()) {
            points.push_back(inst.Id());
            points.push_back(swift::u32{inst.Id()} + 1u);
        }
        using File = compile::GuestStateRegisterFile;
        for (const auto file : {File::GPR, File::FPR})
            for (const swift::u32 home : {17u, 20u, 31u})
                for (const bool reads : {false, true})
                    for (const auto after : points) {
                        CAPTURE(file, home, reads, after);
                        std::vector<swift::u32> expected;
                        const auto op = file == File::GPR
                                ? (reads ? OpCode::GetHostGPR : OpCode::SetHostGPR)
                                : (reads ? OpCode::GetHostFPR : OpCode::SetHostFPR);
                        for (const auto& inst : mixed->GetInstList())
                            if (inst.GetOp() == op && inst.Id() > after &&
                                inst.GetArg<Imm>(reads ? 0 : 1).Get() == home)
                                expected.push_back(inst.Id());
                        CHECK(accesses.FirstAccess(file, home, reads, after) ==
                              (expected.empty() ? std::nullopt : std::optional{expected.front()}));
                        if (!expected.empty())
                            CHECK(accesses.FirstAccess(file, home, reads, after, expected.front()) ==
                                  (expected.size() < 2 ? std::nullopt : std::optional{expected[1]}));
                        for (const auto before : points) {
                            std::vector<swift::u32> actual;
                            for (const auto id : accesses.Positions(file, home, reads, after, before))
                                actual.push_back(id);
                            std::vector<swift::u32> bounded;
                            for (const auto id : expected) if (id < before) bounded.push_back(id);
                            CHECK(actual == bounded);
                        }
                    }
        for (const auto after : points)
            for (const auto before : points) {
                std::vector<const Inst*> expected;
                for (const auto& inst : mixed->GetInstList())
                    if (inst.Id() > after && inst.Id() < before &&
                        compile::IsGuestStateObservationCandidate(inst.GetOp()))
                        expected.push_back(&inst);
                CHECK(std::ranges::equal(
                        compile::GuestStateObservationWindow(observations, after, before), expected));
            }
        CHECK_FALSE(shared.FindIndex(mixed.get()));
        CHECK_FALSE(shared.FindUses(mixed.get()));
        CHECK_FALSE(shared.FindFunctionCFG());
    }

}
