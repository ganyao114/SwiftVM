#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/alloc/pseudo_flags_plan.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"

using namespace swift::runtime;
using namespace swift::runtime::ir;
using namespace swift::runtime::backend::arm64;

namespace {
PseudoFlagsDemand Legacy(Block& block, Inst* producer) {
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
}

TEST_CASE("static pseudo flags preserve merged masks views and branch markers", "[pseudo-flags-plan]") {
    for (unsigned count : {0u, 1u, 2u, 4u, 8u, 256u}) for (bool branch_only : {false, true}) {
        CAPTURE(count, branch_only);
        IntrusivePtr<Block> block{new Block(0, Location{0xd800})};
        const auto value = block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        const auto producer = block->Add(value, ir::Operand{value}).SetType(ValueType::U64);
        constexpr Flags masks[] = {Flags::Carry, Flags::NZ, Flags::Overflow, Flags::Parity, Flags::AuxiliaryCarry, Flags::Positive};
        for (unsigned i = 0; i < count; ++i)
            block->SaveFlags(Value{producer}.SetCastType(ValueType::U8), masks[i % 6]);
        // Non-flag pseudo users coexist with SaveFlags on the same producer;
        // a pseudo never owns pseudos of its own.
        const auto exposed = block->GetFlags(producer, Flags::All).SetType(ValueType::U64);
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
    IntrusivePtr<Block> block{new Block(0, Location{0xd808})};
    const auto value = block->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
    const auto producer = block->Add(value, ir::Operand{value}).SetType(ValueType::U64);
    block->SaveFlags(producer, Flags::All);
    unsigned id = 200;
    for (auto& inst : block->GetInstList()) { inst.SetId(id); id += 3; }
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

