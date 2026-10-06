#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/deadcode_elimination_pass.h"
#include "runtime/ir/opts/integer_width_elimination_pass.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

std::vector<std::string> Disassemble(arm64::JitContext& context) {
    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::vector<std::string> lines;
    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<
            const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<
            const vixl::aarch64::Instruction*>();
    for (auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        lines.emplace_back(disassembler.GetOutput());
    }
    return lines;
}

TEST_CASE("a narrow load writes an adjacent extension register directly") {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x8a00})};
    auto address = block->GetHostGPR(HostRegIndex(29), Imm{0u})
                           .SetType(ValueType::U64);
    auto loaded = block->LoadMemory(Operand{address}).SetType(ValueType::U16);
    auto extended = block->SignExtend(loaded).SetType(ValueType::S32);
    block->StoreUniform(Uniform{64, ValueType::S32}, extended);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &alloc, false, FeatureSet{});
    alloc.MapRegister(address.Id(), HostGPR{29});
    alloc.MapRegister(loaded.Id(), HostGPR{8});
    alloc.MapRegister(extended.Id(), HostGPR{9});
    auto active_gprs = address_space.GetTrampolines().GetGPRRegs();
    active_gprs.Mark(8);
    active_gprs.Mark(9);
    active_gprs.Mark(29);
    auto active_fprs = address_space.GetTrampolines().GetFPRRegs();
    alloc.SetActiveRegs(loaded.Id(), active_gprs, active_fprs);
    REQUIRE(alloc.ValueGPR(address).id == 29);
    REQUIRE(alloc.ValueGPR(loaded).id != alloc.ValueGPR(extended).id);

    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    const auto lines = Disassemble(context);
    const auto direct = std::ranges::count_if(lines, [](const auto& line) {
        return line.find("ldrsh w9") != std::string::npos;
    });
    const auto separate = std::ranges::count_if(lines, [](const auto& line) {
        return line.find("sxth ") != std::string::npos;
    });
    REQUIRE(direct == 1);
    REQUIRE(separate == 0);
}

TEST_CASE("an adjacent narrow extract extends in one instruction") {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x8a20})};
    auto source = block->GetHostGPR(HostRegIndex(22), Imm{0u})
                          .SetType(ValueType::U64);
    auto extract = block->BitExtract(source, Imm{0u}, Imm{16u})
                           .SetType(ValueType::U16);
    auto extended = block->ZeroExtend32(extract).SetType(ValueType::U16);
    block->StoreUniform(Uniform{64, ValueType::U16}, extended);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &alloc, false, FeatureSet{});
    alloc.MapRegister(source.Id(), HostGPR{8});
    alloc.MapRegister(extract.Id(), HostGPR{9});
    alloc.MapRegister(extended.Id(), HostGPR{10});
    REQUIRE(alloc.ValueGPR(extract).id != alloc.ValueGPR(extended).id);

    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    const auto lines = Disassemble(context);
    const auto extensions = std::ranges::count_if(lines, [](const auto& line) {
        return line.find("uxth ") != std::string::npos;
    });
    const auto direct = std::ranges::count_if(lines, [](const auto& line) {
        return line.find("uxth w10, w8") != std::string::npos;
    });
    REQUIRE(extensions == 1);
    REQUIRE(direct == 1);
}

