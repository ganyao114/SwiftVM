#include "tests/support/rewrite_fixture.h"
using swift::runtime::test::RewriteFixture;
#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <array>
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/actions.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/lower_integer_flags.h"
#include "runtime/backend/arm64/compile/lower_predicates.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/runtime.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/compile/workspace.h"
#include "runtime/ir/hir_builder.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("narrow carry plans preserve carry availability and all use views", "[narrow-integer-plan]") {
    for (unsigned scenario = 0; scenario < 10; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbe00})};
        const auto base = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
        const auto producer = (scenario == 2 ? block->Sub(ValueType::U32, base, Operand{Imm{1u}})
                                             : block->Add(ValueType::U32, base, Operand{Imm{1u}}));
        if (scenario != 1) block->SaveFlags(producer, Flags::Carry);
        if (scenario == 3) block->ClearFlags(Flags::Carry);
        if (scenario == 4) (void)block->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{1}}});
        if (scenario == 5) block->AdvancePC(Imm{swift::u64{1}});
        const auto zero = block->LoadImm(ValueType::U32, Imm{0u});
        const auto test = block->TestFlags(ValueType::U32, Flags::Carry);
        const auto operand =
                scenario == 9 ? Operand{test, Imm{1u}, OperandOp{OperandOp::LSL}} : Operand{test};
        const auto carry = block->Add(ValueType::U32, zero, operand);
        const auto result = block->Add(ValueType::U32, base, Operand{carry});
        if (scenario == 7) block->SaveFlags(result, Flags::Zero);
        const auto type = scenario == 6 ? ValueType::U32 : ValueType::U8;
        block->StoreUniform(Uniform{8, type}, Value{result}.SetCastType(type));
        if (scenario == 8) block->StoreUniform(Uniform{16, ValueType::U32}, zero);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BitVector coverage(block->MaxInstrId());
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        compile::CompileWorkspace workspace;
        arm64::IntegerFlagsInputs inputs{coverage, analysis, analysis.PseudoFlags().View()};
        inputs.canonical_carry = scenario == 5;
        REQUIRE(arm64::HasNarrowIntegerFlagsCandidates(*block) == (scenario != 9));
        const auto plan = arm64::LowerNarrowIntegerFlags(analysis, workspace, inputs);
        const bool accepted = scenario == 0 || scenario == 8;
        REQUIRE((plan.Carry(result.Id()) != nullptr) == accepted);
        REQUIRE(plan.Covered(test.Id()) == accepted);
        REQUIRE(plan.Covered(carry.Id()) == accepted);
        REQUIRE(plan.Covered(zero.Id()) == (scenario == 0));
        if (accepted) REQUIRE(plan.Carry(result.Id())->Source(*result.Def()).Def() == base.Def());
    }
}

TEST_CASE("narrow compare plans require proven narrow inputs and carry-only demand", "[narrow-integer-plan]") {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbe08})};
        const auto input =
                block->LoadUniform(scenario == 1 ? ValueType::U32 : ValueType::U8,
                                   Uniform{0, scenario == 1 ? ValueType::U32 : ValueType::U8});
        const auto immediate = block->LoadImm(ValueType::U32, Imm{scenario == 2 ? 256u : 7u});
        const auto sub = block->Sub(ValueType::U8, input, Operand{immediate});
        block->SaveFlags(sub, scenario == 3 ? Flags::All : Flags::Carry);
        if (scenario == 5) block->BranchOnlyFlags(sub, Flags::Carry);
        if (scenario == 4) block->StoreUniform(Uniform{8, ValueType::U32}, immediate);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BitVector coverage(block->MaxInstrId());
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        compile::CompileWorkspace workspace;
        arm64::IntegerFlagsInputs inputs{coverage, analysis, analysis.PseudoFlags().View()};
        const auto plan = arm64::LowerNarrowIntegerFlags(analysis, workspace, inputs);
        const bool accepted = scenario == 0;
        REQUIRE((plan.Compare(sub.Id()) != nullptr) == accepted);
        REQUIRE(plan.Covered(immediate.Id()) == accepted);
        if (accepted) {
            REQUIRE(plan.Compare(sub.Id())->argument == 7);
            REQUIRE(plan.Compare(sub.Id())->Width() == 1);
        }
    }
}

