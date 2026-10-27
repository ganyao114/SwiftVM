#include "tests/support/rewrite_fixture.h"
using swift::runtime::test::RewriteFixture;
#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/compiler.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/ir/hir_builder.h"

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
    auto address = block->GetHostGPR(ValueType::U64, HostRegIndex(29), Imm{0u});
    auto loaded = block->LoadMemory(ValueType::U16, Operand{address});
    auto extended = block->SignExtend(ValueType::S32, loaded);
    block->StoreUniform(Uniform{64, ValueType::S32}, extended);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    AllocationFixture alloc{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            FeatureSet{}};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), alloc, FeatureSet{}),
            false);
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

    ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                              alloc);
    arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
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
    auto source = block->GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
    auto extract = block->BitExtract(ValueType::U16, source, Imm{0u}, Imm{16u});
    auto extended = block->ZeroExtend32(ValueType::U16, extract);
    block->StoreUniform(Uniform{64, ValueType::U16}, Value{extended}.SetCastType(ValueType::U16));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    AllocationFixture alloc{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            FeatureSet{}};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), alloc, FeatureSet{}),
            false);
    alloc.MapRegister(source.Id(), HostGPR{8});
    alloc.MapRegister(extract.Id(), HostGPR{9});
    alloc.MapRegister(extended.Id(), HostGPR{10});
    REQUIRE(alloc.ValueGPR(extract).id != alloc.ValueGPR(extended).id);

    ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                              alloc);
    const auto plan = alloc.Actions().ForBlock(block.get());
    REQUIRE(plan);
    REQUIRE(alloc.Actions().Narrow(extended.Id()));
    REQUIRE(alloc.Actions().Narrow(extended.Id())->kind == arm64::NarrowAction::Extend);
    REQUIRE(alloc.Actions().HasScalar(extract.Id(), arm64::ScalarFlag::BitInputCovered));

    arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
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
        auto source = block->LoadImm(ValueType::U32, Imm{swift::u32{0x12345678}});
        const auto bits = ir::GetValueSizeByte(type) * 8;
        auto extract = block->BitExtract(type, source, Imm{0u}, Imm{bits});
        auto extended = block->ZeroExtend32(type, extract);
        auto shifted = block->LsrImm(ValueType::U32, extended, Imm{1u});
        block->StoreUniform(Uniform{64, ValueType::U32}, shifted);
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
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), alloc, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                                  alloc);
        arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
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
        auto source = block->LoadImm(ValueType::U32, Imm{swift::u32{0xffff1234}});
        auto extract = block->BitExtract(ValueType::U16, source, Imm{0u}, Imm{16u});
        auto mask_value = block->LoadImm(ValueType::U16, Imm{mask});
        auto result = block->And(ValueType::U16, extract, Operand{mask_value});
        block->StoreUniform(Uniform{64, ValueType::U16}, result);
        block->SetTerminal(terminal::ReturnToDispatch{});
        if (fold) {
            swift::runtime::compile::NormalizeWidths(RewriteFixture{block.get(), false}.Get());
            RewriteFixture{block.get()}.Get().CollectDead();
        }
        block->ReIdInstr();

        FeatureSet features{};
        AllocationFixture alloc{block->MaxInstrId(),
                                address_space.GetTrampolines().GetGPRRegs(),
                                address_space.GetTrampolines().GetFPRRegs(),
                                features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &alloc,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), alloc, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                                  alloc);
        arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
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
    auto loaded = block->LoadMemory(ValueType::U16, Operand{Imm{swift::u64{0x1000}}});
    auto extract = block->BitExtract(ValueType::U16, loaded, Imm{0u}, Imm{16u});
    auto extended = block->ZeroExtend32(ValueType::U16, extract);
    block->StoreUniform(Uniform{64, ValueType::U16}, Value{extended}.SetCastType(ValueType::U16));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    AllocationFixture alloc{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            FeatureSet{}};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), alloc, FeatureSet{}),
            false);
    alloc.MapRegister(loaded.Id(), HostGPR{8});
    alloc.MapRegister(extract.Id(), HostGPR{9});
    alloc.MapRegister(extended.Id(), HostGPR{8});
    REQUIRE(alloc.ValueGPR(loaded).id == alloc.ValueGPR(extended).id);
    REQUIRE(alloc.ValueGPR(loaded).id != alloc.ValueGPR(extract).id);

    ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                              alloc);
    arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
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