TEST_CASE("an adjacent narrow extract and right shift emit one bitfield extract") {
    for (auto type : {ValueType::U8, ValueType::U16}) {
        CAPTURE(type);
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        IntrusivePtr<Block> block{new Block(0, Location{0x8a30})};
        auto source = block->LoadImm(Imm{swift::u32{0x12345678}})
                              .SetType(ValueType::U32);
        const auto bits = ir::GetValueSizeByte(type) * 8;
        auto extract = block->BitExtract(source, Imm{0u}, Imm{bits})
                               .SetType(type);
        auto extended = block->ZeroExtend32(extract).SetType(type);
        auto shifted = block->LsrImm(extended, Imm{1u})
                               .SetType(ValueType::U32);
        block->StoreUniform(Uniform{64, ValueType::U32}, shifted);
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

        const auto lines = Disassemble(context);
        const auto width = type == ValueType::U8 ? "#7" : "#15";
        REQUIRE(std::ranges::count_if(lines, [&](const auto& line) {
            return line.find("ubfx w") != std::string::npos &&
                   line.find("#1") != std::string::npos &&
                   line.find(width) != std::string::npos;
        }) == 1);
        REQUIRE(std::ranges::none_of(lines, [](const auto& line) {
            return line.find("uxtb ") != std::string::npos ||
                   line.find("uxth ") != std::string::npos ||
                   line.find("lsr ") != std::string::npos;
        }));
    }
}

TEST_CASE("a narrow AND mask subsumes its low extract only when high bits are clear") {
    auto run = [](swift::u32 mask, bool fold) {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        IntrusivePtr<Block> block{new Block(0, Location{0x8a38})};
        auto source = block->LoadImm(Imm{swift::u32{0xffff1234}})
                              .SetType(ValueType::U32);
        auto extract = block->BitExtract(source, Imm{0u}, Imm{16u})
                               .SetType(ValueType::U16);
        auto mask_value = block->LoadImm(Imm{mask}).SetType(ValueType::U16);
        auto result = block->And(extract, Operand{mask_value})
                              .SetType(ValueType::U16);
        block->StoreUniform(Uniform{64, ValueType::U16}, result);
        block->SetTerminal(terminal::ReturnToDispatch{});
        if (fold) {
            IntegerWidthEliminationPass::Run(block.get());
            DeadCodeEliminationPass::Run(block.get());
        }
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
    };

    const auto fused = run(0xa001, true);
    REQUIRE(std::ranges::none_of(fused, [](const auto& line) {
        return line.find("uxth ") != std::string::npos;
    }));
    REQUIRE(std::ranges::count_if(fused, [](const auto& line) {
        return line.find("and w") != std::string::npos;
    }) == 1);

    // Without the IR rewrite the parent is dead before mask_value. The
    // emitter must retain the extract instead of extending that lifetime.
    const auto unoptimized = run(0xa001, false);
    REQUIRE(std::ranges::count_if(unoptimized, [](const auto& line) {
        return line.find("uxth ") != std::string::npos;
    }) == 1);

    const auto retained = run(0x1a001, true);
    REQUIRE(std::ranges::count_if(retained, [](const auto& line) {
        return line.find("uxth ") != std::string::npos;
    }) == 1);
}

TEST_CASE("a zero-extended narrow load needs no self extension") {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x8a40})};
    auto loaded = block->LoadMemory(Operand{Imm{swift::u64{0x1000}}})
                          .SetType(ValueType::U16);
    auto extract = block->BitExtract(loaded, Imm{0u}, Imm{16u})
                           .SetType(ValueType::U16);
    auto extended = block->ZeroExtend32(extract).SetType(ValueType::U16);
    block->StoreUniform(Uniform{64, ValueType::U16}, extended);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &alloc, false, FeatureSet{});
    alloc.MapRegister(loaded.Id(), HostGPR{8});
    alloc.MapRegister(extract.Id(), HostGPR{9});
    alloc.MapRegister(extended.Id(), HostGPR{8});
    REQUIRE(alloc.ValueGPR(loaded).id == alloc.ValueGPR(extended).id);
    REQUIRE(alloc.ValueGPR(loaded).id != alloc.ValueGPR(extract).id);

    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    const auto lines = Disassemble(context);
    const auto loads = std::ranges::count_if(lines, [](const auto& line) {
        return line.find("ldrh w8") != std::string::npos;
    });
    const auto extensions = std::ranges::count_if(lines, [](const auto& line) {
        return line.find("uxth ") != std::string::npos;
    });
    REQUIRE(loads == 1);
    REQUIRE(extensions == 0);
}

}  // namespace
