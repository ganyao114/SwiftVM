#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/alloc/frozen_gpr_plan.h"
#include "runtime/backend/arm64/alloc/target_allocation_planner.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/opts/register_alloc_pass.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::backend::arm64;
using namespace swift::runtime::ir;

namespace {
#if defined(__aarch64__)
template<class Emitter>
void AtomicBody(Emitter& block, unsigned operation, ValueType type) {
    auto address = block.LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto first = block.LoadUniform(Uniform{8, type}).SetType(type);
    Value result;
    if (operation == 0) {
        auto second = block.LoadUniform(Uniform{16, type}).SetType(type);
        result = block.CompareAndSwap(address, first, second).SetType(type);
    } else if (operation == 1) result = block.AtomicExchange(address, first).SetType(type);
    else result = block.AtomicFetchAdd(address, first).SetType(type);
    block.StoreUniform(Uniform{24, type}, result);
}

void NativeAtomic(unsigned operation, ValueType type, bool lse, bool biased, unsigned block_count) {
    CAPTURE(operation, type, lse, biased, block_count);
    alignas(16) std::array<u8, 32> memory{};
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 64;
    config.region_edges = true;
    if (lse) config.arm64_features |= Arm64Features::Atomics;
    else config.arm64_features &= ~Arm64Features::Atomics;
    if (biased) config.memory_base = memory.data();
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{block_count, true, features};
    auto* function = builder.AppendFunction(Location{0xbc00}, Location{0xbc00 + block_count * 16});
    for (unsigned i = 0; i < block_count; ++i) {
        AtomicBody(*function, operation, type);
        if (i + 1 == block_count) function->EndBlock(terminal::ReturnToHost{});
        else {
            auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0xbc00 + (i + 1) * 16}});
            builder.SetCurBlock(next);
        }
    }
    function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
    RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(function, &allocation, features);
    REQUIRE(CompleteARM64UnitForEmission(module, allocation, nullptr, function));
    JitContext context{module, allocation}; JitTranslator translator{context};
    REQUIRE(translator.Translate(function)); context.Finish();
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
    std::printf("[atomic-fallback] op=%u width=%u lse=%u biased=%u blocks=%u code=",
                operation, GetValueSizeByte(type), lse, biased, block_count);
    for (size_t i = 0; i < context.CurrentBufferSize(); ++i) std::printf("%02x", bytes[i]);
    std::putchar('\n');
    const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
    REQUIRE(id != INVALID_CACHE_ID); context.Flush(buffer);
    space.PushCodeCache(Location{0xbc00}, buffer.exec_data);
    Runtime runtime{&space};
    const size_t width = GetValueSizeByte(type);
    const u64 initial = 11, desired = 19;
    const u64 first = operation == 0 ? initial : operation == 1 ? desired : 3;
    // Offset three exercises the locked fallback, and never the aligned LSE arm.
    std::memcpy(memory.data() + 3, &initial, width);
    const u64 address = biased ? 3 : reinterpret_cast<u64>(memory.data() + 3);
    auto uniform = runtime.GetUniformBuffer();
    std::memcpy(uniform.data(), &address, sizeof(address));
    std::memcpy(uniform.data() + 8, &first, width);
    std::memcpy(uniform.data() + 16, &desired, width);
    runtime.SetLocation(0xbc00); REQUIRE(runtime.Run() == HaltReason::CallHost);
    u64 actual{}, old{};
    std::memcpy(&actual, memory.data() + 3, width);
    std::memcpy(&old, uniform.data() + 24, width);
    REQUIRE(actual == (operation == 2 ? initial + block_count * first : desired));
    REQUIRE(old == (operation == 2 ? initial + (block_count - 1) * first
                                  : block_count == 1 ? initial : desired));
}
#endif
}

TEST_CASE("prepared atomic fallback preserves native unaligned operations and code", "[atomic-fallback-native]") {
#if defined(__aarch64__)
    for (unsigned operation = 0; operation < 3; ++operation)
        for (auto type : {ValueType::U32, ValueType::U64})
            for (bool lse : {false, true}) for (bool biased : {false, true})
                for (unsigned blocks : {1u, 2u, 3u}) NativeAtomic(operation, type, lse, biased, blocks);
#else
    SUCCEED("native atomic fallback execution requires AArch64");
#endif
}

