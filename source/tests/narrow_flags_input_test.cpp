#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/flags_elimination_pass.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

std::vector<std::string> EmitNarrowSub(ValueType type,
                                       bool reuse_extract,
                                       bool branch_only = false,
                                       bool memory_source = false) {
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
    auto source = block->GetHostGPR(HostRegIndex(22), Imm{0u})
                          .SetType(ValueType::U64);
    const auto bits = ir::GetValueSizeByte(type) * 8;
    auto right = Operand{Imm{3u}};
    if (branch_only) {
        right = Operand{block->LoadImm(Imm{3u}).SetType(type)};
    }
    auto extract = memory_source
            ? block->LoadMemory(Operand{source}).SetType(type)
            : block->BitExtract(source, Imm{0u}, Imm{bits}).SetType(type);
    auto result = block->Sub(extract, right).SetType(type);
    block->SaveFlags(result, Flags::All);
    if (branch_only) {
        block->InvertCarry();
        block->AdvancePC(Imm{2u});
        block->BranchOnlyEdges();
        const auto condition = block->LocalCondSet(Cond::EQ)
                                       .SetType(ValueType::U8);
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
        FlagsEliminationPass::Run(block.get(), nullptr, features);
    }
    block->ReIdInstr();

    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
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