TEST_CASE("narrow compare extra flags must be overwritten before reads or faults", "[narrow-integer-plan]") {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbe10})};
        const auto narrow = block->LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
        const auto base = block->LoadUniform(ValueType::U32, Uniform{8, ValueType::U32});
        const auto sub = block->Sub(ValueType::U8, narrow, Operand{Imm{7u}});
        block->SaveFlags(sub, Flags::All);
        block->InvertCarry();
        const auto zero = block->LoadImm(ValueType::U32, Imm{0u});
        const auto test = block->TestFlags(ValueType::U32, Flags::Carry);
        const auto carry = block->Add(ValueType::U32, zero, Operand{test});
        const auto result = block->Add(ValueType::U32, base, Operand{carry});
        block->StoreUniform(Uniform{16, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
        if (scenario == 1) (void)block->TestFlags(Flags::Parity);
        if (scenario == 2) (void)block->LoadMemory(ValueType::U64, Operand{Imm{swift::u64{0}}});
        if (scenario != 3) {
            const auto overwrite = block->Add(ValueType::U32, base, Operand{Imm{1u}});
            block->SaveFlags(overwrite, Flags::All);
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BitVector coverage(block->MaxInstrId());
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        compile::CompileWorkspace workspace;
        arm64::IntegerFlagsInputs inputs{coverage, analysis, analysis.PseudoFlags().View()};
        const auto plan = arm64::LowerNarrowIntegerFlags(analysis, workspace, inputs);
        REQUIRE((plan.Carry(result.Id()) != nullptr));
        REQUIRE((plan.Compare(sub.Id()) != nullptr) == (scenario == 0));
    }
}

TEST_CASE("shifted carry operands retain their arithmetic meaning", "[narrow-integer-plan][regression]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 64;
    config.stack_alignment = 16;
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xbe20})};
    const auto input = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    const auto base = block->LoadUniform(ValueType::U32, Uniform{8, ValueType::U32});
    const auto producer = block->Add(ValueType::U32, input, Operand{Imm{1u}});
    block->SaveFlags(producer, Flags::Carry);
    const auto zero = block->LoadImm(ValueType::U32, Imm{0u});
    const auto carry = block->TestFlags(ValueType::U32, Flags::Carry);
    const auto shifted =
            block->Add(ValueType::U32, zero, Operand{carry, Imm{1u}, OperandOp{OperandOp::LSL}});
    const auto result = block->Add(ValueType::U32, base, Operand{shifted});
    block->StoreUniform(Uniform{16, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    FeatureSet features{};
    AllocationFixture allocation{block->MaxInstrId(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    REQUIRE_NOTHROW(::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false));
    CAPTURE(shifted.Def()->GetArg<Operand>(1).GetOp().type);
    std::unique_ptr<arm64::JitContext> context_owner;
    REQUIRE_NOTHROW(::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation));
    REQUIRE_NOTHROW(context_owner = std::make_unique<arm64::JitContext>(
                            module, allocation, allocation.Actions()));
    auto& context = *context_owner;
    arm64::JitTranslator translator{context};
    REQUIRE_NOTHROW(translator.Translate(block.get()));
    context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);
#if defined(__aarch64__)
    const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
    REQUIRE(index != INVALID_CACHE_ID);
    context.Flush(buffer);
    REQUIRE(module->Push(block.get()));
    (void)module->PublishLinkTarget(block->GetStartLocation(), buffer.exec_data, buffer.exec_data);
    space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
    Runtime runtime{&space};
    const swift::u32 overflowing = UINT32_MAX, initial = 8;
    std::memcpy(runtime.GetUniformBuffer().data(), &overflowing, sizeof(overflowing));
    std::memcpy(runtime.GetUniformBuffer().data() + 8, &initial, sizeof(initial));
    runtime.SetLocation(block->GetStartLocation().Value());
    REQUIRE(runtime.Run() == HaltReason::CallHost);
    // CF=1; the modified operand contributes 2, not the 1 an ADC would add.
    REQUIRE(runtime.GetUniformBuffer()[16] == 10);
#endif
}

TEST_CASE("AllocationFixture freezes narrow integer actions before encoder construction",
          "[narrow-integer-plan][target-planner]") {
    for (unsigned shape : {0u, 1u, 2u}) {
        CAPTURE(shape);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 64; config.stack_alignment = 16;
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        FeatureSet features{};
        IntrusivePtr<Block> block{new Block(0, Location{0xbe30})};
        const Inst* selected{};
        if (shape == 1) {
            const auto base = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
            const auto producer = block->Add(ValueType::U32, base, Operand{Imm{1u}});
            block->SaveFlags(producer, Flags::Carry);
            const auto zero = block->LoadImm(ValueType::U32, Imm{0u});
            const auto carry = block->TestFlags(ValueType::U32, Flags::Carry);
            const auto added = block->Add(ValueType::U32, zero, Operand{carry});
            const auto result = block->Add(ValueType::U32, base, Operand{added});
            block->StoreUniform(Uniform{8, ValueType::U8},
                                Value{result}.SetCastType(ValueType::U8));
            selected = result.Def();
        } else {
            const auto type = shape == 2 ? ValueType::U32 : ValueType::U8;
            const auto input = block->LoadUniform(type, Uniform{0, type});
            const auto immediate = block->LoadImm(ValueType::U32, Imm{7u});
            const auto sub = block->Sub(ValueType::U8, input, Operand{immediate});
            block->SaveFlags(sub, Flags::Carry);
            selected = sub.Def();
        }
        block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
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
        if (shape == 0) {
            REQUIRE(allocation.Actions().NarrowCompare(selected->Id()));
            REQUIRE(allocation.Actions().NarrowCompare(selected->Id())->argument == 7);
        } else if (shape == 1) {
            REQUIRE(allocation.Actions().NarrowCarry(selected->Id()));
        } else
            REQUIRE_FALSE(allocation.Actions().IntegerFlags(selected->Id()));
        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get()); context.Finish(); REQUIRE(context.CurrentBufferSize() > 0);
#if defined(__aarch64__)
        if (shape == 1) {
            const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
            REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer);
            space.PushCodeCache(block->GetStartLocation(), buffer.exec_data + context.GetCodeOffset(0xbe30));
            Runtime runtime{&space};
            for (const swift::u32 input : {swift::u32{3}, UINT32_MAX}) {
                std::memcpy(runtime.GetUniformBuffer().data(), &input, sizeof(input));
                runtime.SetLocation(0xbe30); REQUIRE(runtime.Run() == HaltReason::CallHost);
                REQUIRE(runtime.GetUniformBuffer()[8] == (input == UINT32_MAX ? 0 : 3));
            }
        }
#endif
    }
}

