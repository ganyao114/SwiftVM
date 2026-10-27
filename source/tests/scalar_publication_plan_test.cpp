#include "runtime/compile/workspace.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/lower_consumers.h"
#include "runtime/backend/arm64/compile/lower_flags.h"
#include "runtime/backend/arm64/compile/lower_scalar.h"
#include "runtime/backend/arm64/compile/vector_preparation.h"
#include "runtime/compile/analysis/block_index.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

namespace {
arm64::ScalarFPRActions PrepareResident(const Block* block, compile::BlockAnalysisIndex& index) {
    compile::Allocation locations{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    compile::CompileWorkspace workspace;
    return arm64::PrepareVector(block, locations.View(), index, workspace).resident;
}
}  // namespace

TEST_CASE("vector preparation requests only matching owner and conversion facts",
          "[scalar-publication-plan]") {
    for (unsigned scenario = 0; scenario < 17; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xdc80})};
        IntrusivePtr<Block> external{new Block(1, Location{0xdca0})};
        // Keep resolved-use readiness distinct from direct-use readiness.
        const auto unrelated = block->LoadUniform(ValueType::U64, Uniform{80, ValueType::U64});
        (void)block->BitCast(ValueType::U32, unrelated);
        Value owner, loaded, converted;
        const Inst* low{};
        const Inst* store{};
        if (scenario == 0)
            block->StoreUniform(Uniform{88, ValueType::U64}, unrelated);
        else if (scenario == 1) {
            const auto vector = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
            block->StoreUniform(Uniform{32, ValueType::V128}, vector);
        } else if ((scenario >= 2 && scenario <= 8) || scenario >= 14) {
            if (scenario >= 5 && scenario <= 8)
                owner = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
            const auto zero = block->LoadImm(ValueType::U64, Imm{swift::u64{0}});
            loaded = scenario == 4
                             ? block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64})
                             : block->LoadMemory(scenario == 15 ? ValueType::U32 : ValueType::U64,
                                                 Operand{Imm{swift::u64{0x1000}}});
            auto published = loaded;
            if (scenario == 2 || scenario == 3) published = published.SetCastType(ValueType::U32);
            if (scenario == 15) {
                published = published.SetCastType(ValueType::U16);
                published = block->ZeroExtend64(ValueType::U64, published);
            }
            if (scenario == 8) block->UniformBarrier();
            block->SetHostFPR(published, HostRegIndex(17), Imm{0u});
            low = &block->GetInstList().back();
            auto high = zero;
            if (scenario == 14) high = high.SetCastType(ValueType::U32);
            block->SetHostFPR(high, HostRegIndex(17), Imm{8u});
            if (scenario == 16) {
                owner = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
                external->StoreUniform(Uniform{32, ValueType::V128}, owner);
            }
            if (scenario == 3) {
                const auto ordinary =
                        block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
                block->SetHostFPR(ordinary, HostRegIndex(18), Imm{0u});
                block->SetHostFPR(zero, HostRegIndex(18), Imm{8u});
            }
            if (owner.Defined() && scenario != 7 && scenario != 16)
                block->StoreUniform(Uniform{32, ValueType::V128}, owner);
        } else if (scenario == 9) {
            const auto value = block->GetHostFPR(ValueType::U64, HostRegIndex(17), Imm{0u});
            block->StoreMemory(Operand{Imm{swift::u64{0x1000}}}, value);
            store = &block->GetInstList().back();
        } else if (scenario == 10) {
            const auto vector = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
            const auto extracted = block->VecExtract64(ValueType::U64, vector, Imm{0u});
            block->SetHostFPR(vector, HostRegIndex(17), Imm{0u});
            block->StoreMemory(Operand{Imm{swift::u64{0x1000}}}, extracted);
            store = &block->GetInstList().back();
        } else {
            const auto current = block->GetHostFPR(ValueType::V128, HostRegIndex(17), Imm{0u});
            const auto zero = block->VecXor(ValueType::V128, current, current);
            block->SetHostFPR(zero, HostRegIndex(17), Imm{0u});
            const auto source = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
            converted = block->VecFCvtIntToFloat(ValueType::U64, source, Imm{32u}, Imm{64u});
            block->SetHostFPR(converted, HostRegIndex(17), Imm{0u});
            if (scenario >= 12) {
                if (scenario == 13) block->UniformBarrier();
                block->StoreMemory(Operand{Imm{swift::u64{0x1000}}}, converted);
                store = &block->GetInstList().back();
            }
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        if (owner.Defined())
            allocation.MapRegister(owner.Id(),
                                   HostFPR{static_cast<swift::u16>(scenario == 5 ? 2 : 17)});
        auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
        REQUIRE_FALSE(index.UsesReady());
        REQUIRE_FALSE(index.ResolvedUsesReady());
        const bool low64 = std::any_of(
                block->GetInstList().begin(), block->GetInstList().end(), [](const Inst& inst) {
                    if (inst.GetOp() != OpCode::SetHostFPR || inst.GetArg<Imm>(2).Get() != 0)
                        return false;
                    const auto value = inst.GetArg<Value>(0);
                    return value.Defined() && !IsFloatValueType(value.Type()) &&
                           GetValueSizeByte(value.Type()) == 8;
                });
        REQUIRE(index.Summary().HasLow64FPRWrites() == low64);
        // An empty family must not borrow or clear another active user's buffer.
        if (scenario <= 2 || scenario == 14) {
            compile::LoweringCandidateScope held{allocation.Workspace()};
            held.Entries().push_back({7, 0xffff});
            const auto empty =
                    arm64::PrepareVector(block.get(), allocation, index, allocation.Workspace());
            REQUIRE(empty.publications.actions.empty());
            REQUIRE(empty.resident.actions.empty());
            REQUIRE(held.Entries().size() == 1);
            REQUIRE(held.Entries().front().position == 7);
            REQUIRE(held.Entries().front().roles == 0xffff);
        }
        const auto result =
                arm64::PrepareVector(block.get(), allocation, index, allocation.Workspace());
        const bool resolved = scenario == 6 || scenario == 7 || scenario == 16;
        REQUIRE(index.UsesReady() == (resolved || scenario == 12 || scenario == 13));
        REQUIRE(index.ResolvedUsesReady() == resolved);
        const bool publication = (scenario >= 3 && scenario <= 8) || scenario >= 15;
        REQUIRE(result.publications.actions.size() == (publication ? 1u : 0u));
        if (publication) {
            const auto& action = result.publications.actions.front();
            const bool fused = scenario == 5 || scenario == 7;
            REQUIRE(action.kind ==
                    (fused ? arm64::ScalarFPRAction::Load64 : arm64::ScalarFPRAction::Value));
            REQUIRE(action.target == (scenario == 3 ? 18 : 17));
            if (scenario != 3) REQUIRE(action.instruction == (fused ? loaded.Id() : low->Id()));
        }
        REQUIRE(result.resident.actions.size() == (scenario == 12                    ? 2u
                                                   : scenario >= 9 && scenario <= 13 ? 1u
                                                                                     : 0u));
        if (scenario == 9 || scenario == 10 || scenario == 12) {
            REQUIRE(result.resident.Find(store->Id()));
            REQUIRE(result.resident.Find(store->Id())->kind == arm64::ScalarFPRAction::Store);
        }
        if (converted.Defined()) {
            REQUIRE(result.resident.Find(converted.Id()));
            REQUIRE(result.resident.Find(converted.Id())->kind ==
                    (scenario == 13 ? arm64::ScalarFPRAction::ConversionExport
                                    : arm64::ScalarFPRAction::Conversion));
        }
        const auto repeated =
                arm64::PrepareVector(block.get(), allocation, index, allocation.Workspace());
        REQUIRE(repeated.publications.actions == result.publications.actions);
        REQUIRE(repeated.publications.covered == result.publications.covered);
        REQUIRE(repeated.resident.actions == result.resident.actions);
        REQUIRE(repeated.resident.covered == result.resident.covered);
    }
}