TEST_CASE("atomic fallback preparation retains eligibility and exact shared keys", "[atomic-fallback-plan]") {
    for (unsigned operation = 0; operation < 3; ++operation)
        for (auto type : {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::S32,
                          ValueType::U64, ValueType::S64, ValueType::V128})
            for (unsigned scenario = 0; scenario < 9; ++scenario) {
                CAPTURE(operation, type, scenario);
                IntrusivePtr<Block> block{new Block(0, Location{0xbd00})};
                auto address = block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
                auto first = block->LoadUniform(Uniform{8, type}).SetType(type);
                auto second = block->LoadUniform(Uniform{16, type}).SetType(type);
                std::array<Value, 2> results;
                for (auto& result : results) {
                    if (operation == 0) result = block->CompareAndSwap(address, first, second).SetType(type);
                    else if (operation == 1) result = block->AtomicExchange(address, first).SetType(type);
                    else result = block->AtomicFetchAdd(address, first).SetType(type);
                }
                // An ordinary instruction must never become a fallback site.
                auto ordinary = block->Add(first, ir::Operand{second}).SetType(type);
                block->ReIdInstr();
                RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
                allocation.MapRegister(address.Id(), HostGPR{7});
                allocation.MapRegister(first.Id(), HostGPR{8});
                allocation.MapRegister(second.Id(), HostGPR{9});
                for (auto result : results) allocation.MapRegister(result.Id(), HostGPR{6});
                if (scenario == 1) for (auto result : results) allocation.MapRegister(result.Id(), HostGPR{8});
                if (scenario == 2) for (auto result : results) allocation.MapRegister(result.Id(), HostGPR{9});
                if (scenario == 3 || scenario == 4) allocation.MapRegister(address.Id(), HostFPR{1});
                if (scenario == 5) allocation.MapRegister(first.Id(), HostFPR{1});
                if (scenario == 6) for (auto result : results) allocation.MapMemSpill(result.Id(), SpillSlot{0});
                if (scenario == 7) allocation.MapMemSpill(second.Id(), SpillSlot{0});
                if (scenario == 8) allocation.MapRegister(results[1].Id(), HostGPR{5});
                const std::array<Block*, 1> blocks{block.get()};
                REQUIRE(BuildUnalignedAtomicFallbackPlan(blocks, allocation, false, false).keys.empty());
                const bool biased = scenario == 4;
                const auto plan = BuildUnalignedAtomicFallbackPlan(blocks, allocation, true, biased);
                const bool supported = GetValueSizeByte(type) == 4 || GetValueSizeByte(type) == 8;
                const bool eligible = supported && scenario != 1 && scenario != 3 && scenario != 5 &&
                        scenario != 6 && !(operation == 0 && (scenario == 2 || scenario == 7));
                REQUIRE(plan.keys.size() == (eligible ? 2 : 0));
                REQUIRE_FALSE(plan.keys.contains(ordinary.Def()));
                if (!eligible) { REQUIRE(plan.counts.empty()); continue; }
                const auto& key = plan.keys.at(results[0].Def());
                REQUIRE(key.type == type);
                REQUIRE(key.address == (biased ? vixl::aarch64::mem_scratch.GetCode() : 7));
                REQUIRE(key.first == 8);
                REQUIRE(key.second == (operation == 0 ? 9 : 0));
                REQUIRE(plan.counts.size() == (scenario == 8 ? 2 : 1));
                REQUIRE(plan.counts.at(key) == (scenario == 8 ? 1 : 2));
            }
}