TEST_CASE("narrow carry preserves a zero input used by a later CFG block", "[narrow-integer-plan][cfg]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.region_edges = true; config.uniform_buffer_size = 64; config.stack_alignment = 16;
    AddressSpace space{config}; auto module = space.GetDefaultModule(); FeatureSet features{};
    features.mem_narrow_fuse = true; HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0xbe40}, Location{0xbe43});
    const auto address = function->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    const auto load = function->LoadMemory(ValueType::U8, Operand{address});
    const auto extension = function->ZeroExtend32(ValueType::U32, load);
    function->StoreUniform(Uniform{24, ValueType::U32}, extension);
    auto* carry_block = builder.LinkBlock(terminal::LinkBlock{Location{0xbe41}});
    builder.SetCurBlock(carry_block);
    const auto base = function->LoadUniform(ValueType::U32, Uniform{8, ValueType::U32});
    const auto producer = function->Add(ValueType::U32, base, Operand{Imm{1u}});
    function->SaveFlags(producer, Flags::Carry);
    const auto zero = function->LoadImm(ValueType::U32, Imm{0u});
    const auto test = function->TestFlags(ValueType::U32, Flags::Carry);
    const auto carry = function->Add(ValueType::U32, zero, Operand{test});
    const auto result = function->Add(ValueType::U32, base, Operand{carry});
    function->StoreUniform(Uniform{12, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
    auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0xbe42}});
    builder.SetCurBlock(next);
    function->StoreUniform(Uniform{16, ValueType::U32}, zero);
    function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
    function->ComputeRPO(); function->IdByRPO();
    AllocationFixture allocation{function->MaxInstrCount(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    ::swift::runtime::compile::AllocateFunction(
            function,
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, allocation, features),
            {.block_intervals = false});
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    const auto plan = allocation.Actions().ForBlock(carry_block->GetBlock());
    REQUIRE(plan);
    REQUIRE(allocation.Actions().NarrowCarry(result.Id()));
    arm64::JitContext context{module, allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(function); context.Finish();
#if defined(__aarch64__)
    const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
    REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer);
    space.PushCodeCache(Location{0xbe40}, buffer.exec_data + context.GetCodeOffset(0xbe40));
    Runtime runtime{&space}; swift::u8 memory = 171; const auto pointer = reinterpret_cast<swift::u64>(&memory);
    const swift::u32 input = 3; std::memcpy(runtime.GetUniformBuffer().data(), &pointer, sizeof(pointer));
    std::memcpy(runtime.GetUniformBuffer().data() + 8, &input, sizeof(input));
    runtime.SetLocation(0xbe40); REQUIRE(runtime.Run() == HaltReason::CallHost);
    swift::u32 saved{}; std::memcpy(&saved, runtime.GetUniformBuffer().data() + 16, sizeof(saved));
    REQUIRE(saved == 0);
#endif
    REQUIRE_FALSE(allocation.Actions().InstructionCovered(zero.Id()));
}

