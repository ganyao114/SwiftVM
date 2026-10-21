#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/alloc/scalar_fpr_publication_plan.h"
#include "runtime/backend/arm64/alloc/consumer_plan.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/backend/arm64/jit/scalar_copy_analysis.h"
#include "runtime/backend/arm64/jit/resident_scalar_fpr_analysis.h"
#include "runtime/backend/arm64/jit/raw_carry_branch_analysis.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("scalar publication plans keep barriers live lanes and shape rejection", "[scalar-publication-plan]") {
    for (unsigned scenario = 0; scenario < 9; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xdd00})};
        Value live;
        if (scenario == 4) live = block->LoadUniform(Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        const auto zero = block->LoadImm(Imm{swift::u64{scenario == 5 ? 1u : 0u}})
                .SetType(scenario == 6 ? ValueType::U32 : ValueType::U64);
        const auto loaded = block->LoadMemory(Operand{Imm{swift::u64{0x1000}}}).SetType(ValueType::U64);
        if (scenario == 1) (void)block->CallLambda(Lambda{Imm{swift::u64{1}}});
        if (scenario == 2) (void)block->GetHostFPR(HostRegIndex(17), Imm{0u}).SetType(ValueType::V128);
        if (scenario == 3) block->StoreUniform(Uniform{0, ValueType::U64}, loaded);
        const unsigned target = scenario == 7 ? 15 : 17;
        block->SetHostFPR(loaded, HostRegIndex(target), Imm{0u});
        if (scenario == 8) (void)block->LoadImm(Imm{swift::u64{7}});
        block->SetHostFPR(zero, HostRegIndex(target), Imm{8u});
        if (live.Defined()) block->StoreUniform(Uniform{16, ValueType::V128}, live);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        if (live.Defined()) allocation.MapRegister(live.Id(), HostFPR{17});
        arm64::BlockAnalysisIndex index;
        index.Build(block.get());
        const auto shared = arm64::BuildScalarFPRPublicationPlan(block.get(), allocation, &index);
        const auto independent = arm64::BuildScalarFPRPublicationPlan(block.get(), allocation);
        REQUIRE(shared.loads.size() == (scenario == 0 ? 1 : 0));
        REQUIRE(shared.values.size() == (scenario >= 1 && scenario <= 4 ? 1 : 0));
        REQUIRE(shared.loads.size() == independent.loads.size());
        REQUIRE(shared.values.size() == independent.values.size());
        REQUIRE(shared.covered == independent.covered);
        if (!shared.loads.empty()) REQUIRE(shared.loads.begin()->second.target == 17);
    }
}

