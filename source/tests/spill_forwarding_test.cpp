#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/compile/workspace.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/compile/passes/passes.h"
#include "support/register_alloc_test_support.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iterator>
#include <map>
#include <ranges>
#include <string>
#include <utility>
#include <vector>
#include "runtime/backend/runtime.h"

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/actions.h"
#include "runtime/backend/arm64/compile/lower_scalar.h"
#include "runtime/backend/arm64/compile/scratch_preparation.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/frontend/x86/x87.h"

TEST_CASE("scratch MEM transport uses exact occurrences without a consumer graph",
          "[scratch-facts][spill-forward]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    for (unsigned shape = 0; shape < 14; ++shape) {
        CAPTURE(shape);
        IntrusivePtr<Block> block{new Block(0, Location{0x2520})};
        IntrusivePtr<Block> foreign{new Block(0, Location{0x2528})};
        const auto unused = block->LoadImm(ValueType::U64, Imm{swift::u64{3}});
        (void)block->BitCast(ValueType::U32, unused);
        const auto input = block->LoadImm(ValueType::U64, Imm{swift::u64{9}});
        const auto producer = block->Xor(ValueType::U64, input, Operand{input});
        const auto second = block->LoadImm(ValueType::U64, Imm{swift::u64{11}});
        const auto external = foreign->LoadImm(ValueType::U64, Imm{swift::u64{13}});
        block->AdvancePC(Imm{swift::u64{1}});
        const auto* gap = &block->GetInstList().back();
        if (shape == 5) block->SaveFlags(producer, Flags::None);
        if (shape == 6) block->BranchOnlyFlags(producer, Flags::Carry);
        const Inst* consumer{};
        if (shape == 11) {
            block->SaveFlags(producer, Flags::NZCV);
            consumer = &block->GetInstList().back();
        } else if (shape == 0) {
            block->StoreUniform(Uniform{0, ValueType::U64}, producer);
            consumer = &block->GetInstList().back();
        } else {
            Params params;
            const unsigned count = shape == 10 ? 300 : 3;
            for (unsigned i = 0; i < count; ++i) {
                const auto value = (shape == 7 && i == 1) || shape == 9 ? external
                                   : shape == 12 && i % 2               ? second
                                                                        : producer;
                params.Push(Value{value}.SetCastType(i % 2 ? ValueType::U8 : ValueType::U64));
            }
            const auto call = block->CallDynamic(ValueType::VOID, Lambda{shape == 9 ? external : producer}, std::move(params));
            consumer = call.Def();
            if (shape == 13) {
                Params next;
                for (unsigned i = 0; i < count; ++i) next.Push(producer);
                (void)block->CallDynamic(ValueType::VOID, Lambda{producer}, std::move(next));
            }
        }
        if (shape == 2) block->StoreUniform(Uniform{8, ValueType::U64}, producer);
        if (shape == 3) foreign->StoreUniform(Uniform{0, ValueType::U64}, producer);
        if (shape == 4) foreign->SaveFlags(producer, Flags::Parity);
        block->SetTerminal(terminal::ReturnToHost{});
        foreign->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        foreign->ReIdInstr();
        if (shape == 7 || shape == 9) {
            compile::CompileWorkspace editing_workspace;
            compile::RewriteContext editing{foreign.get(), editing_workspace, nullptr, false};
            editing.Change(foreign.get(), external.Def(), [&](Inst& inst) {
                inst.SetId(shape == 7 ? producer.Id() : gap->Id());
            });
        }
        FeatureSet features{};
        features.scratch_precise = true;
        AllocationFixture allocation{
                block->MaxInstrId(), GPRSMask{~((1u << 8) - 1u)}, FPRSMask{}, features};
        for (const auto& inst : block->GetInstList())
            if (inst.HasValue()) allocation.MapRegister(inst.Id(), HostGPR{8});
        if (shape != 8 && shape != 9) allocation.MapMemSpill(producer.Id(), SpillSlot{0});
        if (shape == 9) allocation.MapMemSpill(gap->Id(), SpillSlot{0});
        if (shape == 12) allocation.MapMemSpill(second.Id(), SpillSlot{1});
        auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
        REQUIRE_FALSE(index.UsesReady());
        REQUIRE_FALSE(index.ResolvedUsesReady());
        const auto unit = arm64::PrepareUnitScratch(allocation.GetGprs(), features);
        arm64::ScratchActions lowered;
        if (shape == 8 || shape == 9) {
            compile::InstructionRowScope held{allocation.Workspace(), 1};
            held[0] = 19;
            lowered = arm64::PrepareScratch(
                    block.get(), allocation, features, unit, index, allocation.Workspace());
            REQUIRE(held.Find(0) == 19);
            REQUIRE(lowered.inputs.empty());
        } else
            lowered = arm64::PrepareScratch(
                    block.get(), allocation, features, unit, index, allocation.Workspace());
        const bool pseudo = shape == 4 || shape == 5 || shape == 6 || shape == 11;
        REQUIRE(index.Summary().HasPseudoConsumers() == pseudo);
        REQUIRE(index.UsesReady() == pseudo);
        REQUIRE_FALSE(index.ResolvedUsesReady());

        // The old relationship is expressed independently through reverse
        // edges, including every raw Params/Lambda occurrence and foreign key.
        compile::BlockAnalysisIndex reference;
        reference.Build(block.get(), nullptr, true);
        struct Expected {
            const Inst* definition{};
            bool last{}, seen{}, conflict{};
        };
        std::map<std::pair<swift::u16, swift::u16>, Expected> expected;
        for (const auto* inst : reference.Instructions())
            inst->ForEachValue([&](Value value) {
                if (!value.Defined() || value.Id() >= allocation.MapCount() ||
                    allocation.ValueType(value) != compile::Allocation::MEM)
                    return;
                const Inst* definition = value.Def();
                bool last = false;
                if (!reference.Contains(definition)) {
                    const auto* local = reference.FindInstruction(value.Id());
                    if (!local || !local->HasValue()) return;
                    definition = nullptr;
                } else {
                    const auto uses = reference.Uses(definition);
                    last = uses.size() == 1 && uses.front().consumer == inst &&
                           uses.front().count == definition->GetUses(false);
                }
                auto& row = expected[{inst->Id(), value.Id()}];
                if (row.seen)
                    row.conflict |= row.definition != definition || row.last != last;
                else
                    row = {definition, last, true, false};
            });
        allocation.BeginTargetPlanning();
        allocation.DeclareBlock(block.get(), index.Summary(), index.PseudoFlags().View());
        allocation.StoreScratch(block.get(), lowered);
        allocation.EndTargetPlanning();
        const auto& actions = allocation.Actions();
        for (const auto& [key, row] : expected) {
            const auto actual =
                    actions.SpillInputFor(reference.FindInstruction(key.first), key.second);
            REQUIRE(actual.definition == (row.conflict ? nullptr : row.definition));
            if (actual.definition) REQUIRE(actual.last_use == row.last);
        }
        for (const auto* inst : reference.Instructions()) {
            const auto* facts = actions.ScratchFor(inst);
            REQUIRE(facts);
            REQUIRE(*facts == arm64::PrepareInstructionScratchWithBlock(
                                      *inst, allocation.GetGprs(), features, block.get()));
        }
        if (shape == 7 || shape == 8 || shape == 9)
            REQUIRE(actions.SpillInputFor(consumer, producer.Id()).definition == nullptr);
        if (shape == 10) {
            REQUIRE(producer.Def()->GetUses(false) == 301);
            REQUIRE(lowered.inputs.size() == 1);
            REQUIRE(actions.SpillInputFor(consumer, producer.Id()).last_use);
        }
        const auto repeated = arm64::PrepareScratch(
                block.get(), allocation, features, unit, index, allocation.Workspace());
        REQUIRE(repeated.ranges.size() == lowered.ranges.size());
        REQUIRE(repeated.inputs.size() == lowered.inputs.size());
        for (size_t i = 0; i < lowered.ranges.size(); ++i) {
            REQUIRE(repeated.ranges[i].first == lowered.ranges[i].first);
            REQUIRE(repeated.ranges[i].last == lowered.ranges[i].last);
            REQUIRE(repeated.ranges[i].facts == lowered.ranges[i].facts);
        }
    }
}