TEST_CASE("consumer and vector selection reuse one exclusive candidate workspace",
          "[scalar-preparation]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xdc90})};
    const auto condition = block->CondSet(ValueType::U8, Cond::EQ);
    const auto branch = block->Goto(condition);
    block->BindLabel(branch);
    const auto current = block->GetHostFPR(ValueType::V128, HostRegIndex(17), Imm{0u});
    const auto zero = block->VecXor(ValueType::V128, current, current);
    block->SetHostFPR(zero, HostRegIndex(17), Imm{0u});
    const auto source = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    const auto converted = block->VecFCvtIntToFloat(ValueType::U64, source, Imm{32u}, Imm{64u});
    block->SetHostFPR(converted, HostRegIndex(17), Imm{0u});
    block->StoreMemory(Operand{Imm{swift::u64{0x1000}}}, converted);
    const auto* store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    auto& index = allocation.PrepareFacts(block.get()).PrepareUses(block.get());
    auto terminal_node = block->GetTerminal();
    const auto consumers = [&] {
        return arm64::LowerConsumers(
                index,
                allocation.Workspace(),
                [](const Inst&) { return true; },
                [](const Inst&) { return false; },
                &terminal_node);
    };
    const auto before = consumers();
    REQUIRE(before.valid);
    REQUIRE(before.Has(condition.Id(), arm64::ConsumerFlag::LocalCondition));
    const auto vector =
            arm64::PrepareVector(block.get(), allocation, index, allocation.Workspace());
    REQUIRE(vector.resident.Find(converted.Id()));
    REQUIRE(vector.resident.Find(store->Id()));
    const auto after = consumers();
    REQUIRE(after.valid);
    REQUIRE(after.actions == before.actions);
    REQUIRE(after.audit == before.audit);
    const auto again = arm64::PrepareVector(block.get(), allocation, index, allocation.Workspace());
    REQUIRE(again.publications.actions == vector.publications.actions);
    REQUIRE(again.publications.covered == vector.publications.covered);
    REQUIRE(again.resident.actions == vector.resident.actions);
    REQUIRE(again.resident.covered == vector.resident.covered);
}

