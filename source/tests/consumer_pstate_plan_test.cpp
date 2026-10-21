#include <catch2/catch_test_macros.hpp>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include "runtime/backend/arm64/alloc/consumer_plan.h"
#include "runtime/backend/arm64/alloc/boolean_select_plan.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/arm64/jit/function_code_object_emitter.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/common/div128.h"

using namespace swift::runtime;
using namespace swift::runtime::ir;
using namespace swift::runtime::backend::arm64;

TEST_CASE("named FCmp representation is consistent across internal block edges", "[consumer-plan][fcmp-condition]") {
#if defined(__aarch64__)
    for (unsigned mode : {0u, 1u, 2u}) {
        DYNAMIC_SECTION("publication mode " << mode) {
        const bool publish = mode == 1;
        CAPTURE(mode, publish);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 4096; config.stack_alignment = 16; config.region_edges = true;
        swift::runtime::backend::AddressSpace space{config};
        auto module = space.GetDefaultModule();
        HIRBuilder builder{1, true, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{0xbf90}, Location{0xbfa1});
        const auto left = function->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        const auto right = function->LoadUniform(Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        const auto compared = function->VecFCmp(left, right, Imm{swift::u32{64}}, Imm{swift::u32{1}}).SetType(ValueType::U64);
        if (publish) function->PublishFCmpFlags(compared, Imm{swift::u32{1}});
        auto* second = builder.LinkBlock(terminal::LinkBlock{Location{0xbfa0}});
        builder.SetCurBlock(second);
        if (!publish) {
            const auto one = function->LoadImm(Imm{swift::u32{1}}).SetType(ValueType::U32);
            const auto result = function->Add(one, ir::Operand{one}).SetType(ValueType::U32);
            function->SaveFlags(result, Flags::NZCV);
        }
        if (mode == 2) function->PublishFCmpFlags(compared, Imm{swift::u32{1}});
        else {
            const auto condition = function->FCmpCondSet(compared, publish ? Cond::VS : Cond::LT).SetType(ValueType::U8);
            function->StoreUniform(Uniform{256, ValueType::U8}, condition);
        }
        function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
        function->ComputeRPO(); function->IdByRPO();
        swift::runtime::backend::RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
        RegisterAllocPass::Run(function, &allocation, FeatureSet{});
        allocation.CompleteGPRAllocation(module);
        JitContext context{module, allocation}; JitTranslator translator{context};
        translator.Translate(function); context.Finish();
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != swift::runtime::backend::INVALID_CACHE_ID);
        context.Flush(buffer); REQUIRE(module->Push(function->GetFunction()));
        function->ReleaseFunctionOwnership();
        space.PushCodeCache(function->GetFunction()->GetStartLocation(), buffer.exec_data);
        swift::runtime::Runtime runtime{&space};
        const double first = publish ? std::numeric_limits<double>::quiet_NaN() : 4.0;
        const double second_value = 3.0;
        std::memcpy(runtime.GetUniformBuffer().data(), &first, sizeof(first));
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &second_value, sizeof(second_value));
        runtime.SetLocation(0xbf90);
        REQUIRE(runtime.Run() == swift::runtime::HaltReason::CallHost);
        if (mode == 2) {
            const auto raw_flags = runtime.GetState()->host_cpu_flags;
            REQUIRE((raw_flags & (swift::u64{1} << HostFlagsBit::C)) != 0);
            REQUIRE((raw_flags & (swift::u64{1} << HostFlagsBit::Z)) == 0);
        } else REQUIRE(runtime.GetUniformBuffer()[256] == publish);
        }
    }
#endif
}