TEST_CASE("narrow carry keeps a zero chain observed by its terminal", "[narrow-integer-plan][cfg]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbe50})};
    const auto base = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    const auto producer = block->Add(ValueType::U32, base, Operand{Imm{1u}});
    block->SaveFlags(producer, Flags::Carry);
    const auto zero = block->LoadImm(ValueType::U32, Imm{0u});
    const auto test = block->TestFlags(ValueType::U32, Flags::Carry);
    const auto carry = block->Add(ValueType::U32, zero, Operand{test});
    const auto result = block->Add(ValueType::U32, base, Operand{carry});
    block->StoreUniform(Uniform{8, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
    block->SetTerminal(terminal::If{BOOL{Value{zero}.SetCastType(ValueType::U8)},
            terminal::ReturnToHost{}, terminal::ReturnToHost{}}); block->ReIdInstr();
    BitVector coverage(block->MaxInstrId());
    compile::BlockAnalysisIndex analysis;
    analysis.Build(block.get(), nullptr, true);
    compile::CompileWorkspace workspace;
    arm64::IntegerFlagsInputs inputs{coverage, analysis, analysis.PseudoFlags().View()};
    REQUIRE(analysis.DirectUseCount(zero.Def()) == 1);
    REQUIRE(zero.Def()->GetUses(false) == 2);
    const auto plan = arm64::LowerNarrowIntegerFlags(analysis, workspace, inputs);
    REQUIRE((plan.Carry(result.Id()) != nullptr));
    REQUIRE_FALSE(plan.Covered(zero.Id()));
}

TEST_CASE("narrow carry retains observable alias and pseudo uses of its zero chain", "[narrow-integer-plan][cfg]") {
    for (const bool pseudo : {false, true}) {
        CAPTURE(pseudo);
        IntrusivePtr<Block> block{new Block(0, Location{0xbe60})};
        const auto base = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
        const auto producer = block->Add(ValueType::U32, base, Operand{Imm{1u}});
        block->SaveFlags(producer, Flags::Carry);
        const auto zero = block->LoadImm(ValueType::U32, Imm{0u});
        const auto alias = block->BitCast(ValueType::U32, zero);
        if (pseudo) block->SaveFlags(alias, Flags::Zero);
        const auto test = block->TestFlags(ValueType::U32, Flags::Carry);
        const auto carry = block->Add(ValueType::U32, zero, Operand{test});
        const auto result = block->Add(ValueType::U32, base, Operand{carry});
        block->StoreUniform(Uniform{8, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
        if (pseudo) block->SetTerminal(terminal::ReturnToHost{});
        else block->SetTerminal(terminal::If{BOOL{Value{alias}.SetCastType(ValueType::U8)},
                terminal::ReturnToHost{}, terminal::ReturnToHost{}});
        block->ReIdInstr();
        BitVector coverage(block->MaxInstrId());
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        compile::CompileWorkspace workspace;
        arm64::IntegerFlagsInputs inputs{coverage, analysis, analysis.PseudoFlags().View()};
        REQUIRE(analysis.DirectUseCount(zero.Def()) == zero.Def()->GetUses(false));
        const auto plan = arm64::LowerNarrowIntegerFlags(analysis, workspace, inputs);
        REQUIRE((plan.Carry(result.Id()) != nullptr));
        REQUIRE_FALSE(plan.Covered(zero.Id()));
        REQUIRE_FALSE(plan.Covered(alias.Id()));
    }
}

TEST_CASE("narrow integer plans are order-independent over independent candidates", "[narrow-integer-plan]") {
    struct Facts {
        size_t carries{}, comparisons{}, covered{};
        swift::u32 immediate{};
        swift::u8 width{};
        bool roles_ok{};
    };
    const auto build = [](bool compare_first) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbe40})};
        Value result, carry, test, sub, immediate;
        const auto make_carry = [&] {
            const auto base = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
            const auto producer = block->Add(ValueType::U32, base, Operand{Imm{1u}});
            block->SaveFlags(producer, Flags::Carry);
            const auto zero = block->LoadImm(ValueType::U32, Imm{0u});
            test = block->TestFlags(ValueType::U32, Flags::Carry);
            carry = block->Add(ValueType::U32, zero, Operand{test});
            result = block->Add(ValueType::U32, base, Operand{carry});
            block->StoreUniform(Uniform{8, ValueType::U8},
                                Value{result}.SetCastType(ValueType::U8));
            return base;
        };
        const auto make_compare = [&] {
            const auto input = block->LoadUniform(ValueType::U8, Uniform{16, ValueType::U8});
            immediate = block->LoadImm(ValueType::U32, Imm{7u});
            sub = block->Sub(ValueType::U8, input, Operand{immediate});
            block->SaveFlags(sub, Flags::Carry);
        };
        Value base;
        if (compare_first) { make_compare(); base = make_carry(); }
        else { base = make_carry(); make_compare(); }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BitVector coverage(block->MaxInstrId());
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        compile::CompileWorkspace workspace;
        arm64::IntegerFlagsInputs inputs{coverage, analysis, analysis.PseudoFlags().View()};
        REQUIRE(arm64::HasNarrowIntegerFlagsCandidates(*block));
        const auto plan = arm64::LowerNarrowIntegerFlags(analysis, workspace, inputs);
        Facts facts;
        facts.carries =
                std::ranges::count_if(plan.actions, [](const auto& row) { return row.IsCarry(); });
        facts.comparisons = std::ranges::count_if(plan.actions,
                                                  [](const auto& row) { return row.IsCompare(); });
        facts.covered = plan.covered.size();
        if ((plan.Compare(sub.Id()) != nullptr)) {
            facts.immediate = plan.Compare(sub.Id())->argument;
            facts.width = plan.Compare(sub.Id())->Width();
        }
        facts.roles_ok = (plan.Carry(result.Id()) != nullptr) &&
                         plan.Carry(result.Id())->Source(*result.Def()).Def() == base.Def() &&
                         plan.Covered(test.Id()) && plan.Covered(carry.Id()) &&
                         plan.Covered(immediate.Id());
        return facts;
    };
    const auto forward = build(false);
    REQUIRE(forward.carries == 1);
    REQUIRE(forward.comparisons == 1);
    REQUIRE(forward.immediate == 7);
    REQUIRE(forward.width == 1);
    REQUIRE(forward.roles_ok);
    const auto swapped = build(true);
    REQUIRE(swapped.carries == forward.carries);
    REQUIRE(swapped.comparisons == forward.comparisons);
    REQUIRE(swapped.covered == forward.covered);
    REQUIRE(swapped.immediate == forward.immediate);
    REQUIRE(swapped.width == forward.width);
    REQUIRE(swapped.roles_ok);
}