TEST_CASE("scalar publication plans keep barriers live lanes and shape rejection", "[scalar-publication-plan]") {
    for (unsigned scenario = 0; scenario < 9; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xdd00})};
        Value live;
        if (scenario == 4) live = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
        const auto zero = block->LoadImm(scenario == 6 ? ValueType::U32 : ValueType::U64,
                                         Imm{swift::u64{scenario == 5 ? 1u : 0u}});
        const auto loaded = block->LoadMemory(ValueType::U64, Operand{Imm{swift::u64{0x1000}}});
        if (scenario == 1) (void)block->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{1}}});
        if (scenario == 2) (void)block->GetHostFPR(ValueType::V128, HostRegIndex(17), Imm{0u});
        if (scenario == 3) block->StoreUniform(Uniform{0, ValueType::U64}, loaded);
        const unsigned target = scenario == 7 ? 15 : 17;
        block->SetHostFPR(loaded, HostRegIndex(target), Imm{0u});
        if (scenario == 8) (void)block->LoadImm(Imm{swift::u64{7}});
        block->SetHostFPR(zero, HostRegIndex(target), Imm{8u});
        if (live.Defined()) block->StoreUniform(Uniform{16, ValueType::V128}, live);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        if (live.Defined()) allocation.MapRegister(live.Id(), HostFPR{17});
        compile::BlockAnalysisIndex index;
        index.Build(block.get());
        const auto shared =
                arm64::PrepareVector(block.get(), allocation, index, allocation.Workspace())
                        .publications;
        compile::BlockAnalysisIndex separate;
        separate.Build(block.get());
        const auto independent =
                arm64::PrepareVector(block.get(), allocation, separate, allocation.Workspace())
                        .publications;
        REQUIRE(std::ranges::count_if(shared.actions, [](const auto& action) {
                    return action.kind != arm64::ScalarFPRAction::Value;
                }) == (scenario == 0 ? 1 : 0));
        REQUIRE(std::ranges::count_if(shared.actions, [](const auto& action) {
                    return action.kind == arm64::ScalarFPRAction::Value;
                }) == (scenario >= 1 && scenario <= 4 ? 1 : 0));
        REQUIRE(shared.actions == independent.actions);
        REQUIRE(shared.covered == independent.covered);
        if (!shared.actions.empty()) REQUIRE(shared.actions.front().target == 17);
    }
}