TEST_CASE("FCmp condition retains its named relation across later flags producers", "[consumer-plan][fcmp-condition]") {
#if defined(__aarch64__)
    for (unsigned bits : {32u, 64u}) for (bool pressure : {false, true})
    for (bool compact : {false, true}) for (bool publish : {false, true})
    for (unsigned change : {0u, 1u, 2u, 3u, 4u, 5u}) for (unsigned relation = 0; relation < 4; ++relation)
    for (unsigned condition_code = 0; condition_code < 16; ++condition_code) {
        // Publication-only cases have no named condition; one relation row
        // verifies their delayed publication without duplicating Cond rows.
        if (change == 5 && (!publish || condition_code)) continue;
        CAPTURE(bits, pressure, compact, publish, change, relation, condition_code);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 4096; config.stack_alignment = 16;
        swift::runtime::backend::AddressSpace space{config};
        auto module = space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block(0, Location{0xbf60})};
        std::vector<Value> live;
        if (pressure) for (unsigned i = 0; i < 36; ++i)
            live.push_back(block->LoadUniform(Uniform{static_cast<swift::u16>(512 + i * 8), ValueType::U64}).SetType(ValueType::U64));
        const auto left = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        const auto right = block->LoadUniform(Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        const auto integer = block->LoadUniform(Uniform{32, ValueType::U32}).SetType(ValueType::U32);
        const auto one = block->LoadImm(Imm{swift::u32{1}}).SetType(ValueType::U8);
        const auto zero = block->LoadImm(Imm{swift::u32{0}}).SetType(ValueType::U8);
        const auto compared = block->VecFCmp(left, right, Imm{static_cast<swift::u32>(bits)}, Imm{swift::u32{compact}}).SetType(ValueType::U64);
        if (publish && change != 5) block->PublishFCmpFlags(compared, Imm{swift::u32{compact}});
        if (change == 4) {
            const auto early = block->FCmpCondSet(compared, Cond::GT).SetType(ValueType::U8);
            block->StoreUniform(Uniform{258, ValueType::U8}, early);
        }
        Value replacement;
        if (change == 1 || change == 2 || change == 4 || change == 5) {
            replacement = block->Add(integer, ir::Operand{integer}).SetType(ValueType::U32);
            if (change == 1 || change == 4 || change == 5) block->SaveFlags(replacement, Flags::NZCV);
        }
        if (change == 5) block->PublishFCmpFlags(compared, Imm{swift::u32{compact}});
        const auto cond = static_cast<Cond>(condition_code);
        const auto condition = change == 5 ? one : block->FCmpCondSet(compared, cond).SetType(ValueType::U8);
        if (change == 2) block->SaveFlags(replacement, Flags::NZCV);
        if (change == 3) {
            replacement = block->Add(integer, ir::Operand{integer}).SetType(ValueType::U32);
            block->SaveFlags(replacement, Flags::NZCV);
        }
        const auto selected = block->Select(condition, one, zero).SetType(ValueType::U8);
        block->StoreUniform(Uniform{256, ValueType::U8}, selected);
        const auto current_zero = block->CondSet(Cond::EQ).SetType(ValueType::U8);
        block->StoreUniform(Uniform{257, ValueType::U8}, current_zero);
        if (publish) {
            const auto carry = block->TestFlags(Flags::Carry).SetType(ValueType::U8);
            block->StoreUniform(Uniform{259, ValueType::U8}, carry);
            block->InvertCarry();
            const auto inverted = block->TestFlags(Flags::Carry).SetType(ValueType::U8);
            block->StoreUniform(Uniform{260, ValueType::U8}, inverted);
        }
        for (auto value : live) block->StoreUniform(Uniform{1024, ValueType::U64}, value);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        swift::runtime::backend::RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
        RegisterAllocPass::Run(block.get(), &allocation, false, FeatureSet{});
        if (pressure) REQUIRE(allocation.SpillCount() > 0);
        allocation.CompleteGPRAllocation(module);
        JitContext context{module, allocation}; JitTranslator translator{context};
        translator.Translate(block.get()); context.Finish();
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != swift::runtime::backend::INVALID_CACHE_ID);
        context.Flush(buffer); REQUIRE(module->Push(block.get()));
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        swift::runtime::Runtime runtime{&space};
        const double first = relation == 0 ? 4.0 : relation == 1 ? 2.0 : relation == 2 ? 3.0 : std::numeric_limits<double>::quiet_NaN();
        const double second = 3.0; const swift::u32 seven = 7;
        if (bits == 32) {
            const float first32 = static_cast<float>(first), second32 = static_cast<float>(second);
            std::memcpy(runtime.GetUniformBuffer().data(), &first32, sizeof(first32));
            std::memcpy(runtime.GetUniformBuffer().data() + 16, &second32, sizeof(second32));
        } else {
            std::memcpy(runtime.GetUniformBuffer().data(), &first, sizeof(first));
            std::memcpy(runtime.GetUniformBuffer().data() + 16, &second, sizeof(second));
        }
        std::memcpy(runtime.GetUniformBuffer().data() + 32, &seven, sizeof(seven));
        runtime.SetLocation(block->GetStartLocation().Value());
        REQUIRE(runtime.Run() == swift::runtime::HaltReason::CallHost);
        const bool n = relation == 1, z = relation == 2, c = relation != 1, v = relation == 3;
        const std::array<bool, 16> expected{z, !z, c, !c, n, !n, v, !v, c && !z, !c || z,
                n == v, n != v, !z && n == v, z || n != v, true, true};
        REQUIRE(runtime.GetUniformBuffer()[256] == (change == 5 ? true : expected[condition_code]));
        if (change) REQUIRE(runtime.GetUniformBuffer()[257] == (change == 5 && (relation == 2 || relation == 3)));
        if (change == 4) REQUIRE(runtime.GetUniformBuffer()[258] == (relation == 0));
        if (publish) {
            const bool carry = change && change != 5 ? false : ((relation == 1 || relation == 3) != compact);
            REQUIRE(runtime.GetUniformBuffer()[259] == carry);
            REQUIRE(runtime.GetUniformBuffer()[260] == !carry);
        }
    }
#endif
}

TEST_CASE("select consumes the condition captured before later flags changes", "[consumer-plan][boolean-select]") {
#if defined(__aarch64__)
    for (bool local : {false, true}) for (bool boolean : {false, true}) for (unsigned change : {0u, 1u, 2u}) {
        CAPTURE(local, boolean, change);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 4096;
        config.stack_alignment = 16;
        swift::runtime::backend::AddressSpace space{config};
        auto module = space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block(0, Location{0xbf40})};
        const auto input = block->LoadImm(Imm{swift::u32{7}}).SetType(ValueType::U32);
        const auto result = block->Sub(input, ir::Operand{input}).SetType(ValueType::U32);
        block->SaveFlags(result, Flags::NZCV);
        const auto condition = (local ? block->LocalCondSet(Cond::EQ) : block->CondSet(Cond::EQ)).SetType(ValueType::U8);
        Value replacement;
        if (change) {
            replacement = block->Add(input, ir::Operand{input}).SetType(ValueType::U32);
            if (change == 1) block->SaveFlags(replacement, Flags::NZCV);
        }
        const auto type = boolean ? ValueType::U8 : ValueType::U64;
        const auto on_true = block->LoadImm(Imm{swift::u64{boolean ? 1u : 11u}}).SetType(type);
        const auto on_false = block->LoadImm(Imm{swift::u64{boolean ? 0u : 22u}}).SetType(type);
        const auto selected = block->Select(condition, on_true, on_false).SetType(type);
        if (change == 2) block->SaveFlags(replacement, Flags::NZCV);
        block->StoreUniform(Uniform{256, type}, selected);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        swift::runtime::backend::RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
        RegisterAllocPass::Run(block.get(), &allocation, false, FeatureSet{});
        allocation.CompleteGPRAllocation(module);
        JitContext context{module, allocation};
        JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != swift::runtime::backend::INVALID_CACHE_ID);
        context.Flush(buffer);
        REQUIRE(module->Push(block.get()));
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        swift::runtime::Runtime runtime{&space};
        swift::u64 value = 0;
        std::memcpy(runtime.GetUniformBuffer().data() + 256, &value, sizeof(value));
        runtime.SetLocation(block->GetStartLocation().Value());
        REQUIRE(runtime.Run() == swift::runtime::HaltReason::CallHost);
        std::memcpy(&value, runtime.GetUniformBuffer().data() + 256, sizeof(value));
        REQUIRE(value == (boolean ? 1u : 11u));
    }
