#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include "runtime/backend/arm64/alloc/narrow_integer_flags_plan.h"
#include "runtime/backend/arm64/alloc/integer_branch_plan.h"
#include "runtime/backend/arm64/alloc/gpr_block_plan.h"
#include "runtime/ir/opts/flags_elimination_pass.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "runtime/ir/hir_builder.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

namespace {
void CaptureDemands(Block& block, arm64::NarrowIntegerFlagsInputs& inputs) {
    for (auto& inst : block.GetInstList()) {
        arm64::StaticFlagDemand demand;
        for (auto* pseudo : CollectPseudoOperations(block, &inst)) {
            if (pseudo->GetOp() == OpCode::SaveFlags) demand.set |= pseudo->GetArg<Flags>(1);
            else if (pseudo->GetOp() == OpCode::BranchOnlyFlags) {
                demand.set |= pseudo->GetArg<Flags>(1); demand.branch_only = true;
            } else if (pseudo->GetOp() == OpCode::ClearFlags) demand.clear |= pseudo->GetArg<Flags>(0);
        }
        if (!demand.Null()) inputs.demands.emplace(&inst, demand);
    }
}
}

TEST_CASE("narrow carry plans preserve carry availability and all use views", "[narrow-integer-plan]") {
    for (unsigned scenario = 0; scenario < 10; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbe00})};
        const auto base = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
        const auto producer = (scenario == 2 ? block->Sub(base, Operand{Imm{1u}})
                                            : block->Add(base, Operand{Imm{1u}})).SetType(ValueType::U32);
        if (scenario != 1) block->SaveFlags(producer, Flags::Carry);
        if (scenario == 3) block->ClearFlags(Flags::Carry);
        if (scenario == 4) (void)block->CallLambda(Lambda{Imm{swift::u64{1}}});
        if (scenario == 5) block->AdvancePC(Imm{swift::u64{1}});
        const auto zero = block->LoadImm(Imm{0u}).SetType(ValueType::U32);
        const auto test = block->TestFlags(Flags::Carry).SetType(ValueType::U32);
        const auto operand = scenario == 9 ? Operand{test, Imm{1u}, OperandOp{OperandOp::LSL}} : Operand{test};
        const auto carry = block->Add(zero, operand).SetType(ValueType::U32);
        const auto result = block->Add(base, Operand{carry}).SetType(ValueType::U32);
        if (scenario == 7) block->SaveFlags(result, Flags::Zero);
        const auto type = scenario == 6 ? ValueType::U32 : ValueType::U8;
        block->StoreUniform(Uniform{8, type}, Value{result}.SetCastType(type));
        if (scenario == 8) block->StoreUniform(Uniform{16, ValueType::U32}, zero);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BitVector coverage(block->MaxInstrId());
        arm64::NarrowIntegerFlagsInputs inputs{coverage};
        inputs.canonical_carry = scenario == 5;
        CaptureDemands(*block, inputs);
        arm64::BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        REQUIRE(arm64::HasNarrowIntegerFlagsCandidates(*block) == (scenario != 9));
        const auto plan = arm64::BuildNarrowIntegerFlagsPlan(analysis, inputs);
        const bool accepted = scenario == 0 || scenario == 8;
        REQUIRE(plan.carries.contains(result.Def()) == accepted);
        REQUIRE(plan.covered.contains(test.Def()) == accepted);
        REQUIRE(plan.covered.contains(carry.Def()) == accepted);
        REQUIRE(plan.covered.contains(zero.Def()) == (scenario == 0));
        if (accepted) REQUIRE(plan.carries.at(result.Def()).value.Def() == base.Def());
    }
}