TEST_CASE("scalar copy analysis is order-independent and replaceable", "[scalar-preparation]") {
    struct Facts { bool a{}, a_left{}, a_right{}, b{}, b_discarded{}, c{}, empty{}; };
    const auto build = [](bool second_first) {
        IntrusivePtr<Block> block{new Block(0, Location{0xdd10})};
        const auto source = block->GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(ValueType::U64);
        Value x_a, l_a, r_a, x_b, v_b, x_c;
        const auto make_a = [&] {
            l_a = block->BitExtract(source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
            r_a = block->BitExtract(source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
            x_a = block->Xor(l_a, Operand{r_a}).SetType(ValueType::U32);
            block->StoreUniform(Uniform{0, ValueType::U32}, x_a);
        };
        const auto make_b = [&] {
            v_b = block->LoadUniform(Uniform{64, ValueType::U32}).SetType(ValueType::U32);
            x_b = block->Xor(v_b, Operand{v_b}).SetType(ValueType::U32);
            block->StoreUniform(Uniform{4, ValueType::U32}, x_b);
            const auto other = block->LoadUniform(Uniform{68, ValueType::U32}).SetType(ValueType::U32);
            x_c = block->Xor(v_b, Operand{other}).SetType(ValueType::U32);
            block->StoreUniform(Uniform{8, ValueType::U32}, x_c);
        };
        if (second_first) { make_b(); make_a(); } else { make_a(); make_b(); }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        arm64::ScalarCopyAnalysis analysis;
        analysis.Analyze(block.get());
        Facts facts{analysis.IsSelfXor(x_a.Def()), analysis.InputDiscarded(l_a.Def()),
                    analysis.InputDiscarded(r_a.Def()), analysis.IsSelfXor(x_b.Def()),
                    analysis.InputDiscarded(v_b.Def()), analysis.IsSelfXor(x_c.Def()), analysis.Empty()};
        analysis.Analyze(block.get());
        REQUIRE(analysis.IsSelfXor(x_a.Def()) == facts.a);
        REQUIRE(analysis.IsSelfXor(x_b.Def()) == facts.b);
        REQUIRE(analysis.InputDiscarded(l_a.Def()) == facts.a_left);
        IntrusivePtr<Block> plain{new Block(0, Location{0xdd14})};
        plain->LoadUniform(Uniform{0, ValueType::U32});
        plain->SetTerminal(terminal::ReturnToHost{});
        plain->ReIdInstr();
        analysis.Analyze(plain.get());
        REQUIRE(analysis.Empty());
        REQUIRE_FALSE(analysis.IsSelfXor(x_a.Def()));
        REQUIRE_FALSE(analysis.InputDiscarded(l_a.Def()));
        return facts;
    };
    const auto forward = build(false);
    REQUIRE(forward.a);
    REQUIRE(forward.a_left);
    REQUIRE(forward.a_right);
    REQUIRE(forward.b);
    REQUIRE_FALSE(forward.b_discarded);
    REQUIRE_FALSE(forward.c);
    REQUIRE_FALSE(forward.empty);
    const auto swapped = build(true);
    REQUIRE(swapped.a == forward.a);
    REQUIRE(swapped.a_left == forward.a_left);
    REQUIRE(swapped.a_right == forward.a_right);
    REQUIRE(swapped.b == forward.b);
    REQUIRE(swapped.b_discarded == forward.b_discarded);
    REQUIRE(swapped.c == forward.c);
    REQUIRE(swapped.empty == forward.empty);
}

TEST_CASE("resident scalar analysis is deterministic across candidates and barriers", "[scalar-preparation]") {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xdd20})};
        auto current = block->GetHostFPR(HostRegIndex(17), Imm{0u}).SetType(ValueType::V128);
        auto view = block->BitCast(current).SetType(ValueType::V128);
        auto zero = block->VecXor(current, view).SetType(ValueType::V128);
        block->SetHostFPR(zero, HostRegIndex(17), Imm{0u});
        if (scenario == 1) block->UniformBarrier();
        auto source = block->LoadUniform(Uniform{8, ValueType::U32});
        auto converted = block->VecFCvtIntToFloat(source, Imm{32u}, Imm{64u}).SetType(ValueType::U64);
        block->SetHostFPR(converted, HostRegIndex(17), Imm{0u});
        auto* publication = &block->GetInstList().back();
        if (scenario == 2) block->StoreUniform(Uniform{16, ValueType::U64}, converted);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        arm64::ResidentScalarFPRAnalysis analysis;
        analysis.Analyze(block.get());
        const auto* recipe = analysis.FindConversion(converted.Def());
        REQUIRE((recipe != nullptr) == (scenario != 1));
        if (recipe) {
            REQUIRE(recipe->target == 17);
            REQUIRE(recipe->export_result == (scenario == 2));
            REQUIRE(analysis.IsDiscarded(publication));
        }
    }
    for (bool overwrite : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0xdd24})};
        auto value = block->GetHostFPR(HostRegIndex(18), Imm{0u}).SetType(ValueType::U64);
        if (overwrite) {
            auto replacement = block->LoadUniform(Uniform{16, ValueType::U64});
            block->SetHostFPR(replacement, HostRegIndex(18), Imm{0u});
        }
        auto address = block->LoadUniform(Uniform{0, ValueType::U64});
        block->StoreMemory(Operand{address}, value);
        auto* store = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        arm64::ResidentScalarFPRAnalysis analysis;
        analysis.Analyze(block.get());
        REQUIRE(analysis.FindMemoryStore(store).has_value() == !overwrite);
        if (!overwrite) {
            REQUIRE(analysis.FindMemoryStore(store) == std::optional<swift::u16>{18});
            REQUIRE(analysis.IsDiscarded(value.Def()));
        }
    }
    {
        IntrusivePtr<Block> block{new Block(0, Location{0xdd28})};
        auto current = block->GetHostFPR(HostRegIndex(17), Imm{0u}).SetType(ValueType::V128);
        auto view = block->BitCast(current).SetType(ValueType::V128);
        auto zero = block->VecXor(current, view).SetType(ValueType::V128);
        block->SetHostFPR(zero, HostRegIndex(17), Imm{0u});
        auto source = block->LoadUniform(Uniform{8, ValueType::U32});
        auto converted = block->VecFCvtIntToFloat(source, Imm{32u}, Imm{64u}).SetType(ValueType::U64);
        block->SetHostFPR(converted, HostRegIndex(17), Imm{0u});
        auto resident = block->GetHostFPR(HostRegIndex(18), Imm{0u}).SetType(ValueType::U64);
        auto address = block->LoadUniform(Uniform{0, ValueType::U64});
        block->StoreMemory(Operand{address}, resident);
        auto* resident_store = &block->GetInstList().back();
        auto vector = block->LoadUniform(Uniform{32, ValueType::V128});
        auto extracted = block->VecExtract64(vector, Imm{0u}).SetType(ValueType::U64);
        block->SetHostFPR(vector, HostRegIndex(19), Imm{0u});
        auto address2 = block->LoadUniform(Uniform{24, ValueType::U64});
        block->StoreMemory(Operand{address2}, extracted);
        auto* extract_store = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        arm64::ResidentScalarFPRAnalysis analysis;
        analysis.Analyze(block.get());
        const auto snapshot = [&] {
            const auto* recipe = analysis.FindConversion(converted.Def());
            return std::tuple{recipe != nullptr, recipe ? recipe->target : swift::u16{},
                              analysis.FindMemoryStore(resident_store), analysis.FindMemoryStore(extract_store),
                              analysis.IsDiscarded(resident.Def()), analysis.IsDiscarded(extracted.Def()),
                              analysis.Empty()};
        };
        const auto first = snapshot();
        REQUIRE(std::get<0>(first));
        REQUIRE(std::get<1>(first) == 17);
        REQUIRE(std::get<2>(first) == std::optional<swift::u16>{18});
        REQUIRE(std::get<3>(first) == std::optional<swift::u16>{19});
        REQUIRE(std::get<4>(first));
        REQUIRE(std::get<5>(first));
        REQUIRE_FALSE(std::get<6>(first));
        analysis.Analyze(block.get());
        REQUIRE(snapshot() == first);
        IntrusivePtr<Block> plain{new Block(0, Location{0xdd2c})};
        plain->LoadUniform(Uniform{0, ValueType::U64});
        plain->SetTerminal(terminal::ReturnToDispatch{});
        plain->ReIdInstr();
        analysis.Analyze(plain.get());
        REQUIRE(analysis.Empty());
        REQUIRE(analysis.FindConversion(converted.Def()) == nullptr);
        REQUIRE_FALSE(analysis.FindMemoryStore(resident_store).has_value());
    }
}

