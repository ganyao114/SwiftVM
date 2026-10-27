#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/lower_scalar.h"
#include "runtime/ir/block.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("scalar actions preserve operand views and local omission points",
          "[lowering-coverage][scalar-actions]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xc248})};
    const auto source = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    const auto viewed = U32{source};
    const auto extract = block->BitExtract(ValueType::U8, viewed, Imm{0u}, Imm{8u});
    const auto wrapper = block->ZeroExtend32(ValueType::U32, extract);
    const auto shifted = block->LsrImm(ValueType::U32, wrapper, Imm{3u});
    block->StoreUniform(Uniform{8, ValueType::U32}, shifted);
    const auto mask_extract = block->BitExtract(ValueType::U16, viewed, Imm{0u}, Imm{16u});
    const auto masked = block->And(ValueType::U16, mask_extract, Operand{Imm{0x3ffu}});
    block->StoreUniform(Uniform{12, ValueType::U16}, masked);
    const auto bit_shift = block->LsrImm(ValueType::U32, viewed, Imm{7u});
    const auto bit = block->And(ValueType::U32, bit_shift, Operand{Imm{1u}});
    block->StoreUniform(Uniform{16, ValueType::U32}, bit);
    const auto left = block->BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
    const auto right = block->BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
    const auto zero = block->Xor(ValueType::U32, left, Operand{right});
    const auto tied = block->ZeroExtend32To64(ValueType::U64, zero);
    block->StoreUniform(Uniform{24, ValueType::U64}, tied);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    for (const auto& inst : block->GetInstList())
        if (inst.HasValue()) allocation.MapRegister(inst.Id(), HostGPR{14});
    const auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
    const auto copies = arm64::LowerScalarCopies(index);
    const auto bits = arm64::LowerNarrowBits(
            index, allocation.Workspace(), allocation, {.copies = &copies});
    allocation.BeginTargetPlanning();
    allocation.StoreScalar(bits);
    allocation.StoreScalar(copies);
    allocation.EndTargetPlanning();
    const auto& actions = allocation.Actions();
    REQUIRE(actions.Narrow(wrapper.Id())->kind == arm64::NarrowAction::DeferredExtend);
    const auto* shift = actions.Narrow(shifted.Id());
    REQUIRE(shift);
    REQUIRE(shift->kind == arm64::NarrowAction::ExtractShift);
    REQUIRE(shift->Width() == 1);
    REQUIRE(shift->Source(*shifted.Def()).Def() == source.Def());
    REQUIRE(shift->Source(*shifted.Def()).Type() == ValueType::U32);
    REQUIRE(source.Type() == ValueType::U64);
    REQUIRE(actions.Narrow(masked.Id())->Source(*masked.Def()).Type() == ValueType::U32);
    REQUIRE(actions.Narrow(bit.Id())->Source(*bit.Def()).Type() == ValueType::U32);
    REQUIRE(actions.Narrow(bit.Id())->parameter == 7);
    for (const auto value : {extract, mask_extract, bit_shift}) {
        REQUIRE(actions.HasScalar(value.Id(), arm64::ScalarFlag::BitInputCovered));
        REQUIRE_FALSE(actions.InstructionCovered(value.Id()));
    }
    REQUIRE(actions.HasScalar(zero.Id(), arm64::ScalarFlag::SelfXor));
    REQUIRE(actions.HasScalar(left.Id(), arm64::ScalarFlag::CopyInputDiscarded));
    REQUIRE(actions.HasScalar(right.Id(), arm64::ScalarFlag::CopyInputDiscarded));
    REQUIRE(actions.HasScalar(tied.Id(), arm64::ScalarFlag::TiedZeroExtend));
    REQUIRE_FALSE(actions.InstructionCovered(tied.Id()));
    allocation.ResetAllocations();
    REQUIRE_FALSE(actions.Narrow(shifted.Id()));
    REQUIRE_FALSE(actions.HasScalar(zero.Id(), arm64::ScalarFlag::SelfXor));
    REQUIRE_FALSE(actions.HasScalar(tied.Id(), arm64::ScalarFlag::TiedZeroExtend));
}

