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
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

enum class PinnedWidthShape {
    NarrowSelect,
    PublishedLow32,
    PublishedLow32MultiUse,
    PublishedZero8,
    PublishedZero8Compare,
    PublishedZero8Store,
    PublishedSign8,
    SelectPublication,
    SelectPublicationFault,
};

std::vector<std::string> EmitWidthFact(PinnedWidthShape shape, bool overwrite_target) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x9750})};

    if (shape == PinnedWidthShape::NarrowSelect) {
        auto address = block->LoadImm(ValueType::U64, Imm{swift::u64{0x1000}});
        auto loaded = block->LoadMemory(ValueType::U16, Operand{address});
        auto widened = block->ZeroExtend32(ValueType::U32, loaded);
        auto published = block->ZeroExtend32To64(ValueType::U64, widened);
        block->SetHostGPR(published, HostRegIndex(22), Imm{0u});
        if (overwrite_target) {
            auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
            block->SetHostGPR(replacement, HostRegIndex(22), Imm{0u});
        }
        auto condition = block->LoadImm(ValueType::U32, Imm{swift::u32{1}});
        auto alternate = block->LoadImm(ValueType::U32, Imm{swift::u32{7}});
        auto selected = block->Select(ValueType::U32, condition, alternate, widened);
        block->StoreUniform(Uniform{64, ValueType::U32}, selected);
    } else if (shape == PinnedWidthShape::PublishedLow32 ||
               shape == PinnedWidthShape::PublishedLow32MultiUse) {
        auto base = block->GetHostGPR(ValueType::U64, HostRegIndex(1), Imm{0u});
        auto value = block->Add(ValueType::U64, base, Operand{Imm{1u}});
        block->SetHostGPR(value, HostRegIndex(22), Imm{0u});
        if (overwrite_target) {
            auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
            block->SetHostGPR(replacement, HostRegIndex(22), Imm{0u});
        }
        auto low = block->BitExtract(ValueType::U32, value, Imm{0u}, Imm{32u});
        auto compare = block->Sub(ValueType::U32, low, Operand{Imm{8u}});
        block->SaveFlags(compare, Flags::All);
        if (shape == PinnedWidthShape::PublishedLow32MultiUse) {
            auto sum = block->Add(ValueType::U32, low, Operand{Imm{3u}});
            block->StoreUniform(Uniform{72, ValueType::U32}, sum);
        }
    } else if (shape == PinnedWidthShape::PublishedZero8 ||
               shape == PinnedWidthShape::PublishedZero8Compare ||
               shape == PinnedWidthShape::PublishedZero8Store) {
        auto address = block->LoadImm(ValueType::U64, Imm{swift::u64{0x1000}});
        auto narrow = block->LoadMemory(ValueType::U8, Operand{address});
        auto widened = block->ZeroExtend32(ValueType::U32, narrow);
        auto published = block->ZeroExtend32To64(ValueType::U64, widened);
        block->SetHostGPR(published, HostRegIndex(22), Imm{0u});
        if (overwrite_target) {
            auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
            block->SetHostGPR(replacement, HostRegIndex(22), Imm{0u});
        }
        if (shape == PinnedWidthShape::PublishedZero8Compare) {
            auto compare = block->Sub(ValueType::U8, narrow, Operand{Imm{swift::u8{5}}});
            block->SaveFlags(compare, Flags::Carry);
        } else if (shape == PinnedWidthShape::PublishedZero8Store) {
            auto output = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
            block->StoreMemory(Operand{output}, narrow);
        } else {
            auto reused = block->ZeroExtend32(ValueType::U32, narrow);
            block->StoreUniform(Uniform{80, ValueType::U32}, reused);
        }
    } else if (shape == PinnedWidthShape::PublishedSign8) {
        auto address = block->LoadImm(ValueType::U64, Imm{swift::u64{0x1000}});
        auto narrow = block->LoadMemory(ValueType::S8, Operand{address});
        auto published = block->SignExtend(ValueType::U64, narrow);
        block->SetHostGPR(published, HostRegIndex(22), Imm{0u});
        if (overwrite_target) {
            auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
            block->SetHostGPR(replacement, HostRegIndex(22), Imm{0u});
        }
        auto reused = block->SignExtend(ValueType::U64, narrow);
        block->StoreUniform(Uniform{88, ValueType::U64}, reused);
    } else {
        auto test = block->LoadImm(ValueType::U64, Imm{swift::u64{3}});
        auto zero = block->LoadImm(ValueType::U64, Imm{swift::u64{0}});
        auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{9}});
        auto selected = block->SelectZero(ValueType::U64, test, zero, value);
        if (shape == PinnedWidthShape::SelectPublicationFault) {
            auto address = block->LoadImm(ValueType::U64, Imm{swift::u64{0x1000}});
            auto observed = block->LoadMemory(ValueType::U64, Operand{address});
            block->StoreUniform(Uniform{80, ValueType::U64}, observed);
        }
        auto published = block->ZeroExtend32To64(ValueType::U64, selected);
        block->SetHostGPR(published, HostRegIndex(22), Imm{0u});
        if (overwrite_target) {
            auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
            block->SetHostGPR(replacement, HostRegIndex(22), Imm{0u});
        }
        auto low = block->BitExtract(ValueType::U32, published, Imm{0u}, Imm{32u});
        auto sum = block->Add(ValueType::U32, low, Operand{Imm{1u}});
        block->StoreUniform(Uniform{88, ValueType::U32}, sum);
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
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::vector<std::string> lines;
    for (auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        lines.emplace_back(disassembler.GetOutput());
    }
    return lines;
}