TEST_CASE("narrow integer plans reject stale analyses and honor coverage and demand gates", "[narrow-integer-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbe48})};
    const auto input = block->LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
    const auto immediate = block->LoadImm(ValueType::U32, Imm{7u});
    const auto sub = block->Sub(ValueType::U8, input, Operand{immediate});
    block->SaveFlags(sub, Flags::Carry);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    BitVector coverage(block->MaxInstrId());
    compile::BlockAnalysisIndex analysis;
    analysis.Build(block.get(), nullptr, true);
    compile::CompileWorkspace workspace;
    arm64::IntegerFlagsInputs inputs{coverage, analysis, analysis.PseudoFlags().View()};
    IntrusivePtr<Block> other{new Block(0, Location{0xbe50})};
    other->LoadUniform(Uniform{0, ValueType::U64});
    other->SetTerminal(terminal::ReturnToHost{});
    other->ReIdInstr();
    compile::BlockAnalysisIndex stale;
    stale.Build(other.get());
    const auto foreign = arm64::LowerNarrowIntegerFlags(stale, workspace, inputs);
    REQUIRE(std::ranges::none_of(foreign.actions, [](const auto& row) { return row.IsCarry(); }));
    REQUIRE(std::ranges::none_of(foreign.actions, [](const auto& row) { return row.IsCompare(); }));
    REQUIRE(foreign.covered.empty());
    const auto fresh = arm64::LowerNarrowIntegerFlags(analysis, workspace, inputs);
    REQUIRE((fresh.Compare(sub.Id()) != nullptr));
    inputs.demands = {};
    REQUIRE_FALSE(arm64::LowerNarrowIntegerFlags(analysis, workspace, inputs).Compare(sub.Id()));
    IntrusivePtr<Block> carry_block{new Block(0, Location{0xbe54})};
    const auto carry_base = carry_block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    const auto carry_producer = carry_block->Add(ValueType::U32, carry_base, Operand{Imm{1u}});
    carry_block->SaveFlags(carry_producer, Flags::Carry);
    const auto carry_zero = carry_block->LoadImm(ValueType::U32, Imm{0u});
    const auto carry_test = carry_block->TestFlags(ValueType::U32, Flags::Carry);
    const auto carry_add = carry_block->Add(ValueType::U32, carry_zero, Operand{carry_test});
    const auto carry_result = carry_block->Add(ValueType::U32, carry_base, Operand{carry_add});
    carry_block->StoreUniform(Uniform{8, ValueType::U8},
                              Value{carry_result}.SetCastType(ValueType::U8));
    carry_block->StoreUniform(Uniform{16, ValueType::U32}, carry_zero);
    auto* zero_store = &carry_block->GetInstList().back();
    carry_block->SetTerminal(terminal::ReturnToHost{});
    carry_block->ReIdInstr();
    BitVector carry_coverage(carry_block->MaxInstrId());
    compile::BlockAnalysisIndex carry_analysis;
    carry_analysis.Build(carry_block.get(), nullptr, true);
    arm64::IntegerFlagsInputs carry_inputs{
            carry_coverage, carry_analysis, carry_analysis.PseudoFlags().View()};
    const auto uncovered = arm64::LowerNarrowIntegerFlags(carry_analysis, workspace, carry_inputs);
    REQUIRE((uncovered.Carry(carry_result.Id()) != nullptr));
    REQUIRE_FALSE(uncovered.Covered(carry_zero.Id()));
    carry_coverage.set(zero_store->Id());
    const auto covered = arm64::LowerNarrowIntegerFlags(carry_analysis, workspace, carry_inputs);
    REQUIRE((covered.Carry(carry_result.Id()) != nullptr));
    REQUIRE(covered.Covered(carry_zero.Id()));
    REQUIRE(covered.covered.size() == uncovered.covered.size() + 1);
    IntrusivePtr<Block> plain{new Block(0, Location{0xbe58})};
    const auto plain_value = plain->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    plain->StoreUniform(Uniform{8, ValueType::U32}, plain_value);
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    REQUIRE_FALSE(arm64::HasNarrowIntegerFlagsCandidates(*plain));
    BitVector plain_coverage(plain->MaxInstrId());
    compile::BlockAnalysisIndex plain_analysis;
    plain_analysis.Build(plain.get(), nullptr, true);
    arm64::IntegerFlagsInputs plain_inputs{
            plain_coverage, plain_analysis, plain_analysis.PseudoFlags().View()};
    const auto zero = arm64::LowerNarrowIntegerFlags(plain_analysis, workspace, plain_inputs);
    REQUIRE(std::ranges::none_of(zero.actions, [](const auto& row) { return row.IsCarry(); }));
    REQUIRE(std::ranges::none_of(zero.actions, [](const auto& row) { return row.IsCompare(); }));
    REQUIRE(zero.covered.empty());
}

namespace {
struct BranchFixture {
    IntrusivePtr<Block> block{new Block(0, Location{0x8a80})};
    Value loaded, condition;
    const Inst* publication{};
    std::vector<const Inst*> extra_clears;
    explicit BranchFixture(const FeatureSet& features, unsigned clear_count = 0) {
        auto base = block->GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
        loaded = block->LoadMemory(ValueType::U64, Operand{base});
        publication = block->AppendInst(OpCode::SetHostGPR, loaded, HostRegIndex(22), Imm{0u});
        block->AdvancePC(Imm{3u});
        auto left = block->BitCast(ValueType::U64, loaded);
        auto right = block->BitCast(ValueType::U64, loaded);
        auto tested = block->And(ValueType::U64, left, Operand{right});
        block->ClearFlags(Flags::Carry | Flags::Overflow | Flags::AuxiliaryCarry);
        block->SaveFlags(tested, Flags::Negate | Flags::Zero | Flags::Parity);
        block->AdvancePC(Imm{3u});
        block->BranchOnlyEdges();
        condition = block->LocalCondSet(ValueType::U8, Cond::EQ);
        block->SetTerminal(terminal::If{condition,
                                        terminal::LinkBlock{Location{0x8a90}},
                                        terminal::LinkBlock{Location{0x8aa0}}});
        swift::runtime::compile::EliminateFlags(RewriteFixture{block.get(), true, nullptr}.Get(), features);
        // Carry is dead on both successors. Keep a long sequence of redundant
        // writes to exercise the backend proof window independently of DCE.
        for (unsigned i = 0; i < clear_count; ++i) {
            auto* clear = block->AppendInst(OpCode::ClearFlags, Flags::Carry);
            block->RemoveInst(clear);
            block->InsertBefore(clear, condition.Def());
            extra_clears.push_back(clear);
        }
        block->ReIdInstr();
    }
};
}

