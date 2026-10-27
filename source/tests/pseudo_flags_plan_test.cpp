#include "runtime/compile/analysis/pseudo_flags.h"
#include "runtime/compile/workspace.h"
using swift::runtime::compile::PseudoFlagsDemand;
#include "runtime/compile/analysis/pseudo_flags.h"
using swift::runtime::compile::PseudoFlagsPlan;
#include "runtime/compile/analysis/block_index.h"
using swift::runtime::compile::BlockAnalysisIndex;
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/analysis/pseudo_flags.h"
#include "runtime/compile/passes/passes.h"
#include "support/allocation_fixture.h"

using namespace swift::runtime;
using namespace swift::runtime::ir;
using namespace swift::runtime::backend::arm64;

namespace {
PseudoFlagsDemand Legacy(Block& block, const Inst* producer) {
    PseudoFlagsDemand demand;
    for (auto* pseudo : CollectPseudoOperations(block, producer)) {
        if (pseudo->GetOp() == OpCode::SaveFlags) demand.set |= pseudo->GetArg<Flags>(1);
        else if (pseudo->GetOp() == OpCode::BranchOnlyFlags) {
            demand.set |= pseudo->GetArg<Flags>(1);
            demand.branch_only = true;
        } else if (pseudo->GetOp() == OpCode::ClearFlags) demand.clear |= pseudo->GetArg<Flags>(0);
    }
    return demand;
}
}  // namespace

TEST_CASE("static pseudo flags preserve merged masks views and branch markers", "[pseudo-flags-plan]") {
    for (unsigned count : {0u, 1u, 2u, 4u, 8u, 256u}) for (bool branch_only : {false, true}) {
        CAPTURE(count, branch_only);
        IntrusivePtr<Block> block{new Block(0, Location{0xd800})};
        const auto value = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        const auto producer = block->Add(ValueType::U64, value, ir::Operand{value});
        constexpr Flags masks[] = {Flags::Carry,
                                   Flags::NZ,
                                   Flags::Overflow,
                                   Flags::Parity,
                                   Flags::AuxiliaryCarry,
                                   Flags::Positive};
        for (unsigned i = 0; i < count; ++i)
            block->SaveFlags(Value{producer}.SetCastType(ValueType::U8), masks[i % 6]);
        // Non-flag pseudo users coexist with SaveFlags on the same producer;
        // a pseudo never owns pseudos of its own.
        const auto exposed = block->GetFlags(ValueType::U64, producer, Flags::All);
        block->StoreUniform(Uniform{16, ValueType::U64}, exposed);
        if (branch_only) block->BranchOnlyFlags(producer, Flags::CV);
        block->ClearFlags(Flags::NZCV); // independent write, not a producer pseudo
        block->ReIdInstr();
        const auto uses = producer.Def()->GetUses(false);
        const auto expected = Legacy(*block, producer.Def());
        BlockAnalysisIndex index;
        index.Build(block.get(), nullptr, true);
        const auto& plan = index.PseudoFlags();
        const auto actual = plan.For(producer.Def());
        REQUIRE(actual.set == expected.set);
        REQUIRE(actual.clear == expected.clear);
        REQUIRE(actual.branch_only == expected.branch_only);
        REQUIRE(producer.Def()->GetUses(false) == uses);
        REQUIRE(plan.Empty() == (count == 0 && !branch_only));
        REQUIRE(plan.For(value.Def()).Null());
        REQUIRE(producer.Def()->GetUses() == 0);
        REQUIRE(exposed.Def()->GetUses() == 1);
        REQUIRE_FALSE(exposed.Def()->HasPseudoOperations());
        REQUIRE_FALSE(swift::runtime::backend::ScanFlagsDemand(*block, *exposed.Def()).has_save);
        REQUIRE(CollectPseudoOperations(*block, exposed.Def()).empty());
        bool owned_lists_empty = true;
        for (const auto* pseudo : CollectPseudoOperations(*block, producer.Def())) {
            owned_lists_empty &= !pseudo->HasPseudoOperations() && pseudo->GetUses() == pseudo->GetUses(false);
        }
        REQUIRE(owned_lists_empty);
    }
}

TEST_CASE("static pseudo flags use the block ID span and empty plan fast path", "[pseudo-flags-plan]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    IntrusivePtr<Block> block{new Block(0, Location{0xd808})};
    const auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
    const auto producer = block->Add(ValueType::U64, value, ir::Operand{value});
    block->SaveFlags(producer, Flags::All);
    unsigned id = 200;
    for (const auto& inst : block->GetInstList()) {
        ::swift::runtime::compile::RewriteContext{
                block.get(), compilation_workspace, nullptr, false}
                .Change(block.get(), &inst, [&](Inst& target) { target.SetId(id); });
        id += 3;
    }
    BlockAnalysisIndex index;
    index.Build(block.get(), nullptr, true);
    const auto& plan = index.PseudoFlags();
    REQUIRE(plan.For(producer.Def()).set == Flags::All);
    REQUIRE(plan.For(value.Def()).Null());
    IntrusivePtr<Block> empty{new Block(0, Location{0xd810})};
    index.Build(empty.get());
    REQUIRE(index.PseudoFlags().Empty());
    REQUIRE(PseudoFlagsPlan{}.For(producer.Def()).Null());
}

