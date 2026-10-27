#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/compiler.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/svm_config.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/ir/hir_builder.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

std::vector<std::string> Disassemble(arm64::JitContext& context) {
    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<
            const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<
            const vixl::aarch64::Instruction*>();
    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::vector<std::string> instructions;
    for (auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        instructions.emplace_back(disassembler.GetOutput());
    }
    return instructions;
}

std::vector<std::string> EmitHelperBoundary(bool exact) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0xb140})};
    auto source = block->LoadUniform<TypedValue<ValueType::U64>>(Uniform{0, ValueType::U64});
    auto first = block->Add(ValueType::U64, source, Operand{Imm{swift::u64{1}}});
    block->SaveFlags(first, Flags::NZCV);

    HelperCallTraits traits{};
    if (exact) {
        traits.guest_state = HelperGuestStateEffect::None;
        traits.fault = HelperFaultEffect::NoDirectFault;
        traits.reentry = HelperReentryEffect::NoReentry;
        traits.host_flags = HostFlagsEffect::PreservesNZCV;
    }
    (void)block->CallLambda(ValueType::VOID, Lambda{DataClass{Imm{swift::u64{1}}}, traits});

    auto second = block->Sub(ValueType::U64, source, Operand{Imm{swift::u64{1}}});
    block->SaveFlags(second, Flags::NZCV);
    block->StoreUniform(Uniform{8, ValueType::U64}, second);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

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
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                              alloc);
    arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    return Disassemble(context);
}

std::size_t Count(const std::vector<std::string>& instructions,
                  std::string_view needle) {
    return std::ranges::count_if(instructions, [&](const auto& instruction) {
        return instruction.find(needle) != std::string::npos;
    });
}

std::size_t CountStackRegister(const std::vector<std::string>& instructions,
                               bool store,
                               std::string_view reg) {
    const std::string single = store ? "str " : "ldr ";
    const std::string pair = store ? "stp " : "ldp ";
    return std::ranges::count_if(instructions, [&](const auto& instruction) {
        const auto stack = instruction.find("[sp");
        return stack != std::string::npos &&
               (instruction.find(single) != std::string::npos ||
                instruction.find(pair) != std::string::npos) &&
               instruction.find(reg) < stack;
    });
}

#if defined(__aarch64__)
struct Sse42HelperEmission {
    std::vector<std::string> instructions;
    swift::u16 carry;
    swift::u16 left;
    swift::u16 right;
};

std::size_t CountStackFPR(const Sse42HelperEmission& emission,
                          bool store, swift::u16 code) {
    const std::string reg = "q" + std::to_string(code) + ",";
    return CountStackRegister(emission.instructions, store, reg);
}

Sse42HelperEmission EmitSse42HelperBoundary(swift::u8 imm,
                                             bool keep_left_live) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0xb160})};
    auto scalar_carry = block->LoadUniform(Uniform{96, ValueType::U64});
    auto carry = block->LoadUniform(Uniform{0, ValueType::V128});
    auto left = block->LoadUniform(Uniform{16, ValueType::V128});
    auto right = block->LoadUniform(Uniform{32, ValueType::V128});
    auto result = block->Sse42Str(ValueType::U64, left, right, Imm{swift::u64{imm}});
    block->StoreUniform(Uniform{48, ValueType::U64}, result);
    block->StoreUniform(Uniform{64, ValueType::V128}, carry);
    block->StoreUniform(Uniform{104, ValueType::U64}, scalar_carry);
    if (keep_left_live) {
        block->StoreUniform(Uniform{80, ValueType::V128}, left);
    }
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

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
    const auto carry_fpr = alloc.ValueFPR(carry).id;
    const auto left_fpr = alloc.ValueFPR(left).id;
    const auto right_fpr = alloc.ValueFPR(right).id;
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                              alloc);
    arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    return {Disassemble(context), carry_fpr, left_fpr, right_fpr};
}
#endif

#if defined(__aarch64__)
extern "C" swift::u64 SwiftRepStos1Resident(swift::u64, swift::u64, swift::u64);

swift::u64 SharedCallTransform(swift::u64 value) { return value * 3 + 7; }