#endif
}

namespace {
SVM_HELPER_PRESERVE_ALL SVM_HELPER_GENERAL_REGS_ONLY
HostPairResult PairResultProbe(swift::u64 a, swift::u64 b, swift::u64 c) {
    return {a ^ c, b + c};
}

ConsumerPlan Plan(Block& block) {
    block.ReIdInstr();
    BlockAnalysisIndex analysis;
    analysis.Build(&block);
    const auto terminal = block.GetTerminal();
    return BuildConsumerPlan(analysis, [](const Inst&) { return true; }, [](const Inst&) { return false; }, &terminal);
}
}

TEST_CASE("pair-result preparation retains owned and explicitly absent secondaries", "[consumer-plan][pair-result]") {
    for (bool cpuid : {false, true}) for (bool secondary : {false, true}) {
        CAPTURE(cpuid, secondary);
        IntrusivePtr<Block> block{new Block(0, Location{0xbfc0})};
        const auto input = block->LoadImm(Imm{swift::u64{17}}).SetType(ValueType::U64);
        const auto producer = (cpuid ? block->Cpuid(input, input, Imm{swift::u64{3}}, Imm{swift::u64{0}})
                                     : block->Div128(input, input, input, Imm{swift::u64{0}})).SetType(ValueType::U64);
        Inst* selected = nullptr;
        if (secondary) selected = (cpuid ? block->CpuidUpper(producer) : block->Div128Remainder(producer))
                .SetType(ValueType::U64).Def();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BlockAnalysisIndex index;
        index.Build(block.get());
        REQUIRE(index.HasConsumerDecisions());
        const auto plan = Plan(*block);
        REQUIRE(plan.valid);
        REQUIRE(plan.pair_results.size() == 1);
        REQUIRE(plan.pair_results.at(producer.Def()).secondary == selected);
        REQUIRE(plan.pair_results.at(producer.Def()).secondary_opcode ==
                (cpuid ? OpCode::CpuidUpper : OpCode::Div128Remainder));
    }
}

TEST_CASE("pair-result preparation rejects foreign duplicate and malformed ownership", "[consumer-plan][pair-result]") {
    for (bool cpuid : {false, true}) for (unsigned failure = 0; failure < 7; ++failure) {
        CAPTURE(cpuid, failure);
        IntrusivePtr<Block> block{new Block(0, Location{0xbfc8})};
        IntrusivePtr<Block> foreign{new Block(1, Location{0xbfd0})};
        const auto input = block->LoadImm(Imm{swift::u64{17}}).SetType(ValueType::U64);
        auto producer = (cpuid ? block->Cpuid(input, input, Imm{swift::u64{3}}, Imm{swift::u64{0}})
                               : block->Div128(input, input, input, Imm{swift::u64{0}})).SetType(ValueType::U64);
        if (failure == 4) producer = input;
        auto* owner = failure == 0 ? foreign.get() : block.get();
        const auto secondary = (cpuid ? owner->CpuidUpper(producer) : owner->Div128Remainder(producer))
                .SetType(ValueType::U64);
        if (failure == 1) {
            if (cpuid) block->CpuidUpper(producer).SetType(ValueType::U64);
            else block->Div128Remainder(producer).SetType(ValueType::U64);
        } else if (failure == 2) secondary.Def()->SetReturn(ValueType::U32);
        else if (failure == 3) {
            if (cpuid) block->Div128Remainder(producer).SetType(ValueType::U64);
            else block->CpuidUpper(producer).SetType(ValueType::U64);
        } else if (failure == 5) {
            block->RemoveInst(secondary.Def());
            block->InsertBefore(secondary.Def(), producer.Def());
        } else if (failure == 6) secondary.Def()->SetArg(0, producer.SetCastType(ValueType::U32));
        block->SetTerminal(terminal::ReturnToHost{});
        REQUIRE_FALSE(Plan(*block).valid);
    }
}