TEST_CASE("raw carry analysis is path-independent and consumer-gated", "[scalar-preparation]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xdd30})};
    const auto source = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
    const auto comparison = block->Sub(source, Operand{Imm{7u}}).SetType(ValueType::U32);
    block->SaveFlags(comparison, Flags::Carry | Flags::Zero);
    block->InvertCarry();
    auto* invert = &block->GetInstList().back();
    block->AdvancePC(Imm{1u});
    const auto carry = block->TestFlags(Flags::Carry).SetType(ValueType::U8);
    const auto no_carry = block->TestZero(carry).SetType(ValueType::U8);
    const auto nonzero = block->CondSet(Cond::NE).SetType(ValueType::U8);
    const auto condition = block->And(no_carry, Operand{nonzero}).SetType(ValueType::U8);
    const auto selected = block->Select(BOOL{condition}, source, source).SetType(ValueType::U32);
    block->StoreUniform(Uniform{16, ValueType::U32}, selected);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    arm64::BlockAnalysisIndex analysis;
    analysis.Build(block.get());
    auto terminal_node = block->GetTerminal();
    const auto consumers = arm64::BuildConsumerPlan(analysis,
            [](const Inst&) { return true; }, [](const Inst&) { return false; }, &terminal_node);
    struct Facts {
        bool fold{};
        const Inst *predicate{}, *combine{}, *other{};
        std::optional<Cond> packed, conditioned;
        const Inst* invert_node{};
        bool suppressed{};
    };
    const auto run = [&](const arm64::ConsumerPlan& plan, bool with_index) {
        arm64::RawCarryBranchAnalysis prepared;
        prepared.Analyze(block.get(), FeatureSet{}, plan, with_index ? &analysis : nullptr);
        Facts facts;
        const auto* fold = prepared.FoldForTest(carry.Def());
        facts.fold = fold != nullptr;
        if (fold) {
            facts.predicate = fold->predicate;
            facts.combine = fold->combine;
            facts.other = fold->other;
            facts.packed = fold->packed_condition;
        }
        facts.conditioned = prepared.ConditionForTest(carry.Def());
        facts.invert_node = prepared.InvertForTest(carry.Def());
        facts.suppressed = prepared.SuppressesInvert(invert);
        return facts;
    };
    const auto indexed = run(consumers, true);
    REQUIRE(indexed.fold);
    REQUIRE(indexed.predicate == no_carry.Def());
    REQUIRE(indexed.combine == condition.Def());
    REQUIRE(indexed.other == nonzero.Def());
    REQUIRE(indexed.packed == Cond::EQ);
    REQUIRE(indexed.conditioned == Cond::HI);
    REQUIRE(indexed.invert_node == invert);
    REQUIRE(indexed.suppressed);
    const auto scanned = run(consumers, false);
    REQUIRE(scanned.fold == indexed.fold);
    REQUIRE(scanned.predicate == indexed.predicate);
    REQUIRE(scanned.combine == indexed.combine);
    REQUIRE(scanned.other == indexed.other);
    REQUIRE(scanned.packed == indexed.packed);
    REQUIRE(scanned.conditioned == indexed.conditioned);
    REQUIRE(scanned.invert_node == indexed.invert_node);
    REQUIRE(scanned.suppressed == indexed.suppressed);
    arm64::ConsumerPlan invalid;
    invalid.valid = false;
    REQUIRE_FALSE(run(invalid, true).fold);
    arm64::ConsumerPlan ungated;
    REQUIRE_FALSE(run(ungated, true).fold);
    arm64::ConsumerPlan fabricated;
    fabricated.local_conditions.insert(condition.Def());
    const auto via_fabricated = run(fabricated, false);
    REQUIRE(via_fabricated.fold);
    REQUIRE(via_fabricated.conditioned == indexed.conditioned);
    REQUIRE(via_fabricated.invert_node == invert);
    IntrusivePtr<Block> plain{new Block(0, Location{0xdd34})};
    plain->LoadUniform(Uniform{0, ValueType::U32});
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    arm64::RawCarryBranchAnalysis empty;
    empty.Analyze(plain.get(), FeatureSet{}, consumers, nullptr);
    REQUIRE(empty.Empty());
}