swift::u64 RunRepeatedHostCalls(unsigned shape, bool dynamic) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = 16,
            .static_program = true,
    };
    AddressSpace space{config};
    constexpr swift::u64 guest = 0xb1c0;
    HIRBuilder builder{1, false, FeatureSet{}};
    auto* function =
            shape ? builder.AppendFunction(Location{guest}, Location{guest + 0x41}) : nullptr;
    IntrusivePtr<Block> block{shape ? nullptr : new Block{0, Location{guest}}};
    const auto call = [&](Value input) {
        const auto address = reinterpret_cast<swift::VAddr>(&SharedCallTransform);
        if (function) {
            const auto lambda = dynamic ? Lambda{function->LoadImm(ValueType::U64, Imm{address})}
                                        : Lambda{Imm{address}};
            return function->CallLambda(ValueType::U64, lambda, input);
        }
        const auto lambda = dynamic ? Lambda{block->LoadImm(ValueType::U64, Imm{address})}
                                    : Lambda{Imm{address}};
        return block->CallLambda(ValueType::U64, lambda, input);
    };
    const auto input = function ? function->LoadImm(ValueType::U64, Imm{swift::u64{5}})
                                : block->LoadImm(ValueType::U64, Imm{swift::u64{5}});
    const auto first = call(input);
    if (shape == 2)
        builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{guest + 0x40}}));
    const auto result = call(first);
    if (function) {
        function->StoreUniform(Uniform{0, ValueType::U64}, result);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
    } else {
        block->StoreUniform(Uniform{0, ValueType::U64}, result);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
    }
    const auto module = space.GetDefaultModule();
    auto* entry = function ? TranslateIR(module, function) : TranslateIR(module, block);
    REQUIRE(entry);
    space.PushCodeCache(guest, entry);
    Runtime runtime{&space};
    runtime.SetLocation(guest);
    REQUIRE(runtime.Run() == HaltReason::CallHost);
    swift::u64 value{};
    std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
    return value;
}

swift::u64 RunPendingFlags(bool call_helper) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = 16,
            .static_program = true,
    };
    AddressSpace address_space{config};
    constexpr swift::u64 guest = 0xb180;
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    std::memset(&block->GetJitCache(), 0, sizeof(JitCache));
    auto left = block->LoadImm(ValueType::U64, Imm{swift::u64{0}});
    auto result = block->Sub(ValueType::U64, left, Operand{Imm{swift::u64{1}}});
    block->SaveFlags(result, Flags::All);
    // A transparent helper must retain PF/AF result state as well as NZCV,
    // even when a partial architectural clear is still waiting to commit.
    block->ClearFlags(Flags::Carry);
    if (call_helper) {
        const auto helper_address = reinterpret_cast<swift::VAddr>(
                &SwiftRepStos1Resident);
        (void)block->CallLambda(ValueType::VOID,
                Lambda{DataClass{Imm{helper_address}},
                       HelperCallTraits{
                               .host_fp = HostFpEffect::FPCRTransparent,
                               .host_registers = HostRegisterEffect::PreservesPinnedState,
                               .guest_state = HelperGuestStateEffect::None,
                               .fault = HelperFaultEffect::NoDirectFault,
                               .reentry = HelperReentryEffect::NoReentry,
                               .host_flags = HostFlagsEffect::PreservesNZCV,
                       }},
                Imm{swift::u64{0}},
                Imm{swift::u64{0}},
                Imm{swift::u64{0}});
    }
    auto flags = block->GetFlags(ValueType::U64, result, Flags::NZCV);
    block->StoreUniform(Uniform{0, ValueType::U64}, flags);
    block->SetTerminal(terminal::LinkBlock{Location{guest + 1}});
    block->ReIdInstr();
    const auto entry = TranslateIR(address_space.GetDefaultModule(), block);
    REQUIRE(entry != nullptr);
    address_space.PushCodeCache(guest, entry);

    Runtime runtime{&address_space};
    runtime.SetLocation(guest);
    REQUIRE(runtime.Run() == HaltReason::CodeMiss);
    swift::u64 value{};
    std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
    return value;
}
#endif

}  // namespace

TEST_CASE("exact helper effects retain pending NZCV across host calls",
          "[helper-effects][codegen]") {
    const auto conservative = EmitHelperBoundary(false);
    const auto exact = EmitHelperBoundary(true);
    REQUIRE(Count(conservative, "nzcv") == 2);
    // Eager flags publish both results; the lazy token can discard the first.
    REQUIRE(Count(exact, "nzcv") == (GetSvmConfig().flags_regs ? 1 : 2));
    const auto call = std::ranges::find_if(exact, [](const auto& instruction) {
        return instruction.find("blr ") != std::string::npos;
    });
    const auto capture = std::ranges::find_if(exact, [](const auto& instruction) {
        return instruction.find("mrs ") != std::string::npos;
    });
    REQUIRE(call != exact.end());
    REQUIRE(capture != exact.end());
    REQUIRE(call < capture);
}