TEST_CASE("narrow compare plans require proven narrow inputs and carry-only demand", "[narrow-integer-plan]") {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbe08})};
        const auto input = block->LoadUniform(Uniform{0, scenario == 1 ? ValueType::U32 : ValueType::U8})
                .SetType(scenario == 1 ? ValueType::U32 : ValueType::U8);
        const auto immediate = block->LoadImm(Imm{scenario == 2 ? 256u : 7u}).SetType(ValueType::U32);
        const auto sub = block->Sub(input, Operand{immediate}).SetType(ValueType::U8);
        block->SaveFlags(sub, scenario == 3 ? Flags::All : Flags::Carry);
        if (scenario == 4) block->StoreUniform(Uniform{8, ValueType::U32}, immediate);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BitVector coverage(block->MaxInstrId());
        arm64::NarrowIntegerFlagsInputs inputs{coverage};
        CaptureDemands(*block, inputs);
        if (scenario == 5) inputs.excluded_comparisons.insert(sub.Def());
        arm64::BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        const auto plan = arm64::BuildNarrowIntegerFlagsPlan(analysis, inputs);
        const bool accepted = scenario == 0;
        REQUIRE(plan.comparisons.contains(sub.Def()) == accepted);
        REQUIRE(plan.covered.contains(immediate.Def()) == accepted);
        if (accepted) {
            REQUIRE(plan.comparisons.at(sub.Def()).immediate == 7);
            REQUIRE(plan.comparisons.at(sub.Def()).width == 1);
        }
    }
}

