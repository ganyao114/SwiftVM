#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/runtime.h"
#include "runtime/ir/opts/register_alloc_pass.h"

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
    auto source = block->LoadUniform<TypedValue<ValueType::U64>>(
            Uniform{0, ValueType::U64});
    auto first = block->Add(source, Operand{Imm{swift::u64{1}}})
                         .SetType(ValueType::U64);
    block->SaveFlags(first, Flags::NZCV);

    HelperCallTraits traits{};
    if (exact) {
        traits.guest_state = HelperGuestStateEffect::None;
        traits.fault = HelperFaultEffect::NoDirectFault;
        traits.reentry = HelperReentryEffect::NoReentry;
        traits.host_flags = HostFlagsEffect::PreservesNZCV;
    }
    (void)block->CallLambda(Lambda{DataClass{Imm{swift::u64{1}}}, traits});

    auto second = block->Sub(source, Operand{Imm{swift::u64{1}}})
                          .SetType(ValueType::U64);
    block->SaveFlags(second, Flags::NZCV);
    block->StoreUniform(Uniform{8, ValueType::U64}, second);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
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
    auto result = block->Sse42Str(left, right, Imm{swift::u64{imm}})
                          .SetType(ValueType::U64);
    block->StoreUniform(Uniform{48, ValueType::U64}, result);
    block->StoreUniform(Uniform{64, ValueType::V128}, carry);
    block->StoreUniform(Uniform{104, ValueType::U64}, scalar_carry);
    if (keep_left_live) {
        block->StoreUniform(Uniform{80, ValueType::V128}, left);
    }
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    const auto carry_fpr = alloc.ValueFPR(carry).id;
    const auto left_fpr = alloc.ValueFPR(left).id;
    const auto right_fpr = alloc.ValueFPR(right).id;
    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    return {Disassemble(context), carry_fpr, left_fpr, right_fpr};
}
#endif

#if defined(__aarch64__)
extern "C" swift::u64 SwiftRepStos1Resident(swift::u64, swift::u64, swift::u64);

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
    auto left = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
    auto result = block->Sub(left, Operand{Imm{swift::u64{1}}})
                          .SetType(ValueType::U64);
    auto token = block->SaveFlags(result, Flags::NZCV);
    if (call_helper) {
        const auto helper_address = reinterpret_cast<swift::VAddr>(
                &SwiftRepStos1Resident);
        (void)block->CallLambda(
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
    auto flags = block->GetFlags(token, Flags::NZCV).SetType(ValueType::U64);
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
    REQUIRE(Count(exact, "nzcv") == 1);
}

TEST_CASE("resident string helper preserves pending NZCV",
          "[helper-effects][production]") {
#if defined(__aarch64__)
    const auto baseline = RunPendingFlags(false);
    REQUIRE(baseline != 0);
    REQUIRE(RunPendingFlags(true) == baseline);
#else
    SUCCEED("resident string helper requires an AArch64 host");
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