TEST_CASE("narrow masks reject nodes owned by earlier lowering", "[lowering-coverage]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xc200})};
    const auto source = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    const auto extract = block->BitExtract(ValueType::U8, source, Imm{0u}, Imm{8u});
    const auto mask = block->And(ValueType::U8, extract, Operand{Imm{7u}});
    block->StoreUniform(Uniform{4, ValueType::U8}, mask);
    block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.MapRegister(source.Id(), HostGPR{14});
    allocation.MapRegister(extract.Id(), HostGPR{15});
    allocation.MapRegister(mask.Id(), HostGPR{16});
    const auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
    arm64::ScalarBitInputs inputs;
    REQUIRE(arm64::LowerNarrowBits(index, allocation.Workspace(), allocation, inputs)
                    .Narrow(mask.Id()) != nullptr);
    BitVector earlier(block->MaxInstrId());
    inputs.earlier_coverage = &earlier;
    for (auto* node : {extract.Def(), mask.Def()}) {
        earlier.reset(); earlier.set(node->Id());
        const auto plan = arm64::LowerNarrowBits(index, allocation.Workspace(), allocation, inputs);
        REQUIRE_FALSE(plan.Narrow(mask.Id()));
        REQUIRE_FALSE(plan.Has(extract.Id(), arm64::ScalarFlag::BitInputCovered));
    }
}

TEST_CASE("tied zero extensions claim W-form ALU producers", "[lowering-coverage]") {
    const auto build = [](bool tied) {
        IntrusivePtr<Block> block{new Block(0, Location{0xc204})};
        const auto left = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
        const auto right = block->LoadUniform(ValueType::U32, Uniform{4, ValueType::U32});
        const auto sum = block->Add(ValueType::U32, left, Operand{right});
        const auto ext = block->ZeroExtend32To64(ValueType::U64, sum);
        const auto shift = block->LsrImm(ValueType::U64, ext, Imm{3u});
        const auto ext2 = block->ZeroExtend32To64(
                ValueType::U64, block->BitExtract(ValueType::U32, shift, Imm{0u}, Imm{32u}));
        block->StoreUniform(Uniform{8, ValueType::U64}, ext2);
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(left.Id(), HostGPR{14});
        allocation.MapRegister(right.Id(), HostGPR{15});
        allocation.MapRegister(sum.Id(), HostGPR{16});
        allocation.MapRegister(ext.Id(), HostGPR{static_cast<uint16_t>(tied ? 16 : 17)});
        allocation.MapRegister(shift.Id(), HostGPR{18});
        const auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
        auto plan = arm64::LowerNarrowBits(index, allocation.Workspace(), allocation, {});
        return std::tuple{std::move(block), std::move(plan), ext};
    };
    const auto tied_case = build(true);
    REQUIRE(std::get<1>(tied_case).Has(std::get<2>(tied_case).Id(),
                                       arm64::ScalarFlag::TiedZeroExtend));
    const auto untied_case = build(false);
    REQUIRE(!std::get<1>(untied_case)
                     .Has(std::get<2>(untied_case).Id(), arm64::ScalarFlag::TiedZeroExtend));
    // Immediate and select producers write the W form for 32-bit results
    // too, so tied extensions over them are claimed the same way.
    IntrusivePtr<Block> block{new Block(0, Location{0xc205})};
    const auto imm = block->LoadImm(ValueType::U32, Imm{uint64_t{5}});
    const auto imm_ext = block->ZeroExtend32To64(ValueType::U64, imm);
    const auto sel = block->CondSelect(ValueType::U32, Cond::EQ, imm, imm);
    const auto sel_ext = block->ZeroExtend32To64(ValueType::U64, sel);
    block->StoreUniform(Uniform{0, ValueType::U64}, imm_ext);
    block->StoreUniform(Uniform{8, ValueType::U64}, sel_ext);
    block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.MapRegister(imm.Id(), HostGPR{14});
    allocation.MapRegister(imm_ext.Id(), HostGPR{14});
    allocation.MapRegister(sel.Id(), HostGPR{15});
    allocation.MapRegister(sel_ext.Id(), HostGPR{15});
    const auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
    const auto plan = arm64::LowerNarrowBits(index, allocation.Workspace(), allocation, {});
    REQUIRE(plan.Has(imm_ext.Id(), arm64::ScalarFlag::TiedZeroExtend));
    REQUIRE(plan.Has(sel_ext.Id(), arm64::ScalarFlag::TiedZeroExtend));
}

