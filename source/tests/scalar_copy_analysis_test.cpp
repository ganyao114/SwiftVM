#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/alloc/consumer_plan.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/arm64/jit/scalar_copy_analysis.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "runtime/common/svm_config.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

std::vector<std::string> EmitSelfXor() {
    IntrusivePtr<Block> block{new Block(0, Location{0x8af0})};
    auto source = block->GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(ValueType::U64);
    auto left = block->BitExtract(source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    auto right = block->BitExtract(source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    auto result = block->Xor(left, Operand{right}).SetType(ValueType::U32);
    block->ClearFlags(Flags::Carry | Flags::Overflow | Flags::AuxiliaryCarry);
    block->SaveFlags(result, Flags::Negate | Flags::Zero | Flags::Parity);
    auto wide = block->ZeroExtend32To64(result).SetType(ValueType::U64);
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
    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(),
                   features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    alloc.CompleteGPRAllocation(module);
    const auto plan = alloc.GPRPlanFor(block.get());
    REQUIRE(plan);
    REQUIRE(plan->scalar_preparation);
    REQUIRE(plan->scalar_preparation->copies.IsSelfXor(result.Def()));
    REQUIRE(plan->scalar_preparation->copies.InputDiscarded(left.Def()));
    REQUIRE(plan->scalar_preparation->copies.InputDiscarded(right.Def()));
    REQUIRE(plan->pinned->MatchesFrozenIR());
    arm64::JitContext context{module, alloc};
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
    auto source = block.GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(ValueType::U64);
    auto left = block.BitExtract(source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    auto right = block.BitExtract(source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    auto result = block.Xor(left, Operand{right}).SetType(ValueType::U32);
    block.StoreUniform(Uniform{0, ValueType::U32}, left);

    arm64::ScalarCopyAnalysis analysis;
    analysis.Analyze(&block);

    REQUIRE(analysis.IsSelfXor(result.Def()));
    REQUIRE_FALSE(analysis.InputDiscarded(left.Def()));
    REQUIRE(analysis.InputDiscarded(right.Def()));

    Block unsafe{0, Location{0x8b40}};
    auto unsafe_source = unsafe.GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(ValueType::U64);
    auto low = unsafe.BitExtract(unsafe_source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    auto shifted = unsafe.BitExtract(unsafe_source, Imm{1u}, Imm{32u}).SetType(ValueType::U32);
    auto unsafe_result = unsafe.Xor(low, Operand{shifted}).SetType(ValueType::U32);
    analysis.Analyze(&unsafe);

    REQUIRE_FALSE(analysis.IsSelfXor(unsafe_result.Def()));
    REQUIRE_FALSE(analysis.InputDiscarded(low.Def()));
    REQUIRE_FALSE(analysis.InputDiscarded(shifted.Def()));
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

TEST_CASE("RegAlloc freezes raw carry branch conditions before encoder construction", "[scalar-preparation]") {
    for (const bool shared : {false, true}) for (const bool canonical : {false, true}) for (const unsigned clobber : {0u, 1u, 2u}) {
        CAPTURE(shared, canonical, clobber);
        IntrusivePtr<Block> block{new Block(0, Location{0x8b60})};
        const auto source = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
        const auto comparison = block->Sub(source, Operand{Imm{7u}}).SetType(ValueType::U32);
        block->SaveFlags(comparison, Flags::Carry | Flags::Zero);
        block->InvertCarry();
        auto* invert = &block->GetInstList().back();
        block->AdvancePC(Imm{1u});
        const auto carry = block->TestFlags(Flags::Carry).SetType(ValueType::U8);
        if (clobber == 2) {
            const auto changed = block->Add(source, Operand{Imm{1u}}).SetType(ValueType::U32);
            block->SaveFlags(changed, Flags::Carry | Flags::Zero);
        }
        const auto no_carry = block->TestZero(carry).SetType(ValueType::U8);
        const auto nonzero = block->CondSet(Cond::NE).SetType(ValueType::U8);
        const auto condition = block->And(no_carry, Operand{nonzero}).SetType(ValueType::U8);
        if (shared) block->StoreUniform(Uniform{8, ValueType::U8}, carry);
        if (clobber == 1) {
            const auto changed = block->Add(source, Operand{Imm{1u}}).SetType(ValueType::U32);
            block->SaveFlags(changed, Flags::Carry | Flags::Zero);
        }
        const auto alternative = block->LoadUniform(Uniform{24, ValueType::U32}).SetType(ValueType::U32);
        const auto selected = block->Select(BOOL{condition}, source, alternative).SetType(ValueType::U32);
        block->StoreUniform(Uniform{16, ValueType::U32}, selected);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 64;
        config.stack_alignment = 16;
        if (canonical) config.arm64_features = Arm64Features::FlagM;
        AddressSpace space{config}; auto module = space.GetDefaultModule(); FeatureSet features{};
        RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        allocation.CompleteGPRAllocation(module);
        const auto plan = allocation.GPRPlanFor(block.get()); REQUIRE(plan);
        if (shared || clobber || !canonical || !GetSvmConfig().flags_cfinv || !FlagsRegsEnabled()) REQUIRE_FALSE(plan->scalar_preparation);
        else {
            REQUIRE(plan->scalar_preparation);
            REQUIRE(plan->scalar_preparation->raw_carry.ConditionForTest(carry.Def()) == Cond::HI);
            REQUIRE(plan->scalar_preparation->raw_carry.SuppressesInvert(invert));
            REQUIRE(plan->scalar_preparation->raw_carry.InvertForTest(carry.Def()) == invert);
        }
        arm64::JitContext context{module, allocation}; arm64::JitTranslator translator{context};
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
    const auto source = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
    const auto comparison = block->Sub(source, Operand{Imm{7u}}).SetType(ValueType::U32);
    block->CallLambda(Lambda{DataClass{Imm{swift::u64{1}}}, HelperCallTraits{}});
    block->SaveFlags(comparison, Flags::Carry | Flags::Zero);
    block->InvertCarry(); block->AdvancePC(Imm{1u});
    const auto carry = block->TestFlags(Flags::Carry).SetType(ValueType::U8);
    const auto no_carry = block->TestZero(carry).SetType(ValueType::U8);
    const auto nonzero = block->CondSet(Cond::NE).SetType(ValueType::U8);
    const auto condition = block->And(no_carry, Operand{nonzero}).SetType(ValueType::U8);
    block->SetTerminal(terminal::If{BOOL{condition}, terminal::ReturnToHost{}, terminal::ReturnToHost{}});
    block->ReIdInstr();
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64,
                  .arm64_features = Arm64Features::FlagM};
    config.uniform_buffer_size = 64;
    AddressSpace space{config}; auto module = space.GetDefaultModule(); FeatureSet features{};
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    allocation.CompleteGPRAllocation(module);
    arm64::JitContext context{module, allocation}; arm64::JitTranslator translator{context};
    REQUIRE_NOTHROW(translator.Translate(block.get()));
}

TEST_CASE("raw carry fold proves the TestFlags carrier through its final consumer", "[scalar-preparation][regression]") {
    for (const bool terminal : {false, true}) for (const unsigned obstruction : {0u, 1u, 2u, 3u, 4u}) {
        CAPTURE(terminal, obstruction);
        IntrusivePtr<Block> block{new Block(0, Location{0x8b80})};
        const auto source = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
        const auto comparison = block->Sub(source, Operand{Imm{7u}}).SetType(ValueType::U32);
        block->SaveFlags(comparison, Flags::Carry | Flags::Zero);
        block->InvertCarry(); block->AdvancePC(Imm{1u});
        const auto carry = block->TestFlags(Flags::Carry).SetType(ValueType::U8);
        const auto no_carry = block->TestZero(carry).SetType(ValueType::U8);
        const auto nonzero = block->CondSet(Cond::NE).SetType(ValueType::U8);
        const auto overwrite = [&] {
            const auto changed = block->Add(source, Operand{Imm{1u}}).SetType(ValueType::U32);
            block->SaveFlags(changed, Flags::Carry | Flags::Zero);
        };
        if (obstruction == 1) overwrite();
        const auto condition = block->And(no_carry, Operand{nonzero}).SetType(ValueType::U8);
        if (obstruction == 2) overwrite();
        if (obstruction == 3) block->CallLambda(Lambda{DataClass{Imm{swift::u64{1}}}, HelperCallTraits{}});
        if (obstruction == 4) block->StoreUniform(Uniform{8, ValueType::U8}, nonzero);
        if (terminal) {
            block->SetTerminal(terminal::If{BOOL{condition}, terminal::ReturnToHost{}, terminal::ReturnToHost{}});
        } else {
            const auto selected = block->Select(BOOL{condition}, source, source).SetType(ValueType::U32);
            block->StoreUniform(Uniform{16, ValueType::U32}, selected);
            block->SetTerminal(terminal::ReturnToHost{});
        }
        block->ReIdInstr();
        arm64::BlockAnalysisIndex analysis; analysis.Build(block.get());
        arm64::RawCarryBranchAnalysis prepared; FeatureSet features{};
        auto terminal_node = block->GetTerminal();
        const auto consumers = arm64::BuildConsumerPlan(analysis,
                [](const Inst&) { return true; }, [](const Inst&) { return false; }, &terminal_node);
        prepared.Analyze(block.get(), features, consumers, &analysis);
        REQUIRE((prepared.FoldForTest(carry.Def()) != nullptr) == (obstruction == 0));
        REQUIRE(prepared.ConditionForTest(carry.Def()).has_value() == (obstruction == 0));
        if (terminal && obstruction == 0) {
            block->SetTerminal(terminal::Condition{Cond::NE, terminal_node, terminal::ReturnToHost{}});
            analysis.Build(block.get());
            terminal_node = block->GetTerminal();
            const auto rejected = arm64::BuildConsumerPlan(analysis,
                    [](const Inst&) { return true; }, [](const Inst&) { return false; }, &terminal_node);
            REQUIRE_FALSE(rejected.local_conditions.contains(condition.Def()));
            prepared.Analyze(block.get(), features, rejected, &analysis);
            REQUIRE(prepared.Empty());
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
            RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                                space.GetTrampolines().GetFPRRegs(), features};
            RegisterAllocPass::Run(block, &allocation, false, features);
            allocation.CompleteGPRAllocation(module);
            arm64::JitContext context{module, allocation};
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
            const auto value = target->LoadImm(Imm{marker}).SetType(ValueType::U32);
            target->StoreUniform(Uniform{16, ValueType::U32}, value);
            target->SetTerminal(terminal::ReturnToHost{});
            publish(target.get());
        }
        IntrusivePtr<Block> block{new Block(0, Location{0x8c00})};
        const auto selector = block->LoadUniform(Uniform{24, ValueType::U64}).SetType(ValueType::U64);
        const auto source = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
        const auto comparison = block->Sub(source, Operand{Imm{7u}}).SetType(ValueType::U32);
        block->SaveFlags(comparison, Flags::Carry | Flags::Zero | Flags::Negate | Flags::Overflow);
        block->InvertCarry();
        auto* invert = &block->GetInstList().back();
        block->AdvancePC(Imm{1u});
        const auto carry = block->TestFlags(Flags::Carry).SetType(ValueType::U8);
        const auto predicate = (below_equal ? block->TestNotZero(carry) : block->TestZero(carry)).SetType(ValueType::U8);
        const auto other = block->CondSet(below_equal ? Cond::EQ : Cond::NE).SetType(ValueType::U8);
        const auto condition = (below_equal ? block->Or(predicate, Operand{other})
                                            : block->And(predicate, Operand{other})).SetType(ValueType::U8);
        const Terminal branch = terminal::If{BOOL{condition}, terminal::LinkBlock{Location{0x8d00}},
                                            terminal::LinkBlock{Location{0x8e00}}};
        Terminal selected = branch;
        if (shape >= 2) selected = terminal::Condition{Cond::NE, selected,
                                                     terminal::LinkBlock{Location{0x8f00}}};
        if (shape & 1) selected = terminal::Switch{selector, {{Imm{swift::u64{1}}, selected}}};
        if (shape == 4) selected = terminal::Switch{selector, {{Imm{swift::u64{0}}, branch},
                                                             {Imm{swift::u64{1}}, selected}}};
        block->SetTerminal(selected);
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        allocation.CompleteGPRAllocation(module);
        const auto plan = allocation.GPRPlanFor(block.get());
        REQUIRE(plan);
        REQUIRE(plan->consumers);
        REQUIRE(plan->consumers->local_conditions.contains(condition.Def()) == (shape < 2));
        const bool expected_raw = shape < 2 && FlagsRegsEnabled() && GetSvmConfig().flags_cfinv;
        const auto* raw = plan->scalar_preparation ? &plan->scalar_preparation->raw_carry : nullptr;
        REQUIRE((raw && raw->FoldForTest(carry.Def())) == expected_raw);
        REQUIRE((raw && raw->SuppressesInvert(invert)) == expected_raw);
        REQUIRE((raw && raw->ConditionForTest(carry.Def()).has_value()) == expected_raw);
        arm64::JitContext context{module, allocation};
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