TEST_CASE("scalar FPR actions preserve width coverage and allocation identity",
          "[scalar-publication-plan]") {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xdd08})};
        const auto zero = block->LoadImm(ValueType::U64, Imm{swift::u64{0}});
        const auto loaded = block->LoadMemory(scenario == 1 ? ValueType::U32 : ValueType::U64,
                                              Operand{Imm{swift::u64{0x1000}}});
        const auto extension =
                scenario == 1 ? block->ZeroExtend64(ValueType::U64, loaded) : Value{};
        if (scenario == 2) (void)block->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{1}}});
        block->SetHostFPR(extension.Defined() ? extension : loaded, HostRegIndex(17), Imm{0u});
        const auto* low = &block->GetInstList().back();
        block->SetHostFPR(zero, HostRegIndex(17), Imm{8u});
        const auto* high = &block->GetInstList().back();
        if (scenario == 3) block->StoreUniform(Uniform{0, ValueType::U64}, zero);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        auto& index = allocation.PrepareFacts(block.get()).PrepareUses(block.get());
        const auto lowered =
                arm64::PrepareVector(block.get(), allocation, index, allocation.Workspace())
                        .publications;
        allocation.BeginTargetPlanning();
        allocation.StoreScalarFPR(lowered);
        const auto& actions = allocation.Actions();
        if (scenario == 2) {
            REQUIRE_FALSE(actions.ScalarFPRLoad(loaded.Id()));
            REQUIRE(actions.ScalarFPRValue(low->Id()));
            REQUIRE(actions.ScalarFPRValue(low->Id())->target == 17);
        } else {
            const auto* load = actions.ScalarFPRLoad(loaded.Id());
            REQUIRE(load);
            REQUIRE(load->target == 17);
            REQUIRE(load->kind == (scenario == 1 ? arm64::ScalarFPRAction::Load32
                                                 : arm64::ScalarFPRAction::Load64));
            REQUIRE_FALSE(actions.ScalarFPRValue(low->Id()));
        }
        REQUIRE_FALSE(actions.InstructionCovered(loaded.Id()));
        REQUIRE(actions.InstructionCovered(low->Id()) == (scenario != 2));
        REQUIRE(actions.InstructionCovered(high->Id()));
        REQUIRE(actions.InstructionCovered(zero.Id()) == (scenario != 3));
        if (extension.Defined()) REQUIRE(actions.InstructionCovered(extension.Id()));
        allocation.EndTargetPlanning();
        REQUIRE(actions.Ready());
        REQUIRE(actions.InstructionCovered(high->Id()));
        REQUIRE((actions.ScalarFPRLoad(loaded.Id()) != nullptr) == (scenario != 2));
        REQUIRE((actions.ScalarFPRValue(low->Id()) != nullptr) == (scenario == 2));
        allocation.ResetAllocations();
        REQUIRE_FALSE(actions.InstructionCovered(high->Id()));
        REQUIRE_FALSE(actions.ScalarFPRLoad(loaded.Id()));
        REQUIRE_FALSE(actions.ScalarFPRValue(low->Id()));
    }
}