TEST_CASE("pair-result admission rejects missing immutable facts before output", "[consumer-plan][pair-result][pin-plan]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    swift::runtime::backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xbfe0})};
    const auto input = block->LoadImm(Imm{swift::u64{17}}).SetType(ValueType::U64);
    const auto producer = block->Div128(input, input, input, Imm{swift::u64{0}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, producer);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    FeatureSet features{};
    swift::runtime::backend::RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
            space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    allocation.CompleteGPRAllocation(module);
    const auto owner = allocation.GPRPlanFor(block.get());
    REQUIRE(owner->consumers);
    REQUIRE(owner->consumers->pair_results.at(producer.Def()).secondary == nullptr);
    auto replacement = std::make_shared<GPRBlockPlan>(*owner);
    replacement->consumers.reset();
    allocation.StoreGPRPlan(block.get(), replacement);
    JitContext context{module, allocation};
    JitTranslator translator{context};
    REQUIRE_FALSE(translator.Translate(block.get()));
    REQUIRE(context.CurrentBufferSize() == 0);
    allocation.StoreGPRPlan(block.get(), owner);
    REQUIRE(owner->region->MatchesUnit(block.get(), nullptr, allocation, config, features));
#endif
}

TEST_CASE("cyclic pair pseudo links reject before early flags preparation", "[consumer-plan][pair-result][pin-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbfe8})};
    const auto input = block->LoadImm(Imm{swift::u64{17}}).SetType(ValueType::U64);
    const auto producer = block->Div128(input, input, input, Imm{swift::u64{0}}).SetType(ValueType::U64);
    const auto secondary = block->Div128Remainder(producer).SetType(ValueType::U64);
    block->SetTerminal(terminal::ReturnToHost{});
    secondary.Def()->SetArg(1, producer);
    // Use accounting and index construction no longer walk the pseudo
    // chain (un-chaining step 3), so a malformed double use cannot
    // hang or corrupt them; the pair relationship itself is still
    // rejected by consumer-plan validation below.
    REQUIRE(producer.Def()->GetUses(false) == 2);
    REQUIRE(producer.Def()->GetUses() == 0);
    BlockAnalysisIndex index;
    index.Build(block.get(), nullptr, true);
    REQUIRE_FALSE(Plan(*block).valid);
}

TEST_CASE("prepared pair results preserve native helper and spilled value oracles", "[consumer-plan][pair-result][pressure]") {
#if defined(__aarch64__)
    for (unsigned shape = 0; shape < 3; ++shape) for (bool sign : {false, true})
    for (bool secondary : {false, true}) for (bool pressure : {false, true}) {
        if (shape == 2 && sign) continue;
        CAPTURE(shape, sign, secondary, pressure);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 4096;
        config.stack_alignment = 16;
        swift::runtime::backend::AddressSpace space{config};
        auto module = space.GetDefaultModule();
        const auto features = ResolveFeatureSet(module->GetModuleConfig());
        IntrusivePtr<Block> block{new Block(0, Location{0xbff0})};
        std::vector<Value> live;
        if (pressure) for (unsigned i = 0; i < 36; ++i)
            live.push_back(block->LoadUniform(Uniform{512 + i * 8, ValueType::U64}).SetType(ValueType::U64));
        const auto low = block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        const auto divisor = block->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
        auto high = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
        if (shape == 0 && sign) high = block->AsrImm(low, Imm{swift::u8{63}}).SetType(ValueType::U64);
        if (shape == 1) high = block->LoadUniform(Uniform{16, ValueType::U64}).SetType(ValueType::U64);
        const auto producer = (shape == 2
                ? block->Cpuid(low, divisor, Imm{swift::u64{9}}, Imm{reinterpret_cast<swift::u64>(&PairResultProbe)})
                : block->Div128(high, low, divisor, Imm{swift::u64{sign}})).SetType(ValueType::U64);
        Value other;
        if (secondary) other = (shape == 2 ? block->CpuidUpper(producer) : block->Div128Remainder(producer))
                .SetType(ValueType::U64);
        block->StoreUniform(Uniform{256, ValueType::U64}, producer);
        if (secondary) block->StoreUniform(Uniform{264, ValueType::U64}, other);
        for (unsigned i = 0; i < live.size(); ++i)
            block->StoreUniform(Uniform{2048 + i * 8, ValueType::U64}, live[i]);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        swift::runtime::backend::RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        allocation.CompleteGPRAllocation(module);
        const auto plan = allocation.GPRPlanFor(block.get());
        if (pressure) REQUIRE(allocation.SpillCount() > 0);
        REQUIRE(plan->consumers->pair_results.at(producer.Def()).secondary == (secondary ? other.Def() : nullptr));
        if (shape != 2) {
            REQUIRE(plan->pinned->ScratchFor(producer.Def()));
            REQUIRE(plan->pinned->ScratchFor(producer.Def())->div128_native == (shape == 0));
        }
        JitContext context{module, allocation};
        JitTranslator translator{context};
        REQUIRE(translator.Translate(block.get()));
        context.Finish();
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != swift::runtime::backend::INVALID_CACHE_ID);
        context.Flush(buffer);
        REQUIRE(module->Push(block.get()));
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        swift::runtime::Runtime runtime{&space};
        const auto put = [&](unsigned offset, swift::u64 value) {
            std::memcpy(runtime.GetUniformBuffer().data() + offset, &value, sizeof(value));
        };
        const auto get = [&](unsigned offset) {
            swift::u64 value{};
            std::memcpy(&value, runtime.GetUniformBuffer().data() + offset, sizeof(value));
            return value;
        };
        for (unsigned sample = 0; sample < 3; ++sample) {
            const swift::u64 input = sample == 1 ? static_cast<swift::u64>(-101ll) : 100;
            const swift::u64 d = sample == 2 ? 0 : 7;
            const swift::u64 hi = shape == 1 ? 1 : sign && static_cast<swift::s64>(input) < 0 ? UINT64_MAX : 0;
            std::memset(runtime.GetUniformBuffer().data(), 0xa5, 4096);
            put(0, input); put(8, d); put(16, hi);
            for (unsigned i = 0; i < live.size(); ++i) put(512 + i * 8, 0x1122334455667788ull + i * 17 + sample);
            const auto expected = shape == 2 ? PairResultProbe(input, d, 9)
                    : sign ? DivideSigned128(hi, input, d) : DivideUnsigned128(hi, input, d);
            runtime.SetLocation(block->GetStartLocation().Value());
            REQUIRE(runtime.Run() == swift::runtime::HaltReason::CallHost);
            REQUIRE(get(256) == expected.first);
            REQUIRE(get(264) == (secondary ? expected.second : 0xa5a5a5a5a5a5a5a5ull));
            for (unsigned i = 0; i < live.size(); ++i)
                REQUIRE(get(2048 + i * 8) == 0x1122334455667788ull + i * 17 + sample);
        }
    }
#endif
}

TEST_CASE("a later block missing pair facts rejects the whole function before output", "[consumer-plan][pair-result][region-plan]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.region_edges = true;
    swift::runtime::backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0xc010}, Location{0xc021});
    const auto marker = function->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
    function->StoreUniform(Uniform{0, ValueType::U64}, marker);
    auto* later = builder.LinkBlock(terminal::LinkBlock{Location{0xc020}});
    builder.SetCurBlock(later);
    const auto input = function->LoadImm(Imm{swift::u64{17}}).SetType(ValueType::U64);
    const auto producer = function->Div128(input, input, input, Imm{swift::u64{0}}).SetType(ValueType::U64);
    function->StoreUniform(Uniform{8, ValueType::U64}, producer);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO(); function->IdByRPO();
    swift::runtime::backend::RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
            space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(function, &allocation, features);
    allocation.CompleteGPRAllocation(module);
    const auto owner = allocation.GPRPlanFor(later->GetBlock());
    REQUIRE(owner->consumers->pair_results.contains(producer.Def()));
    auto replacement = std::make_shared<GPRBlockPlan>(*owner);
    replacement->consumers.reset();
    allocation.StoreGPRPlan(later->GetBlock(), replacement);
    JitContext context{module, allocation};
    JitTranslator translator{context};
    REQUIRE_FALSE(translator.Translate(function));
    REQUIRE(context.CurrentBufferSize() == 0);
