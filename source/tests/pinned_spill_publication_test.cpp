#include "tests/support/allocation_fixture.h"
#include "runtime/backend/arm64/compile/inputs.h"
using swift::runtime::test::AllocationFixture;
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/fixed_validation.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "support/register_alloc_test_support.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"

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
    const auto long0 = block->LoadImm(ValueType::U32, Imm{1u});
    const auto long1 = block->LoadImm(ValueType::U32, Imm{2u});
    const auto long2 = block->LoadImm(ValueType::U32, Imm{3u});
    const auto source = block->LoadImm(ValueType::U32, Imm{4u});
    const auto result = block->Add(ValueType::U32, source, Operand{Imm{1u}});
    const auto extended = block->ZeroExtend32To64(ValueType::U64, result);
    block->SetHostGPR(extended, HostRegIndex(22), Imm{0u});
    const auto alias = block->BitExtract(ValueType::U32, result, Imm{0u}, Imm{32u});
    const auto compared = block->Sub(ValueType::U32, alias, Operand{long0});
    const auto total = block->Add(ValueType::U32, compared, Operand{long1});
    const auto retained = block->Add(ValueType::U32, total, Operand{long2});
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
        retained.push_back(block->LoadImm(ValueType::U64, Imm{value}));
    }
    const auto result = [&] {
        switch (producer) {
            case AdjacentProducer::Memory: {
                const auto address = block->GetHostGPR(ValueType::U64, HostRegIndex(6), Imm{0u});
                return block->LoadMemory(ValueType::U64, Operand{address});
            }
            case AdjacentProducer::Add: {
                const auto source = block->LoadImm(ValueType::U32, Imm{7u});
                return block->Add(ValueType::U32, source, Operand{Imm{1u}});
            }
            case AdjacentProducer::Immediate:
                return block->LoadImm(ValueType::U32, Imm{9u});
        }
        PANIC();
    }();
    block->SetHostGPR(result, HostRegIndex(22), Imm{0u});
    auto total = retained.front();
    for (std::size_t i = 1; i < retained.size(); ++i) {
        total = block->Add(ValueType::U64, total, Operand{retained[i]});
    }
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), result};
}

std::vector<std::string> Emit(SpillPublicationBlock input) {
    GPRSMask gprs{~((1u << 6) - 1u)};
    FPRSMask fprs{~((1u << 8) - 1u)};
    AllocationFixture alloc{input.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    // Exercise the spill fallback independently of successful preallocation.
    // The fixed-GPR tests cover the new capture/spill-free publication path.
    auto features = FeatureSet{};
    features.ra_spill_evict = false;
    auto prepared = ::swift::runtime::backend::arm64::PrepareAllocationInputs(
            input.block.get(), alloc, features);
    prepared.fixed.clear();
    RegisterAllocTestSupport::RunForSpillEvictTest(input.block.get(), &alloc, std::move(prepared));
    REQUIRE(alloc.ValueType(input.result) == AllocationFixture::MEM);

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                              alloc);
    REQUIRE(alloc.FPRContractRetryCount() == 0);
    REQUIRE(alloc.ValueType(input.result) == AllocationFixture::GPR);
    REQUIRE(alloc.ValueGPR(input.result).id == 22);
    arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
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

TEST_CASE("Single-home constraints are honored by the first allocation under pressure", "[fixed-gpr]") {
    for (auto producer : {AdjacentProducer::Immediate, AdjacentProducer::Memory, AdjacentProducer::Add}) {
        auto input = MakeAdjacentSpillPublicationBlock(producer);
        const GPRSMask gprs{~((1u << 6) - 1u)};
        const FPRSMask fprs{~((1u << 8) - 1u)};
        AllocationFixture baseline{input.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
        auto features = FeatureSet{};
        features.ra_spill_evict = false;
        auto prepared = ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                input.block.get(), baseline, features);
        prepared.fixed.clear();
        RegisterAllocTestSupport::RunForSpillEvictTest(
                input.block.get(), &baseline, std::move(prepared));
        AllocationFixture optimized{input.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocTestSupport::RunForSpillEvictTest(input.block.get(), &optimized, false);
        REQUIRE(optimized.HasFixedGPRDefinition(input.result.Id()));
        REQUIRE(optimized.ValueType(input.result) == AllocationFixture::GPR);
        REQUIRE(optimized.ValueGPR(input.result).id == 22);
        REQUIRE(optimized.SpillCount() <= baseline.SpillCount());
        REQUIRE(optimized.FPRContractRetryCount() == 0);
        auto& check_index =
                optimized.PrepareFacts(input.block.get()).PrepareUses(input.block.get());
        arm64::FixedValidationDescription described{
                input.block.get(), optimized, FeatureSet{}, check_index};
        REQUIRE_NOTHROW(
                compile::VerifyFixedConstraints(input.block.get(), optimized, described.Inputs()));
    }
}