TEST_CASE("scalar copy analysis is order-independent and replaceable", "[scalar-preparation]") {
    struct Facts {
        bool a{}, a_left{}, a_right{}, b{}, b_discarded{}, c{}, empty{};
    };
    const auto build = [](bool second_first) {
        IntrusivePtr<Block> block{new Block(0, Location{0xdd10})};
        const auto source = block->GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
        Value x_a, l_a, r_a, x_b, v_b, x_c;
        const auto make_a = [&] {
            l_a = block->BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
            r_a = block->BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
            x_a = block->Xor(ValueType::U32, l_a, Operand{r_a});
            block->StoreUniform(Uniform{0, ValueType::U32}, x_a);
        };
        const auto make_b = [&] {
            v_b = block->LoadUniform(ValueType::U32, Uniform{64, ValueType::U32});
            x_b = block->Xor(ValueType::U32, v_b, Operand{v_b});
            block->StoreUniform(Uniform{4, ValueType::U32}, x_b);
            const auto other = block->LoadUniform(ValueType::U32, Uniform{68, ValueType::U32});
            x_c = block->Xor(ValueType::U32, v_b, Operand{other});
            block->StoreUniform(Uniform{8, ValueType::U32}, x_c);
        };
        if (second_first) {
            make_b();
            make_a();
        } else {
            make_a();
            make_b();
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::BlockAnalysisIndex index;
        index.Build(block.get());
        auto analysis = arm64::LowerScalarCopies(index);
        Facts facts{analysis.Has(x_a.Id(), arm64::ScalarFlag::SelfXor),
                    analysis.Has(l_a.Id(), arm64::ScalarFlag::CopyInputDiscarded),
                    analysis.Has(r_a.Id(), arm64::ScalarFlag::CopyInputDiscarded),
                    analysis.Has(x_b.Id(), arm64::ScalarFlag::SelfXor),
                    analysis.Has(v_b.Id(), arm64::ScalarFlag::CopyInputDiscarded),
                    analysis.Has(x_c.Id(), arm64::ScalarFlag::SelfXor),
                    analysis.flags.empty()};
        analysis = arm64::LowerScalarCopies(index);
        REQUIRE(analysis.Has(x_a.Id(), arm64::ScalarFlag::SelfXor) == facts.a);
        REQUIRE(analysis.Has(x_b.Id(), arm64::ScalarFlag::SelfXor) == facts.b);
        REQUIRE(analysis.Has(l_a.Id(), arm64::ScalarFlag::CopyInputDiscarded) == facts.a_left);
        IntrusivePtr<Block> plain{new Block(0, Location{0xdd14})};
        plain->LoadUniform(Uniform{0, ValueType::U32});
        plain->SetTerminal(terminal::ReturnToHost{});
        plain->ReIdInstr();
        index.Build(plain.get());
        analysis = arm64::LowerScalarCopies(index);
        REQUIRE(analysis.flags.empty());
        REQUIRE_FALSE(analysis.Has(x_a.Id(), arm64::ScalarFlag::SelfXor));
        REQUIRE_FALSE(analysis.Has(l_a.Id(), arm64::ScalarFlag::CopyInputDiscarded));
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
        auto current = block->GetHostFPR(ValueType::V128, HostRegIndex(17), Imm{0u});
        auto view = block->BitCast(ValueType::V128, current);
        auto zero = block->VecXor(ValueType::V128, current, view);
        block->SetHostFPR(zero, HostRegIndex(17), Imm{0u});
        if (scenario == 1) block->UniformBarrier();
        auto source = block->LoadUniform(Uniform{8, ValueType::U32});
        auto converted = block->VecFCvtIntToFloat(ValueType::U64, source, Imm{32u}, Imm{64u});
        block->SetHostFPR(converted, HostRegIndex(17), Imm{0u});
        auto* publication = &block->GetInstList().back();
        if (scenario == 2) block->StoreUniform(Uniform{16, ValueType::U64}, converted);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        compile::BlockAnalysisIndex index;
        index.Build(block.get());
        auto analysis = PrepareResident(block.get(), index);
        const auto* recipe = analysis.Find(converted.Id());
        REQUIRE((recipe != nullptr) == (scenario != 1));
        if (recipe) {
            REQUIRE(recipe->target == 17);
            REQUIRE(recipe->ExportsResult() == (scenario == 2));
            REQUIRE(analysis.Covers(publication->Id()));
        }
    }
    for (bool overwrite : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0xdd24})};
        auto value = block->GetHostFPR(ValueType::U64, HostRegIndex(18), Imm{0u});
        if (overwrite) {
            auto replacement = block->LoadUniform(Uniform{16, ValueType::U64});
            block->SetHostFPR(replacement, HostRegIndex(18), Imm{0u});
        }
        auto address = block->LoadUniform(Uniform{0, ValueType::U64});
        block->StoreMemory(Operand{address}, value);
        auto* store = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        compile::BlockAnalysisIndex index;
        index.Build(block.get());
        auto analysis = PrepareResident(block.get(), index);
        const auto* action = analysis.Find(store->Id());
        REQUIRE((action != nullptr) == !overwrite);
        if (!overwrite) {
            REQUIRE(action->kind == arm64::ScalarFPRAction::Store);
            REQUIRE(action->target == 18);
            REQUIRE(analysis.Covers(value.Id()));
        }
    }
    {
        IntrusivePtr<Block> block{new Block(0, Location{0xdd28})};
        auto current = block->GetHostFPR(ValueType::V128, HostRegIndex(17), Imm{0u});
        auto view = block->BitCast(ValueType::V128, current);
        auto zero = block->VecXor(ValueType::V128, current, view);
        block->SetHostFPR(zero, HostRegIndex(17), Imm{0u});
        auto source = block->LoadUniform(Uniform{8, ValueType::U32});
        auto converted = block->VecFCvtIntToFloat(ValueType::U64, source, Imm{32u}, Imm{64u});
        block->SetHostFPR(converted, HostRegIndex(17), Imm{0u});
        auto resident = block->GetHostFPR(ValueType::U64, HostRegIndex(18), Imm{0u});
        auto address = block->LoadUniform(Uniform{0, ValueType::U64});
        block->StoreMemory(Operand{address}, resident);
        auto* resident_store = &block->GetInstList().back();
        auto vector = block->LoadUniform(Uniform{32, ValueType::V128});
        auto extracted = block->VecExtract64(ValueType::U64, vector, Imm{0u});
        block->SetHostFPR(vector, HostRegIndex(19), Imm{0u});
        auto address2 = block->LoadUniform(Uniform{24, ValueType::U64});
        block->StoreMemory(Operand{address2}, extracted);
        auto* extract_store = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        compile::BlockAnalysisIndex index;
        index.Build(block.get());
        auto analysis = PrepareResident(block.get(), index);
        const auto snapshot = [&] {
            const auto* recipe = analysis.Find(converted.Id());
            const auto target = [&](const Inst* store) -> std::optional<swift::u16> {
                const auto* action = analysis.Find(store->Id());
                if (!action || action->kind != arm64::ScalarFPRAction::Store) return std::nullopt;
                return action->target;
            };
            return std::tuple{recipe != nullptr,
                              recipe ? recipe->target : swift::u16{},
                              target(resident_store),
                              target(extract_store),
                              analysis.Covers(resident.Id()),
                              analysis.Covers(extracted.Id()),
                              analysis.actions.empty() && analysis.covered.empty()};
        };
        const auto first = snapshot();
        REQUIRE(std::get<0>(first));
        REQUIRE(std::get<1>(first) == 17);
        REQUIRE(std::get<2>(first) == std::optional<swift::u16>{18});
        REQUIRE(std::get<3>(first) == std::optional<swift::u16>{19});
        REQUIRE(std::get<4>(first));
        REQUIRE(std::get<5>(first));
        REQUIRE_FALSE(std::get<6>(first));
        analysis = PrepareResident(block.get(), index);
        REQUIRE(snapshot() == first);
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.BeginTargetPlanning();
        allocation.StoreScalarFPR(analysis);
        const auto& actions = allocation.Actions();
        REQUIRE(actions.ScalarFPRConversion(converted.Id()));
        REQUIRE_FALSE(actions.ScalarFPRLoad(converted.Id()));
        REQUIRE_FALSE(actions.ScalarFPRValue(converted.Id()));
        REQUIRE(actions.ScalarFPRStore(resident_store->Id()));
        REQUIRE_FALSE(actions.ScalarFPRConversion(resident_store->Id()));
        REQUIRE(actions.InstructionCovered(resident.Id()));
        allocation.EndTargetPlanning();
        REQUIRE(actions.Ready());
        REQUIRE(actions.ScalarFPRConversion(converted.Id())->target == 17);
        REQUIRE(actions.ScalarFPRStore(extract_store->Id())->target == 19);
        allocation.ResetAllocations();
        REQUIRE_FALSE(actions.ScalarFPRConversion(converted.Id()));
        REQUIRE_FALSE(actions.ScalarFPRStore(resident_store->Id()));
        REQUIRE_FALSE(actions.InstructionCovered(resident.Id()));
        IntrusivePtr<Block> plain{new Block(0, Location{0xdd2c})};
        plain->LoadUniform(Uniform{0, ValueType::U64});
        plain->SetTerminal(terminal::ReturnToDispatch{});
        plain->ReIdInstr();
        index.Build(plain.get());
        analysis = PrepareResident(plain.get(), index);
        REQUIRE((analysis.actions.empty() && analysis.covered.empty()));
        REQUIRE(analysis.Find(converted.Id()) == nullptr);
        REQUIRE(analysis.Find(resident_store->Id()) == nullptr);
    }
}