TEST_CASE("scratch ranges preserve dense sparse and block boundary lookup", "[scratch-facts]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    for (bool sparse : {false, true}) {
        CAPTURE(sparse);
        IntrusivePtr<Block> block{new Block(0, Location{0x2530})};
        auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{1}});
        for (unsigned i = 0; i < 128; ++i)
            value = block->Add(ValueType::U64, value, Operand{Imm{swift::u64{1}}});
        block->StoreUniform(Uniform{0, ValueType::U64}, value);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        if (sparse) {
            compile::CompileWorkspace editing_workspace;
            compile::RewriteContext editing{block.get(), editing_workspace, nullptr, false};
            for (const auto& inst : block->GetInstList())
                editing.Change(
                        block.get(), &inst, [](Inst& node) { node.SetId(100 + node.Id() * 3); });
        }
        FeatureSet features{};
        features.scratch_precise = true;
        AllocationFixture allocation{1000, GPRSMask{~((1u << 8) - 1u)}, FPRSMask{}, features};
        for (const auto& inst : block->GetInstList())
            if (inst.HasValue()) allocation.MapRegister(inst.Id(), HostGPR{8});
        auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
        const auto lowered =
                arm64::PrepareScratch(block.get(),
                                      allocation,
                                      features,
                                      arm64::PrepareUnitScratch(allocation.GetGprs(), features),
                                      index,
                                      allocation.Workspace());
        REQUIRE_FALSE(index.UsesReady());
        REQUIRE_FALSE(index.ResolvedUsesReady());
        const auto nodes = index.Instructions();
        REQUIRE(lowered.ranges.capacity() <= nodes.size());
        if (sparse)
            REQUIRE(lowered.ranges.size() == nodes.size());
        else
            REQUIRE(lowered.ranges.size() < nodes.size() / 8);
        allocation.BeginTargetPlanning();
        allocation.DeclareBlock(block.get(), index.Summary());
        allocation.StoreScratch(block.get(), lowered);
        allocation.EndTargetPlanning();
        const auto view = allocation.Actions().ScratchForBlock(block.get());
        size_t cursor{};
        const auto check = [&](const Inst* inst) {
            const auto* facts = view.For(inst, cursor);
            REQUIRE(facts == allocation.Actions().ScratchFor(inst));
            REQUIRE(facts);
            REQUIRE(*facts == arm64::PrepareInstructionScratchWithBlock(
                                      *inst, allocation.GetGprs(), features, block.get()));
        };
        for (const auto* inst : nodes) check(inst);
        for (const auto* inst : std::views::reverse(nodes)) check(inst);
        for (size_t i = 0; i < nodes.size(); i += 7) check(nodes[i]);
        REQUIRE(view.For(nullptr, cursor) == nullptr);
        if (sparse) {
            auto incomplete = lowered.ranges;
            const auto absent = incomplete[incomplete.size() / 2].first;
            incomplete.erase(incomplete.begin() + incomplete.size() / 2);
            const arm64::InstructionScratchView missing{incomplete, block.get()};
            REQUIRE(missing.For(index.FindInstruction(absent)) == nullptr);
        }
    }
    // Equal facts at the boundary still belong to two separately owned blocks.
    std::array<IntrusivePtr<Block>, 2> blocks{IntrusivePtr<Block>{new Block(0, Location{0x2540})},
                                              IntrusivePtr<Block>{new Block(1, Location{0x2548})}};
    for (unsigned b = 0; b < 2; ++b) {
        for (unsigned i = 0; i < 32; ++i) blocks[b]->AdvancePC(Imm{swift::u64{1}});
        blocks[b]->SetTerminal(terminal::ReturnToHost{});
        blocks[b]->ReIdInstr();
        if (b) {
            compile::CompileWorkspace editing_workspace;
            compile::RewriteContext editing{blocks[b].get(), editing_workspace, nullptr, false};
            for (const auto& inst : blocks[b]->GetInstList())
                editing.Change(
                        blocks[b].get(), &inst, [](Inst& node) { node.SetId(node.Id() + 32); });
        }
    }
    AllocationFixture allocation{64, GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.BeginTargetPlanning();
    for (const auto& block : blocks) {
        compile::BlockAnalysisIndex index;
        index.Build(block.get());
        allocation.DeclareBlock(block.get(), index.Summary());
        const auto lowered =
                arm64::PrepareScratch(block.get(),
                                      allocation,
                                      FeatureSet{},
                                      arm64::PrepareUnitScratch(allocation.GetGprs(), FeatureSet{}),
                                      index,
                                      allocation.Workspace());
        REQUIRE(lowered.ranges.size() == 1);
        allocation.StoreScratch(block.get(), lowered);
    }
    allocation.EndTargetPlanning();
    for (unsigned b = 0; b < 2; ++b) {
        const auto view = allocation.Actions().ScratchForBlock(blocks[b].get());
        REQUIRE(view.ranges.size() == 1);
        REQUIRE(view.ranges.front().first == b * 32);
        REQUIRE(view.ranges.front().last == b * 32 + 31);
        REQUIRE(view.For(&blocks[1 - b]->GetInstList().front()) == nullptr);
    }
}

TEST_CASE("Div128 uniform proofs stop at control flow joins", "[div128-cfg]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    struct Sample {
        unsigned branch;
        swift::u64 low, divisor, high, quotient, remainder;
        bool native;
        bool sign{};
    };
    const Sample samples[] = {
            {0, 100, 7, 0, 14, 2, true},
            {1, 3, 5, 1, 3689348814741910323ull, 4, false},
            {2, 3, 5, 1, 3689348814741910323ull, 4, false},
            {3, 100, 7, 0, 14, 2, true},
            {0, swift::u64(-100), 7, ~swift::u64{0}, swift::u64(-14), swift::u64(-2), true, true},
            {3, swift::u64(-100), 7, ~swift::u64{0}, swift::u64(-14), swift::u64(-2), true, true},
    };
    for (const auto& sample : samples) {
        INFO("branch=" << sample.branch << " signed=" << sample.sign);
        constexpr swift::u32 low_offset = 512, divisor_offset = 520, high_offset = 528;
        constexpr swift::u32 quotient_offset = 544, remainder_offset = 552;
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 1024;
        config.stack_alignment = 16;
        config.enable_asm_interp = false;
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block(0, Location{0x2500 + sample.branch * 4})};
        const auto low = block->LoadUniform(ValueType::U64, Uniform{low_offset, ValueType::U64});
        const auto divisor =
                block->LoadUniform(ValueType::U64, Uniform{divisor_offset, ValueType::U64});
        auto high = sample.sign ? block->AsrImm(ValueType::U64, low, Imm{swift::u32{63}})
                                : block->LoadImm(ValueType::U64, Imm{swift::u64{0}});
        if (sample.branch == 3) {
            block->StoreUniform(Uniform{high_offset, ValueType::U64}, high);
            high = block->LoadUniform(ValueType::U64, Uniform{high_offset, ValueType::U64});
        } else if (sample.branch != 0) {
            // The taken branch skips the zero store, preserving incoming high=1.
            const auto condition =
                    block->LoadImm(ValueType::U32, Imm{swift::u32{sample.branch == 1 ? 1u : 0u}});
            const auto join =
                    sample.branch == 1 ? block->Goto(condition) : block->NotGoto(condition);
            block->StoreUniform(Uniform{high_offset, ValueType::U64}, high);
            block->BindLabel(join);
            high = block->LoadUniform(ValueType::U64, Uniform{high_offset, ValueType::U64});
        }
        const auto quotient =
                block->Div128(ValueType::U64, high, low, divisor, Imm{swift::u64{sample.sign}});
        const auto remainder = block->Div128Remainder(ValueType::U64, quotient);
        block->StoreUniform(Uniform{quotient_offset, ValueType::U64}, quotient);
        block->StoreUniform(Uniform{remainder_offset, ValueType::U64}, remainder);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        const auto features = FeatureSet{};
        AllocationFixture allocation{block->MaxInstrId(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        const auto plan = allocation.Actions().ForBlock(block.get());
        REQUIRE(plan);
        const auto* scratch = allocation.Actions().ScratchFor(quotient.Def());
        REQUIRE(scratch);
        CHECK(scratch->div128_native == sample.native);
        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        REQUIRE(translator.Translate(block.get()));
        context.Finish();
        auto& masm = context.GetMasm();
        const auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
        const auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        unsigned divisions = 0;
        for (auto* instruction = first; instruction < last; instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction);
            const std::string line = disassembler.GetOutput();
            divisions += line.starts_with("udiv ") || line.starts_with("sdiv ");
        }
        CHECK(divisions == (sample.native ? 1u : 0u));
#if defined(__aarch64__)
        const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID);
        context.Flush(buffer);
        REQUIRE(module->Push(block.get()));
        (void)module->PublishLinkTarget(block->GetStartLocation(), buffer.exec_data, buffer.exec_data);
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        Runtime runtime{&space};
        auto uniform = runtime.GetUniformBuffer();
        std::memcpy(uniform.data() + low_offset, &sample.low, sizeof(sample.low));
        std::memcpy(uniform.data() + divisor_offset, &sample.divisor, sizeof(sample.divisor));
        std::memcpy(uniform.data() + high_offset, &sample.high, sizeof(sample.high));
        runtime.SetLocation(block->GetStartLocation().Value());
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        swift::u64 actual_quotient{}, actual_remainder{};
        std::memcpy(&actual_quotient, uniform.data() + quotient_offset, sizeof(actual_quotient));
        std::memcpy(&actual_remainder, uniform.data() + remainder_offset, sizeof(actual_remainder));
        CHECK(actual_quotient == sample.quotient);
        CHECK(actual_remainder == sample.remainder);