TEST_CASE("resident string helper preserves pending NZCV",
          "[helper-effects][production]") {
#if defined(__aarch64__)
    const auto baseline = RunPendingFlags(false);
    REQUIRE((baseline & 0xf0000000u) == 0x80000000u);  // 0 - 1, with C explicitly cleared.
    REQUIRE(RunPendingFlags(true) == baseline);
#else
    SUCCEED("resident string helper requires an AArch64 host");
#endif
}

TEST_CASE("prepared shared host calls outlive builders and execute both call policies",
          "[helper-effects][production][ownership]") {
#if defined(__aarch64__)
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = 16,
    };
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    const auto address = reinterpret_cast<swift::VAddr>(&SharedCallTransform);
    std::optional<arm64::Arm64PreparedUnit> pending;
    const swift::u64* target_storage{};
    {
        HIRBuilder builder{1, false, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{0xb1e0}, Location{0xb221});
        const auto first =
                function->CallLambda(ValueType::U64, Lambda{Imm{address}}, Imm{swift::u64{5}});
        builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0xb220}}));
        const auto result = function->CallLambda(ValueType::U64, Lambda{Imm{address}}, first);
        function->StoreUniform(Uniform{0, ValueType::U64}, result);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        pending = arm64::PrepareFunction(module, function);
        REQUIRE(pending);
        const auto targets = pending->Actions().SharedHostCallTargets();
        REQUIRE(targets.size() == 1);
        REQUIRE(targets.front() == address);
        target_storage = targets.data();
    }
    arm64::Arm64PreparedUnit moved{std::move(*pending)};
    pending.reset();
    REQUIRE(moved.ReadyForEmission(module));
    REQUIRE(moved.Actions().SharedHostCallTargets().data() == target_storage);
    arm64::JitContext context{module, moved.Allocation(), moved.Actions(), false};
    arm64::JitTranslator translator{context};
    REQUIRE(translator.Translate(moved.Function()));
    context.Finish();
    REQUIRE(Count(Disassemble(context), "bl ") == 2);
    for (const unsigned shape : {0u, 1u, 2u})
        for (const bool dynamic : {false, true}) {
            CAPTURE(shape, dynamic);
            REQUIRE(RunRepeatedHostCalls(shape, dynamic) == 73);
        }
#else
    SUCCEED("shared host calls require an AArch64 host");
#endif
}

TEST_CASE("SSE4.2 helper preserves only live-through vector values",
          "[helper-effects][sse42]") {
#if defined(__aarch64__)
    for (swift::u8 imm : {swift::u8{0x1a}, swift::u8{0x02}}) {
        CAPTURE(imm);
        const auto dead_arguments = EmitSse42HelperBoundary(imm, false);
        REQUIRE(CountStackFPR(dead_arguments, true, dead_arguments.carry) == 1);
        REQUIRE(CountStackFPR(dead_arguments, true, dead_arguments.left) == 0);
        REQUIRE(CountStackFPR(dead_arguments, true, dead_arguments.right) == 0);
        REQUIRE(CountStackFPR(dead_arguments, false, dead_arguments.carry) == 1);
        REQUIRE(CountStackFPR(dead_arguments, false, dead_arguments.left) == 0);
        REQUIRE(CountStackFPR(dead_arguments, false, dead_arguments.right) == 0);
        REQUIRE(Count(dead_arguments.instructions, "str w16, [sp") == 0);
        REQUIRE(Count(dead_arguments.instructions, "sub sp, sp") == 0);
        REQUIRE(Count(dead_arguments.instructions, "add sp, sp") == 0);
        const size_t link_saves = 0;
        REQUIRE(CountStackRegister(
                        dead_arguments.instructions, true, "x30") ==
                link_saves);
        REQUIRE(CountStackRegister(
                        dead_arguments.instructions, false, "x30") ==
                link_saves);

        const auto live_left = EmitSse42HelperBoundary(imm, true);
        REQUIRE(CountStackFPR(live_left, true, live_left.left) == 1);
        REQUIRE(CountStackFPR(live_left, false, live_left.left) == 1);
    }
#else
    SUCCEED("SSE4.2 native helper requires an AArch64 host");
#endif
}
