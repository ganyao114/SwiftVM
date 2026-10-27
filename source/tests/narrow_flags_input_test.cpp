#include "tests/support/rewrite_fixture.h"
using swift::runtime::test::RewriteFixture;
#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/compiler.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/actions.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/passes/passes.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

std::vector<std::string> EmitNarrowSub(ValueType type,
                                       bool reuse_extract,
                                       bool branch_only = false,
                                       bool memory_source = false,
                                       bool source_spilled = false) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .global_opts = Optimizations::All,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x8b00})};
    auto source = block->GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
    const auto bits = ir::GetValueSizeByte(type) * 8;
    auto right = Operand{Imm{3u}};
    if (branch_only) {
        right = Operand{block->LoadImm(type, Imm{3u})};
    }
    auto extract = memory_source ? block->LoadMemory(type, Operand{source})
                                 : block->BitExtract(type, source, Imm{0u}, Imm{bits});
    auto result = block->Sub(type, extract, right);
    block->SaveFlags(result, Flags::All);
    if (branch_only) {
        block->InvertCarry();
        block->AdvancePC(Imm{2u});
        block->BranchOnlyEdges();
        const auto condition = block->LocalCondSet(ValueType::U8, Cond::EQ);
        block->SetTerminal(terminal::If{
                condition,
                terminal::LinkBlock{Location{0x8b10}},
                terminal::LinkBlock{Location{0x8b20}},
        });
    }
    if (reuse_extract) {
        block->StoreUniform(Uniform{8, type}, extract);
    }
    if (!branch_only) {
        block->SetTerminal(terminal::ReturnToDispatch{});
    }
    FeatureSet features{};
    if (branch_only) {
        swift::runtime::compile::EliminateFlags(RewriteFixture{block.get(), true, nullptr}.Get(), features);
    }
    block->ReIdInstr();

    AllocationFixture alloc{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(block.get(), alloc, features),
            false);
    if (source_spilled) {
        const auto source_reg = alloc.ValueGPR(source);
        alloc.MapMemSpill(source.Id(), SpillSlot{2});
        // The value lives in a reload register only through its final IR use.
        // Its stack slot never receives a write and cannot satisfy later reads.
        alloc.MapSpillReload(source.Id(), source.Id(), extract.Id(),
                             source_reg, true);
    }
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                              alloc);
    const auto frozen = alloc.Actions().ForBlock(block.get());
    REQUIRE(frozen);
    const bool forwarded =
            !reuse_extract && !memory_source && alloc.ValueType(source) != AllocationFixture::MEM;
    REQUIRE((alloc.Actions().FlagsInput(extract.Id()) != nullptr) == forwarded);
    if (forwarded) {
        const auto* input = alloc.Actions().FlagsInput(extract.Id());
        REQUIRE(input->Source(*result.Def()).Def() ==
                (alloc.GPRLowView(extract.Id()) ? extract.Def() : source.Def()));
        REQUIRE(input->argument == result.Id());
    }

    if (branch_only) {
        const auto plan = alloc.Actions().ForBlock(block.get());
        REQUIRE(plan);
        const auto* branch = alloc.Actions().IntegerBranch(block.get());
        REQUIRE(branch);
        REQUIRE(branch->producer == result.Id());
        REQUIRE(branch->IsNarrow());
        REQUIRE(branch->immediate == 3);
        REQUIRE(branch->width == ir::GetValueSizeByte(type));
    }
    arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
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

std::size_t Count(const std::vector<std::string>& instructions,
                  std::string_view value) {
    return std::ranges::count_if(instructions, [&](const auto& instruction) {
        return instruction.find(value) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("narrow flag alignment consumes low extracts directly") {
    for (const auto type : {ValueType::U8, ValueType::U16}) {
        CAPTURE(type);
        const auto instructions = EmitNarrowSub(type, false);
        REQUIRE(Count(instructions, type == ValueType::U8 ? "uxtb " : "uxth ") ==
                0);
        REQUIRE(Count(instructions, "subs ") == 1);
    }
}

TEST_CASE("narrow flag input keeps shared extracts computed") {
    const auto instructions = EmitNarrowSub(ValueType::U8, true);
    REQUIRE(Count(instructions, "uxtb ") == 1);
    REQUIRE(Count(instructions, "subs ") == 1);
}

TEST_CASE("narrow flag input consumes spilled sources within their reload lifetime") {
    for (const auto type : {ValueType::U8, ValueType::U16}) {
        CAPTURE(type);
        const auto instructions = EmitNarrowSub(type, false, false, false, true);
        CHECK(Count(instructions, "ldr ") == 0);
        CHECK(Count(instructions, type == ValueType::U8 ? "uxtb " : "uxth ") == 1);
    }
}

TEST_CASE("dead narrow immediate branches consume low extracts directly") {
    const auto instructions = EmitNarrowSub(ValueType::U8, false, true);
    REQUIRE(Count(instructions, "uxtb ") == 1);
    REQUIRE(Count(instructions, "cmp ") == 1);
    REQUIRE(Count(instructions, "lsl ") == 0);
}

TEST_CASE("dead narrow immediate branches keep loaded widths") {
    for (const auto type : {ValueType::U8, ValueType::U16}) {
        CAPTURE(type);
        const auto instructions = EmitNarrowSub(type, false, true, true);
        REQUIRE(Count(instructions, type == ValueType::U8 ? "ldrb " : "ldrh ") ==
                1);
        REQUIRE(Count(instructions, type == ValueType::U8 ? "uxtb " : "uxth ") ==
                0);
        REQUIRE(Count(instructions, "cmp ") == 1);
    }
}