#endif
}

TEST_CASE("a later code-object region missing pair facts rejects before any output", "[consumer-plan][pair-result][region-plan]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.region_edges = true;
    swift::runtime::backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{2, true, features};
    const auto make = [&](swift::u64 start, bool pair) {
        auto* function = builder.AppendFunction(Location{start}, Location{start + 16});
        const auto input = function->LoadImm(Imm{swift::u64{17}}).SetType(ValueType::U64);
        const auto result = pair ? function->Div128(input, input, input, Imm{swift::u64{0}}).SetType(ValueType::U64) : input;
        function->StoreUniform(Uniform{pair ? 8u : 0u, ValueType::U64}, result);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        return function;
    };
    auto* first = make(0xc040, false);
    auto* second = make(0xc060, true);
    swift::runtime::backend::RegAlloc first_allocation{first->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
            space.GetTrampolines().GetFPRRegs(), features};
    swift::runtime::backend::RegAlloc second_allocation{second->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
            space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(first, &first_allocation, features); first_allocation.CompleteGPRAllocation(module);
    RegisterAllocPass::Run(second, &second_allocation, features); second_allocation.CompleteGPRAllocation(module);
    auto* second_block = second->GetHIRBlocksRPO().front().GetBlock();
    const auto owner = second_allocation.GPRPlanFor(second_block);
    REQUIRE(owner->consumers);
    REQUIRE_FALSE(owner->consumers->pair_results.empty());
    auto replacement = std::make_shared<GPRBlockPlan>(*owner);
    replacement->consumers.reset();
    second_allocation.StoreGPRPlan(second_block, replacement);
    const std::array<FunctionRegionEmission, 2> regions{{{first, &first_allocation}, {second, &second_allocation}}};
    FunctionCodeObjectEmitter emitter{module, regions};
    REQUIRE_FALSE(emitter.Emit(false));
    REQUIRE(emitter.CurrentBufferSize() == 0);
#endif
}

TEST_CASE("local captured condition plans verify through direct and terminal users", "[consumer-plan][fcmp-condition]") {
    for (bool local : {false, true}) for (bool terminal : {false, true})
    for (unsigned change = 0; change < 4; ++change) {
        CAPTURE(local, terminal, change);
        IntrusivePtr<Block> block{new Block(0, Location{0xbf68})};
        const auto vector = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        const auto compared = block->VecFCmp(vector, vector, Imm{swift::u32{64}}, Imm{swift::u32{0}}).SetType(ValueType::U64);
        const auto condition = (local ? block->LocalCondSet(Cond::EQ) : block->FCmpCondSet(compared, Cond::EQ)).SetType(ValueType::U8);
        if (change) {
            const auto value = block->LoadImm(Imm{swift::u32{1}}).SetType(ValueType::U32);
            const auto added = block->Add(value, ir::Operand{value}).SetType(ValueType::U32);
            if (change == 1) block->SaveFlags(added, Flags::NZCV);
            if (change == 2) block->BranchOnlyFlags(added, Flags::NZCV);
            if (change == 3) block->SaveFlags(added, Flags::Parity);
        }
        if (terminal) block->SetTerminal(terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToHost{}});
        else { (void)block->Goto(condition); block->SetTerminal(terminal::ReturnToHost{}); }
        const auto plan = Plan(*block);
        // The shared conservative window contract also excludes PF-only
        // pseudos: qualification does not guess at their eventual encoding.
        REQUIRE(plan.local_conditions.contains(condition.Def()) == (change == 0));
    }
}

TEST_CASE("compact publication plans preserve named comparison before publication", "[consumer-plan][fcmp-condition]") {
    for (unsigned change = 0; change < 4; ++change) {
        CAPTURE(change);
        IntrusivePtr<Block> block{new Block(0, Location{0xbf70})};
        const auto vector = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        const auto compared = block->VecFCmp(vector, vector, Imm{swift::u32{64}}, Imm{swift::u32{1}}).SetType(ValueType::U64);
        if (change) {
            const auto value = block->LoadImm(Imm{swift::u32{1}}).SetType(ValueType::U32);
            const auto added = block->Add(value, ir::Operand{value}).SetType(ValueType::U32);
            if (change == 1) block->SaveFlags(added, Flags::NZCV);
            if (change == 2) block->BranchOnlyFlags(added, Flags::NZCV);
            if (change == 3) block->SaveFlags(added, Flags::Parity);
        }
        block->PublishFCmpFlags(compared, Imm{swift::u32{1}});
        block->SetTerminal(terminal::ReturnToHost{});
        const auto plan = Plan(*block);
        REQUIRE(plan.comparisons.at(compared.Def()).packed_relation == (change != 0));
    }
}