TEST_CASE("prepared flags own block slices and scoped spill certificates",
          "[pseudo-flags-plan][regression]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    struct Fixture {
        IntrusivePtr<Block> block;
        Value producer;
        const Inst* consumer{};
        compile::BlockSummary summary;
    };
    const auto make = [](swift::u64 location, unsigned first, bool parity) {
        ::swift::runtime::compile::CompileWorkspace compilation_workspace;
        Fixture f{IntrusivePtr<Block>{new Block(0, Location{location})}};
        const auto value = f.block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
        f.producer = f.block->Add(ValueType::U64, value, ir::Operand{value});
        f.block->SaveFlags(f.producer, Flags::CV | (parity ? Flags::Parity : Flags::None));
        f.consumer = &f.block->GetInstList().back();
        f.block->BranchOnlyFlags(f.producer, Flags::Parity);
        for (const auto& inst : f.block->GetInstList()) {
            ::swift::runtime::compile::RewriteContext{
                    f.block.get(), compilation_workspace, nullptr, false}
                    .Change(f.block.get(), &inst, [&](Inst& target) { target.SetId(first); });
            first += 3;
            f.summary.Record(inst);
        }
        return f;
    };
    const auto high = make(0xd818, 200, true);
    const auto low = make(0xd820, 20, false);
    const auto foreign = make(0xd828, 200, true);
    IntrusivePtr<Block> empty{new Block(0, Location{0xd830})};
    swift::runtime::test::AllocationFixture allocation{
            256, backend::GPRSMask{}, backend::FPRSMask{}, FeatureSet{}};
    const auto& actions = allocation.Actions();
    allocation.BeginTargetPlanning();
    // Deliberately append disjoint ID spans in reverse order. Every semantic
    // index is destroyed before the frozen emission result is queried.
    for (const auto* f : {&high, &low}) {
        BlockAnalysisIndex index;
        index.Build(f->block.get(), nullptr, true);
        allocation.DeclareBlock(f->block.get(), f->summary, index.PseudoFlags().View());
    }
    allocation.DeclareBlock(empty.get(), compile::BlockSummary{});
    const FlagsOnlySpillAction spills[] = {{high.consumer->Id(), high.producer.Id()},
                                           {low.consumer->Id(), low.producer.Id()},
                                           {high.consumer->Id(), high.producer.Id()}};
    allocation.StoreFlagsOnlySpills(spills);
    allocation.EndTargetPlanning();
    for (const auto* f : {&high, &low}) {
        const auto flags = actions.PseudoFlagsForBlock(f->block.get());
        REQUIRE(flags.For(f->producer.Def()).set == (Flags::CV | Flags::Parity));
        REQUIRE(flags.For(f->producer.Def()).branch_only);
        REQUIRE(flags.For(nullptr).Null());
        REQUIRE(flags.SavesParity(f->producer.Def()) == (f == &high));
        REQUIRE(actions.DiscardsFlagsOnlySpill(f->consumer, f->producer.Id()));
        REQUIRE_FALSE(actions.DiscardsFlagsOnlySpill(f->consumer, f->producer.Id() + 1));
    }
    REQUIRE(actions.PseudoFlagsForBlock(empty.get()).Empty());
    REQUIRE(actions.PseudoFlagsForBlock(foreign.block.get()).Empty());
    REQUIRE(high.consumer->Id() == foreign.consumer->Id());
    REQUIRE_FALSE(actions.DiscardsFlagsOnlySpill(foreign.consumer, foreign.producer.Id()));
    REQUIRE_FALSE(actions.DiscardsFlagsOnlySpill(nullptr, high.producer.Id()));
    REQUIRE_FALSE(actions.DiscardsFlagsOnlySpill(high.consumer, UINT32_MAX));
    allocation.ResetAllocations();
    REQUIRE(actions.PseudoFlagsForBlock(high.block.get()).Empty());
    REQUIRE_FALSE(actions.DiscardsFlagsOnlySpill(high.consumer, high.producer.Id()));
    allocation.BeginTargetPlanning();
    allocation.DeclareBlock(low.block.get(), low.summary);
    allocation.EndTargetPlanning();
    REQUIRE(actions.PseudoFlagsForBlock(low.block.get()).Empty());
    REQUIRE_FALSE(actions.DiscardsFlagsOnlySpill(low.consumer, low.producer.Id()));
    allocation.ResetAllocations();
    allocation.BeginTargetPlanning();
    allocation.DeclareBlock(high.block.get(), high.summary);
    allocation.StoreFlagsOnlySpills(spills);
    allocation.AbortTargetPlanning();
    REQUIRE(actions.PseudoFlagsForBlock(high.block.get()).Empty());
    REQUIRE_FALSE(actions.DiscardsFlagsOnlySpill(high.consumer, high.producer.Id()));

    IntrusivePtr<Block> edge{new Block(0, Location{0xd838})};
    const auto last = edge->LoadImm(ValueType::U64, Imm{swift::u64{1}});
    ::swift::runtime::compile::RewriteContext{edge.get(), compilation_workspace, nullptr, false}
            .Change(edge.get(), last.Def(), [](Inst& target) { target.SetId(UINT16_MAX); });
    const swift::u16 words[] = {0, 0x8000 | static_cast<swift::u16>(Flags::Parity)};
    const compile::PseudoFlagsView boundary{words, 2, UINT16_MAX - 1};
    REQUIRE(boundary.For(last.Def()).set == Flags::Parity);
    REQUIRE(boundary.SavesParity(last.Def()));
    REQUIRE(boundary.For(nullptr).Null());
}
