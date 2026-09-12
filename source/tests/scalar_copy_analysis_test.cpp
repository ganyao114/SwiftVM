#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/scalar_copy_analysis.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"

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
    REQUIRE_FALSE(Contains(instructions, "mrs "));
}