TEST_CASE("atomic preparation lifecycle rejects stale allocation state and empty inputs stay empty", "[atomic-fallback-lifecycle]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.arm64_features |= Arm64Features::Atomics;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());

    IntrusivePtr<Block> block{new Block(0, Location{0xbd40})};
    AtomicBody(*block, 2, ValueType::U64);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE(CompleteARM64UnitForEmission(module, allocation, block.get(), nullptr));
    const auto* actions = allocation.GPRPlanViewFor(block.get());
    REQUIRE(actions != nullptr);
    REQUIRE(actions->region);
    REQUIRE(actions->region->unaligned_atomics.keys.size() == 1);
    REQUIRE(actions->region->MatchesUnit(block.get(), nullptr, allocation, config, features));
    REQUIRE(actions->region->generation == allocation.GPRLocationGeneration());
    REQUIRE(actions->region->allocation_attempt == allocation.AllocationAttempt());
    // Directly remapping a frozen allocation is stopped by the allocator's
    // own !gpr_locations_frozen CHECK, so the value-level lifecycle surface
    // is identity binding: an independently built allocation for the same
    // block that never completed register allocation must not match.
    RegAlloc incomplete{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    REQUIRE_FALSE(actions->region->MatchesUnit(block.get(), nullptr, incomplete, config, features));

    IntrusivePtr<Block> plain{new Block(0, Location{0xbd60})};
    auto left = plain->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto sum = plain->Add(left, ir::Operand{Imm{u64{1}}}).SetType(ValueType::U64);
    plain->StoreUniform(Uniform{8, ValueType::U64}, sum);
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    const std::array<Block*, 1> plain_blocks{plain.get()};
    RegAlloc plain_allocation{plain->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    for (bool lse : {false, true}) for (bool biased : {false, true}) {
        const auto empty = BuildUnalignedAtomicFallbackPlan(plain_blocks, plain_allocation, lse, biased);
        REQUIRE(empty.keys.empty());
        REQUIRE(empty.counts.empty());
    }

    IntrusivePtr<Block> second{new Block(0, Location{0xbd80})};
    AtomicBody(*second, 2, ValueType::U64);
    second->SetTerminal(terminal::ReturnToHost{}); second->ReIdInstr();
    RegAlloc pair_allocation{std::max(block->MaxInstrId(), second->MaxInstrId()), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    const auto map_atomic = [&](Block& value) {
        unsigned loads = 0;
        for (auto& inst : value.GetInstList()) {
            if (inst.GetOp() == OpCode::LoadUniform) pair_allocation.MapRegister(inst.Id(), HostGPR{u8(7 + loads++)});
            if (inst.GetOp() == OpCode::AtomicFetchAdd) pair_allocation.MapRegister(inst.Id(), HostGPR{6});
        }
        REQUIRE(loads == 2);
    };
    map_atomic(*block);
    map_atomic(*second);
    const std::array<Block*, 2> forward{block.get(), second.get()};
    const std::array<Block*, 2> reversed{second.get(), block.get()};
    const auto forward_plan = BuildUnalignedAtomicFallbackPlan(forward, pair_allocation, true, false);
    const auto reversed_plan = BuildUnalignedAtomicFallbackPlan(reversed, pair_allocation, true, false);
    REQUIRE(forward_plan.keys.size() == 2);
    REQUIRE(forward_plan.counts.size() == 1);
    REQUIRE(forward_plan.keys.size() == reversed_plan.keys.size());
    REQUIRE(forward_plan.counts == reversed_plan.counts);
    for (const auto& [inst, key] : forward_plan.keys) {
        const auto other = reversed_plan.keys.find(inst);
        REQUIRE(other != reversed_plan.keys.end());
        REQUIRE(other->second == key);
    }
#else
    SUCCEED("native atomic lifecycle requires AArch64");
#endif
}

TEST_CASE("flags regs audit diagnostics never change emitted code", "[atomic-fallback-diagnostics]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.arm64_features |= Arm64Features::Atomics;
    config.region_edges = true;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto base_features = ResolveFeatureSet(module->GetModuleConfig());
    const auto compile = [&](bool audit) {
        auto features = base_features;
        features.flags_regs_audit = audit;
        HIRBuilder builder{3, true, features};
        auto* function = builder.AppendFunction(Location{0xbe00}, Location{0xbe30});
        auto* first_block = function->GetCurrentBlock();
        for (unsigned i = 0; i < 3; ++i) {
            AtomicBody(*function, 2, ValueType::U64);
            if (i + 1 == 3) function->EndBlock(terminal::ReturnToHost{});
            else {
                auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0xbe00 + (i + 1) * 16}});
                builder.SetCurBlock(next);
            }
        }
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(function, &allocation, features);
        REQUIRE(CompleteARM64UnitForEmission(module, allocation, nullptr, function));
        const auto& plan = *allocation.GPRPlanViewFor(first_block->GetBlock())->region;
        JitContext context{module, allocation}; JitTranslator translator{context};
        REQUIRE(translator.Translate(function)); context.Finish();
        const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
        return std::pair{plan, std::vector<u8>(bytes, bytes + context.CurrentBufferSize())};
    };
    const auto [plain_plan, plain_code] = compile(false);
    const auto [audit_plan, audit_code] = compile(true);
    REQUIRE(plain_code == audit_code);
    REQUIRE(plain_plan.unaligned_atomics.keys.size() == audit_plan.unaligned_atomics.keys.size());
    REQUIRE(plain_plan.unaligned_atomics.counts == audit_plan.unaligned_atomics.counts);
    REQUIRE(plain_plan.options.atomic_lse == audit_plan.options.atomic_lse);
    REQUIRE(plain_plan.options.atomic_memory_base == audit_plan.options.atomic_memory_base);
#else
    SUCCEED("native atomic diagnostics requires AArch64");
#endif
}