#endif
    }
}

TEST_CASE("Div128 scratch proofs require local uniform membership", "[scratch-facts][div128-cfg]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    for (bool sign : {false, true}) for (bool alias : {false, true}) {
        CAPTURE(sign, alias);
        IntrusivePtr<Block> parent{new Block(0, Location{0x2510})};
        IntrusivePtr<Block> block{new Block(0, Location{0x2514})};
        const auto low = block->LoadUniform(ValueType::U64, Uniform{512, ValueType::U64});
        const auto divisor = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
        const auto source = sign ? block->AsrImm(ValueType::U64, low, Imm{swift::u32{63}})
                                 : block->LoadImm(ValueType::U64, Imm{swift::u64{0}});
        block->StoreUniform(Uniform{528, ValueType::U64}, source);
        auto local = block->LoadUniform(ValueType::U64, Uniform{528, ValueType::U64});
        auto incoming = parent->LoadUniform(ValueType::U64, Uniform{528, ValueType::U64});
        auto* local_load = local.Def();
        auto* incoming_load = incoming.Def();
        if (alias) {
            local = block->BitCast(ValueType::U64, local);
            incoming = block->BitCast(ValueType::U64, incoming);
        }
        const auto local_div =
                block->Div128(ValueType::U64, local, low, divisor, Imm{swift::u64{sign}});
        const auto incoming_div =
                block->Div128(ValueType::U64, incoming, low, divisor, Imm{swift::u64{sign}});
        const auto pure_div =
                block->Div128(ValueType::U64, source, low, divisor, Imm{swift::u64{sign}});
        block->ReIdInstr();
        compile::RewriteContext{parent.get(), compilation_workspace, nullptr, false}.Change(
                parent.get(), incoming_load, [&](Inst& target) { target.SetId(local_load->Id()); });
        compile::SemanticFacts facts{block.get()};
        const auto& index = facts.PrepareIndex(block.get());
        CHECK_FALSE(index.UsesReady());
        CHECK(arm64::SelectNativeDiv128(*local_div.Def(), index));
        CHECK_FALSE(arm64::SelectNativeDiv128(*incoming_div.Def(), index));
        CHECK_FALSE(index.UsesReady());
        const GPRSMask pool{};
        const FeatureSet features{};
        CHECK_FALSE(arm64::PrepareInstructionScratch(*local_div.Def(), pool, features).div128_native);
        CHECK_FALSE(arm64::PrepareInstructionScratch(*incoming_div.Def(), pool, features).div128_native);
        CHECK(arm64::PrepareInstructionScratch(*pure_div.Def(), pool, features).div128_native);
        CHECK(arm64::PrepareInstructionScratchWithBlock(*local_div.Def(), pool, features, block.get()).div128_native);
        CHECK_FALSE(arm64::PrepareInstructionScratchWithBlock(*incoming_div.Def(), pool, features, block.get()).div128_native);
    }
}

TEST_CASE("Div128 uniform scratch proofs stop at partial stores and barriers", "[scratch-facts][div128-cfg]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    for (unsigned boundary = 0; boundary < 3; ++boundary) {
        CAPTURE(boundary);
        IntrusivePtr<Block> block{new Block(0, Location{0x2518})};
        const auto low = block->LoadImm(ValueType::U64, Imm{swift::u64{100}});
        const auto divisor = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
        const auto zero = block->LoadImm(ValueType::U64, Imm{swift::u64{0}});
        block->StoreUniform(Uniform{528, ValueType::U64}, zero);
        if (boundary == 1) block->StoreUniform(Uniform{532, ValueType::U32}, Value{divisor}.SetCastType(ValueType::U32));
        if (boundary == 2) block->AppendInst(OpCode::UniformBarrier);
        const auto high = block->LoadUniform(ValueType::U64, Uniform{528, ValueType::U64});
        const auto quotient = block->Div128(ValueType::U64, high, low, divisor, Imm{swift::u64{0}});
        block->ReIdInstr();
        compile::SemanticFacts facts{block.get()};
        const auto& index = facts.PrepareIndex(block.get());
        CHECK_FALSE(index.UsesReady());
        CHECK(arm64::SelectNativeDiv128(*quotient.Def(), index) == (boundary == 0));
        CHECK_FALSE(index.UsesReady());
    }
    for (const bool sign : {false, true})
        for (const bool operand_wrappers : {false, true})
            for (const unsigned depth : {15u, 16u, 17u}) {
                CAPTURE(sign, operand_wrappers, depth);
                Block block{0, Location{0x251c}};
                const auto low = block.LoadUniform(ValueType::U64, Uniform{512, ValueType::U64});
                const auto divisor = block.LoadImm(ValueType::U64, Imm{swift::u64{7}});
                auto high = sign ? block.AsrImm(ValueType::U64, low, Imm{swift::u32{63}})
                                 : block.LoadImm(ValueType::U64, Imm{swift::u64{0}});
                for (unsigned i = 0; i < depth; ++i)
                    high = operand_wrappers
                                   ? block.GetOperand(
                                             ValueType::U64,
                                             Operand{DataClass{high}, DataClass{}, OperandNone})
                                   : block.BitCast(ValueType::U64, high);
                const auto quotient =
                        block.Div128(ValueType::U64, high, low, divisor, Imm{swift::u64{sign}});
                block.ReIdInstr();
                compile::CompileWorkspace workspace;
                compile::RewriteContext editing{&block, workspace, nullptr, false};
                for (const auto& inst : block.GetInstList())
                    editing.Change(
                            &block, &inst, [](Inst& node) { node.SetId(100 + node.Id() * 3); });
                compile::SemanticFacts facts{&block};
                const auto& index = facts.PrepareIndex(&block);
                CHECK(arm64::SelectNativeDiv128(*quotient.Def(), index) == (depth <= 16));
                CHECK(arm64::PrepareManualDiv128(*quotient.Def(), nullptr) == (depth <= 16));
                CHECK_FALSE(index.UsesReady());
            }
}

TEST_CASE("prepared scratch preserves fixed and audit exclusions", "[scratch-facts][regression]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    constexpr swift::u32 x11 = 1u << 11, x12 = 1u << 12, x13 = 1u << 13;
    constexpr swift::u32 ip0 = 1u << 16, ip1 = 1u << 17;
    for (bool requested_xpool : {false, true}) for (int missing : {-1, 0, 9, 10, 12}) {
        const bool xpool = requested_xpool || GetSvmConfig().x86_pin_ext >= 3;
        CAPTURE(requested_xpool, xpool, missing);
        GPRSMask pool{(1u << 14) - 1};
        if (missing >= 0) pool.Clear(missing);
        FeatureSet features{};
        features.jit_scratch_xpool = requested_xpool;
        IntrusivePtr<Block> block{new Block(0, Location{0x251a})};
        const auto scalar = block->LoadImm(ValueType::U64, Imm{swift::u64{1}});
        const auto vector = block->LoadUniform(ValueType::V128, Uniform{512, ValueType::V128});
        const auto add = block->Add(ValueType::U64, scalar, Operand{scalar});
        const auto atomic = block->CompareAndSwap(ValueType::U64, scalar, scalar, scalar);
        const auto fp = block->VecFAdd(ValueType::V128, vector, vector, Imm{swift::u32{32}});
        const auto x87 = block->X87Op(ValueType::U64,
                                      scalar,
                                      Imm{static_cast<swift::u64>(swift::x86::X87Action::Compare)},
                                      scalar);
        auto* xchg = block->AppendInst(OpCode::XchgBarrier, Imm{swift::u32{3}});
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        compile::BlockAnalysisIndex index;
        index.Build(block.get(), nullptr, true);
        allocation.BeginTargetPlanning();
        allocation.DeclareBlock(block.get(), index.Summary());
        allocation.StoreScratch(
                block.get(),
                arm64::PrepareScratch(block.get(),
                                      allocation,
                                      features,
                                      arm64::PrepareUnitScratch(allocation.GetGprs(), features),
                                      index,
                                      allocation.Workspace()));
        allocation.EndTargetPlanning();
        const bool scratch_only = GetSvmConfig().x86_pin_ext >= 2 && !xpool &&
                missing != 0 && missing != 12;
        const swift::u32 profile = xpool && GetSvmConfig().exec_prof ? ip0 | ip1 : 0;
        const bool planned_scratch_only = GetSvmConfig().x86_pin_ext >= 2 && !xpool && missing != 0;
        const bool alu_lease = GetSvmConfig().x86_pin_ext >= 3 &&
                missing != 0 && missing != 9 && missing != 10;
        struct Expected {
            const Inst* inst;
            swift::u32 xpool_clobbers, scratch_clobbers, published_home;
            bool alu{};
        };
        const swift::u32 published_home = GetSvmConfig().static_regs ? 1u << 20 : 0;
        const Expected expected[] = {
            {add.Def(), 0, 0, 0, true},
            {atomic.Def(), x11 | x12 | x13, x11 | x12 | x13, 0},
            {fp.Def(), 0, 0, 0},
            {x87.Def(), ip0 | x12 | x13, 0, 0},
            {xchg, 0, 0, published_home},
        };
        for (const auto& row : expected) {
            const auto* frozen = allocation.Actions().ScratchFor(row.inst);
            REQUIRE(frozen);
            const auto manual = arm64::PrepareInstructionScratch(*row.inst, pool, features);
            // AllocationFixture restores x12/x13 reservations without XPOOL. Manual
            // preparation receives the supplied mask without normalization.
            for (const auto [facts, scratch] : {
                    std::pair{frozen, planned_scratch_only}, std::pair{&manual, scratch_only}}) {
                const swift::u32 fixed = profile | row.published_home |
                        (xpool ? row.xpool_clobbers : (scratch ? row.scratch_clobbers : 0));
                const swift::u32 audit = profile | row.published_home | (xpool ? row.xpool_clobbers : 0);
                const swift::u32 terminal = scratch ? x13 | (GetSvmConfig().flags_regs ? 0 : x12) : 0;
                CHECK(facts->fixed_clobbers == fixed);
                CHECK(facts->audit_clobbers == audit);
                CHECK(facts->leases == ((terminal & ~fixed) | (row.alu && alu_lease ? 1u << 10 : 0)));
            }
        }
    }
}

