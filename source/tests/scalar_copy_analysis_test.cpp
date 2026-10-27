#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/compile/workspace.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/compiler.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/lower_consumers.h"
#include "runtime/backend/arm64/compile/lower_flags.h"
#include "runtime/backend/arm64/compile/lower_scalar.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/context.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/svm_config.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/analysis/block_index.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

std::vector<std::string> EmitSelfXor() {
    IntrusivePtr<Block> block{new Block(0, Location{0x8af0})};
    auto source = block->GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
    auto left = block->BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
    auto right = block->BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
    auto result = block->Xor(ValueType::U32, left, Operand{right});
    block->ClearFlags(Flags::Carry | Flags::Overflow | Flags::AuxiliaryCarry);
    block->SaveFlags(result, Flags::Negate | Flags::Zero | Flags::Parity);
    auto wide = block->ZeroExtend32To64(ValueType::U64, result);
    block->SetHostGPR(wide, HostRegIndex(22), Imm{0u});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .global_opts = Optimizations::All,
            .arm64_features = Arm64Features::AXFlag,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    FeatureSet features{};
    AllocationFixture alloc{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(block.get(), alloc, features),
            false);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    const auto plan = alloc.Actions().ForBlock(block.get());
    REQUIRE(plan);
    REQUIRE(alloc.Actions().HasScalar(result.Id(), arm64::ScalarFlag::SelfXor));
    REQUIRE(alloc.Actions().HasScalar(left.Id(), arm64::ScalarFlag::CopyInputDiscarded));
    REQUIRE(alloc.Actions().HasScalar(right.Id(), arm64::ScalarFlag::CopyInputDiscarded));
    REQUIRE(alloc.Actions().MatchesAllocation(alloc));
    arm64::JitContext context{module, alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::vector<std::string> instructions;
    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
    for (auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        instructions.emplace_back(disassembler.GetOutput());
    }
    return instructions;
}

bool Contains(const std::vector<std::string>& instructions, std::string_view value) {
    return std::ranges::any_of(instructions, [&](const auto& instruction) {
        return instruction.find(value) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("self XOR discards only equivalent exclusive views") {
    Block block{0, Location{0x8b20}};
    auto source = block.GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
    auto left = block.BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
    auto right = block.BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
    auto result = block.Xor(ValueType::U32, left, Operand{right});
    block.StoreUniform(Uniform{0, ValueType::U32}, left);

    block.ReIdInstr();
    compile::BlockAnalysisIndex index;
    index.Build(&block);
    auto analysis = arm64::LowerScalarCopies(index);

    REQUIRE(analysis.Has(result.Id(), arm64::ScalarFlag::SelfXor));
    REQUIRE_FALSE(analysis.Has(left.Id(), arm64::ScalarFlag::CopyInputDiscarded));
    REQUIRE(analysis.Has(right.Id(), arm64::ScalarFlag::CopyInputDiscarded));

    Block unsafe{0, Location{0x8b40}};
    auto unsafe_source = unsafe.GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
    auto low = unsafe.BitExtract(ValueType::U32, unsafe_source, Imm{0u}, Imm{32u});
    auto shifted = unsafe.BitExtract(ValueType::U32, unsafe_source, Imm{1u}, Imm{32u});
    auto unsafe_result = unsafe.Xor(ValueType::U32, low, Operand{shifted});
    unsafe.ReIdInstr();
    index.Build(&unsafe);
    analysis = arm64::LowerScalarCopies(index);

    REQUIRE_FALSE(analysis.Has(unsafe_result.Id(), arm64::ScalarFlag::SelfXor));
    REQUIRE_FALSE(analysis.Has(low.Id(), arm64::ScalarFlag::CopyInputDiscarded));
    REQUIRE_FALSE(analysis.Has(shifted.Id(), arm64::ScalarFlag::CopyInputDiscarded));

    Block predecessor{0, Location{0x8b48}};
    const auto incoming_source = predecessor.GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
    const auto incoming =
            predecessor.BitExtract(ValueType::U32, incoming_source, Imm{0u}, Imm{32u});
    predecessor.ReIdInstr();
    Block local{0, Location{0x8b50}};
    local.LoadImm(ValueType::U32, Imm{0u});
    const auto shadow = local.LoadImm(ValueType::U32, Imm{1u});
    const auto local_result = local.Xor(ValueType::U32, incoming, Operand{incoming});
    local.StoreUniform(Uniform{0, ValueType::U32}, shadow);
    local.ReIdInstr();
    REQUIRE(shadow.Id() == incoming.Id());
    index.Build(&local);
    analysis = arm64::LowerScalarCopies(index);
    REQUIRE(analysis.Has(local_result.Id(), arm64::ScalarFlag::SelfXor));
    REQUIRE_FALSE(analysis.Has(shadow.Id(), arm64::ScalarFlag::CopyInputDiscarded));
}

TEST_CASE("self XOR emits zero and logical flags together") {
    const auto instructions = EmitSelfXor();
    REQUIRE(std::ranges::count_if(instructions, [](const auto& instruction) {
                return instruction.find("ands ") != std::string::npos;
            }) == 1);
    REQUIRE_FALSE(Contains(instructions, "eor "));
    REQUIRE_FALSE(Contains(instructions, "tst "));
    REQUIRE_FALSE(Contains(instructions, "lsr "));
    if (GetSvmConfig().flags_regs) {
        REQUIRE_FALSE(Contains(instructions, "mrs "));
    } else {
        REQUIRE(std::ranges::count_if(instructions, [](const auto& instruction) {
                    return instruction.find("mrs ") != std::string::npos;
                }) == 1);
        REQUIRE(Contains(instructions, "#30, #2"));
        REQUIRE(Contains(instructions, "bfi x26"));
        REQUIRE(Contains(instructions, "bfxil x26"));
        REQUIRE(Contains(instructions, "bfc x26, #26, #4"));
    }
}

TEST_CASE("scalar copy pseudo uses invalidate frozen preparation", "[scalar-preparation]") {
    REQUIRE_FALSE(EmitSelfXor().empty());
}

TEST_CASE("AllocationFixture freezes raw carry branch conditions before encoder construction",
          "[scalar-preparation]") {
    for (const bool shared : {false, true}) for (const bool canonical : {false, true}) for (const unsigned clobber : {0u, 1u, 2u}) {
        CAPTURE(shared, canonical, clobber);
        IntrusivePtr<Block> block{new Block(0, Location{0x8b60})};
        const auto source = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
        const auto comparison = block->Sub(ValueType::U32, source, Operand{Imm{7u}});
        block->SaveFlags(comparison, Flags::Carry | Flags::Zero);
        block->InvertCarry();
        auto* invert = &block->GetInstList().back();
        block->AdvancePC(Imm{1u});
        const auto carry = block->TestFlags(ValueType::U8, Flags::Carry);
        if (clobber == 2) {
            const auto changed = block->Add(ValueType::U32, source, Operand{Imm{1u}});
            block->SaveFlags(changed, Flags::Carry | Flags::Zero);
        }
        const auto no_carry = block->TestZero(ValueType::U8, carry);
        const auto nonzero = block->CondSet(ValueType::U8, Cond::NE);
        const auto condition = block->And(ValueType::U8, no_carry, Operand{nonzero});
        if (shared) block->StoreUniform(Uniform{8, ValueType::U8}, carry);
        if (clobber == 1) {
            const auto changed = block->Add(ValueType::U32, source, Operand{Imm{1u}});
            block->SaveFlags(changed, Flags::Carry | Flags::Zero);
        }
        const auto alternative = block->LoadUniform(ValueType::U32, Uniform{24, ValueType::U32});
        const auto selected = block->Select(ValueType::U32, BOOL{condition}, source, alternative);
        block->StoreUniform(Uniform{16, ValueType::U32}, selected);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 64;
        config.stack_alignment = 16;
        if (canonical) config.arm64_features = Arm64Features::FlagM;
        AddressSpace space{config}; auto module = space.GetDefaultModule(); FeatureSet features{};
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
        const auto& actions = allocation.Actions();
        const auto* raw = actions.CarryForTest(carry.Id());
        if (shared || clobber || !canonical || !GetSvmConfig().flags_cfinv || !FlagsRegsEnabled()) {
            REQUIRE_FALSE(raw);
            REQUIRE_FALSE(actions.SuppressesCarryInvert(invert->Id()));
        } else {
            REQUIRE(raw);
            REQUIRE(raw->RawCondition() == Cond::HI);
            REQUIRE(actions.SuppressesCarryInvert(invert->Id()));
            REQUIRE(raw->invert == invert->Id());
        }
        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        REQUIRE_NOTHROW(translator.Translate(block.get())); context.Finish();
#if defined(__aarch64__)
        const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer);
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        Runtime runtime{&space}; const swift::u32 other = 42;
        std::memcpy(runtime.GetUniformBuffer().data() + 24, &other, sizeof(other));
        for (const swift::u32 input : {5u, 7u, 8u}) {
            std::memcpy(runtime.GetUniformBuffer().data(), &input, sizeof(input));
            runtime.SetLocation(0x8b60); REQUIRE(runtime.Run() == HaltReason::CallHost);
            swift::u32 output{}; std::memcpy(&output, runtime.GetUniformBuffer().data() + 16, sizeof(output));
            REQUIRE(output == ((clobber == 2 ? input >= 7 : input > 7) ? input : other));
        }
#endif
    }
}

TEST_CASE("raw carry branch falls back after an observing helper between producer and publication", "[scalar-preparation][regression]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x8b70})};
    const auto source = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    const auto comparison = block->Sub(ValueType::U32, source, Operand{Imm{7u}});
    block->CallLambda(ValueType::VOID, Lambda{DataClass{Imm{swift::u64{1}}}, HelperCallTraits{}});
    block->SaveFlags(comparison, Flags::Carry | Flags::Zero);
    block->InvertCarry();
    block->AdvancePC(Imm{1u});
    const auto carry = block->TestFlags(ValueType::U8, Flags::Carry);
    const auto no_carry = block->TestZero(ValueType::U8, carry);
    const auto nonzero = block->CondSet(ValueType::U8, Cond::NE);
    const auto condition = block->And(ValueType::U8, no_carry, Operand{nonzero});
    block->SetTerminal(
            terminal::If{BOOL{condition}, terminal::ReturnToHost{}, terminal::ReturnToHost{}});
    block->ReIdInstr();
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64,
                  .arm64_features = Arm64Features::FlagM};
    config.uniform_buffer_size = 64;
    AddressSpace space{config}; auto module = space.GetDefaultModule(); FeatureSet features{};
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
    arm64::JitContext context{module, allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
    REQUIRE_NOTHROW(translator.Translate(block.get()));
}

TEST_CASE("raw carry fold proves the TestFlags carrier through its final consumer", "[scalar-preparation][regression]") {
    for (const bool terminal : {false, true}) for (const unsigned obstruction : {0u, 1u, 2u, 3u, 4u}) {
        CAPTURE(terminal, obstruction);
        IntrusivePtr<Block> block{new Block(0, Location{0x8b80})};
        const auto source = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
        const auto comparison = block->Sub(ValueType::U32, source, Operand{Imm{7u}});
        block->SaveFlags(comparison, Flags::Carry | Flags::Zero);
        block->InvertCarry();
        block->AdvancePC(Imm{1u});
        const auto carry = block->TestFlags(ValueType::U8, Flags::Carry);
        const auto no_carry = block->TestZero(ValueType::U8, carry);
        const auto nonzero = block->CondSet(ValueType::U8, Cond::NE);
        const auto overwrite = [&] {
            const auto changed = block->Add(ValueType::U32, source, Operand{Imm{1u}});
            block->SaveFlags(changed, Flags::Carry | Flags::Zero);
        };
        if (obstruction == 1) overwrite();
        const auto condition = block->And(ValueType::U8, no_carry, Operand{nonzero});
        if (obstruction == 2) overwrite();
        if (obstruction == 3) block->CallLambda(ValueType::VOID, Lambda{DataClass{Imm{swift::u64{1}}}, HelperCallTraits{}});
        if (obstruction == 4) block->StoreUniform(Uniform{8, ValueType::U8}, nonzero);
        if (terminal) {
            block->SetTerminal(terminal::If{BOOL{condition}, terminal::ReturnToHost{}, terminal::ReturnToHost{}});
        } else {
            const auto selected = block->Select(ValueType::U32, BOOL{condition}, source, source);
            block->StoreUniform(Uniform{16, ValueType::U32}, selected);
            block->SetTerminal(terminal::ReturnToHost{});
        }
        block->ReIdInstr();
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        FeatureSet features{};
        auto terminal_node = block->GetTerminal();
        compile::CompileWorkspace workspace;
        const auto consumers = arm64::LowerConsumers(
                analysis,
                workspace,
                [](const Inst&) { return true; },
                [](const Inst&) { return false; },
                &terminal_node);
        auto prepared = arm64::LowerCarryConditions(block.get(), analysis, features, consumers);
        REQUIRE((prepared.Find(carry.Id()) != nullptr) == (obstruction == 0));
        REQUIRE((prepared.Find(carry.Id()) &&
                 prepared.Find(carry.Id())->RawCondition().has_value()) == (obstruction == 0));
        if (terminal && obstruction == 0) {
            block->SetTerminal(
                    terminal::Condition{Cond::NE, terminal_node, terminal::ReturnToHost{}});
            analysis.Build(block.get());
            terminal_node = block->GetTerminal();
            const auto rejected = arm64::LowerConsumers(
                    analysis,
                    workspace,
                    [](const Inst&) { return true; },
                    [](const Inst&) { return false; },
                    &terminal_node);
            REQUIRE_FALSE(rejected.Has(condition.Id(), arm64::ConsumerFlag::LocalCondition));
            prepared = arm64::LowerCarryConditions(block.get(), analysis, features, rejected);
            REQUIRE(prepared.actions.empty());
        }
    }
}

TEST_CASE("raw carry folds follow finalized terminal admission", "[scalar-preparation][regression]") {
    for (const bool below_equal : {false, true}) for (const unsigned shape : {0u, 1u, 2u, 3u, 4u}) {
        CAPTURE(below_equal, shape);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64,
                      .uniform_buffer_size = 64, .arm64_features = Arm64Features::FlagM};
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        FeatureSet features{};
        const auto publish = [&](Block* block) {
            block->ReIdInstr();
            AllocationFixture allocation{block->MaxInstrId(),
                                         space.GetTrampolines().GetGPRRegs(),
                                         space.GetTrampolines().GetFPRRegs(),
                                         features};
            ::swift::runtime::compile::AllocateBlock(
                    block,
                    &allocation,
                    ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                            block, allocation, features),
                    false);
            ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
            arm64::JitContext context{module, allocation, allocation.Actions()};
            arm64::JitTranslator translator{context};
            REQUIRE_NOTHROW(translator.Translate(block));
            context.Finish();
#if defined(__aarch64__)
            const auto [index, buffer] = module->AllocCodeCache(
                    context.CurrentBufferSize(), context.RequiresRegionTrampoline());
            REQUIRE(index != INVALID_CACHE_ID);
            context.Flush(buffer);
            space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
#endif
        };
        for (const auto [location, marker] :
             {std::pair{0x8d00u, 1u}, std::pair{0x8e00u, 2u}, std::pair{0x8f00u, 3u}}) {
            IntrusivePtr<Block> target{new Block(0, Location{location})};
            const auto value = target->LoadImm(ValueType::U32, Imm{marker});
            target->StoreUniform(Uniform{16, ValueType::U32}, value);
            target->SetTerminal(terminal::ReturnToHost{});
            publish(target.get());
        }
        IntrusivePtr<Block> block{new Block(0, Location{0x8c00})};
        const auto selector = block->LoadUniform(ValueType::U64, Uniform{24, ValueType::U64});
        const auto source = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
        const auto comparison = block->Sub(ValueType::U32, source, Operand{Imm{7u}});
        block->SaveFlags(comparison, Flags::Carry | Flags::Zero | Flags::Negate | Flags::Overflow);
        block->InvertCarry();
        auto* invert = &block->GetInstList().back();
        block->AdvancePC(Imm{1u});
        const auto carry = block->TestFlags(ValueType::U8, Flags::Carry);
        const auto predicate = (below_equal ? block->TestNotZero(ValueType::U8, carry)
                                            : block->TestZero(ValueType::U8, carry));
        const auto other = block->CondSet(ValueType::U8, below_equal ? Cond::EQ : Cond::NE);
        const auto condition = (below_equal ? block->Or(ValueType::U8, predicate, Operand{other})
                                            : block->And(ValueType::U8, predicate, Operand{other}));
        const Terminal branch = terminal::If{BOOL{condition},
                                             terminal::LinkBlock{Location{0x8d00}},
                                             terminal::LinkBlock{Location{0x8e00}}};
        Terminal selected = branch;
        if (shape >= 2) selected = terminal::Condition{Cond::NE, selected,
                                                     terminal::LinkBlock{Location{0x8f00}}};
        if (shape & 1) selected = terminal::Switch{selector, {{Imm{swift::u64{1}}, selected}}};
        if (shape == 4) selected = terminal::Switch{selector, {{Imm{swift::u64{0}}, branch},
                                                             {Imm{swift::u64{1}}, selected}}};
        block->SetTerminal(selected);
        block->ReIdInstr();
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
        REQUIRE(allocation.Actions().Ready());
        const auto* condition_action = allocation.Actions().Consumer(condition.Id());
        REQUIRE((condition_action && condition_action->Has(arm64::ConsumerFlag::LocalCondition)) ==
                (shape < 2));
        const bool expected_raw = shape < 2 && FlagsRegsEnabled() && GetSvmConfig().flags_cfinv;
        const auto* raw = allocation.Actions().CarryForTest(carry.Id());
        REQUIRE((raw != nullptr) == expected_raw);
        REQUIRE(allocation.Actions().SuppressesCarryInvert(invert->Id()) == expected_raw);
        REQUIRE((raw && raw->RawCondition().has_value()) == expected_raw);
        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        REQUIRE_NOTHROW(translator.Translate(block.get()));
        context.Finish();
#if defined(__aarch64__)
        const auto [index, buffer] = module->AllocCodeCache(
                context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID);
        context.Flush(buffer);
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        Runtime runtime{&space};
        const swift::u64 key = 1;
        std::memcpy(runtime.GetUniformBuffer().data() + 24, &key, sizeof(key));
        for (const swift::u32 input : {5u, 7u, 8u}) {
            CAPTURE(input);
            std::memcpy(runtime.GetUniformBuffer().data(), &input, sizeof(input));
            runtime.GetState()->host_cpu_flags = 0;
            runtime.SetLocation(0x8c00);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            swift::u32 marker{};
            std::memcpy(&marker, runtime.GetUniformBuffer().data() + 16, sizeof(marker));
            const auto expected = shape >= 2 && input == 7 ? 3u
                    : ((below_equal ? input <= 7 : input > 7) ? 1u : 2u);
            REQUIRE(marker == expected);
            REQUIRE((runtime.GetState()->host_cpu_flags & 0xf0000000u) ==
                    (input < 7 ? 0xa0000000u : input == 7 ? 0x40000000u : 0u));
        }
#endif
    }
}