TEST_CASE("narrow bit plans are order-independent over independent candidates", "[lowering-coverage]") {
    struct Facts {
        size_t masks{}, shift_masks{};
        std::array<swift::u8, 2> widths{};
        swift::u8 shift_offset{};
        bool pairs_ok{};
    };
    const auto build = [](bool second_first) {
        IntrusivePtr<Block> block{new Block(0, Location{0xc228})};
        Value sources[3], parts[3], consumers[3];
        const auto make_mask = [&](unsigned i, ValueType type, unsigned bits, swift::u64 imm) {
            sources[i] = block->LoadUniform(ValueType::U32, Uniform{i * 16, ValueType::U32});
            parts[i] = block->BitExtract(type, sources[i], Imm{0u}, Imm{bits});
            consumers[i] = block->And(type, parts[i], Operand{Imm{imm}});
            block->StoreUniform(Uniform{i * 16 + 4, type}, consumers[i]);
        };
        const auto make_all = [&](bool swapped) {
            if (swapped) { make_mask(1, ValueType::U16, 16, 0x3ff); make_mask(0, ValueType::U8, 8, 7);
            } else {
                make_mask(0, ValueType::U8, 8, 7);
                make_mask(1, ValueType::U16, 16, 0x3ff);
            }
            sources[2] = block->LoadUniform(ValueType::U32, Uniform{32, ValueType::U32});
            parts[2] = block->LsrImm(ValueType::U32, sources[2], Imm{5u});
            consumers[2] = block->And(ValueType::U32, parts[2], Operand{Imm{1u}});
            block->StoreUniform(Uniform{36, ValueType::U32}, consumers[2]);
        };
        make_all(second_first);
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        for (unsigned i = 0; i < 3; ++i) {
            allocation.MapRegister(sources[i].Id(), HostGPR{static_cast<swift::u16>(14 + i * 3)});
            allocation.MapRegister(parts[i].Id(), HostGPR{static_cast<swift::u16>(15 + i * 3)});
            allocation.MapRegister(consumers[i].Id(), HostGPR{static_cast<swift::u16>(16 + i * 3)});
        }
        const auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
        const auto plan = arm64::LowerNarrowBits(index, allocation.Workspace(), allocation, {});
        Facts facts;
        facts.masks = std::ranges::count(
                plan.operations, arm64::NarrowAction::Mask, &arm64::NarrowAction::kind);
        facts.shift_masks = std::ranges::count(
                plan.operations, arm64::NarrowAction::ShiftMask, &arm64::NarrowAction::kind);
        facts.widths = {plan.Narrow(consumers[0].Id())->Width(),
                        plan.Narrow(consumers[1].Id())->Width()};
        facts.shift_offset = plan.Narrow(consumers[2].Id())->parameter;
        facts.pairs_ok = plan.operations.size() == 3 &&
                         plan.Narrow(consumers[0].Id())->Source(*consumers[0].Def()).Def() ==
                                 sources[0].Def() &&
                         plan.Has(parts[0].Id(), arm64::ScalarFlag::BitInputCovered) &&
                         plan.Has(parts[1].Id(), arm64::ScalarFlag::BitInputCovered) &&
                         plan.Narrow(consumers[2].Id())->Source(*consumers[2].Def()).Def() ==
                                 sources[2].Def() &&
                         plan.Has(parts[2].Id(), arm64::ScalarFlag::BitInputCovered);
        return facts;
    };
    const auto forward = build(false);
    REQUIRE(forward.masks == 2);
    REQUIRE(forward.shift_masks == 1);
    REQUIRE(forward.widths[0] == 1);
    REQUIRE(forward.widths[1] == 2);
    REQUIRE(forward.shift_offset == 5);
    REQUIRE(forward.pairs_ok);
    const auto swapped = build(true);
    REQUIRE(swapped.masks == forward.masks);
    REQUIRE(swapped.shift_masks == forward.shift_masks);
    REQUIRE(swapped.widths == forward.widths);
    REQUIRE(swapped.shift_offset == forward.shift_offset);
    REQUIRE(swapped.pairs_ok);
}