namespace {
struct FCmpMetrics {
    swift::u64 ns{}, hash{1469598103934665603ull}, spills{};
    size_t bytes{}, raw{}, carrier{};
};
FCmpMetrics CompileFCmp(unsigned shape, unsigned length, unsigned pressure) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 4096;
    swift::runtime::backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xbf78})};
    std::vector<Value> live, comparisons;
    for (unsigned i = 0; i < pressure; ++i)
        live.push_back(block->LoadUniform(Uniform{static_cast<swift::u16>(512 + i * 8), ValueType::U64}).SetType(ValueType::U64));
    for (unsigned i = 0; i < length; ++i) {
        const auto left = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        const auto right = block->LoadUniform(Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        const bool compact = shape == 1;
        const auto compared = block->VecFCmp(left, right, Imm{swift::u32{64}}, Imm{swift::u32{compact}}).SetType(ValueType::U64);
        if (shape) block->PublishFCmpFlags(compared, Imm{swift::u32{compact}});
        const auto condition = block->FCmpCondSet(compared, Cond::GT).SetType(ValueType::U8);
        block->StoreUniform(Uniform{256, ValueType::U8}, condition);
        if (shape == 3) {
            // A neutral reader and a long intervening window must not force
            // the next named reader to reconstruct an unchanged relation.
            for (unsigned gap = 0; gap < 8; ++gap)
                (void)block->LoadUniform(Uniform{128, ValueType::U64}).SetType(ValueType::U64);
            const auto another = block->FCmpCondSet(compared, Cond::LT).SetType(ValueType::U8);
            block->StoreUniform(Uniform{257, ValueType::U8}, another);
        }
        comparisons.push_back(compared);
    }
    for (auto value : live) block->StoreUniform(Uniform{1024, ValueType::U64}, value);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    swift::runtime::backend::RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
            space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    const auto begin = std::chrono::steady_clock::now();
    RegisterAllocPass::Run(block.get(), &allocation, false, FeatureSet{});
    allocation.CompleteGPRAllocation(module);
    JitContext context{module, allocation}; JitTranslator translator{context};
    translator.Translate(block.get()); context.Finish();
    FCmpMetrics result;
    result.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    result.bytes = context.CurrentBufferSize(); result.spills = allocation.SpillCount();
    const auto plan = allocation.GPRPlanFor(block.get());
    for (auto compared : comparisons) {
        const auto& action = plan->consumers->comparisons.at(compared.Def());
        result.raw += action.raw_condition != nullptr; result.carrier += action.compact_carrier;
    }
    const auto* code = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < result.bytes; ++i) { result.hash ^= code[i]; result.hash *= 1099511628211ull; }
    return result;
}
}
TEST_CASE("FCmp condition compilation metrics retain safe target selections", "[.fcmp-condition-metrics]") {
    for (unsigned shape : {0u, 1u, 2u, 3u}) for (unsigned pressure : {0u, 36u})
    for (unsigned length : {2u, 8u, 32u, 128u}) {
        // The historical full-publication path aborts under this pressure.
        // Keep its comparable timing cells separate from correctness pressure.
        if (shape >= 2 && pressure) continue;
        CAPTURE(shape, pressure, length);
        std::vector<swift::u64> times; FCmpMetrics sample;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = CompileFCmp(shape, length, pressure);
            REQUIRE(result.raw == (shape == 0 ? length : 0));
            REQUIRE(result.carrier == (shape == 1 ? length : 0));
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("fcmp-metrics shape=%u pressure=%u length=%u bytes=%zu hash=%llu raw=%zu carrier=%zu spills=%llu median_ns=%llu\n",
                shape, pressure, length, sample.bytes, (unsigned long long)sample.hash, sample.raw, sample.carrier,
                (unsigned long long)sample.spills, (unsigned long long)times[times.size() / 2]);
        std::fflush(stdout);
    }
}
TEST_CASE("boolean select plans preserve flags windows and exclusive local coverage", "[consumer-plan][boolean-select]") {
    for (unsigned scenario = 0; scenario < 16; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xbf48})};
        IntrusivePtr<Block> parent{new Block(0, Location{0xbf50})};
        auto condition = (scenario == 4 ? parent.get() : block.get())->CondSet(Cond::EQ).SetType(ValueType::U8);
        const auto select_type = scenario == 8 ? ValueType::U64 : ValueType::U8;
        auto on_true = (scenario == 13 ? parent.get() : block.get())->LoadImm(Imm{swift::u64{scenario == 7 ? 2u : 1u}})
                .SetType(select_type);
        const auto on_false = block->LoadImm(Imm{swift::u64{0}}).SetType(select_type);
        if (scenario == 1) block->ClearFlags(Flags::Zero);
        if (scenario == 2) (void)block->CallLambda(Lambda{Imm{swift::u64{1}}});
        if (scenario == 3) (void)block->Goto(on_false);
        if (scenario == 5) block->StoreUniform(Uniform{0, ValueType::U8}, condition);
        Value future_flags;
        if (scenario == 15) future_flags = block->Add(on_true, ir::Operand{on_true}).SetType(ValueType::U8);
        if (scenario == 9 || scenario == 10)
            for (unsigned depth = 0; depth < (scenario == 9 ? 3u : 4u); ++depth)
                condition = block->And(condition, ir::Operand{on_true}).SetType(ValueType::U8);
        const auto selected = block->Select(condition, on_true, on_false)
                .SetType(select_type);
        if (scenario == 6) block->StoreUniform(Uniform{8, ValueType::U8}, on_true);
        if (scenario == 11) block->SetTerminal(terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToHost{}});
        else block->SetTerminal(terminal::ReturnToHost{});
        if (scenario == 12) (void)block->Select(condition, on_true, on_false).SetType(ValueType::U8);
        if (scenario == 14) block->SaveFlags(on_true, Flags::Zero);
        if (scenario == 15) block->SaveFlags(future_flags, Flags::Zero);
        parent->ReIdInstr(); block->ReIdInstr();
        BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        REQUIRE(analysis.HasBooleanSelects());
        const auto plan = BuildBooleanSelectPlan(block.get(), analysis,
                [](const Inst& inst) { return inst.GetOp() != OpCode::ClearFlags && inst.GetOp() != OpCode::Goto &&
                                             inst.GetOp() != OpCode::CallLambda; });
        const bool boolean = scenario != 7 && scenario != 8 && scenario != 10;
        const bool direct = scenario == 0 || scenario == 6 || scenario == 7 || scenario == 8 || scenario == 13;
        REQUIRE((plan.ActionFor(selected.Def()) != nullptr) == (boolean || direct));
        if (boolean || direct) {
            REQUIRE(plan.ActionFor(selected.Def())->boolean == boolean);
            REQUIRE(plan.ActionFor(selected.Def())->direct_condition.has_value() == direct);
        }
        REQUIRE(plan.ActionFor(nullptr) == nullptr);
        const auto incoming = parent->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
        incoming.Def()->SetId(selected.Id());
        REQUIRE(plan.ActionFor(incoming.Def()) == nullptr);
        const auto covered = [&](Inst* node) {
            return std::find(plan.covered.begin(), plan.covered.end(), node) != plan.covered.end();
        };
        REQUIRE(covered(condition.Def()) == direct);
        REQUIRE(covered(on_true.Def()) == (boolean && scenario != 6 && scenario != 9 && scenario != 13 && scenario != 14 && scenario != 15));
        REQUIRE(covered(on_false.Def()) == (boolean && scenario != 3));
        if (scenario == 12) REQUIRE(plan.actions.size() == 2);
        parent->ReIdInstr();
        analysis.Build(parent.get());
        REQUIRE_FALSE(analysis.HasBooleanSelects());
    }
}
TEST_CASE("local condition eligibility preserves direct and terminal use contracts", "[consumer-plan]") {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbf00})};
        const auto condition = block->CondSet(Cond::EQ);
        if (scenario == 0) (void)block->Goto(condition);
        if (scenario == 1) block->StoreUniform(Uniform{0, ValueType::U8}, condition);
        if (scenario == 2) { (void)block->Goto(condition); block->StoreUniform(Uniform{0, ValueType::U8}, condition); }
        if (scenario >= 3) {
            Terminal terminal{terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToHost{}}};
            if (scenario == 4) terminal = terminal::CheckHalt{terminal};
            block->SetTerminal(terminal);
        } else block->SetTerminal(terminal::ReturnToHost{});
        const auto plan = Plan(*block);
        REQUIRE(plan.local_conditions.contains(condition.Def()) == (scenario == 0 || scenario >= 3));
    }
}
TEST_CASE("flag prefix snapshots preserve polarity clears and single-user multiplicity", "[consumer-plan]") {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbf08})};
        const auto value = block->LoadImm(Imm{7u}).SetType(ValueType::U32);
        const auto producer = (scenario == 1 || scenario == 2 ? block->Sub(value, ir::Operand{Imm{1u}})
                                                             : block->Add(value, ir::Operand{Imm{1u}})).SetType(ValueType::U32);
        block->SaveFlags(producer, Flags::Carry | Flags::Zero | Flags::Overflow);
        if (scenario == 2) block->InvertCarry();
        if (scenario == 3) block->ClearFlags(Flags::Carry | Flags::Zero);
        if (scenario == 4) (void)block->CallLambda(Lambda{Imm{swift::u64{1}}});
        const auto test = block->TestFlags(Flags::Carry).SetType(ValueType::U32);
        const auto user = block->Add(test, ir::Operand{scenario == 5 ? test : value}).SetType(ValueType::U32);
        block->SetTerminal(terminal::ReturnToHost{});
        const auto plan = Plan(*block);
        REQUIRE(plan.pstate.has_value());
        const auto flags = plan.pstate->At(test.Def());
        REQUIRE(True(flags & Flags::Carry) == (scenario == 0 || scenario == 2 || scenario == 5));
        REQUIRE(True(flags & Flags::Zero) == (scenario != 3 && scenario != 4));
        REQUIRE(plan.sole_users.contains(test.Def()) == (scenario != 5));
        if (scenario != 5) REQUIRE(plan.sole_users.at(test.Def()) == user.Def());
    }
}
TEST_CASE("later PSTATE demand includes nested terminals and ignores earlier readers", "[consumer-plan]") {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbf10})};
        (void)block->CondSet(Cond::EQ);
        const auto test = block->TestFlags(Flags::NZCV);
        if (scenario == 1) (void)block->CondSet(Cond::NE);
        if (scenario == 2) block->SetTerminal(terminal::CheckHalt{
                terminal::Condition{Cond::EQ, terminal::ReturnToHost{}, terminal::ReturnToHost{}}});
        else block->SetTerminal(terminal::ReturnToHost{});
        const auto plan = Plan(*block);
        REQUIRE(plan.later_pstate.contains(test.Def()) == (scenario != 0));
    }
}