TEST_CASE("unit scratch admission rejects changed feature policy", "[scratch-facts][pin-plan][regression]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x251c})};
    const auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
    block->StoreUniform(Uniform{0, ValueType::U64}, value);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    const FeatureSet features{};
    AllocationFixture allocation{block->MaxInstrId(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    const auto plan = allocation.Actions().ForBlock(block.get());
    REQUIRE(plan);
    REQUIRE(allocation.Actions().HasPreparedScratch(block.get()));
    REQUIRE(allocation.Actions().MatchesScratchInputs(block.get(), allocation.GetGprs(), features));
    REQUIRE(allocation.Actions().Certificate()->MatchesUnit(
            block.get(), nullptr, allocation, allocation.Actions(), config, features));
    for (auto field : {&FeatureSet::jit_scratch_xpool, &FeatureSet::scratch_precise,
                       &FeatureSet::fpr_scratch_precise}) {
        auto changed = features;
        changed.*field = !(changed.*field);
        CHECK_FALSE(allocation.Actions().MatchesScratchInputs(
                block.get(), allocation.GetGprs(), changed));
        CHECK_FALSE(allocation.Actions().Certificate()->MatchesUnit(
                block.get(), nullptr, allocation, allocation.Actions(), config, changed));
    }
    CHECK(allocation.Actions().MatchesScratchInputs(block.get(), allocation.GetGprs(), features));
}

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

struct SpillForwardingBlock {
    IntrusivePtr<Block> block;
    Value arriving;
};

enum class WidthSpillShape {
    FinalUse,
    MultiUse,
    DeferredPublication,
};

SpillForwardingBlock MakeSpillForwardingBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x2400})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto first = block->Add(arriving, Operand{shortest});
    const auto second = block->Add(first, Operand{middle});
    const auto total = block->Add(second, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillWidthTransferBlock(
        WidthSpillShape shape = WidthSpillShape::FinalUse) {
    IntrusivePtr<Block> block{new Block(0, Location{0x2420})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(ValueType::U32, Imm{swift::u64{4}});
    const auto extended = block->ZeroExtend32To64(ValueType::U64, arriving);
    Value total;
    if (shape == WidthSpillShape::DeferredPublication) {
        block->SetHostGPR(extended, HostRegIndex(22), Imm{0u});
        const auto first = block->Add(shortest, Operand{middle});
        total = block->Add(first, Operand{longest});
    } else {
        if (shape == WidthSpillShape::MultiUse) {
            block->StoreUniform(Uniform{8, ValueType::U32}, arriving);
        }
        const auto first = block->Add(extended, Operand{shortest});
        const auto second = block->Add(first, Operand{middle});
        total = block->Add(second, Operand{longest});
    }
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillFlagsOnlyBlock(OpCode consumer) {
    IntrusivePtr<Block> block{new Block(0, Location{0x2430})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    block->AppendInst(consumer, arriving, Flags::NZCV);
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillAddressBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x2438})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto address =
            block->GetOperand(ValueType::U64, Operand{arriving, Imm{swift::u64{16}}, OperandPlus});
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, address);
    block->StoreUniform(Uniform{8, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillMemoryAddressBlock(bool multi_use,
                                                 bool spill_result = false) {
    IntrusivePtr<Block> block{new Block(0, Location{0x243a})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto loaded = block->LoadMemory(ValueType::U64, Operand{arriving});
    if (spill_result) {
        block->StoreUniform(Uniform{8, ValueType::U64}, loaded);
    } else {
        block->SetHostGPR(loaded, HostRegIndex(22), Imm{0u});
    }
    if (multi_use) {
        block->StoreUniform(Uniform{16, ValueType::U64}, arriving);
    }
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillMemoryAddressNoRegionBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x243e})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto loaded = block->LoadMemory(ValueType::U64, Operand{arriving});
    block->AppendInst(OpCode::TestNotZero, loaded);
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillMemoryOwnedAddressBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x243f})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto base = block->LoadImm(Imm{swift::u64{4}});
    const auto address =
            block->GetOperand(ValueType::U64, Operand{base, Imm{swift::u64{16}}, OperandPlus});
    const auto loaded = block->LoadMemory(ValueType::U64, Operand{address});
    block->StoreUniform(Uniform{8, ValueType::U64}, loaded);
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), address};
}

SpillForwardingBlock MakeSpillMemoryStoreAddressBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x243d})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto stored = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    block->StoreMemory(Operand{arriving}, stored);
    const auto first = block->Add(stored, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillMemoryValueBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x243b})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    block->StoreMemory(Operand{Imm{swift::u64{0x1000}}}, arriving);
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillPublicationBlock(bool fixed_home) {
    IntrusivePtr<Block> block{new Block(0, Location{0x243c})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    if (fixed_home) {
        block->SetHostGPR(arriving, HostRegIndex(15), Imm{0u});
    } else {
        block->StoreUniform(Uniform{0, ValueType::U64}, arriving);
    }
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{8, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeRepeatedSpillUseBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x2440})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto first = block->Add(arriving, Operand{shortest});
    const auto bridge = block->Add(first, Operand{middle});
    const auto second = block->Add(arriving, Operand{longest});
    const auto total = block->Add(second, Operand{bridge});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillPublicationRegionBlock(
        bool overwritten = false) {
    IntrusivePtr<Block> block{new Block(0, Location{0x2448})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto target = overwritten ? HostRegIndex(22) : HostRegIndex(15);
    block->SetHostGPR(arriving, target, Imm{0u});
    if (overwritten) {
        block->SetHostGPR(shortest, target, Imm{0u});
    }
    const auto first = block->Add(arriving, Operand{shortest});
    const auto second = block->Add(first, Operand{middle});
    const auto total = block->Add(second, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpilledPublicationWindowBlock(bool faulting = false) {
    IntrusivePtr<Block> block{new Block(0, Location{0x2480})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto published = block->GetOperand(ValueType::U64, Operand{Imm{swift::u64{0x1234}}});
    if (faulting) {
        (void)block->LoadMemory(ValueType::U64, Operand{Imm{swift::u64{0x1000}}});
    } else {
        block->ClearFlags(Flags::Carry | Flags::Overflow);
    }
    block->AppendInst(OpCode::SetHostGPR, published, HostRegIndex(29), Imm{0u});
    const auto first = block->Add(longest, Operand{middle});
    const auto total = block->Add(first, Operand{shortest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), published};
}

SpillForwardingBlock MakeSpilledBitExtractPublicationBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x24a0})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto source = block->LoadImm(Imm{swift::u64{0x123400}});
    const auto published = block->BitExtract(ValueType::U64, source, Imm{16u}, Imm{8u});
    block->AppendInst(OpCode::SetHostGPR, published, HostRegIndex(29), Imm{0u});
    const auto first = block->Add(longest, Operand{middle});
    const auto total = block->Add(first, Operand{shortest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), published};
}

std::vector<std::string> Emit(SpillForwardingBlock input, GPRSMask gprs) {
    FPRSMask fprs{~((1u << 8) - 1u)};
    AllocationFixture alloc{input.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    // These fixtures explicitly exercise arriving spill backing; the initial
    // target-home optimization is covered separately by the pin-plan tests.
    auto features = FeatureSet{};
    features.ra_spill_evict = false;
    auto prepared = ::swift::runtime::backend::arm64::PrepareAllocationInputs(
            input.block.get(), alloc, features);
    prepared.fixed.clear();
    RegisterAllocTestSupport::RunForSpillEvictTest(input.block.get(), &alloc, std::move(prepared));
    REQUIRE(alloc.ValueType(input.arriving) == AllocationFixture::MEM);

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.MapModule(
            LocationDescriptor{0x2400}, LocationDescriptor{0x2500}, ModuleConfig{});
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(input.block.get());
    context.Finish();

    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::vector<std::string> lines;
    for (auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        lines.emplace_back(disassembler.GetOutput());
    }
    return lines;
}

void RequireCanonicalSpillRoundTrip(const std::vector<std::string>& emitted,
                                    bool wide_definition = false) {
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    REQUIRE(definition->find(wide_definition ? "mov x" : "mov w") !=
            std::string::npos);
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = wide_definition
            ? definition->substr(4, delimiter - 4)
            : "x" + definition->substr(5, delimiter - 5);
    const auto store = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                       std::string::npos;
            });
    REQUIRE(store != emitted.end());
    const auto address_begin = store->find("[x28");
    const auto address_end = store->find(']', address_begin);
    REQUIRE(address_begin != std::string::npos);
    REQUIRE(address_end != std::string::npos);
    const auto address = store->substr(
            address_begin, address_end - address_begin + 1);
    REQUIRE(std::any_of(
            std::next(store), emitted.end(), [&](const auto& line) {
                return line.find("ldr x") != std::string::npos &&
                       line.find(address) != std::string::npos;
            }));
}

}  // namespace