TEST_CASE("integer branch plans reject stale analyses and foreign physical indexes",
          "[narrow-integer-plan][integer-branch]") {
    compile::CompileWorkspace predicate_workspace;
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config};
    const FeatureSet features{};
    struct Facts {
        bool has_value{}, narrow{};
        std::optional<swift::u16> zero_target;
        Flags required{};
        Cond raw{Cond::NE};
        size_t discarded{};
    };
    const auto build = [&](BranchFixture& fixture) {
        AllocationFixture alloc{fixture.block->MaxInstrId(),
                                space.GetTrampolines().GetGPRRegs(),
                                space.GetTrampolines().GetFPRRegs(),
                                features};
        ::swift::runtime::compile::AllocateBlock(
                fixture.block.get(),
                &alloc,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        fixture.block.get(), alloc, features),
                false);
        alloc.MapRegister(fixture.loaded.Id(), HostGPR{22});
        alloc.MarkHostWriteCoalesced(fixture.publication->Id());
        compile::SemanticFacts facts{fixture.block.get()};
        auto& analysis = facts.PrepareIndex(fixture.block.get());
        REQUIRE_FALSE(analysis.UsesReady());
        const auto plan = arm64::LowerIntegerBranch(fixture.block.get(),
                                                    analysis,
                                                    predicate_workspace,
                                                    alloc,
                                                    features,
                                                    alloc.PrepareFixedGPRHomes());
        REQUIRE_FALSE(analysis.UsesReady());
        Facts out;
        out.has_value = plan.has_value();
        if (plan) {
            out.narrow = plan->action.IsNarrow();
            out.zero_target = plan->action.ZeroTarget();
            out.required = plan->action.Required();
            out.raw = plan->action.raw_condition;
            out.discarded = plan->covered.size();
            REQUIRE(plan->action.condition == fixture.condition.Id());
            REQUIRE(std::ranges::is_sorted(plan->covered));
            for (const auto* clear : fixture.extra_clears)
                REQUIRE(std::binary_search(
                        plan->covered.begin(), plan->covered.end(), clear->Id()));
        }
        IntrusivePtr<Block> other{new Block(0, Location{0x8ab0})};
        other->LoadUniform(Uniform{0, ValueType::U64});
        other->SetTerminal(terminal::ReturnToHost{});
        other->ReIdInstr();
        compile::BlockAnalysisIndex stale_analysis;
        stale_analysis.Build(other.get());
        REQUIRE_FALSE(arm64::LowerIntegerBranch(fixture.block.get(),
                                                stale_analysis,
                                                predicate_workspace,
                                                alloc,
                                                features,
                                                alloc.PrepareFixedGPRHomes()));
        AllocationFixture foreign{fixture.block->MaxInstrId(),
                                  space.GetTrampolines().GetGPRRegs(),
                                  space.GetTrampolines().GetFPRRegs(),
                                  features};
        REQUIRE_FALSE(arm64::LowerIntegerBranch(fixture.block.get(),
                                                analysis,
                                                predicate_workspace,
                                                alloc,
                                                features,
                                                foreign.PrepareFixedGPRHomes()));
        return out;
    };
    BranchFixture first{features}, second{features, 64};
    const auto a = build(first);
    const auto b = build(second);
    REQUIRE(a.has_value);
    REQUIRE_FALSE(a.narrow);
    REQUIRE(a.zero_target == std::optional<swift::u16>{22});
    REQUIRE(b.has_value == a.has_value);
    REQUIRE(b.narrow == a.narrow);
    REQUIRE(b.zero_target == a.zero_target);
    REQUIRE(b.required == a.required);
    REQUIRE(b.raw == a.raw);
    REQUIRE(b.discarded == a.discarded + 64);
}

TEST_CASE("integer branch plans follow allocation lifecycle without leaking decisions", "[narrow-integer-plan][integer-branch]") {
    compile::CompileWorkspace predicate_workspace;
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config};
    const FeatureSet features{};
    BranchFixture fixture{features};
    AllocationFixture alloc{fixture.block->MaxInstrId(),
                            space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateBlock(
            fixture.block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    fixture.block.get(), alloc, features),
            false);
    alloc.MarkHostWriteCoalesced(fixture.publication->Id());
    compile::SemanticFacts facts{fixture.block.get()};
    auto& analysis = facts.PrepareIndex(fixture.block.get());
    const auto build = [&] {
        const auto result = arm64::LowerIntegerBranch(fixture.block.get(),
                                                      analysis,
                                                      predicate_workspace,
                                                      alloc,
                                                      features,
                                                      alloc.PrepareFixedGPRHomes());
        REQUIRE_FALSE(analysis.UsesReady());
        return result;
    };
    alloc.MapRegister(fixture.loaded.Id(), HostGPR{22});
    const auto fresh = build();
    REQUIRE(fresh);
    REQUIRE(fresh->action.ZeroTarget() == std::optional<swift::u16>{22});
    alloc.MapRegister(fixture.loaded.Id(), HostGPR{23});
    REQUIRE_FALSE(build());
    alloc.MapMemSpill(fixture.loaded.Id(), SpillSlot{2});
    REQUIRE_FALSE(build());
    alloc.MapRegister(fixture.loaded.Id(), HostGPR{22});
    const auto restored = build();
    REQUIRE(restored);
    REQUIRE(restored->action.ZeroTarget() == fresh->action.ZeroTarget());
    REQUIRE(restored->action.Required() == fresh->action.Required());
    REQUIRE(restored->action.raw_condition == fresh->action.raw_condition);
    REQUIRE(restored->action.condition == fresh->action.condition);
    REQUIRE(restored->action.producer == fresh->action.producer);
    REQUIRE(restored->covered.size() == fresh->covered.size());
    REQUIRE(restored->action.IsNarrow() == fresh->action.IsNarrow());
    alloc.BeginTargetPlanning();
    alloc.StoreIntegerBranch(*restored);
    alloc.DeclareBlock(fixture.block.get(), analysis.Summary());
    alloc.EndTargetPlanning();
    REQUIRE(alloc.Actions().IntegerBranch(fixture.block.get()));
    REQUIRE(*alloc.Actions().IntegerBranch(fixture.block.get()) == restored->action);
    BranchFixture foreign{features};
    REQUIRE(foreign.condition.Id() == fixture.condition.Id());
    REQUIRE_FALSE(alloc.Actions().IntegerBranch(foreign.block.get()));
    alloc.ResetAllocations();
    REQUIRE_FALSE(alloc.Actions().IntegerBranch(fixture.block.get()));
}