TEST_CASE("raw carry analysis is path-independent and consumer-gated", "[scalar-preparation]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xdd30})};
    const auto source = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    const auto comparison = block->Sub(ValueType::U32, source, Operand{Imm{7u}});
    block->SaveFlags(comparison, Flags::Carry | Flags::Zero);
    block->InvertCarry();
    auto* invert = &block->GetInstList().back();
    block->AdvancePC(Imm{1u});
    const auto carry = block->TestFlags(ValueType::U8, Flags::Carry);
    const auto no_carry = block->TestZero(ValueType::U8, carry);
    const auto nonzero = block->CondSet(ValueType::U8, Cond::NE);
    const auto condition = block->And(ValueType::U8, no_carry, Operand{nonzero});
    const auto selected = block->Select(ValueType::U32, BOOL{condition}, source, source);
    block->StoreUniform(Uniform{16, ValueType::U32}, selected);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    compile::BlockAnalysisIndex analysis;
    analysis.Build(block.get());
    auto terminal_node = block->GetTerminal();
    compile::CompileWorkspace workspace;
    const auto consumers = arm64::LowerConsumers(
            analysis,
            workspace,
            [](const Inst&) { return true; },
            [](const Inst&) { return false; },
            &terminal_node);
    const auto run = [&](const arm64::ConsumerActions& plan, bool with_index) {
        compile::BlockAnalysisIndex independent;
        if (!with_index) independent.Build(block.get());
        return arm64::LowerCarryConditions(
                block.get(), with_index ? analysis : independent, FeatureSet{}, plan);
    };
    const auto indexed = run(consumers, true);
    const auto* fold = indexed.Find(carry.Id());
    REQUIRE(fold);
    REQUIRE(fold->instruction == carry.Id());
    REQUIRE(fold->combine == condition.Id());
    REQUIRE(fold->packed_condition == Cond::EQ);
    REQUIRE(fold->RawCondition() == Cond::HI);
    REQUIRE(fold->invert == invert->Id());
    REQUIRE(indexed.actions == run(consumers, false).actions);
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.BeginTargetPlanning();
    allocation.StoreCarry(indexed);
    const auto& actions = allocation.Actions();
    REQUIRE(actions.CarryForTest(carry.Id()));
    REQUIRE(actions.SuppressesCarryInvert(invert->Id()));
    REQUIRE_FALSE(actions.InstructionCovered(invert->Id()));
    allocation.EndTargetPlanning();
    REQUIRE(actions.Ready());
    REQUIRE(actions.CarryForTest(carry.Id())->combine == condition.Id());
    REQUIRE(actions.SuppressesCarryInvert(invert->Id()));
    allocation.ResetAllocations();
    REQUIRE_FALSE(actions.CarryForTest(carry.Id()));
    REQUIRE_FALSE(actions.SuppressesCarryInvert(invert->Id()));
    arm64::ConsumerActions invalid;
    invalid.valid = false;
    REQUIRE(run(invalid, true).actions.empty());
    arm64::ConsumerActions ungated;
    REQUIRE(run(ungated, true).actions.empty());
    arm64::ConsumerActions fabricated;
    arm64::ConsumerAction local{condition.Id()};
    local.Mark(arm64::ConsumerFlag::LocalCondition);
    fabricated.actions.push_back(local);
    REQUIRE(run(fabricated, false).actions == indexed.actions);
    IntrusivePtr<Block> plain{new Block(0, Location{0xdd34})};
    plain->LoadUniform(Uniform{0, ValueType::U32});
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    analysis.Build(plain.get());
    const auto empty = arm64::LowerCarryConditions(plain.get(), analysis, FeatureSet{}, consumers);
    REQUIRE(empty.actions.empty());
}