TEST_CASE("adjacent spilled scalar def-use retains its free scratch") {
    const auto emitted = Emit(MakeSpillForwardingBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(std::next(definition), emitted.end(), [&](const auto& line) {
        return line.find("add ") != std::string::npos &&
               line.find(scratch) != std::string::npos;
    });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(std::next(definition), consumer, [&](const auto& line) {
        return (line.find("str " + scratch + ", [x28") != std::string::npos) ||
               (line.find("ldr " + scratch + ", [x28") != std::string::npos);
    }));
    const auto after_consumer = std::next(consumer);
    REQUIRE(after_consumer != emitted.end());
    REQUIRE(after_consumer->find("str " + scratch + ", [x28") == std::string::npos);
}

TEST_CASE("x18 spill forwarding yields to a live allocated value") {
#if defined(__linux__) && !defined(__ANDROID__)
    const auto emitted = Emit(MakeSpillForwardingBlock(),
                              GPRSMask{~(((1u << 6) - 1u) << 18)});
    REQUIRE(std::ranges::any_of(emitted, [](const auto& line) {
        return line.find("mov x18, #0x1") != std::string::npos;
    }));
    REQUIRE(std::ranges::none_of(emitted, [](const auto& line) {
        return line.find("mov x18, #0x4") != std::string::npos;
    }));
#else
    SUCCEED("x18 spill scratch is Linux-only");
#endif
}

TEST_CASE("final-use width consumers retain spilled inputs") {
    const auto emitted = Emit(MakeSpillWidthTransferBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov w") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch_code = definition->substr(5, delimiter - 5);
    const auto scratch = "x" + scratch_code;
    const auto source_register = "w" + scratch_code;
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("mov w") != std::string::npos &&
                       line.find(source_register) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(std::next(definition), consumer,
                         [&](const auto& line) {
                             return line.find("str " + scratch + ", [x28") !=
                                            std::string::npos ||
                                    line.find("ldr " + scratch + ", [x28") !=
                                            std::string::npos;
                         }));
}

TEST_CASE("multi-use width sources retain canonical spill backing") {
    RequireCanonicalSpillRoundTrip(Emit(
            MakeSpillWidthTransferBlock(WidthSpillShape::MultiUse),
            GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)}));
}

TEST_CASE("deferred pinned width publication retains spill backing") {
    RequireCanonicalSpillRoundTrip(Emit(
            MakeSpillWidthTransferBlock(
                    WidthSpillShape::DeferredPublication),
            GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)}));
}

TEST_CASE("flags-only consumers discard dead spill results") {
    auto check = [](OpCode consumer) {
        const auto emitted = Emit(
                MakeSpillFlagsOnlyBlock(consumer),
                GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
        const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
            return line.find("mov x") != std::string::npos &&
                   line.find("#0x4") != std::string::npos;
        });
        REQUIRE(definition != emitted.end());
        const auto delimiter = definition->find(',');
        REQUIRE(delimiter != std::string::npos);
        const auto scratch = definition->substr(4, delimiter - 4);
        const auto next_value = std::find_if(std::next(definition), emitted.end(),
                                             [](const auto& line) {
            return line.find("add ") != std::string::npos;
        });
        REQUIRE(next_value != emitted.end());
        REQUIRE(std::none_of(std::next(definition), next_value,
                             [&](const auto& line) {
            return line.find("str " + scratch + ", [x28") != std::string::npos ||
                   line.find("ldr " + scratch + ", [x28") != std::string::npos;
        }));
    };

    SECTION("branch-only marker") { check(OpCode::BranchOnlyFlags); }
    SECTION("architectural flags marker") { check(OpCode::SaveFlags); }
}

TEST_CASE("address consumers retain final-use spilled inputs") {
    const auto emitted = Emit(MakeSpillAddressBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(std::next(definition), emitted.end(),
                                       [&](const auto& line) {
        return line.find("add ") != std::string::npos &&
               line.find(scratch) != std::string::npos &&
               line.find("#0x10") != std::string::npos;
    });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(std::next(definition), consumer,
                         [&](const auto& line) {
        return line.find("str " + scratch + ", [x28") != std::string::npos ||
               line.find("ldr " + scratch + ", [x28") != std::string::npos;
    }));
}

TEST_CASE("faulting load with a direct result consumes a pending address") {
    const auto emitted = Emit(MakeSpillMemoryAddressBlock(false),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("ldr x") != std::string::npos &&
                       line.find("[" + scratch) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                               std::string::npos ||
                       line.find("ldr " + scratch + ", [x28") !=
                               std::string::npos;
            }));
}

TEST_CASE("faulting load with a local spill result consumes a pending address") {
    const auto emitted = Emit(MakeSpillMemoryAddressNoRegionBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("ldr x") != std::string::npos &&
                       line.find("[" + scratch) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                               std::string::npos ||
                       line.find("ldr " + scratch + ", [x28") !=
                               std::string::npos;
            }));
}

TEST_CASE("faulting store consumes a final-use pending address") {
    const auto emitted = Emit(MakeSpillMemoryStoreAddressBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("str x") != std::string::npos &&
                       line.find("[" + scratch) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                               std::string::npos ||
                       line.find("ldr " + scratch + ", [x28") !=
                               std::string::npos;
            }));
}

TEST_CASE("faulting store consumes a final-use pending value") {
    const auto emitted = Emit(MakeSpillMemoryValueBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("str " + scratch + ", [") !=
                               std::string::npos &&
                       line.find("[x28") == std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                               std::string::npos ||
                       line.find("ldr " + scratch + ", [x28") !=
                               std::string::npos;
            }));
}

TEST_CASE("faulting memory retains backing for multi-use pending inputs") {
    RequireCanonicalSpillRoundTrip(
            Emit(MakeSpillMemoryAddressBlock(true),
                 GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)}),
            true);
}

TEST_CASE("faulting region-owned loads retain backing without reloading addresses") {
    const auto emitted = Emit(MakeSpillMemoryOwnedAddressBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("add x") != std::string::npos &&
               line.find("#0x10") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto address = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("ldr x") != std::string::npos &&
                       line.find("[" + address) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(consumer->find("ldr " + address + ",") == std::string::npos);
    REQUIRE(std::count_if(
                    std::next(definition), consumer, [&](const auto& line) {
                        return line.find("str " + address + ", [x28") !=
                               std::string::npos;
                    }) == 1);
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("ldr " + address + ", [x28") !=
                       std::string::npos;
            }));
}

TEST_CASE("publication consumers retain final-use spilled inputs") {
    auto check = [](bool fixed_home) {
        const auto emitted = Emit(
                MakeSpillPublicationBlock(fixed_home),
                GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
        const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
            return line.find("mov x") != std::string::npos &&
                   line.find("#0x4") != std::string::npos;
        });
        REQUIRE(definition != emitted.end());
        const auto delimiter = definition->find(',');
        REQUIRE(delimiter != std::string::npos);
        const auto scratch = definition->substr(4, delimiter - 4);
        const auto consumer = std::find_if(std::next(definition), emitted.end(),
                                           [&](const auto& line) {
            return fixed_home
                    ? line.find("mov x15, " + scratch) != std::string::npos
                    : line.find("str " + scratch + ", [x28") != std::string::npos;
        });
        REQUIRE(consumer != emitted.end());
        REQUIRE(std::none_of(std::next(definition), consumer,
                             [&](const auto& line) {
            return line.find("str " + scratch + ", [x28") != std::string::npos ||
                   line.find("ldr " + scratch + ", [x28") != std::string::npos;
        }));
    };

    SECTION("guest register publication") { check(true); }
    SECTION("uniform memory") { check(false); }
}