TEST_CASE("narrow carry proofs isolate selections and reject dead-use cycles",
          "[narrow-integer-plan][regression]") {
    for (const unsigned count : {2u, 300u})
        for (const bool cycle : {false, true}) {
            CAPTURE(count, cycle);
            compile::CompileWorkspace workspace;
            IntrusivePtr<Block> block{new Block{0, Location{0xbec0}}};
            const auto base = block->LoadUniform<U32>(Uniform{0, ValueType::U32});
            const auto zero = block->LoadImm<U32>(Imm{0u});
            const auto first_dead = block->Add<U32>(zero, Operand{Imm{0u}});
            const auto last_dead = block->Add<U32>(first_dead, Operand{Imm{0u}});
            if (cycle) {
                compile::RewriteContext rewrite{block.get(), workspace, nullptr, false};
                rewrite.SetArg(block.get(), first_dead.Def(), 1, Operand{last_dead});
            }
            std::vector<Value> results, tests, adds;
            for (unsigned i = 0; i < count; ++i) {
                const auto producer = block->Add<U32>(base, Operand{Imm{1u}});
                block->SaveFlags(producer, Flags::Carry);
                tests.push_back(block->TestFlags<U32>(Flags::Carry).SetCastType(ValueType::U32));
                adds.push_back(block->Add<U32>(zero, Operand{tests.back()}));
                results.push_back(block->Add<U32>(base, Operand{adds.back()}));
                block->StoreUniform(Uniform{8, ValueType::U8},
                                    Value{results.back()}.SetCastType(ValueType::U8));
            }
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            if (count == 2) {
                unsigned id = 50000;
                compile::RewriteContext rewrite{block.get(), workspace, nullptr, false};
                for (const auto& inst : block->GetInstList()) {
                    rewrite.Change(block.get(), &inst, [&](Inst& target) { target.SetId(id); });
                    ++id;
                }
            }
            BitVector coverage(65536);
            compile::BlockAnalysisIndex analysis;
            analysis.Build(block.get(), nullptr, true);
            arm64::IntegerFlagsInputs inputs{coverage, analysis, analysis.PseudoFlags().View()};
            const auto plan = arm64::LowerNarrowIntegerFlags(analysis, workspace, inputs);
            REQUIRE(plan.actions.size() == count);
            for (unsigned i = 0; i < count; ++i) {
                REQUIRE(plan.Carry(results[i].Id()));
                REQUIRE(plan.Carry(results[i].Id())->Source(*results[i].Def()).Def() == base.Def());
                REQUIRE(plan.Covered(tests[i].Id()));
                REQUIRE(plan.Covered(adds[i].Id()));
            }
            REQUIRE(plan.Covered(zero.Id()) == !cycle);
            REQUIRE_FALSE(plan.Covered(first_dead.Id()));
            REQUIRE_FALSE(plan.Covered(last_dead.Id()));
            REQUIRE(std::adjacent_find(plan.covered.begin(), plan.covered.end()) ==
                    plan.covered.end());
        }
}

TEST_CASE("narrow compare demand uses only later selected carry tests",
          "[narrow-integer-plan][regression]") {
    for (const bool reject_second : {false, true}) {
        CAPTURE(reject_second);
        compile::CompileWorkspace workspace;
        IntrusivePtr<Block> block{new Block{0, Location{0xbed0}}};
        const auto base = block->LoadUniform<U32>(Uniform{0, ValueType::U32});
        const auto narrow = block->LoadUniform<U8>(Uniform{8, ValueType::U8});
        const auto compare = [&] {
            const auto result = block->Sub<U8>(narrow, Operand{Imm{7u}});
            block->SaveFlags(result, Flags::All);
            return result;
        };
        const auto carry = [&](bool rejected) {
            const auto producer = block->Add<U32>(base, Operand{Imm{1u}});
            block->SaveFlags(producer, Flags::Carry);
            const auto zero = block->LoadImm<U32>(Imm{0u});
            const auto test = block->TestFlags<U32>(Flags::Carry).SetCastType(ValueType::U32);
            const auto add = block->Add<U32>(zero, Operand{test});
            const auto result = block->Add<U32>(base, Operand{add});
            const auto type = rejected ? ValueType::U32 : ValueType::U8;
            block->StoreUniform(Uniform{16, type}, Value{result}.SetCastType(type));
            return result;
        };
        const auto first = compare();
        const auto first_carry = carry(false);
        const auto middle = compare();
        const auto second_carry = carry(reject_second);
        const auto last = compare();
        const auto overwrite = block->Add<U32>(base, Operand{Imm{2u}});
        block->SaveFlags(overwrite, Flags::All);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        BitVector coverage(block->MaxInstrId());
        arm64::IntegerFlagsInputs inputs{coverage, analysis, analysis.PseudoFlags().View()};
        const auto plan = arm64::LowerNarrowIntegerFlags(analysis, workspace, inputs);
        REQUIRE(plan.Carry(first_carry.Id()));
        REQUIRE((plan.Carry(second_carry.Id()) != nullptr) == !reject_second);
        REQUIRE(plan.Compare(first.Id()));
        REQUIRE((plan.Compare(middle.Id()) != nullptr) == !reject_second);
        REQUIRE_FALSE(plan.Compare(last.Id()));
    }
}

