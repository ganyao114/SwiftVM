#include "support/register_alloc_test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

struct SpilledEaBlock {
    IntrusivePtr<Block> block;
    Value address;
};

SpilledEaBlock MakeSpilledEaBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x8c20})};
    std::vector<Value> retained;
    for (swift::u64 value = 1; value <= 6; ++value) {
        retained.push_back(block->LoadImm(Imm{value}).SetType(ValueType::U64));
    }
    const auto base = block->GetHostGPR(HostRegIndex(6), Imm{0u})
                              .SetType(ValueType::U64);
    const auto address = block->GetOperand(
            Operand{base, Imm{24u}, OperandPlus}).SetType(ValueType::U64);
    const auto stored = block->LoadImm(Imm{swift::u64{0x1122334455667788ull}})
                                .SetType(ValueType::U64);
    block->StoreMemory(Operand{address}, stored);
    auto total = retained.front();
    for (std::size_t i = 1; i < retained.size(); ++i) {
        total = block->Add(total, Operand{retained[i]}).SetType(ValueType::U64);
    }
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), address};
}

std::vector<std::string> Emit(SpilledEaBlock input, bool biased) {
    const GPRSMask gprs{~((1u << 6) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    RegAlloc alloc{input.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocTestSupport::RunForSpillEvictTest(input.block.get(), &alloc, false);
    REQUIRE(alloc.ValueType(input.address) == RegAlloc::MEM);

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .page_table = biased ? reinterpret_cast<void*>(0x1000) : nullptr,
    };
    AddressSpace address_space{config};
    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(input.block.get());
    context.Finish();

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::vector<std::string> instructions;
    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<
            const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<
            const vixl::aarch64::Instruction*>();
    for (auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        instructions.emplace_back(disassembler.GetOutput());
    }
    return instructions;
}

bool Contains(const std::vector<std::string>& instructions,
              std::string_view value) {
    return std::ranges::any_of(instructions, [&](const auto& instruction) {
        return instruction.find(value) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("spilled pinned-base EA materializes at its memory consumer") {
    const auto direct = Emit(MakeSpilledEaBlock(), false);
    const auto biased = Emit(MakeSpilledEaBlock(), true);

    REQUIRE(Contains(direct, "[x6, #24]"));
    REQUIRE(Contains(biased, "add x10, x6, #0x18 (24)"));
    REQUIRE(Contains(biased, "[x10, x24]"));
}