TEST_CASE("spilled scalar definitions transfer into multi-use reload regions") {
    const auto emitted = Emit(MakeRepeatedSpillUseBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto resident = definition->substr(4, delimiter - 4);
    REQUIRE(std::count_if(std::next(definition), emitted.end(), [&](const auto& line) {
        return line.find("add ") != std::string::npos &&
               line.find(resident) != std::string::npos;
    }) >= 2);
    REQUIRE(std::none_of(std::next(definition), emitted.end(), [&](const auto& line) {
        return (line.find("str " + resident + ", [x28") != std::string::npos) ||
               (line.find("ldr " + resident + ", [x28") != std::string::npos);
    }));
}

TEST_CASE("pending spill definitions hand off to complete reload regions") {
    const auto emitted = Emit(
            MakeSpillPublicationRegionBlock(),
            GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto definition_register = definition->substr(4, delimiter - 4);
    const auto publication = std::find_if(
            std::next(definition), emitted.end(), [](const auto& line) {
                return line.find("mov x15, x") != std::string::npos;
            });
    REQUIRE(publication != emitted.end());
    const auto source_delimiter = publication->find(',');
    REQUIRE(source_delimiter != std::string::npos);
    const auto resident = publication->substr(source_delimiter + 2);
    const auto consumer = std::find_if(
            std::next(publication), emitted.end(), [&](const auto& line) {
                return line.find("add ") != std::string::npos &&
                       line.find(resident) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("str " + definition_register + ", [x28") !=
                               std::string::npos ||
                       line.find("ldr " + resident + ", [x28") !=
                               std::string::npos;
            }));
}

TEST_CASE("elided publications retain complete-region spill backing") {
    const auto emitted = Emit(
            MakeSpillPublicationRegionBlock(true),
            GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto store = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                       std::string::npos;
            });
    REQUIRE(store != emitted.end());
    const auto address_begin = store->find("[x28");
    const auto address_end = store->find(']', address_begin);
    REQUIRE(address_begin != std::string::npos);
    REQUIRE(address_end != std::string::npos);
    const auto address = store->substr(
            address_begin, address_end - address_begin + 1);
    REQUIRE(std::any_of(
            std::next(store), emitted.end(), [&](const auto& line) {
                return line.find("ldr x") != std::string::npos &&
                       line.find(address) != std::string::npos;
            }));
}