TEST_CASE("compound clear plans require exact logical shape and flags contract", "[consumer-plan][compound-flags]") {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xbf18})};
        const auto input = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
        const auto producer = (scenario == 1 ? block->Add(input, ir::Operand{input})
                                             : block->Xor(input, ir::Operand{input})).SetType(ValueType::U32);
        if (scenario == 2) (void)block->LoadImm(Imm{1u});
        block->ClearFlags(scenario == 3 ? Flags::Carry : Flags::CV | Flags::AuxiliaryCarry);
        auto* clear = &block->GetInstList().back();
        block->SaveFlags(producer, scenario == 4 ? Flags::NZ : Flags::NZ | Flags::Parity);
        if (scenario == 5) block->BranchOnlyFlags(producer, Flags::NZCV);
        block->SetTerminal(terminal::ReturnToHost{});
        const auto plan = Plan(*block);
        REQUIRE(plan.compound_clears.contains(clear) == (scenario == 0));
        if (scenario == 0) REQUIRE(plan.compound_clears.at(clear) == producer.Def());
    }
}
TEST_CASE("boolean select plans are order-independent over independent selects", "[boolean-select]") {
    struct Facts {
        bool a_boolean{}, a_direct{}, b_boolean{}, b_direct{};
        Cond b_cond{Cond::NE};
        std::vector<const Inst*> covered;
        const Inst *select_a{}, *select_b{}, *cond_b{}, *one{}, *zero{};
        size_t actions{};
    };
    const auto build = [](bool second_first) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbf60})};
        const auto one = block->LoadImm(Imm{swift::u64{1}}).SetType(ValueType::U8);
        const auto zero = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U8);
        Value cond_a, cond_b, select_a, select_b;
        const auto make_a = [&] {
            cond_a = block->CondSet(Cond::NE).SetType(ValueType::U8);
            block->StoreUniform(Uniform{0, ValueType::U8}, cond_a);
            select_a = block->Select(cond_a, one, zero).SetType(ValueType::U8);
            block->StoreUniform(Uniform{8, ValueType::U8}, select_a);
        };
        const auto make_b = [&] {
            cond_b = block->CondSet(Cond::EQ).SetType(ValueType::U8);
            select_b = block->Select(cond_b, one, zero).SetType(ValueType::U8);
            block->StoreUniform(Uniform{16, ValueType::U8}, select_b);
        };
        if (second_first) { make_b(); make_a(); } else { make_a(); make_b(); }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        const auto plan = BuildBooleanSelectPlan(block.get(), analysis, [](const Inst&) { return true; });
        Facts facts;
        facts.actions = plan.actions.size();
        facts.select_a = select_a.Def(); facts.select_b = select_b.Def();
        facts.cond_b = cond_b.Def(); facts.one = one.Def(); facts.zero = zero.Def();
        const auto* a = plan.ActionFor(select_a.Def());
        const auto* b = plan.ActionFor(select_b.Def());
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        facts.a_boolean = a->boolean; facts.a_direct = a->direct_condition.has_value();
        facts.b_boolean = b->boolean; facts.b_direct = b->direct_condition.has_value();
        if (b->direct_condition) facts.b_cond = *b->direct_condition;
        facts.covered.assign(plan.covered.begin(), plan.covered.end());
        REQUIRE(plan.ActionFor(cond_a.Def()) == nullptr);
        REQUIRE(plan.ActionFor(nullptr) == nullptr);
        return facts;
    };
    const auto forward = build(false);
    REQUIRE(forward.actions == 2);
    REQUIRE(forward.a_boolean);
    REQUIRE_FALSE(forward.a_direct);
    REQUIRE(forward.b_boolean);
    REQUIRE(forward.b_direct);
    REQUIRE(forward.b_cond == Cond::EQ);
    const auto covered_has = [](const Facts& facts, const Inst* node) {
        return std::find(facts.covered.begin(), facts.covered.end(), node) != facts.covered.end();
    };
    REQUIRE(covered_has(forward, forward.cond_b));
    REQUIRE(covered_has(forward, forward.one));
    REQUIRE(covered_has(forward, forward.zero));
    REQUIRE(forward.covered.size() == 3);
    const auto swapped = build(true);
    REQUIRE(swapped.actions == forward.actions);
    REQUIRE(swapped.a_boolean == forward.a_boolean);
    REQUIRE(swapped.a_direct == forward.a_direct);
    REQUIRE(swapped.b_boolean == forward.b_boolean);
    REQUIRE(swapped.b_direct == forward.b_direct);
    REQUIRE(swapped.b_cond == forward.b_cond);
    REQUIRE(swapped.covered.size() == forward.covered.size());
    REQUIRE(covered_has(swapped, swapped.cond_b));
    REQUIRE(covered_has(swapped, swapped.one));
    REQUIRE(covered_has(swapped, swapped.zero));
}
TEST_CASE("boolean select plans reject stale analyses and gate direct arms on the defer predicate", "[boolean-select]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbf68})};
    const auto one = block->LoadImm(Imm{swift::u64{1}}).SetType(ValueType::U8);
    const auto zero = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U8);
    const auto condition = block->CondSet(Cond::EQ).SetType(ValueType::U8);
    const auto filler = block->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U8);
    block->StoreUniform(Uniform{24, ValueType::U8}, filler);
    const auto selected = block->Select(condition, one, zero).SetType(ValueType::U8);
    block->StoreUniform(Uniform{0, ValueType::U8}, selected);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    IntrusivePtr<Block> other{new Block(0, Location{0xbf70})};
    other->LoadUniform(Uniform{0, ValueType::U64});
    other->SetTerminal(terminal::ReturnToHost{});
    other->ReIdInstr();
    BlockAnalysisIndex stale;
    stale.Build(other.get());
    const auto foreign = BuildBooleanSelectPlan(block.get(), stale, [](const Inst&) { return true; });
    REQUIRE(foreign.actions.empty());
    REQUIRE(foreign.covered.empty());
    REQUIRE(foreign.ActionFor(selected.Def()) == nullptr);
    BlockAnalysisIndex analysis;
    analysis.Build(block.get());
    const auto gated = BuildBooleanSelectPlan(block.get(), analysis, [](const Inst&) { return false; });
    const auto* gated_action = gated.ActionFor(selected.Def());
    REQUIRE(gated_action != nullptr);
    REQUIRE(gated_action->boolean);
    REQUIRE_FALSE(gated_action->direct_condition.has_value());
    REQUIRE(std::find(gated.covered.begin(), gated.covered.end(), condition.Def()) == gated.covered.end());
    const auto open = BuildBooleanSelectPlan(block.get(), analysis, [](const Inst&) { return true; });
    const auto* open_action = open.ActionFor(selected.Def());
    REQUIRE(open_action != nullptr);
    REQUIRE(open_action->boolean);
    REQUIRE(open_action->direct_condition == Cond::EQ);
    REQUIRE(std::find(open.covered.begin(), open.covered.end(), condition.Def()) != open.covered.end());
    IntrusivePtr<Block> foreign_block{new Block(0, Location{0xbf78})};
    const auto foreign_value = foreign_block->LoadUniform(Uniform{0, ValueType::U8}).SetType(ValueType::U8);
    const auto foreign_select = foreign_block->Select(foreign_value, foreign_value, foreign_value).SetType(ValueType::U8);
    foreign_block->SetTerminal(terminal::ReturnToHost{});
    foreign_block->ReIdInstr();
    foreign_select.Def()->SetId(selected.Id());
    REQUIRE(open.ActionFor(foreign_select.Def()) == nullptr);
    IntrusivePtr<Block> plain{new Block(0, Location{0xbf80})};
    const auto plain_condition = plain->CondSet(Cond::EQ).SetType(ValueType::U8);
    plain->StoreUniform(Uniform{0, ValueType::U8}, plain_condition);
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    BlockAnalysisIndex plain_analysis;
    plain_analysis.Build(plain.get());
    const auto zero_plan = BuildBooleanSelectPlan(plain.get(), plain_analysis, [](const Inst&) { return true; });
    REQUIRE(zero_plan.actions.empty());
    REQUIRE(zero_plan.covered.empty());
    REQUIRE(zero_plan.ActionFor(plain_condition.Def()) == nullptr);
}