std::size_t Count(const std::vector<std::string>& lines,
                  std::string_view first,
                  std::string_view second = {}) {
    return std::ranges::count_if(lines, [&](const auto& line) {
        return line.find(first) != std::string::npos && line.find(second) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("a published narrow load feeds Select from its pinned W view") {
    const auto lines = EmitWidthFact(PinnedWidthShape::NarrowSelect, false);
    REQUIRE(Count(lines, "ldrh w22") == 1);
    REQUIRE(Count(lines, "csel w", ", w22, ne") == 1);
}

TEST_CASE("overwriting a published narrow value rejects its pinned W view") {
    const auto lines = EmitWidthFact(PinnedWidthShape::NarrowSelect, true);
    REQUIRE(Count(lines, "ldrh w22") == 0);
    REQUIRE(Count(lines, "csel w", ", w22, ne") == 0);
}

TEST_CASE("a published full-width value exposes its pinned low W view") {
    const auto lines = EmitWidthFact(PinnedWidthShape::PublishedLow32, false);
    REQUIRE(Count(lines, "lsr w", "#0") == 0);
    REQUIRE(Count(lines, "subs w", "w22, #0x8") == 1);
}

TEST_CASE("overwriting a full-width publication preserves its low capture") {
    const auto lines = EmitWidthFact(PinnedWidthShape::PublishedLow32, true);
    REQUIRE(Count(lines, "lsr w", "#0") == 1);
    REQUIRE(Count(lines, "subs w", "w22, #0x8") == 0);
}

TEST_CASE("a published low view serves multiple consumers from one value version") {
    const auto lines = EmitWidthFact(
            PinnedWidthShape::PublishedLow32MultiUse, false);
    REQUIRE(Count(lines, "lsr w", "#0") == 0);
    REQUIRE(Count(lines, "subs w", "w22, #0x8") == 1);
    REQUIRE(Count(lines, "add w", "w22, #0x3") == 1);
}

TEST_CASE("a published U8 zero extension serves a later extension from its pinned home") {
    const auto lines = EmitWidthFact(PinnedWidthShape::PublishedZero8, false);
    // The first extension still cleans into the publication home; the
    // later extension is tied to the load result register and elided.
    REQUIRE(Count(lines, "uxtb") == 1);
    REQUIRE(Count(lines, "mov w", ", w22") == 0);
}

TEST_CASE("a published S8 sign extension serves a later extension from its pinned home") {
    const auto lines = EmitWidthFact(PinnedWidthShape::PublishedSign8, false);
    REQUIRE(Count(lines, "sxtb") == 1);
    REQUIRE(Count(lines, "mov x", ", x22") == 1);
}

TEST_CASE("published narrow facts feed compare and memory consumers") {
    const auto compare = EmitWidthFact(
            PinnedWidthShape::PublishedZero8Compare, false);
    const auto store = EmitWidthFact(PinnedWidthShape::PublishedZero8Store, false);
    REQUIRE(Count(compare, "cmp w22", "#0x5") == 1);
    REQUIRE(Count(store, "strb w22") == 1);
}

TEST_CASE("overwriting narrow extension facts rejects the pinned home") {
    const auto zero = EmitWidthFact(PinnedWidthShape::PublishedZero8, true);
    const auto sign = EmitWidthFact(PinnedWidthShape::PublishedSign8, true);
    REQUIRE(Count(zero, "uxtb") == 1);
    REQUIRE(Count(zero, "mov w", ", w22") == 0);
    REQUIRE(Count(sign, "sxtb") == 2);
    REQUIRE(Count(sign, "mov x", ", x22") == 0);
}

TEST_CASE("a zero-extended SelectZero result publishes directly to its pinned home") {
    const auto lines = EmitWidthFact(PinnedWidthShape::SelectPublication, false);
    REQUIRE(Count(lines, "csel w22", "eq") == 1);
    REQUIRE(Count(lines, "ubfx", "w22") == 0);
}

TEST_CASE("overwriting a SelectZero publication preserves its allocated result") {
    const auto lines = EmitWidthFact(PinnedWidthShape::SelectPublication, true);
    REQUIRE(Count(lines, "csel w22", "eq") == 0);
}

TEST_CASE("a fault before SelectZero publication keeps the architectural home unchanged") {
    const auto lines = EmitWidthFact(PinnedWidthShape::SelectPublicationFault, false);
    REQUIRE(Count(lines, "csel w22", "eq") == 0);
}
