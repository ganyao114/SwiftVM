#include <catch2/catch_test_macros.hpp>
#include <array>
#include "runtime/backend/arm64/alloc/narrow_bit_plan.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("narrow masks reject nodes owned by earlier lowering", "[lowering-coverage]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xc200})};
    const auto source = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
    const auto extract = block->BitExtract(source, Imm{0u}, Imm{8u}).SetType(ValueType::U8);
    const auto mask = block->And(extract, Operand{Imm{7u}}).SetType(ValueType::U8);
    block->StoreUniform(Uniform{4, ValueType::U8}, mask);
    block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.MapRegister(source.Id(), HostGPR{14});
    allocation.MapRegister(extract.Id(), HostGPR{15});
    allocation.MapRegister(mask.Id(), HostGPR{16});
    arm64::NarrowBitInputs inputs;
    REQUIRE(arm64::BuildNarrowBitPlan(block.get(), allocation, inputs).masks.contains(mask.Def()));
    BitVector earlier(block->MaxInstrId());
    inputs.earlier_coverage = &earlier;
    for (auto* node : {extract.Def(), mask.Def()}) {
        earlier.reset(); earlier.set(node->Id());
        const auto plan = arm64::BuildNarrowBitPlan(block.get(), allocation, inputs);
        REQUIRE(plan.masks.empty());
        REQUIRE(plan.masked_extracts.empty());
    }
}