TEST_CASE("frozen bit and funnel actions preserve values under spill pressure", "[bit-funnel-plan][pressure]") {
#if defined(__aarch64__)
    for (unsigned shape = 0; shape < 6; ++shape) for (const bool pressure : {false, true}) {
        CAPTURE(shape, pressure);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 128; config.stack_alignment = 16;
        AddressSpace space{config}; auto module = space.GetDefaultModule(); FeatureSet features{};
        IntrusivePtr<Block> block{new Block(0, Location{0x8da0})};
        std::vector<Value> live;
        if (pressure)
            for (unsigned i = 0; i < 36; ++i)
                live.push_back(block->LoadUniform(ValueType::U64, Uniform{32, ValueType::U64}));
        Value result;
        if (shape < 3) {
            const auto type = shape == 2 ? ValueType::U8 : ValueType::U32;
            const auto input = block->LoadUniform(type, Uniform{0, type});
            const auto extract = block->BitExtract(ValueType::U8, input, Imm{0u}, Imm{8u});
            if (shape == 2)
                result = block->And(ValueType::U8, extract, Operand{Imm{15u}});
            else {
                result = block->ZeroExtend32(ValueType::U8, extract);
                if (shape == 1) result = block->LsrImm(ValueType::U32, result, Imm{1u});
            }
        } else if (shape == 3) {
            const auto input = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
            const auto shifted = block->LsrImm(ValueType::U32, input, Imm{8u});
            result = block->And(ValueType::U32, shifted, Operand{Imm{1u}});
        } else {
            const auto low = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
            const auto high = block->LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
            const auto right = block->LsrImm(ValueType::U64, low, Imm{32u});
            const auto left = block->LslImm(ValueType::U64, high, Imm{32u});
            result = block->Or(ValueType::U64, right, Operand{left});
            if (shape == 5) block->StoreUniform(Uniform{24, ValueType::U64}, right);
        }
        block->StoreUniform(Uniform{16, result.Type()}, result);
        for (const auto value : live) block->StoreUniform(Uniform{32, ValueType::U64}, value);
        block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        REQUIRE(allocation.Actions().ForBlock(block.get()));
        if (pressure) REQUIRE(allocation.SpillCount() > 0);
        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get()); context.Finish();
        const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer);
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        Runtime runtime{&space}; const swift::u64 low = 0x123456789abcdef0ull;
        const swift::u64 high = 0xfedcba9876543210ull, dummy = 0x1234;
        std::memcpy(runtime.GetUniformBuffer().data(), &low, sizeof(low));
        std::memcpy(runtime.GetUniformBuffer().data() + 8, &high, sizeof(high));
        std::memcpy(runtime.GetUniformBuffer().data() + 32, &dummy, sizeof(dummy));
        runtime.SetLocation(0x8da0); REQUIRE(runtime.Run() == HaltReason::CallHost);
        swift::u64 output{};
        std::memcpy(&output, runtime.GetUniformBuffer().data() + 16, GetValueSizeByte(result.Type()));
        const swift::u64 expected = shape == 0 ? low & 255 : shape == 1 ? (low & 255) >> 1 :
                shape == 2 ? low & 15 : shape == 3 ? (low >> 8) & 1 : (high << 32) | (low >> 32);
        REQUIRE(output == expected);
        if (shape == 5) {
            swift::u64 shared{}; std::memcpy(&shared, runtime.GetUniformBuffer().data() + 24, sizeof(shared));
            REQUIRE(shared == low >> 32);
        }
    }
#endif
}

TEST_CASE("bit preparation preserves a foreign extract and selects a later local mask", "[bit-funnel-plan][cfg]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.region_edges = true; config.uniform_buffer_size = 64; config.stack_alignment = 16;
    AddressSpace space{config}; auto module = space.GetDefaultModule(); FeatureSet features{};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0x8db0}, Location{0x8db2});
    const auto input = function->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    const auto extract = function->BitExtract(ValueType::U8, input, Imm{0u}, Imm{8u});
    const auto masked = function->And(ValueType::U8, extract, Operand{Imm{15u}});
    function->StoreUniform(Uniform{16, ValueType::U8}, masked);
    auto* first = &function->EditBlock(function->GetCurrentBlock());
    auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x8db1}});
    builder.SetCurBlock(next);
    const auto local = function->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    const auto shifted = function->LsrImm(ValueType::U32, local, Imm{8u});
    const auto result = function->And(ValueType::U32, shifted, Operand{Imm{1u}});
    function->StoreUniform(Uniform{16, ValueType::U32}, result);
    function->StoreUniform(Uniform{24, ValueType::U8}, extract);
    function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
    function->ComputeRPO(); function->IdByRPO();
    AllocationFixture allocation{function->MaxInstrCount(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    ::swift::runtime::compile::AllocateFunction(
            function,
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, allocation, features),
            {.block_intervals = false});
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    const auto first_plan = allocation.Actions().ForBlock(first->GetBlock());
    const auto next_plan = allocation.Actions().ForBlock(next->GetBlock());
    REQUIRE(first_plan);
    REQUIRE_FALSE(allocation.Actions().Narrow(masked.Id()));
    REQUIRE(next_plan);
    REQUIRE(allocation.Actions().Narrow(result.Id()));
    REQUIRE(allocation.Actions().Narrow(result.Id())->kind == arm64::NarrowAction::ShiftMask);
    arm64::JitContext context{module, allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(function); context.Finish();
#if defined(__aarch64__)
    const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
    REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer);
    space.PushCodeCache(Location{0x8db0}, buffer.exec_data + context.GetCodeOffset(0x8db0));
    Runtime runtime{&space}; const swift::u32 value = 0x12345678;
    std::memcpy(runtime.GetUniformBuffer().data(), &value, sizeof(value));
    runtime.SetLocation(0x8db0); REQUIRE(runtime.Run() == HaltReason::CallHost);
    swift::u32 output{}; std::memcpy(&output, runtime.GetUniformBuffer().data() + 16, sizeof(output));
    REQUIRE(output == ((value >> 8) & 1));
    REQUIRE(runtime.GetUniformBuffer()[24] == (value & 255));
#endif
}

}  // namespace
