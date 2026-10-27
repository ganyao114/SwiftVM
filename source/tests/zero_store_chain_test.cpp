#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/compiler.h"

#include <algorithm>
#include <string>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

IntrusivePtr<Block> MakeZeroStore(bool nested, bool observed) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8c40})};
    auto zero = block->LoadImm(ValueType::U64, Imm{swift::u64{0}});
    auto narrowed = block->ZeroExtend32(ValueType::U32, zero);
    Value stored = narrowed;
    if (nested) {
        stored = block->ZeroExtend32To64(ValueType::U64, narrowed);
    }
    if (observed) {
        auto one = block->LoadImm(stored.Type(), Imm{swift::u64{1}});
        (void)block->Add(stored.Type(), stored, Operand{one});
    }
    auto address = block->LoadUniform(Uniform{0, ValueType::U64});
    block->StoreMemory(Operand{address}, stored);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

std::vector<std::string> Emit(IntrusivePtr<Block> block) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
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

std::size_t Count(const std::vector<std::string>& lines, std::string_view text) {
    return std::ranges::count_if(
            lines, [&](const auto& line) { return line.find(text) != std::string::npos; });
}

}  // namespace

TEST_CASE("zero-preserving width chains store through the zero register") {
    const auto narrow = Emit(MakeZeroStore(false, false));
    REQUIRE(Count(narrow, "str wzr") == 1);
    REQUIRE(Count(narrow, "mov w") == 0);
    REQUIRE(Count(narrow, "mov x") == 0);

    const auto nested = Emit(MakeZeroStore(true, false));
    REQUIRE(Count(nested, "str xzr") == 1);
    REQUIRE(Count(nested, "mov w") == 0);
    REQUIRE(Count(nested, "mov x") == 0);

    const auto observed = Emit(MakeZeroStore(false, true));
    REQUIRE(Count(observed, "str wzr") == 0);
    REQUIRE(Count(observed, "str w") == 1);
    REQUIRE(Count(observed, "mov w") + Count(observed, "mov x") > 0);
}