TEST_CASE("narrow compare extra flags must be overwritten before reads or faults", "[narrow-integer-plan]") {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbe10})};
        const auto narrow = block->LoadUniform(Uniform{0, ValueType::U8}).SetType(ValueType::U8);
        const auto base = block->LoadUniform(Uniform{8, ValueType::U32}).SetType(ValueType::U32);
        const auto sub = block->Sub(narrow, Operand{Imm{7u}}).SetType(ValueType::U8);
        block->SaveFlags(sub, Flags::All);
        block->InvertCarry();
        const auto zero = block->LoadImm(Imm{0u}).SetType(ValueType::U32);
        const auto test = block->TestFlags(Flags::Carry).SetType(ValueType::U32);
        const auto carry = block->Add(zero, Operand{test}).SetType(ValueType::U32);
        const auto result = block->Add(base, Operand{carry}).SetType(ValueType::U32);
        block->StoreUniform(Uniform{16, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
        if (scenario == 1) (void)block->TestFlags(Flags::Parity);
        if (scenario == 2) (void)block->LoadMemory(Operand{Imm{swift::u64{0}}});
        if (scenario != 3) {
            const auto overwrite = block->Add(base, Operand{Imm{1u}}).SetType(ValueType::U32);
            block->SaveFlags(overwrite, Flags::All);
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BitVector coverage(block->MaxInstrId());
        arm64::NarrowIntegerFlagsInputs inputs{coverage};
        CaptureDemands(*block, inputs);
        arm64::BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        const auto plan = arm64::BuildNarrowIntegerFlagsPlan(analysis, inputs);
        REQUIRE(plan.carries.contains(result.Def()));
        REQUIRE(plan.comparisons.contains(sub.Def()) == (scenario == 0));
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
    const auto input = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
    const auto base = block->LoadUniform(Uniform{8, ValueType::U32}).SetType(ValueType::U32);
    const auto producer = block->Add(input, Operand{Imm{1u}}).SetType(ValueType::U32);
    block->SaveFlags(producer, Flags::Carry);
    const auto zero = block->LoadImm(Imm{0u}).SetType(ValueType::U32);
    const auto carry = block->TestFlags(Flags::Carry).SetType(ValueType::U32);
    const auto shifted = block->Add(zero, Operand{carry, Imm{1u}, OperandOp{OperandOp::LSL}}).SetType(ValueType::U32);
    const auto result = block->Add(base, Operand{shifted}).SetType(ValueType::U32);
    block->StoreUniform(Uniform{16, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    FeatureSet features{};
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    REQUIRE_NOTHROW(RegisterAllocPass::Run(block.get(), &allocation, false, features));
    CAPTURE(shifted.Def()->GetArg<Operand>(1).GetOp().type);
    std::unique_ptr<arm64::JitContext> context_owner;
    REQUIRE_NOTHROW(allocation.CompleteGPRAllocation(module));
    REQUIRE_NOTHROW(context_owner = std::make_unique<arm64::JitContext>(module, allocation));
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

TEST_CASE("RegAlloc freezes narrow integer actions before encoder construction", "[narrow-integer-plan][target-planner]") {
    for (unsigned shape : {0u, 1u, 2u}) {
        CAPTURE(shape);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 64; config.stack_alignment = 16;
        AddressSpace space{config}; auto module = space.GetDefaultModule(); FeatureSet features{};
        IntrusivePtr<Block> block{new Block(0, Location{0xbe30})}; Inst* selected{};
        if (shape == 1) {
            const auto base = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
            const auto producer = block->Add(base, Operand{Imm{1u}}).SetType(ValueType::U32);
            block->SaveFlags(producer, Flags::Carry);
            const auto zero = block->LoadImm(Imm{0u}).SetType(ValueType::U32);
            const auto carry = block->TestFlags(Flags::Carry).SetType(ValueType::U32);
            const auto added = block->Add(zero, Operand{carry}).SetType(ValueType::U32);
            const auto result = block->Add(base, Operand{added}).SetType(ValueType::U32);
            block->StoreUniform(Uniform{8, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
            selected = result.Def();
        } else {
            const auto type = shape == 2 ? ValueType::U32 : ValueType::U8;
            const auto input = block->LoadUniform(Uniform{0, type}).SetType(type);
            const auto immediate = block->LoadImm(Imm{7u}).SetType(ValueType::U32);
            const auto sub = block->Sub(input, Operand{immediate}).SetType(ValueType::U8);
            block->SaveFlags(sub, Flags::Carry); selected = sub.Def();
        }
        block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        allocation.CompleteGPRAllocation(module);
        const auto plan = allocation.GPRPlanFor(block.get()); REQUIRE(plan);
        if (shape == 0) {
            REQUIRE(plan->integer_flags);
            REQUIRE(plan->integer_flags->integer.comparisons.contains(selected));
            REQUIRE(plan->integer_flags->integer.comparisons.at(selected).immediate == 7);
        } else if (shape == 1) {
            REQUIRE(plan->integer_flags);
            REQUIRE(plan->integer_flags->integer.carries.contains(selected));
        } else REQUIRE_FALSE(plan->integer_flags);
        arm64::JitContext context{module, allocation}; arm64::JitTranslator translator{context};
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
    const auto address = function->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    const auto load = function->LoadMemory(Operand{address}).SetType(ValueType::U8);
    const auto extension = function->ZeroExtend32(load).SetType(ValueType::U32);
    function->StoreUniform(Uniform{24, ValueType::U32}, extension);
    auto* carry_block = builder.LinkBlock(terminal::LinkBlock{Location{0xbe41}});
    builder.SetCurBlock(carry_block);
    const auto base = function->LoadUniform(Uniform{8, ValueType::U32}).SetType(ValueType::U32);
    const auto producer = function->Add(base, Operand{Imm{1u}}).SetType(ValueType::U32);
    function->SaveFlags(producer, Flags::Carry);
    const auto zero = function->LoadImm(Imm{0u}).SetType(ValueType::U32);
    const auto test = function->TestFlags(Flags::Carry).SetType(ValueType::U32);
    const auto carry = function->Add(zero, Operand{test}).SetType(ValueType::U32);
    const auto result = function->Add(base, Operand{carry}).SetType(ValueType::U32);
    function->StoreUniform(Uniform{12, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
    auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0xbe42}});
    builder.SetCurBlock(next);
    function->StoreUniform(Uniform{16, ValueType::U32}, zero);
    function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
    function->ComputeRPO(); function->IdByRPO();
    RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(function, &allocation, false, features);
    allocation.CompleteGPRAllocation(module);
    const auto plan = allocation.GPRPlanFor(carry_block->GetBlock());
    REQUIRE(plan); REQUIRE(plan->integer_flags);
    REQUIRE(plan->integer_flags->integer.carries.contains(result.Def()));
    arm64::JitContext context{module, allocation}; arm64::JitTranslator translator{context};
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
    REQUIRE_FALSE(plan->integer_flags->integer.covered.contains(zero.Def()));
}

TEST_CASE("narrow carry keeps a zero chain observed by its terminal", "[narrow-integer-plan][cfg]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbe50})};
    const auto base = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
    const auto producer = block->Add(base, Operand{Imm{1u}}).SetType(ValueType::U32);
    block->SaveFlags(producer, Flags::Carry);
    const auto zero = block->LoadImm(Imm{0u}).SetType(ValueType::U32);
    const auto test = block->TestFlags(Flags::Carry).SetType(ValueType::U32);
    const auto carry = block->Add(zero, Operand{test}).SetType(ValueType::U32);
    const auto result = block->Add(base, Operand{carry}).SetType(ValueType::U32);
    block->StoreUniform(Uniform{8, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
    block->SetTerminal(terminal::If{BOOL{Value{zero}.SetCastType(ValueType::U8)},
            terminal::ReturnToHost{}, terminal::ReturnToHost{}}); block->ReIdInstr();
    BitVector coverage(block->MaxInstrId()); arm64::NarrowIntegerFlagsInputs inputs{coverage};
    CaptureDemands(*block, inputs); arm64::BlockAnalysisIndex analysis; analysis.Build(block.get());
    REQUIRE(analysis.DirectUseCount(zero.Def()) == 1);
    REQUIRE(zero.Def()->GetUses(false) == 2);
    const auto plan = arm64::BuildNarrowIntegerFlagsPlan(analysis, inputs);
    REQUIRE(plan.carries.contains(result.Def()));
    REQUIRE_FALSE(plan.covered.contains(zero.Def()));
}

TEST_CASE("narrow carry retains observable alias and pseudo uses of its zero chain", "[narrow-integer-plan][cfg]") {
    for (const bool pseudo : {false, true}) {
        CAPTURE(pseudo);
        IntrusivePtr<Block> block{new Block(0, Location{0xbe60})};
        const auto base = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
        const auto producer = block->Add(base, Operand{Imm{1u}}).SetType(ValueType::U32);
        block->SaveFlags(producer, Flags::Carry);
        const auto zero = block->LoadImm(Imm{0u}).SetType(ValueType::U32);
        const auto alias = block->BitCast(zero).SetType(ValueType::U32);
        if (pseudo) block->SaveFlags(alias, Flags::Zero);
        const auto test = block->TestFlags(Flags::Carry).SetType(ValueType::U32);
        const auto carry = block->Add(zero, Operand{test}).SetType(ValueType::U32);
        const auto result = block->Add(base, Operand{carry}).SetType(ValueType::U32);
        block->StoreUniform(Uniform{8, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
        if (pseudo) block->SetTerminal(terminal::ReturnToHost{});
        else block->SetTerminal(terminal::If{BOOL{Value{alias}.SetCastType(ValueType::U8)},
                terminal::ReturnToHost{}, terminal::ReturnToHost{}});
        block->ReIdInstr();
        BitVector coverage(block->MaxInstrId()); arm64::NarrowIntegerFlagsInputs inputs{coverage};
        CaptureDemands(*block, inputs); arm64::BlockAnalysisIndex analysis; analysis.Build(block.get());
        REQUIRE(analysis.DirectUseCount(zero.Def()) == zero.Def()->GetUses(false));
        const auto plan = arm64::BuildNarrowIntegerFlagsPlan(analysis, inputs);
        REQUIRE(plan.carries.contains(result.Def()));
        REQUIRE_FALSE(plan.covered.contains(zero.Def()));
        REQUIRE_FALSE(plan.covered.contains(alias.Def()));
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
            const auto base = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
            const auto producer = block->Add(base, Operand{Imm{1u}}).SetType(ValueType::U32);
            block->SaveFlags(producer, Flags::Carry);
            const auto zero = block->LoadImm(Imm{0u}).SetType(ValueType::U32);
            test = block->TestFlags(Flags::Carry).SetType(ValueType::U32);
            carry = block->Add(zero, Operand{test}).SetType(ValueType::U32);
            result = block->Add(base, Operand{carry}).SetType(ValueType::U32);
            block->StoreUniform(Uniform{8, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
            return base;
        };
        const auto make_compare = [&] {
            const auto input = block->LoadUniform(Uniform{16, ValueType::U8}).SetType(ValueType::U8);
            immediate = block->LoadImm(Imm{7u}).SetType(ValueType::U32);
            sub = block->Sub(input, Operand{immediate}).SetType(ValueType::U8);
            block->SaveFlags(sub, Flags::Carry);
        };
        Value base;
        if (compare_first) { make_compare(); base = make_carry(); }
        else { base = make_carry(); make_compare(); }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BitVector coverage(block->MaxInstrId());
        arm64::NarrowIntegerFlagsInputs inputs{coverage};
        CaptureDemands(*block, inputs);
        arm64::BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        REQUIRE(arm64::HasNarrowIntegerFlagsCandidates(*block));
        const auto plan = arm64::BuildNarrowIntegerFlagsPlan(analysis, inputs);
        Facts facts;
        facts.carries = plan.carries.size();
        facts.comparisons = plan.comparisons.size();
        facts.covered = plan.covered.size();
        if (plan.comparisons.contains(sub.Def())) {
            facts.immediate = plan.comparisons.at(sub.Def()).immediate;
            facts.width = plan.comparisons.at(sub.Def()).width;
        }
        facts.roles_ok = plan.carries.contains(result.Def()) &&
                plan.carries.at(result.Def()).value.Def() == base.Def() &&
                plan.carries.at(result.Def()).carry_test == test.Def() &&
                plan.carries.at(result.Def()).carry_add == carry.Def() &&
                plan.covered.contains(test.Def()) && plan.covered.contains(carry.Def()) &&
                plan.covered.contains(immediate.Def());
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
    const auto input = block->LoadUniform(Uniform{0, ValueType::U8}).SetType(ValueType::U8);
    const auto immediate = block->LoadImm(Imm{7u}).SetType(ValueType::U32);
    const auto sub = block->Sub(input, Operand{immediate}).SetType(ValueType::U8);
    block->SaveFlags(sub, Flags::Carry);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    BitVector coverage(block->MaxInstrId());
    arm64::NarrowIntegerFlagsInputs inputs{coverage};
    CaptureDemands(*block, inputs);
    IntrusivePtr<Block> other{new Block(0, Location{0xbe50})};
    other->LoadUniform(Uniform{0, ValueType::U64});
    other->SetTerminal(terminal::ReturnToHost{});
    other->ReIdInstr();
    arm64::BlockAnalysisIndex stale;
    stale.Build(other.get());
    const auto foreign = arm64::BuildNarrowIntegerFlagsPlan(stale, inputs);
    REQUIRE(foreign.carries.empty());
    REQUIRE(foreign.comparisons.empty());
    REQUIRE(foreign.covered.empty());
    arm64::BlockAnalysisIndex analysis;
    analysis.Build(block.get());
    const auto fresh = arm64::BuildNarrowIntegerFlagsPlan(analysis, inputs);
    REQUIRE(fresh.comparisons.contains(sub.Def()));
    inputs.demands.erase(sub.Def());
    REQUIRE(arm64::BuildNarrowIntegerFlagsPlan(analysis, inputs).comparisons.empty());
    IntrusivePtr<Block> carry_block{new Block(0, Location{0xbe54})};
    const auto carry_base = carry_block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
    const auto carry_producer = carry_block->Add(carry_base, Operand{Imm{1u}}).SetType(ValueType::U32);
    carry_block->SaveFlags(carry_producer, Flags::Carry);
    const auto carry_zero = carry_block->LoadImm(Imm{0u}).SetType(ValueType::U32);
    const auto carry_test = carry_block->TestFlags(Flags::Carry).SetType(ValueType::U32);
    const auto carry_add = carry_block->Add(carry_zero, Operand{carry_test}).SetType(ValueType::U32);
    const auto carry_result = carry_block->Add(carry_base, Operand{carry_add}).SetType(ValueType::U32);
    carry_block->StoreUniform(Uniform{8, ValueType::U8}, Value{carry_result}.SetCastType(ValueType::U8));
    carry_block->StoreUniform(Uniform{16, ValueType::U32}, carry_zero);
    auto* zero_store = &carry_block->GetInstList().back();
    carry_block->SetTerminal(terminal::ReturnToHost{});
    carry_block->ReIdInstr();
    BitVector carry_coverage(carry_block->MaxInstrId());
    arm64::NarrowIntegerFlagsInputs carry_inputs{carry_coverage};
    CaptureDemands(*carry_block, carry_inputs);
    arm64::BlockAnalysisIndex carry_analysis;
    carry_analysis.Build(carry_block.get());
    const auto uncovered = arm64::BuildNarrowIntegerFlagsPlan(carry_analysis, carry_inputs);
    REQUIRE(uncovered.carries.contains(carry_result.Def()));
    REQUIRE_FALSE(uncovered.covered.contains(carry_zero.Def()));
    REQUIRE(uncovered.carries.at(carry_result.Def()).dead_inputs.empty());
    carry_coverage.set(zero_store->Id());
    const auto covered = arm64::BuildNarrowIntegerFlagsPlan(carry_analysis, carry_inputs);
    REQUIRE(covered.carries.contains(carry_result.Def()));
    REQUIRE(covered.covered.contains(carry_zero.Def()));
    REQUIRE(covered.carries.at(carry_result.Def()).dead_inputs.size() == 1);
    REQUIRE(covered.carries.at(carry_result.Def()).dead_inputs.front() == carry_zero.Def());
    IntrusivePtr<Block> plain{new Block(0, Location{0xbe58})};
    const auto plain_value = plain->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
    plain->StoreUniform(Uniform{8, ValueType::U32}, plain_value);
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    REQUIRE_FALSE(arm64::HasNarrowIntegerFlagsCandidates(*plain));
    BitVector plain_coverage(plain->MaxInstrId());
    arm64::NarrowIntegerFlagsInputs plain_inputs{plain_coverage};
    arm64::BlockAnalysisIndex plain_analysis;
    plain_analysis.Build(plain.get());
    const auto zero = arm64::BuildNarrowIntegerFlagsPlan(plain_analysis, plain_inputs);
    REQUIRE(zero.carries.empty());
    REQUIRE(zero.comparisons.empty());
    REQUIRE(zero.covered.empty());
}

namespace {
struct BranchFixture {
    IntrusivePtr<Block> block{new Block(0, Location{0x8a80})};
    Value loaded, condition;
    Inst* publication{};
    explicit BranchFixture(const FeatureSet& features) {
        auto base = block->GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(ValueType::U64);
        loaded = block->LoadMemory(Operand{base}).SetType(ValueType::U64);
        publication = block->AppendInst(OpCode::SetHostGPR, loaded, HostRegIndex(22), Imm{0u});
        block->AdvancePC(Imm{3u});
        auto left = block->BitCast(loaded).SetType(ValueType::U64);
        auto right = block->BitCast(loaded).SetType(ValueType::U64);
        auto tested = block->And(left, Operand{right}).SetType(ValueType::U64);
        block->ClearFlags(Flags::Carry | Flags::Overflow | Flags::AuxiliaryCarry);
        block->SaveFlags(tested, Flags::Negate | Flags::Zero | Flags::Parity);
        block->AdvancePC(Imm{3u});
        block->BranchOnlyEdges();
        condition = block->LocalCondSet(Cond::EQ).SetType(ValueType::U8);
        block->SetTerminal(terminal::If{condition, terminal::LinkBlock{Location{0x8a90}},
                                         terminal::LinkBlock{Location{0x8aa0}}});
        FlagsEliminationPass::Run(block.get(), nullptr, features);
        block->ReIdInstr();
    }
};
}

TEST_CASE("integer branch plans are deterministic and reject stale analyses and facts", "[narrow-integer-plan][integer-branch]") {
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
        RegAlloc alloc{fixture.block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                       space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(fixture.block.get(), &alloc, false, features);
        alloc.MapRegister(fixture.loaded.Id(), HostGPR{22});
        alloc.MarkHostWriteCoalesced(fixture.publication->Id());
        arm64::BlockAnalysisIndex analysis;
        analysis.Build(fixture.block.get());
        arm64::GuestStateMap facts;
        facts.Analyze(fixture.block.get(), features);
        const auto plan = arm64::BuildIntegerBranchPlan(fixture.block.get(), analysis, alloc, features, facts);
        Facts out;
        out.has_value = plan.has_value();
        if (plan) {
            out.narrow = plan->narrow.has_value();
            out.zero_target = plan->edge.zero_target;
            out.required = plan->edge.required;
            out.raw = plan->edge.raw_condition;
            out.discarded = plan->edge.discarded.size();
            REQUIRE(plan->edge.condition == fixture.condition.Def());
        }
        IntrusivePtr<Block> other{new Block(0, Location{0x8ab0})};
        other->LoadUniform(Uniform{0, ValueType::U64});
        other->SetTerminal(terminal::ReturnToHost{});
        other->ReIdInstr();
        arm64::BlockAnalysisIndex stale_analysis;
        stale_analysis.Build(other.get());
        REQUIRE_FALSE(arm64::BuildIntegerBranchPlan(fixture.block.get(), stale_analysis, alloc, features, facts));
        arm64::GuestStateMap stale_facts;
        stale_facts.Analyze(other.get(), features);
        const auto foreign_facts = arm64::BuildIntegerBranchPlan(fixture.block.get(), analysis, alloc, features, stale_facts);
        REQUIRE(foreign_facts.has_value() == plan.has_value());
        if (plan) {
            REQUIRE(foreign_facts->edge.zero_target == plan->edge.zero_target);
            REQUIRE(foreign_facts->edge.required == plan->edge.required);
            REQUIRE(foreign_facts->edge.raw_condition == plan->edge.raw_condition);
            REQUIRE(foreign_facts->narrow.has_value() == plan->narrow.has_value());
        }
        return out;
    };
    BranchFixture first{features}, second{features};
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
    REQUIRE(b.discarded == a.discarded);
}

TEST_CASE("integer branch plans follow allocation lifecycle without leaking decisions", "[narrow-integer-plan][integer-branch]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config};
    const FeatureSet features{};
    BranchFixture fixture{features};
    RegAlloc alloc{fixture.block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                   space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(fixture.block.get(), &alloc, false, features);
    alloc.MarkHostWriteCoalesced(fixture.publication->Id());
    arm64::BlockAnalysisIndex analysis;
    analysis.Build(fixture.block.get());
    arm64::GuestStateMap facts;
    facts.Analyze(fixture.block.get(), features);
    const auto build = [&] {
        return arm64::BuildIntegerBranchPlan(fixture.block.get(), analysis, alloc, features, facts);
    };
    alloc.MapRegister(fixture.loaded.Id(), HostGPR{22});
    const auto fresh = build();
    REQUIRE(fresh);
    REQUIRE(fresh->edge.zero_target == std::optional<swift::u16>{22});
    alloc.MapRegister(fixture.loaded.Id(), HostGPR{23});
    REQUIRE_FALSE(build());
    alloc.MapMemSpill(fixture.loaded.Id(), SpillSlot{2});
    REQUIRE_FALSE(build());
    alloc.MapRegister(fixture.loaded.Id(), HostGPR{22});
    const auto restored = build();
    REQUIRE(restored);
    REQUIRE(restored->edge.zero_target == fresh->edge.zero_target);
    REQUIRE(restored->edge.required == fresh->edge.required);
    REQUIRE(restored->edge.raw_condition == fresh->edge.raw_condition);
    REQUIRE(restored->edge.condition == fresh->edge.condition);
    REQUIRE(restored->edge.producer == fresh->edge.producer);
    REQUIRE(restored->edge.discarded.size() == fresh->edge.discarded.size());
    REQUIRE(restored->narrow.has_value() == fresh->narrow.has_value());
}