TEST_CASE("narrow bit plans honor input gates, adjacency and allocation homes", "[lowering-coverage]") {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xc230})};
        const auto source = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
        const auto extract = block->BitExtract(ValueType::U8, source, Imm{0u}, Imm{8u});
        if (scenario == 3) block->StoreUniform(Uniform{8, ValueType::U32}, source);
        const auto mask = block->And(ValueType::U8, extract, Operand{Imm{7u}});
        block->StoreUniform(Uniform{4, ValueType::U8}, mask);
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(source.Id(), HostGPR{14});
        allocation.MapRegister(extract.Id(), HostGPR{15});
        allocation.MapRegister(mask.Id(), HostGPR{16});
        const auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
        auto run = [&](const arm64::ScalarBitInputs& inputs) {
            return arm64::LowerNarrowBits(index, allocation.Workspace(), allocation, inputs);
        };
        if (scenario == 0) {
            arm64::ScalarBitInputs inputs;
            REQUIRE(run(inputs).Narrow(mask.Id()) != nullptr);
            arm64::ScalarActions copies;
            copies.flags.push_back({extract.Id(), arm64::ScalarFlag::CopyInputDiscarded});
            inputs.copies = &copies;
            const auto plan = run(inputs);
            REQUIRE_FALSE(plan.Narrow(mask.Id()));
            REQUIRE_FALSE(plan.Has(extract.Id(), arm64::ScalarFlag::BitInputCovered));
        } else if (scenario == 1) {
            allocation.MapMemSpill(source.Id(), SpillSlot{2});
            REQUIRE(run({}).Narrow(mask.Id()) == nullptr);
            allocation.MapRegister(source.Id(), HostGPR{14});
            REQUIRE(run({}).Narrow(mask.Id()) != nullptr);
        } else if (scenario == 2) {
            REQUIRE(run({}).Narrow(mask.Id()) != nullptr);
        } else if (scenario == 3) {
            REQUIRE(run({}).Narrow(mask.Id()) == nullptr);
        } else {
            IntrusivePtr<Block> shifted{new Block(0, Location{0xc238})};
            const auto shift_source =
                    shifted->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
            const auto shift = shifted->LsrImm(ValueType::U32, shift_source, Imm{5u});
            const auto consumer = shifted->And(ValueType::U32, shift, Operand{Imm{1u}});
            shifted->StoreUniform(Uniform{4, ValueType::U32}, consumer);
            shifted->ReIdInstr();
            AllocationFixture shift_allocation{
                    shifted->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
            shift_allocation.MapRegister(shift_source.Id(), HostGPR{14});
            shift_allocation.MapRegister(shift.Id(), HostGPR{15});
            shift_allocation.MapRegister(consumer.Id(), HostGPR{16});
            const auto& shift_index =
                    shift_allocation.PrepareFacts(shifted.get()).PrepareIndex(shifted.get());
            const auto run_shift = [&](const arm64::ScalarBitInputs& inputs) {
                return arm64::LowerNarrowBits(
                        shift_index, shift_allocation.Workspace(), shift_allocation, inputs);
            };
            REQUIRE(run_shift({}).Narrow(consumer.Id()) != nullptr);
            const arm64::IntegerBranchAction consumer_branch{.producer = consumer.Id()};
            arm64::ScalarBitInputs flagged_consumer{.branch = &consumer_branch};
            REQUIRE(run_shift(flagged_consumer).Narrow(consumer.Id()) == nullptr);
            const arm64::IntegerBranchAction shift_branch{.producer = shift.Id()};
            arm64::ScalarBitInputs flagged_shift{.branch = &shift_branch};
            REQUIRE(run_shift(flagged_shift).Narrow(consumer.Id()) == nullptr);
            arm64::ScalarActions copies;
            copies.flags.push_back({shift.Id(), arm64::ScalarFlag::CopyInputDiscarded});
            arm64::ScalarBitInputs discarded{.copies = &copies};
            REQUIRE(run_shift(discarded).Narrow(consumer.Id()) == nullptr);
        }
    }
    for (bool separated : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0xc240})};
        const auto source = block->LoadUniform(ValueType::U16, Uniform{0, ValueType::U16});
        const auto extract = block->BitExtract(ValueType::U16, source, Imm{0u}, Imm{16u});
        if (separated) block->StoreUniform(Uniform{8, ValueType::U16}, source);
        const auto wrapper = block->ZeroExtend32(ValueType::U32, extract);
        const auto shift = block->LsrImm(ValueType::U32, wrapper, Imm{4u});
        block->StoreUniform(Uniform{12, ValueType::U32}, shift);
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(source.Id(), HostGPR{14});
        allocation.MapRegister(extract.Id(), HostGPR{15});
        allocation.MapRegister(wrapper.Id(), HostGPR{16});
        allocation.MapRegister(shift.Id(), HostGPR{17});
        const auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
        const auto plan = arm64::LowerNarrowBits(index, allocation.Workspace(), allocation, {});
        REQUIRE((plan.Narrow(wrapper.Id()) != nullptr) == !separated);
        if (!separated) {
            const auto* action = plan.Narrow(wrapper.Id());
            REQUIRE(action->kind == arm64::NarrowAction::DeferredExtend);
            REQUIRE(action->Source(*wrapper.Def()).Def() == source.Def());
            REQUIRE(action->Width() == 2);
            REQUIRE(action->SourceHighZero());
            REQUIRE(plan.Has(extract.Id(), arm64::ScalarFlag::BitInputCovered));
            REQUIRE(plan.Narrow(shift.Id())->kind == arm64::NarrowAction::ExtractShift);
        }
    }
}