TEST_CASE("spilled fixed publications cross a fault-safe instruction window") {
    const auto emitted = Emit(MakeSpilledPublicationWindowBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    REQUIRE(std::ranges::any_of(emitted, [](const auto& line) {
        return line.find("mov x29, #0x1234") != std::string::npos;
    }));
    REQUIRE(std::ranges::none_of(emitted, [](const auto& line) {
        return line.find("str x29, [x28") != std::string::npos ||
               line.find("ldr x29, [x28") != std::string::npos;
    }));
}

TEST_CASE("spilled fixed publications retain canonical state across an untracked fault") {
    const auto emitted = Emit(MakeSpilledPublicationWindowBlock(true),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    REQUIRE(std::ranges::none_of(emitted, [](const auto& line) {
        return line.find("mov x29, #0x1234") != std::string::npos;
    }));
}

TEST_CASE("spilled bit extracts publish directly to a fixed home") {
    const auto emitted = Emit(MakeSpilledBitExtractPublicationBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    REQUIRE(std::ranges::any_of(emitted, [](const auto& line) {
        return line.find("ubfx x29") != std::string::npos;
    }));
    REQUIRE(std::ranges::none_of(emitted, [](const auto& line) {
        return line.find("str x29, [x28") != std::string::npos ||
               line.find("ldr x29, [x28") != std::string::npos;
    }));
}

TEST_CASE("spill reload regions stop at local control flow") {
    IntrusivePtr<Block> block{new Block(0, Location{0x2460})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto first = block->Add(arriving, Operand{shortest});
    const auto condition = block->LoadImm<BOOL>(Imm{swift::u64{1}});
    const auto skip = block->NotGoto(condition);
    block->Add(first, Operand{middle});
    block->BindLabel(skip);
    const auto second = block->Add(arriving, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, second);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    GPRSMask gprs{~((1u << 5) - 1u) & ~(1u << 18)};
    FPRSMask fprs{~((1u << 8) - 1u)};
    AllocationFixture alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocTestSupport::RunForSpillEvictTest(block.get(), &alloc, false);

    REQUIRE(alloc.ValueType(arriving) == AllocationFixture::MEM);
    REQUIRE_FALSE(alloc.HasSpillReload(arriving.Id(), first.Id()));
    REQUIRE_FALSE(alloc.HasSpillReload(arriving.Id(), second.Id()));
}

TEST_CASE("prepared spill segments drive physical ownership and respect local boundaries",
          "[allocation][spill-inputs]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    enum Shape { Ordinary, ClearedInput, LocalBoundary };
    for (const auto shape : {Ordinary, ClearedInput, LocalBoundary}) {
        CAPTURE(shape);
        IntrusivePtr<Block> block{new Block(0, Location{0x2464})};
        const auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
        block->StoreUniform(Uniform{0, ValueType::U64}, value);
        const auto* first = &block->GetInstList().back();
        if (shape == LocalBoundary) {
            const auto condition = block->LoadImm<BOOL>(Imm{swift::u64{1}});
            const auto label = block->NotGoto(condition);
            block->BindLabel(label);
        }
        block->StoreUniform(Uniform{8, ValueType::U64}, value);
        const auto* last = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        const FeatureSet features{};
        const GPRSMask pool{~0x1fu};
        AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{~0xffffu}, features};
        auto inputs = arm64::PrepareAllocationInputs(block.get(), allocation, features);
        REQUIRE(inputs.spill);
        const auto* range = inputs.spill->BlockFor(block.get());
        REQUIRE(range);
        REQUIRE(inputs.spill->Segments(*range).size() == (shape == LocalBoundary ? 2 : 1));
        if (shape == ClearedInput) inputs.spill.reset();
        const auto* payload = inputs.spill.get();
        auto moved = std::move(inputs);
        REQUIRE(moved.spill.get() == payload);
        for (const auto& inst : block->GetInstList()) {
            if (inst.HasValue()) {
                if (inst.Id() == value.Id())
                    allocation.MapMemSpill(inst.Id(), SpillSlot{0});
                else
                    allocation.MapRegister(inst.Id(), HostGPR{4});
            }
            allocation.SetActiveRegs(inst.Id(), allocation.GetGprs(), allocation.GetFprs());
        }
        RegisterAllocTestSupport::BuildSpillReloadsForTest(block.get(), &allocation, moved);
        REQUIRE(allocation.HasSpillReload(value.Id(), first->Id()) == (shape == Ordinary));
        REQUIRE(allocation.HasSpillReload(value.Id(), last->Id()) == (shape == Ordinary));
        if (shape == Ordinary) {
            const auto* reload = allocation.SpillReloadAt(value.Id(), first->Id());
            REQUIRE(reload);
            REQUIRE(reload->owns_all_uses);
        }
        allocation.FreezeGPRLocations();
    }
}
TEST_CASE("spill ownership keeps occurrence counts separate from consumer positions and aliases",
          "[allocation][spill-inputs]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    enum Shape {
        Repeated,
        External,
        Pseudo,
        Terminal,
        RawAlias,
        BoundAlias,
        Params,
        Split,
        TwoValues,
        Sparse,
        ShiftedSegment,
        NestedTerminal
    };
    for (const auto shape : {Repeated,
                             External,
                             Pseudo,
                             Terminal,
                             RawAlias,
                             BoundAlias,
                             Params,
                             Split,
                             TwoValues,
                             Sparse,
                             ShiftedSegment,
                             NestedTerminal}) {
        CAPTURE(shape);
        IntrusivePtr<Block> block{new Block(0, Location{0x2466})};
        IntrusivePtr<Block> foreign{new Block(0, Location{0x2468})};
        if (shape == ShiftedSegment) {
            const auto condition = block->LoadImm<BOOL>(Imm{swift::u64{1}});
            const auto label = block->NotGoto(condition);
            block->BindLabel(label);
        }
        const auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
        const Inst* removed{};
        if (shape == Sparse) {
            block->AdvancePC(Imm{swift::u64{1}});
            removed = &block->GetInstList().back();
        }
        const auto second =
                shape == TwoValues ? block->LoadImm(ValueType::U64, Imm{swift::u64{9}}) : Value{};
        const auto alias = shape == RawAlias || shape == BoundAlias
                                   ? block->BitCast(ValueType::U32, value)
                                   : Value{};
        const auto input = alias.Defined() ? alias : value;
        Value first;
        if (shape == Params) {
            ir::Params params;
            for (unsigned i = 0; i < 300; ++i)
                params.Push(Value{value}.SetCastType(i % 2 ? ValueType::U8 : ValueType::U64));
            first = block->CallDynamic(ValueType::VOID, Lambda{value}, std::move(params));
        } else
            first = block->Add(
                    ValueType::U64,
                    Value{input}.SetCastType(ValueType::U64),
                    Operand{Value{second.Defined() ? second : input}.SetCastType(ValueType::U8)});
        if (shape == External) foreign->StoreUniform(Uniform{0, ValueType::U64}, value);
        if (shape == Pseudo) block->SaveFlags(value, Flags::None);
        if (shape == Split) {
            const auto condition = block->LoadImm<BOOL>(Imm{swift::u64{1}});
            const auto label = block->NotGoto(condition);
            block->BindLabel(label);
        }
        block->StoreUniform(Uniform{0, ValueType::U64}, value);
        const auto* last = &block->GetInstList().back();
        if (second.Defined()) block->StoreUniform(Uniform{8, ValueType::U64}, second);
        if (shape == Terminal)
            block->SetTerminal(
                    terminal::If{BOOL{value}, terminal::ReturnToHost{}, terminal::ReturnToHost{}});
        else if (shape == NestedTerminal) {
            const Vector<terminal::Switch::Case> cases{
                    {Imm{swift::u32{0}},
                     terminal::If{BOOL{value}, terminal::ReturnToHost{}, terminal::ReturnToHost{}}},
                    {Imm{swift::u32{1}}, terminal::ReturnToHost{}}};
            block->SetTerminal(terminal::CheckHalt{terminal::Condition{
                    Cond::EQ,
                    terminal::If{BOOL{value}, terminal::ReturnToHost{}, terminal::ReturnToHost{}},
                    terminal::Switch{Value{value}.SetCastType(ValueType::U32), cases}}});
        } else
            block->SetTerminal(terminal::ReturnToHost{});
        foreign->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        foreign->ReIdInstr();
        if (removed) block->DestroyInst(removed);
        FeatureSet features{};
        features.scratch_precise = true;
        AllocationFixture allocation{
                block->MaxInstrId(), GPRSMask{~0x1fu}, FPRSMask{~0xffffu}, features};
        const auto inputs = arm64::PrepareAllocationInputs(block.get(), allocation, features);
        REQUIRE(inputs.spill);
        const auto* graph = allocation.PrepareFacts().FindIndex(block.get());
        REQUIRE(graph);
        REQUIRE(graph->DenseInstructionIDs() == (shape != Sparse));
        for (const auto& inst : block->GetInstList()) {
            if (inst.HasValue()) allocation.MapRegister(inst.Id(), HostGPR{4});
            allocation.SetActiveRegs(inst.Id(), allocation.GetGprs(), allocation.GetFprs());
        }
        allocation.MapMemSpill(value.Id(), SpillSlot{0});
        if (second.Defined()) allocation.MapMemSpill(second.Id(), SpillSlot{1});
        if (alias.Defined()) {
            if (shape == BoundAlias)
                allocation.MapReference(value.Id(), alias.Id());
            else
                allocation.MapMemSpill(alias.Id(), SpillSlot{2});
        }
        RegisterAllocTestSupport::BuildSpillReloadsForTest(block.get(), &allocation, inputs);
        const auto* begin = allocation.SpillReloadAt(value.Id(), first.Id());
        const auto* end = allocation.SpillReloadAt(value.Id(), last->Id());
        if (shape == Split) {
            REQUIRE_FALSE(begin);
            REQUIRE_FALSE(end);
        } else {
            REQUIRE(begin);
            REQUIRE(end);
            REQUIRE(begin->region == end->region);
            const bool complete = shape == Repeated || shape == TwoValues || shape == Sparse ||
                                  shape == ShiftedSegment;
            REQUIRE(begin->owns_all_uses == complete);
            REQUIRE(end->owns_all_uses == complete);
            if (complete) REQUIRE(allocation.HasSpillReload(value.Id(), value.Id()));
        }
        if (shape == Params) REQUIRE(value.Def()->GetUses(false) == 302);
        if (shape == RawAlias) REQUIRE_FALSE(allocation.HasSpillReload(alias.Id(), first.Id()));
        if (second.Defined()) {
            const auto* other = allocation.SpillReloadAt(second.Id(), first.Id());
            REQUIRE(other);
            REQUIRE(other->owns_all_uses);
            REQUIRE(other->reg != begin->reg);
        }
        allocation.FreezeGPRLocations();
    }
}

TEST_CASE("spill load ownership preserves segment membership and exact instruction adjacency",
          "[allocation][spill-inputs]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    enum Shape { Ordinary, PrefixSegment, IdGap, LocalBoundary, ExtraInputUse, TerminalResult };
    for (bool sparse : {false, true})
        for (const auto shape :
             {Ordinary, PrefixSegment, IdGap, LocalBoundary, ExtraInputUse, TerminalResult}) {
            CAPTURE(sparse, shape);
            IntrusivePtr<Block> block{new Block(0, Location{0x246a})};
            if (shape == PrefixSegment) {
                const auto condition = block->LoadImm<BOOL>(Imm{swift::u64{1}});
                const auto label = block->NotGoto(condition);
                block->BindLabel(label);
            }
            const auto address =
                    block->GetOperand(ValueType::U64, Operand{Imm{swift::u64{0x4000}}});
            const Inst* adjacency_gap{};
            if (shape == IdGap) {
                block->AdvancePC(Imm{swift::u64{1}});
                adjacency_gap = &block->GetInstList().back();
            }
            if (shape == LocalBoundary) {
                const auto condition = block->LoadImm<BOOL>(Imm{swift::u64{1}});
                const auto label = block->NotGoto(condition);
                block->BindLabel(label);
            }
            const auto loaded = block->LoadMemory(ValueType::U64, Operand{address});
            const Inst* sparse_gap{};
            if (sparse) {
                block->AdvancePC(Imm{swift::u64{1}});
                sparse_gap = &block->GetInstList().back();
            }
            block->StoreUniform(Uniform{0, ValueType::U64}, loaded);
            const auto* first = &block->GetInstList().back();
            block->StoreUniform(Uniform{8, ValueType::U64}, loaded);
            const auto* last = &block->GetInstList().back();
            if (shape == ExtraInputUse) block->StoreUniform(Uniform{16, ValueType::U64}, address);
            if (shape == TerminalResult)
                block->SetTerminal(terminal::CheckHalt{terminal::If{
                        BOOL{loaded}, terminal::ReturnToHost{}, terminal::ReturnToHost{}}});
            else
                block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            if (adjacency_gap) block->DestroyInst(adjacency_gap);
            if (sparse_gap) block->DestroyInst(sparse_gap);
            FeatureSet features{};
            features.scratch_precise = true;
            AllocationFixture allocation{
                    block->MaxInstrId(), GPRSMask{~0x1fu}, FPRSMask{~0xffffu}, features};
            const auto inputs = arm64::PrepareAllocationInputs(block.get(), allocation, features);
            REQUIRE(inputs.spill);
            const auto* graph = allocation.PrepareFacts().FindIndex(block.get());
            REQUIRE(graph);
            REQUIRE(graph->DenseInstructionIDs() == (!sparse && shape != IdGap));
            for (const auto& inst : block->GetInstList()) {
                if (inst.HasValue()) allocation.MapRegister(inst.Id(), HostGPR{4});
                allocation.SetActiveRegs(inst.Id(), allocation.GetGprs(), allocation.GetFprs());
            }
            allocation.MapMemSpill(address.Id(), SpillSlot{0});
            allocation.MapMemSpill(loaded.Id(), SpillSlot{1});
            RegisterAllocTestSupport::BuildSpillReloadsForTest(block.get(), &allocation, inputs);
            const bool pair = shape == Ordinary || shape == PrefixSegment;
            const auto* input = allocation.SpillReloadAt(address.Id(), loaded.Id());
            REQUIRE((input != nullptr) == (pair || shape == ExtraInputUse));
            if (input) {
                REQUIRE(input->fault_backed == pair);
                REQUIRE(input->owns_all_uses == pair);
                REQUIRE(allocation.HasSpillReload(address.Id(), address.Id()) == pair);
            }
            const auto* begin = allocation.SpillReloadAt(loaded.Id(), first->Id());
            const auto* end = allocation.SpillReloadAt(loaded.Id(), last->Id());
            REQUIRE(begin);
            REQUIRE(end);
            REQUIRE(begin->region == end->region);
            REQUIRE(begin->owns_all_uses == (shape != TerminalResult));
            if (input) REQUIRE(input->reg != begin->reg);
            allocation.FreezeGPRLocations();
        }
}

TEST_CASE("prepared scratch keeps local demand and exact repeated spill inputs",
          "[scratch-facts][spill-forward][regression]") {
    for (unsigned shape : {0u, 1u, 2u, 3u}) {
        CAPTURE(shape);
        IntrusivePtr<Block> block{new Block(0, Location{0x24a0})};
        IntrusivePtr<Block> foreign{new Block(0, Location{0x24a8})};
        const auto producer = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
        const auto input = block->LoadImm(ValueType::U64, Imm{swift::u64{9}});
        const auto empty_save = block->Xor(ValueType::U64, input, Operand{input});
        block->SaveFlags(empty_save, Flags::None);
        const auto foreign_flags = block->Xor(ValueType::U64, input, Operand{input});
        const auto external = foreign->LoadImm(ValueType::U64, Imm{swift::u64{11}});
        Params params;
        params.Push(Value{producer}.SetCastType(ValueType::U8));
        params.Push(Imm{swift::u64{1}});
        params.Push(Value{producer}.SetCastType(ValueType::U64));
        params.Push(shape == 2 ? external : producer);
        const auto call = block->CallDynamic(ValueType::VOID, Lambda{producer}, std::move(params));
        if (shape == 1) block->StoreUniform(Uniform{0, ValueType::U64}, producer);
        foreign->SaveFlags(foreign_flags, Flags::NZCV | Flags::Parity);
        if (shape == 3) foreign->SaveFlags(producer, Flags::Carry);
        block->ReIdInstr();
        foreign->ReIdInstr();
        REQUIRE(external.Id() == producer.Id());
        FeatureSet features{};
        features.scratch_precise = true;
        AllocationFixture allocation{
                block->MaxInstrId(), GPRSMask{~((1u << 8) - 1u)}, FPRSMask{}, features};
        allocation.BeginTargetPlanning();
        allocation.MapMemSpill(producer.Id(), SpillSlot{0});
        {
            compile::BlockAnalysisIndex index;
            index.Build(block.get(), nullptr, true);
            allocation.DeclareBlock(block.get(), index.Summary(), index.PseudoFlags().View());
            allocation.StoreScratch(
                    block.get(),
                    arm64::PrepareScratch(block.get(),
                                          allocation,
                                          features,
                                          arm64::PrepareUnitScratch(allocation.GetGprs(), features),
                                          index,
                                          allocation.Workspace()));
        }
        allocation.EndTargetPlanning();
        const auto& actions = allocation.Actions();
        for (const auto& inst : block->GetInstList()) {
            const auto* planned = actions.ScratchFor(&inst);
            REQUIRE(planned);
            // Independent manual preparation scans this block. It observes the
            // zero-mask save, and excludes the save in the foreign block.
            const auto manual = arm64::PrepareInstructionScratchWithBlock(
                    inst, allocation.GetGprs(), features, block.get());
            REQUIRE(*planned == manual);
        }
        const auto facts = actions.SpillInputFor(call.Def(), producer.Id());
        if (shape == 2)
            REQUIRE(facts.definition == nullptr);
        else {
            REQUIRE(facts.definition == producer.Def());
            REQUIRE(facts.last_use == (shape == 0));
        }
        REQUIRE(actions.SpillInputFor(call.Def(), input.Id()).definition == nullptr);
        REQUIRE(actions.SpillInputFor(nullptr, producer.Id()).definition == nullptr);
        REQUIRE(actions.SpillInputFor(call.Def(), UINT32_MAX).definition == nullptr);
        REQUIRE(actions.ScratchFor(external.Def()) == nullptr);
        allocation.ResetAllocations();
        REQUIRE(actions.ScratchFor(call.Def()) == nullptr);
        REQUIRE(actions.SpillInputFor(call.Def(), producer.Id()).definition == nullptr);
    }
}

TEST_CASE("normal allocation preserves flags-only spilled XOR and surviving uses",
          "[spill-flags-only][regression]") {
    for (unsigned shape : {0u, 1u, 2u}) {
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 1024;
        config.stack_alignment = 16;
        config.enable_asm_interp = false;
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block(0, Location{0x24b0})};
        // AF is explicitly initialized then preserved; XOR's undefined x86 AF
        // is not used as an oracle for a new AF publication contract.
        block->ClearFlags(Flags::AuxiliaryCarry);
        const auto left = block->LoadUniform(ValueType::U64, Uniform{64, ValueType::U64});
        const auto right = block->LoadUniform(ValueType::U64, Uniform{72, ValueType::U64});
        std::vector<Value> live;
        for (unsigned i = 0; i < 36; ++i)
            live.push_back(
                    block->LoadUniform(ValueType::U64, Uniform{128 + i * 8, ValueType::U64}));
        const auto result = block->Xor(ValueType::U64, left, Operand{right});
        block->SaveFlags(result, Flags::NZCV | Flags::Parity);
        const auto* flags_consumer = &block->GetInstList().back();
        const Flags tests[] = {Flags::Parity, Flags::AuxiliaryCarry, Flags::Carry,
                               Flags::Overflow, Flags::Zero, Flags::Negate};
        for (unsigned i = 0; i < 6; ++i) {
            const auto bit = block->TestFlags(ValueType::U8, tests[i]);
            block->StoreUniform(Uniform{i, ValueType::U8}, bit);
        }
        if (shape == 1) block->StoreUniform(Uniform{8, ValueType::U64}, result);
        if (shape == 2) {
            const auto alias = block->BitCast(ValueType::U64, result);
            block->StoreUniform(Uniform{8, ValueType::U64}, alias);
        }
        auto total = block->Add(ValueType::U64, left, Operand{right});
        for (const auto value : live) total = block->Add(ValueType::U64, total, Operand{value});
        block->StoreUniform(Uniform{16, ValueType::U64}, total);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        auto features = FeatureSet{};
        features.ra_spill_evict = false;
        AllocationFixture allocation{block->MaxInstrId(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        INFO("shape=" << shape);
        REQUIRE(allocation.ValueType(result) == AllocationFixture::MEM);
        REQUIRE(allocation
                        .GPRLocationAt(result, result.Id(), AllocationFixture::GPRView::WholeValue)
                        .kind == AllocationFixture::MEM);
        const auto actions = allocation.Actions().ForBlock(block.get());
        REQUIRE(actions);
        CHECK(allocation.Actions().DiscardsFlagsOnlySpill(flags_consumer, result.Id()) ==
              (shape == 0));
        CHECK_FALSE(allocation.Actions().DiscardsFlagsOnlySpill(flags_consumer, left.Id()));

        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        REQUIRE(translator.Translate(block.get()));
        context.Finish();
#if defined(__aarch64__)
        const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID);
        context.Flush(buffer);
        REQUIRE(module->Push(block.get()));
        (void)module->PublishLinkTarget(block->GetStartLocation(), buffer.exec_data, buffer.exec_data);
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        for (const auto inputs : {std::pair<swift::u64, swift::u64>{0x55, 0x55},
                                  {0x55, 0x54}, {0, 0x8000000000000000ull}}) {
            Runtime runtime{&space};
            auto uniform = runtime.GetUniformBuffer();
            std::memcpy(uniform.data() + 64, &inputs.first, sizeof(inputs.first));
            std::memcpy(uniform.data() + 72, &inputs.second, sizeof(inputs.second));
            swift::u64 expected_sum = inputs.first + inputs.second;
            for (unsigned i = 0; i < 36; ++i) {
                const swift::u64 v = 100 + i;
                std::memcpy(uniform.data() + 128 + i * 8, &v, sizeof(v));
                expected_sum += v;
            }
            runtime.SetLocation(block->GetStartLocation().Value());
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            const swift::u64 expected = inputs.first ^ inputs.second;
            CHECK(uniform[0] == (std::popcount(static_cast<unsigned>(expected & 0xff)) % 2 == 0));
            CHECK(uniform[1] == 0); // explicit incoming AF preserved
            CHECK(uniform[2] == 0);
            CHECK(uniform[3] == 0);
            CHECK(uniform[4] == (expected == 0));
            CHECK(uniform[5] == (expected >> 63));
            swift::u64 actual_sum{};
            std::memcpy(&actual_sum, uniform.data() + 16, sizeof(actual_sum));
            CHECK(actual_sum == expected_sum);
            if (shape != 0) {
                swift::u64 actual{};
                std::memcpy(&actual, uniform.data() + 8, sizeof(actual));
                CHECK(actual == expected);
            }
        }
#endif
    }
}
