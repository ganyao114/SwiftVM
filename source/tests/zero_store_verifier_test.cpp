#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "runtime/backend/arm64/alloc/zero_store_verifier.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/runtime.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/opts/register_alloc_pass.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("zero store certificates require all compatible local consumers", "[zero-store-plan]") {
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xc100})};
        const auto zero = block->LoadImm(Imm{scenario == 1 ? 1u : 0u}).SetType(ValueType::U32);
        Value extended;
        if (scenario == 2 || scenario == 3 || scenario == 4) {
            extended = block->ZeroExtend32To64(zero).SetType(ValueType::U64);
            block->StoreUniform(Uniform{0, ValueType::U64}, extended);
        } else if (scenario == 5) {
            const auto result = block->Add(zero, Operand{Imm{1u}}).SetType(ValueType::U32);
            block->StoreUniform(Uniform{0, ValueType::U32}, result);
        } else if (scenario == 6) {
            block->StoreMemory(Operand{zero}, zero);
        } else block->StoreUniform(Uniform{0, ValueType::U32}, zero);
        if (scenario == 4) block->StoreUniform(Uniform{8, ValueType::U32}, zero);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(zero.Id(), HostGPR{14});
        if (extended.Defined()) allocation.MapRegister(extended.Id(), HostGPR{15});
        if (scenario == 3) allocation.MapMemSpill(extended.Id(), SpillSlot{0});
        if (scenario == 7) allocation.MapMemSpill(zero.Id(), SpillSlot{0});
        arm64::BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        const auto ids = arm64::VerifyZeroStoreValues(analysis, allocation);
        const bool accepted = scenario == 0 || scenario == 2 || scenario == 4;
        REQUIRE((std::find(ids.begin(), ids.end(), zero.Id()) != ids.end()) == accepted);
        if (extended.Defined()) REQUIRE((std::find(ids.begin(), ids.end(), extended.Id()) != ids.end()) == (scenario != 3));
    }
}

TEST_CASE("zero store validation memoizes shared extension consumers", "[zero-store-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xc108})};
    auto value = block->LoadImm(Imm{0u}).SetType(ValueType::U32);
    std::vector<Value> values{value};
    for (unsigned i = 0; i < 128; ++i) {
        value = block->ZeroExtend32(value).SetType(ValueType::U32);
        values.push_back(value);
        block->StoreUniform(Uniform{0, ValueType::U32}, value);
    }
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    for (auto item : values) allocation.MapRegister(item.Id(), HostGPR{14});
    arm64::BlockAnalysisIndex analysis;
    analysis.Build(block.get());
    const auto ids = arm64::VerifyZeroStoreValues(analysis, allocation);
    REQUIRE(ids.size() == values.size());
    allocation.BeginTargetPlanning();
    for (auto id : ids) allocation.CertifyZeroStore(id);
    REQUIRE(allocation.ZeroStoreVerified(values.front().Id()));
    allocation.AbortTargetPlanning();
    REQUIRE_FALSE(allocation.ZeroStoreVerified(values.front().Id()));
}

TEST_CASE("incoming zero store proof is scoped to its consuming block", "[zero-store-plan][incoming]") {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        IntrusivePtr<Block> predecessor{new Block(0, Location{0xc110})};
        const auto zero = predecessor->LoadImm(Imm{0u}).SetType(ValueType::U32);
        if (scenario == 1) predecessor->StoreUniform(Uniform{0, ValueType::U32}, zero);
        predecessor->ReIdInstr();
        IntrusivePtr<Block> block{new Block(0, Location{0xc118})};
        Value extension;
        if (scenario == 2) {
            extension = block->ZeroExtend32To64(zero).SetType(ValueType::U64);
            block->StoreUniform(Uniform{8, ValueType::U64}, extension);
        } else block->StoreUniform(Uniform{8, ValueType::U32}, zero);
        swift::u16 id = 4;
        for (auto& inst : block->GetInstList()) inst.SetId(id++);
        RegAlloc allocation{8, GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(zero.Id(), HostGPR{14});
        if (extension.Defined()) allocation.MapRegister(extension.Id(), HostGPR{15});
        if (scenario == 3) allocation.MapMemSpill(zero.Id(), SpillSlot{0});
        arm64::BlockAnalysisIndex before, consumer;
        before.Build(predecessor.get()); consumer.Build(block.get());
        REQUIRE(arm64::VerifyZeroStoreValues(before, allocation).empty());
        std::vector<const Inst*> incoming;
        const auto local = arm64::VerifyZeroStoreValues(consumer, allocation, &incoming);
        REQUIRE(incoming.size() == (scenario == 0 || scenario == 2 ? 1 : 0));
        if (!incoming.empty()) REQUIRE(incoming.front() == zero.Def());
        REQUIRE((std::find(local.begin(), local.end(), zero.Id()) == local.end()));
        REQUIRE(local.size() == (scenario == 2 ? 1 : 0));
    }
}

TEST_CASE("function finalization binds incoming zero without eliding predecessor materialization", "[zero-store-plan][incoming]") {
    const auto store_type = GENERATE(ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64);
    CAPTURE(store_type);
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.region_edges = true;
    config.uniform_buffer_size = 64;
    config.stack_alignment = 16;
    AddressSpace space{config};
    HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(Location{0xc120}, Location{0xc122});
    const auto zero = function->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
    auto* body = builder.LinkBlock(terminal::LinkBlock{Location{0xc121}});
    builder.SetCurBlock(body);
    function->StoreUniform(Uniform{0, store_type}, Value{zero}.SetCastType(store_type));
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    FeatureSet features{};
    RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(function, &allocation, features);
    allocation.CompleteGPRAllocation(space.GetDefaultModule());
    arm64::JitContext context{space.GetDefaultModule(), allocation};
    REQUIRE_FALSE(allocation.ZeroStoreVerified(zero.Id()));
    const auto plan = allocation.GPRPlanFor(body->GetBlock());
    REQUIRE(plan);
    REQUIRE(plan->consumers);
    REQUIRE(plan->consumers->incoming_zeros.contains(zero.Def()));
    arm64::JitTranslator translator{context};
    REQUIRE_NOTHROW(translator.Translate(function));
    context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);
#if defined(__aarch64__)
    auto module = space.GetDefaultModule();
    const auto [cache_id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
    REQUIRE(cache_id != INVALID_CACHE_ID);
    context.Flush(buffer);
    REQUIRE(module->Push(function->GetFunction()));
    function->ReleaseFunctionOwnership();
    space.PushCodeCache(Location{0xc120}, buffer.exec_data + context.GetCodeOffset(LocationDescriptor{0xc120}));
    const auto body_offset = context.GetCodeOffset(LocationDescriptor{0xc121});
    space.PushCodeCache(Location{0xc121}, buffer.exec_data + body_offset);
    for (const auto entry : {swift::u64{0xc120}, swift::u64{0xc121}}) {
        Runtime runtime{&space};
        std::memset(runtime.GetUniformBuffer().data(), 0xa5, runtime.GetUniformBuffer().size());
        runtime.SetLocation(entry);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        const auto width = GetValueSizeByte(store_type);
        for (unsigned byte = 0; byte < width; ++byte) REQUIRE(runtime.GetUniformBuffer()[byte] == 0);
        REQUIRE(runtime.GetUniformBuffer()[width] == 0xa5);
    }
#endif
}