TEST_CASE("region scratch inputs bind pool and feature drift exactly", "[atomic-fallback-lifecycle]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.arm64_features |= Arm64Features::Atomics;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    IntrusivePtr<Block> block{new Block(0, Location{0xbd40})};
    AtomicBody(*block, 2, ValueType::U64);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE(CompleteARM64UnitForEmission(module, allocation, block.get(), nullptr));
    const auto* actions = allocation.GPRPlanViewFor(block.get());
    REQUIRE(actions != nullptr);
    REQUIRE(actions->region);
    const auto& plan = *actions->region;
    const auto pool = allocation.GetGprs();
    REQUIRE(plan.MatchesScratchInputs(pool, features));
    REQUIRE(plan.scratch_inputs == CaptureScratchPreparationInputs(pool, features));
    int first_set = -1, first_clear = -1;
    for (u32 bit = 0; bit < 32; ++bit) {
        if (pool.Get(bit) && first_set < 0) first_set = int(bit);
        if (!pool.Get(bit) && first_clear < 0) first_clear = int(bit);
    }
    REQUIRE(first_set >= 0);
    REQUIRE(first_clear >= 0);
    auto shrunk = pool; shrunk.Clear(u32(first_set));
    REQUIRE_FALSE(plan.MatchesScratchInputs(shrunk, features));
    auto grown = pool; grown.Mark(u32(first_clear));
    REQUIRE_FALSE(plan.MatchesScratchInputs(grown, features));
    auto drifted = features; drifted.jit_scratch_xpool = !drifted.jit_scratch_xpool;
    REQUIRE_FALSE(plan.MatchesScratchInputs(pool, drifted));
    drifted = features; drifted.scratch_precise = !drifted.scratch_precise;
    REQUIRE_FALSE(plan.MatchesScratchInputs(pool, drifted));
    drifted = features; drifted.fpr_scratch_precise = !drifted.fpr_scratch_precise;
    REQUIRE_FALSE(plan.MatchesScratchInputs(pool, drifted));
    drifted = features; drifted.sse_nan_coldpath = !drifted.sse_nan_coldpath;
    REQUIRE_FALSE(plan.MatchesScratchInputs(pool, drifted));
#else
    SUCCEED("native scratch lifecycle requires AArch64");
#endif
}

TEST_CASE("atomic preparation binds LSE and raw or biased target admission", "[atomic-fallback-admission]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.arm64_features |= Arm64Features::Atomics;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    IntrusivePtr<Block> block{new Block(0, Location{0xbd20})};
    AtomicBody(*block, 2, ValueType::U64);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE(CompleteARM64UnitForEmission(module, allocation, block.get(), nullptr));
    const auto* actions = allocation.GPRPlanViewFor(block.get());
    REQUIRE(actions->region->options.atomic_lse);
    REQUIRE_FALSE(actions->region->options.atomic_memory_base);
    REQUIRE(actions->region->MatchesUnit(block.get(), nullptr, allocation, config, features));
    auto drift = config;
    drift.arm64_features &= ~Arm64Features::Atomics;
    REQUIRE_FALSE(actions->region->MatchesUnit(block.get(), nullptr, allocation, drift, features));
    drift = config; drift.memory_base = reinterpret_cast<void*>(4096);
    REQUIRE_FALSE(actions->region->MatchesTargetInputs(allocation, drift, features));
    drift = config; drift.page_table = reinterpret_cast<decltype(drift.page_table)>(4096);
    REQUIRE_FALSE(actions->region->MatchesTargetInputs(allocation, drift, features));
    // Both mechanisms select the reserved address scratch register.
    const auto base = CaptureRegionPreparationOptions(drift, features);
    drift.page_table = nullptr; drift.memory_base = reinterpret_cast<void*>(4096);
    REQUIRE(CaptureRegionPreparationOptions(drift, features) == base);
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        CAPTURE(mutation);
        auto incompatible = config;
        if (mutation == 0) incompatible.arm64_features &= ~Arm64Features::Atomics;
        else if (mutation == 1) incompatible.memory_base = reinterpret_cast<void*>(4096);
        else incompatible.page_table = reinterpret_cast<void*>(4096);
        AddressSpace other{incompatible};
        JitContext rejected{other.GetDefaultModule(), allocation}; JitTranslator emitter{rejected};
        REQUIRE_FALSE(emitter.Translate(block.get())); REQUIRE(rejected.CurrentBufferSize() == 0);
    }
    JitContext context{module, allocation}; JitTranslator translator{context};
    REQUIRE(translator.Translate(block.get())); REQUIRE(context.CurrentBufferSize() > 0);
#else
    SUCCEED("native atomic target admission requires AArch64");
#endif
}