TEST_CASE("narrow flags borrow packed demand with producer identity and branch overrides",
          "[narrow-integer-plan][regression]") {
    for (const bool sparse : {false, true}) {
        CAPTURE(sparse);
        compile::CompileWorkspace workspace;
        std::array<IntrusivePtr<Block>, 2> blocks;
        std::array<Value, 2> comparisons;
        std::array<compile::BlockAnalysisIndex, 2> indices;
        for (unsigned i = 0; i < blocks.size(); ++i) {
            blocks[i] = IntrusivePtr<Block>{new Block{0, Location{0xbf10 + i * 16}}};
            const auto input = blocks[i]->LoadUniform<U8>(Uniform{0, ValueType::U8});
            comparisons[i] = blocks[i]->Sub<U8>(input, Operand{Imm{7u}});
            blocks[i]->SaveFlags(comparisons[i], Flags::Carry);
            blocks[i]->SetTerminal(terminal::ReturnToHost{});
            blocks[i]->ReIdInstr();
            if (sparse) {
                unsigned id = 200;
                compile::RewriteContext rewrite{blocks[i].get(), workspace, nullptr, false};
                for (const auto& inst : blocks[i]->GetInstList()) {
                    rewrite.Change(blocks[i].get(), &inst, [&](Inst& target) { target.SetId(id); });
                    id += 3;
                }
            }
            indices[i].Build(blocks[i].get(), nullptr, true);
        }
        REQUIRE(comparisons[0].Id() == comparisons[1].Id());
        BitVector coverage(256);
        arm64::IntegerFlagsInputs inputs{coverage, indices[0], indices[0].PseudoFlags().View()};
        REQUIRE(inputs.demands.For(comparisons[1].Def()).set == Flags::Carry);
        REQUIRE(arm64::LowerNarrowIntegerFlags(indices[0], workspace, inputs).Compare(comparisons[0].Id()));
        REQUIRE_FALSE(
                arm64::LowerNarrowIntegerFlags(indices[1], workspace, inputs).Compare(comparisons[1].Id()));
        arm64::IntegerBranchAction branch{.producer = static_cast<swift::u16>(comparisons[0].Id()),
                                          .condition = 0,
                                          .raw_condition = Cond::EQ,
                                          .required_mask = static_cast<swift::u8>(Flags::Carry)};
        inputs.selected_branch = &branch;
        REQUIRE_FALSE(
                arm64::LowerNarrowIntegerFlags(indices[0], workspace, inputs).Compare(comparisons[0].Id()));
        ++branch.producer;
        REQUIRE(arm64::LowerNarrowIntegerFlags(indices[0], workspace, inputs).Compare(comparisons[0].Id()));
        inputs.selected_branch = nullptr;
        inputs.demands = {};
        REQUIRE_FALSE(
                arm64::LowerNarrowIntegerFlags(indices[0], workspace, inputs).Compare(comparisons[0].Id()));
        REQUIRE(indices[0].PseudoFlags().For(comparisons[0].Def()).set == Flags::Carry);
    }
}

TEST_CASE("integer flags actions preserve sealed input views and receiving identity",
          "[narrow-integer-plan][regression]") {
    for (const bool low : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbf04})};
        const auto source = block->LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
        const auto view = Value{source}.SetCastType(ValueType::U64);
        const auto extract = block->BitExtract(ValueType::U8, view, Imm{0u}, Imm{8u});
        const auto compare = block->Sub(ValueType::U8, extract, Operand{Imm{7u}});
        block->SaveFlags(compare, Flags::Carry);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture alloc{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        alloc.MapRegister(source.Id(), HostGPR{13});
        if (low) alloc.AssignGPRLowView(extract.Id(), 13);
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        BitVector coverage(block->MaxInstrId());
        compile::CompileWorkspace workspace;
        const auto lowered = arm64::LowerIntegerFlags(
                analysis, workspace, alloc, analysis.PseudoFlags(), nullptr, coverage, nullptr, false);
        const auto* input = lowered.Input(extract.Id());
        const auto* comparison = lowered.Compare(compare.Id());
        REQUIRE(input);
        REQUIRE(comparison);
        const auto expected = low ? Value{extract} : view;
        REQUIRE(input->Source(*compare.Def()).Def() == expected.Def());
        REQUIRE(input->Source(*compare.Def()).Type() == expected.Type());
        REQUIRE(comparison->Source(*compare.Def()).Def() == expected.Def());
        REQUIRE(comparison->Source(*compare.Def()).Type() == expected.Type());
        REQUIRE(comparison->argument == 7);
        REQUIRE(comparison->Width() == 1);
        IntrusivePtr<Block> foreign{new Block(0, Location{0xbf08})};
        const auto other_source = foreign->LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
        const auto other_extract =
                foreign->BitExtract(ValueType::U8, other_source, Imm{0u}, Imm{8u});
        foreign->SetTerminal(terminal::ReturnToHost{});
        foreign->ReIdInstr();
        REQUIRE(other_extract.Id() == extract.Id());
        REQUIRE_FALSE(input->MatchesInput(*compare.Def(), other_extract));
        alloc.BeginTargetPlanning();
        alloc.StoreIntegerFlags(lowered);
        alloc.EndTargetPlanning();
        REQUIRE(alloc.Actions().Ready());
        REQUIRE(alloc.Actions().FlagsInput(extract.Id()));
        REQUIRE_FALSE(alloc.Actions().NarrowCarry(extract.Id()));
        REQUIRE_FALSE(alloc.Actions().NarrowCompare(extract.Id()));
        REQUIRE(alloc.Actions().NarrowCompare(compare.Id()));
        REQUIRE_FALSE(alloc.Actions().FlagsInput(compare.Id()));
        REQUIRE_FALSE(alloc.Actions().IntegerFlags(UINT32_MAX));
        alloc.ResetAllocations();
        REQUIRE_FALSE(alloc.Actions().FlagsInput(extract.Id()));
        REQUIRE_FALSE(alloc.Actions().NarrowCompare(compare.Id()));
    }
}
