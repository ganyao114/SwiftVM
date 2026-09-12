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

struct SpillPublicationBlock {
    IntrusivePtr<Block> block;
    Value result;
};

SpillPublicationBlock MakeSpillPublicationBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x8b40})};
    const auto long0 = block->LoadImm(Imm{1u}).SetType(ValueType::U32);
    const auto long1 = block->LoadImm(Imm{2u}).SetType(ValueType::U32);
    const auto long2 = block->LoadImm(Imm{3u}).SetType(ValueType::U32);
    const auto source = block->LoadImm(Imm{4u}).SetType(ValueType::U32);
    const auto result = block->Add(source, Operand{Imm{1u}})
                                .SetType(ValueType::U32);
    const auto extended = block->ZeroExtend32To64(result)
                                  .SetType(ValueType::U64);
    block->SetHostGPR(extended, HostRegIndex(22), Imm{0u});
    const auto alias = block->BitExtract(result, Imm{0u}, Imm{32u})
                               .SetType(ValueType::U32);
    const auto compared = block->Sub(alias, Operand{long0})
                                  .SetType(ValueType::U32);
    const auto total = block->Add(compared, Operand{long1})
                               .SetType(ValueType::U32);
    const auto retained = block->Add(total, Operand{long2})
                                  .SetType(ValueType::U32);
    block->StoreUniform(Uniform{0, ValueType::U32}, retained);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), result};
}

enum class AdjacentProducer {
    Immediate,
    Memory,
    Add,
};

SpillPublicationBlock MakeAdjacentSpillPublicationBlock(
        AdjacentProducer producer) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8b60})};
    std::vector<Value> retained;
    for (swift::u64 value = 1; value <= 6; ++value) {
        retained.push_back(block->LoadImm(Imm{value}).SetType(ValueType::U64));
    }
    const auto result = [&] {
        switch (producer) {
            case AdjacentProducer::Memory: {
                const auto address = block->GetHostGPR(HostRegIndex(6), Imm{0u})
                                             .SetType(ValueType::U64);
                return block->LoadMemory(Operand{address}).SetType(ValueType::U64);
            }
            case AdjacentProducer::Add: {
                const auto source = block->LoadImm(Imm{7u}).SetType(ValueType::U32);
                return block->Add(source, Operand{Imm{1u}}).SetType(ValueType::U32);
            }
            case AdjacentProducer::Immediate:
                return block->LoadImm(Imm{9u}).SetType(ValueType::U32);
        }
        PANIC();
    }();
    block->SetHostGPR(result, HostRegIndex(22), Imm{0u});
    auto total = retained.front();
    for (std::size_t i = 1; i < retained.size(); ++i) {
        total = block->Add(total, Operand{retained[i]}).SetType(ValueType::U64);
    }
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), result};
}

std::vector<std::string> Emit(SpillPublicationBlock input) {
    GPRSMask gprs{~((1u << 6) - 1u)};
    FPRSMask fprs{~((1u << 8) - 1u)};
    RegAlloc alloc{input.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocTestSupport::RunForSpillEvictTest(input.block.get(), &alloc, false);
    REQUIRE(alloc.ValueType(input.result) == RegAlloc::MEM);

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
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

TEST_CASE("spilled U32 additions publish directly to pinned GPRs") {
    const auto instructions = Emit(MakeSpillPublicationBlock());
    REQUIRE(Contains(instructions, "add w22"));
    REQUIRE(Contains(instructions, "sub w"));
    REQUIRE_FALSE(Contains(instructions, "mov w22, w18"));
}

TEST_CASE("adjacent spilled values publish in their pinned GPR") {
    const auto immediate = Emit(MakeAdjacentSpillPublicationBlock(
            AdjacentProducer::Immediate));
    const auto memory = Emit(MakeAdjacentSpillPublicationBlock(
            AdjacentProducer::Memory));
    const auto add = Emit(MakeAdjacentSpillPublicationBlock(
            AdjacentProducer::Add));
    REQUIRE(Contains(immediate, "mov w22, #0x9"));
    REQUIRE(Contains(memory, "ldr x22, [x6]"));
    REQUIRE(Contains(add, "add w22"));
}