TEST_CASE("tied zero extensions claim W-form ALU producers", "[lowering-coverage]") {
    const auto build = [](bool tied) {
        IntrusivePtr<Block> block{new Block(0, Location{0xc204})};
        const auto left = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
        const auto right = block->LoadUniform(Uniform{4, ValueType::U32}).SetType(ValueType::U32);
        const auto sum = block->Add(left, Operand{right}).SetType(ValueType::U32);
        const auto ext = block->ZeroExtend32To64(sum).SetType(ValueType::U64);
        const auto shift = block->LsrImm(ext, Imm{3u}).SetType(ValueType::U64);
        const auto ext2 = block->ZeroExtend32To64(
                block->BitExtract(shift, Imm{0u}, Imm{32u}).SetType(ValueType::U32))
                .SetType(ValueType::U64);
        block->StoreUniform(Uniform{8, ValueType::U64}, ext2);
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(left.Id(), HostGPR{14});
        allocation.MapRegister(right.Id(), HostGPR{15});
        allocation.MapRegister(sum.Id(), HostGPR{16});
        allocation.MapRegister(ext.Id(), HostGPR{static_cast<uint16_t>(tied ? 16 : 17)});
        allocation.MapRegister(shift.Id(), HostGPR{18});
        auto plan = arm64::BuildNarrowBitPlan(block.get(), allocation, {});
        return std::tuple{std::move(block), std::move(plan), ext};
    };
    const auto tied_case = build(true);
    REQUIRE(std::get<1>(tied_case).tied_load_extensions.contains(std::get<2>(tied_case).Def()));
    const auto untied_case = build(false);
    REQUIRE(!std::get<1>(untied_case).tied_load_extensions.contains(std::get<2>(untied_case).Def()));
    // Immediate and select producers write the W form for 32-bit results
    // too, so tied extensions over them are claimed the same way.
    IntrusivePtr<Block> block{new Block(0, Location{0xc205})};
    const auto imm = block->LoadImm(Imm{uint64_t{5}}).SetType(ValueType::U32);
    const auto imm_ext = block->ZeroExtend32To64(imm).SetType(ValueType::U64);
    const auto sel = block->CondSelect(Cond::EQ, imm, imm).SetType(ValueType::U32);
    const auto sel_ext = block->ZeroExtend32To64(sel).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, imm_ext);
    block->StoreUniform(Uniform{8, ValueType::U64}, sel_ext);
    block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.MapRegister(imm.Id(), HostGPR{14});
    allocation.MapRegister(imm_ext.Id(), HostGPR{14});
    allocation.MapRegister(sel.Id(), HostGPR{15});
    allocation.MapRegister(sel_ext.Id(), HostGPR{15});
    const auto plan = arm64::BuildNarrowBitPlan(block.get(), allocation, {});
    REQUIRE(plan.tied_load_extensions.contains(imm_ext.Def()));
    REQUIRE(plan.tied_load_extensions.contains(sel_ext.Def()));
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
            sources[i] = block->LoadUniform(Uniform{i * 16, ValueType::U32}).SetType(ValueType::U32);
            parts[i] = block->BitExtract(sources[i], Imm{0u}, Imm{bits}).SetType(type);
            consumers[i] = block->And(parts[i], Operand{Imm{imm}}).SetType(type);
            block->StoreUniform(Uniform{i * 16 + 4, type}, consumers[i]);
        };
        const auto make_all = [&](bool swapped) {
            if (swapped) { make_mask(1, ValueType::U16, 16, 0x3ff); make_mask(0, ValueType::U8, 8, 7); }
            else { make_mask(0, ValueType::U8, 8, 7); make_mask(1, ValueType::U16, 16, 0x3ff); }
            sources[2] = block->LoadUniform(Uniform{32, ValueType::U32}).SetType(ValueType::U32);
            parts[2] = block->LsrImm(sources[2], Imm{5u}).SetType(ValueType::U32);
            consumers[2] = block->And(parts[2], Operand{Imm{1u}}).SetType(ValueType::U32);
            block->StoreUniform(Uniform{36, ValueType::U32}, consumers[2]);
        };
        make_all(second_first);
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        for (unsigned i = 0; i < 3; ++i) {
            allocation.MapRegister(sources[i].Id(), HostGPR{static_cast<swift::u16>(14 + i * 3)});
            allocation.MapRegister(parts[i].Id(), HostGPR{static_cast<swift::u16>(15 + i * 3)});
            allocation.MapRegister(consumers[i].Id(), HostGPR{static_cast<swift::u16>(16 + i * 3)});
        }
        const auto plan = arm64::BuildNarrowBitPlan(block.get(), allocation, {});
        Facts facts;
        facts.masks = plan.masks.size();
        facts.shift_masks = plan.shift_masks.size();
        facts.widths = {plan.masks.at(consumers[0].Def()).width, plan.masks.at(consumers[1].Def()).width};
        facts.shift_offset = plan.shift_masks.at(consumers[2].Def()).offset;
        facts.pairs_ok = plan.extensions.empty() && plan.extracts.empty() &&
                plan.masks.at(consumers[0].Def()).extract == parts[0].Def() &&
                plan.masks.at(consumers[0].Def()).source.Def() == sources[0].Def() &&
                plan.masked_extracts.at(parts[0].Def()) == consumers[0].Def() &&
                plan.masked_extracts.at(parts[1].Def()) == consumers[1].Def() &&
                plan.shift_masks.at(consumers[2].Def()).shift == parts[2].Def() &&
                plan.masked_shifts.at(parts[2].Def()) == consumers[2].Def();
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
        const auto source = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
        const auto extract = block->BitExtract(source, Imm{0u}, Imm{8u}).SetType(ValueType::U8);
        if (scenario == 3) block->StoreUniform(Uniform{8, ValueType::U32}, source);
        const auto mask = block->And(extract, Operand{Imm{7u}}).SetType(ValueType::U8);
        block->StoreUniform(Uniform{4, ValueType::U8}, mask);
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(source.Id(), HostGPR{14});
        allocation.MapRegister(extract.Id(), HostGPR{15});
        allocation.MapRegister(mask.Id(), HostGPR{16});
        auto run = [&](const arm64::NarrowBitInputs& inputs) {
            return arm64::BuildNarrowBitPlan(block.get(), allocation, inputs);
        };
        if (scenario == 0) {
            arm64::NarrowBitInputs inputs;
            REQUIRE(run(inputs).masks.contains(mask.Def()));
            inputs.blocked_extracts.insert(extract.Def());
            const auto plan = run(inputs);
            REQUIRE(plan.masks.empty());
            REQUIRE(plan.masked_extracts.empty());
        } else if (scenario == 1) {
            allocation.MapMemSpill(source.Id(), SpillSlot{2});
            REQUIRE(run({}).masks.empty());
            allocation.MapRegister(source.Id(), HostGPR{14});
            REQUIRE(run({}).masks.contains(mask.Def()));
        } else if (scenario == 2) {
            REQUIRE(run({}).masks.contains(mask.Def()));
        } else if (scenario == 3) {
            REQUIRE(run({}).masks.empty());
        } else {
            IntrusivePtr<Block> shifted{new Block(0, Location{0xc238})};
            const auto shift_source = shifted->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
            const auto shift = shifted->LsrImm(shift_source, Imm{5u}).SetType(ValueType::U32);
            const auto consumer = shifted->And(shift, Operand{Imm{1u}}).SetType(ValueType::U32);
            shifted->StoreUniform(Uniform{4, ValueType::U32}, consumer);
            shifted->ReIdInstr();
            RegAlloc shift_allocation{shifted->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
            shift_allocation.MapRegister(shift_source.Id(), HostGPR{14});
            shift_allocation.MapRegister(shift.Id(), HostGPR{15});
            shift_allocation.MapRegister(consumer.Id(), HostGPR{16});
            const auto run_shift = [&](const arm64::NarrowBitInputs& inputs) {
                return arm64::BuildNarrowBitPlan(shifted.get(), shift_allocation, inputs);
            };
            REQUIRE(run_shift({}).shift_masks.contains(consumer.Def()));
            arm64::NarrowBitInputs flagged_consumer; flagged_consumer.flagged.insert(consumer.Def());
            REQUIRE(run_shift(flagged_consumer).shift_masks.empty());
            arm64::NarrowBitInputs flagged_shift; flagged_shift.flagged.insert(shift.Def());
            REQUIRE(run_shift(flagged_shift).shift_masks.empty());
            arm64::NarrowBitInputs discarded; discarded.discarded.insert(shift.Def());
            REQUIRE(run_shift(discarded).shift_masks.empty());
        }
    }
    for (bool separated : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0xc240})};
        const auto source = block->LoadUniform(Uniform{0, ValueType::U16}).SetType(ValueType::U16);
        const auto extract = block->BitExtract(source, Imm{0u}, Imm{16u}).SetType(ValueType::U16);
        if (separated) block->StoreUniform(Uniform{8, ValueType::U16}, source);
        const auto wrapper = block->ZeroExtend32(extract).SetType(ValueType::U32);
        const auto shift = block->LsrImm(wrapper, Imm{4u}).SetType(ValueType::U32);
        block->StoreUniform(Uniform{12, ValueType::U32}, shift);
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(source.Id(), HostGPR{14});
        allocation.MapRegister(extract.Id(), HostGPR{15});
        allocation.MapRegister(wrapper.Id(), HostGPR{16});
        allocation.MapRegister(shift.Id(), HostGPR{17});
        const auto plan = arm64::BuildNarrowBitPlan(block.get(), allocation, {});
        REQUIRE(plan.extensions.contains(wrapper.Def()) == !separated);
        if (!separated) {
            const auto& action = plan.extensions.at(wrapper.Def());
            REQUIRE(action.extract == extract.Def());
            REQUIRE(action.shift == shift.Def());
            REQUIRE(action.width == 2);
            REQUIRE(action.source_high_zero);
            REQUIRE(plan.extracts.at(extract.Def()) == wrapper.Def());
            REQUIRE(plan.extract_shifts.at(shift.Def()) == wrapper.Def());
        }
    }
}
