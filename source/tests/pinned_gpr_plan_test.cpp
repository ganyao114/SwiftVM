#include "runtime/backend/arm64/alloc/pinned_gpr_allocation.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <chrono>
#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/arm64/alloc/fixed_gpr_constraints.h"
#include "runtime/backend/arm64/alloc/gpr_plan_verifier.h"
#include "runtime/backend/arm64/alloc/gpr_width_verifier.h"

#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "support/register_alloc_test_support.h"
#include "runtime/common/svm_config.h"

namespace {
// Tests pinning verifier rejection behaviour opt into the full plan audit.
struct ScopedVerifyPlan {
    ScopedVerifyPlan() {
        const char* old = swift::runtime::GetRawSvmConfigEnvForTest("SVM_VERIFY_PLAN");
        had = old != nullptr;
        if (had) saved = old;
        swift::runtime::SetSvmConfigEnvForTest("SVM_VERIFY_PLAN", "1", 1);
    }
    ~ScopedVerifyPlan() {
        if (had) swift::runtime::SetSvmConfigEnvForTest("SVM_VERIFY_PLAN", saved.c_str(), 1);
        else swift::runtime::UnsetSvmConfigEnvForTest("SVM_VERIFY_PLAN");
    }
    bool had{};
    std::string saved;
};
} // namespace

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

struct GPRBenchmarkMetrics {
    swift::u64 ns{}, moves{}, spills{}, reloads{}, stores{}, replays{};
    size_t bytes{};
};

std::vector<swift::u8> EmitPlanFixture(unsigned shape, bool pressure, GPRBenchmarkMetrics* metrics = nullptr,
                                    bool use_preplanning = false) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48,
                  .enable_jit = true, .has_local_operation = false,
                  .backend_isa = kArm64};
    AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x9870})};
    std::vector<Value> retained;
    if (pressure) {
        for (swift::u64 i = 1; i <= 10; ++i)
            retained.push_back(block->LoadImm(Imm{i}).SetType(ValueType::U64));
    }
    auto old = block->GetHostGPR(HostRegIndex(1), Imm{0u}).SetType(ValueType::U64);
    if (shape == 0) {
        auto input = block->LoadImm(Imm{0x87654321u}).SetType(ValueType::U32);
        auto sum = block->Add(input, Operand{Imm{1u}}).SetType(ValueType::U32);
        auto wide = block->ZeroExtend32To64(sum).SetType(ValueType::U64);
        block->SetHostGPR(wide, HostRegIndex(22), Imm{0u});
    } else if (shape == 1) {
        auto low = old.SetCastType(ValueType::U8);
        block->SetHostGPR(low, HostRegIndex(23), Imm{1u});
        block->SetHostGPR(old.SetCastType(ValueType::U16), HostRegIndex(22), Imm{0u});
    } else if (shape == 2) {
        block->SetHostGPR(old, HostRegIndex(23), Imm{0u});
        auto replacement = block->LoadImm(Imm{swift::u64{0x3000}}).SetType(ValueType::U64);
        block->SetHostGPR(replacement, HostRegIndex(1), Imm{0u});
        auto result = block->Add(old, Operand{Imm{swift::u64{7}}}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{32, ValueType::U64}, result);
    } else if (shape == 3) {
        auto address = block->GetOperand(Operand{old}).SetType(ValueType::U64);
        auto loaded = block->LoadMemory(Operand{address}).SetType(ValueType::U64);
        block->SetHostGPR(loaded, HostRegIndex(22), Imm{0u});
    } else {
        auto initial = block->LoadImm(Imm{swift::u64{0x8877665544332211}}).SetType(ValueType::U64);
        block->SetHostGPR(initial, HostRegIndex(20), Imm{0u});
        auto version = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
        auto first = block->Add(version, Operand{Imm{swift::u64{7}}}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{16, ValueType::U64}, first);
        auto replacement = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
        block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
        auto last = block->Add(version, Operand{Imm{swift::u64{9}}}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{24, ValueType::U64}, last);
    }
    if (pressure) {
        auto total = retained.front();
        for (size_t i = 1; i < retained.size(); ++i)
            total = block->Add(total, Operand{retained[i]}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{40, ValueType::U64}, total);
    }
    block->StoreUniform(Uniform{48, old.Type()}, old);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    auto gprs = pressure ? GPRSMask{~((1u << 6) - 1u)}
                         : space.GetTrampolines().GetGPRRegs();
    RegAlloc allocation{block->MaxInstrId(), gprs,
                        space.GetTrampolines().GetFPRRegs(), features};
    const auto start = std::chrono::steady_clock::now();
    if (pressure) {
        if (shape < 4 && !use_preplanning) allocation.FinishFixedGPRConstraints();
        RegisterAllocTestSupport::RunForSpillEvictTest(block.get(), &allocation, false);
        REQUIRE(allocation.SpillCount() > 0);
    } else {
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
    }
    allocation.CompleteGPRAllocation(space.GetDefaultModule());
    arm64::JitContext context{space.GetDefaultModule(), allocation};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    if (metrics) {
        metrics->ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        metrics->bytes = context.CurrentBufferSize();
        metrics->spills = allocation.SpillCount();
        metrics->reloads = allocation.RAShape().spill_loads;
        metrics->stores = allocation.RAShape().spill_stores;
        metrics->replays = allocation.TargetReplayCount();
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler; decoder.AppendVisitor(&disassembler);
        auto* instruction = context.GetMasm().GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
        const auto* end = context.GetMasm().GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
        for (; instruction < end; instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction);
            if (std::string_view{disassembler.GetOutput()}.starts_with("mov ")) ++metrics->moves;
        }
    }
    auto* begin = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    return {begin, begin + context.CurrentBufferSize()};
}
}

TEST_CASE("pinned GPR allocation quality metrics", "[.pin-gpr-metrics]") {
    for (bool pressure : {false, true}) for (unsigned shape : {0u, 2u, 3u, 4u}) {
        std::vector<swift::u64> times;
        GPRBenchmarkMetrics sample;
        for (unsigned iteration = 0; iteration < 110; ++iteration) {
            GPRBenchmarkMetrics metrics;
            const auto code = EmitPlanFixture(shape, pressure, &metrics);
            REQUIRE_FALSE(code.empty());
            if (iteration >= 10) times.push_back(metrics.ns);
            sample = metrics;
        }
        std::sort(times.begin(), times.end());
        std::printf("gpr-metrics pressure=%u shape=%u bytes=%zu moves=%llu spill_defs=%llu reloads=%llu stores=%llu median_ns=%llu target_replays=%llu\n",
                    pressure, shape, sample.bytes, static_cast<unsigned long long>(sample.moves),
                    static_cast<unsigned long long>(sample.spills), static_cast<unsigned long long>(sample.reloads),
                    static_cast<unsigned long long>(sample.stores), static_cast<unsigned long long>(times[times.size() / 2]),
                    static_cast<unsigned long long>(sample.replays));
    }
}

TEST_CASE("initial GPR constraint compilation metrics", "[.pin-gpr-initial-metrics]") {
    for (bool pressure : {false, true}) for (unsigned shape : {0u, 2u, 3u, 4u}) {
        std::vector<swift::u64> times;
        GPRBenchmarkMetrics sample;
        for (unsigned iteration = 0; iteration < 110; ++iteration) {
            GPRBenchmarkMetrics metrics;
            const auto code = EmitPlanFixture(shape, pressure, &metrics, true);
            REQUIRE_FALSE(code.empty());
            if (iteration >= 10) times.push_back(metrics.ns);
            sample = metrics;
        }
        std::sort(times.begin(), times.end());
        std::printf("gpr-initial pressure=%u shape=%u bytes=%zu spill_defs=%llu reloads=%llu stores=%llu median_ns=%llu target_replays=%llu\n",
                    pressure, shape, sample.bytes, static_cast<unsigned long long>(sample.spills),
                    static_cast<unsigned long long>(sample.reloads), static_cast<unsigned long long>(sample.stores),
                    static_cast<unsigned long long>(times[times.size() / 2]),
                    static_cast<unsigned long long>(sample.replays));
    }
}

TEST_CASE("frozen GPR plans retain operand views and reject stale IR", "[pin-plan]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48,
                  .enable_jit = true, .has_local_operation = false,
                  .backend_isa = kArm64};
    AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x9880})};
    auto definition = block->LoadImm(Imm{swift::u64{0x8877665544332211}})
                                 .SetType(ValueType::U64);
    auto byte = definition;
    byte = byte.SetCastType(ValueType::U8);
    block->StoreUniform(Uniform{0, ValueType::U8}, byte);
    auto* byte_store = &block->GetInstList().back();
    block->StoreUniform(Uniform{8, ValueType::U64}, definition);
    auto* wide_store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    arm64::GuestStateMap guest;
    guest.Analyze(block.get(), features);
    arm64::GPRPlanBuilder builder{allocation, features, guest};
    builder.Prepare(block.get());
    builder.AssignValueHome(definition.Def(), 22);
    auto plan = builder.Freeze();
    REQUIRE(allocation.GPRLocationsFrozen());
    REQUIRE(plan->MatchesFrozenIR());
    REQUIRE_THROWS_AS(builder.ElideZeroExtend(definition.Def()), std::logic_error);
    REQUIRE_THROWS_AS(builder.AssignValueHome(definition.Def(), 22), std::logic_error);
    const auto operand = [&](Inst* consumer) -> const arm64::FrozenGPRPlan::OperandPlan& {
        const auto found = std::find_if(plan->Operands().begin(), plan->Operands().end(),
                [&](const auto& use) { return use.consumer == consumer && use.argument == 1; });
        REQUIRE(found != plan->Operands().end());
        return *found;
    };
    REQUIRE(operand(byte_store).type == ValueType::U8);
    REQUIRE(operand(wide_store).type == ValueType::U64);
    REQUIRE(plan->UseLocation(byte, byte_store).bits == 32);
    REQUIRE(plan->UseLocation(byte, byte_store).slot == 22);
    REQUIRE(plan->UseLocation(definition, wide_store).bits == 64);
    const auto generation = plan->Generation();
    const auto attempt = allocation.AllocationAttempt();
    allocation.ResetAllocations();
    REQUIRE_FALSE(plan->MatchesFrozenIR());
    REQUIRE(allocation.AllocationAttempt() > attempt);
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    guest.Analyze(block.get(), features);
    builder.Prepare(block.get());
    plan = builder.Freeze();
    REQUIRE(plan->Generation() > generation);
    REQUIRE(plan->MatchesFrozenIR());
}

TEST_CASE("an imported GPR plan shares ownership and rejects allocation retries", "[pin-plan][regression]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48,
                  .enable_jit = true, .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x9890})};
    auto value = block->LoadImm(Imm{swift::u64{123}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, value);
    auto* store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    arm64::GuestStateMap guest;
    guest.Analyze(block.get(), features);
    arm64::GPRPlanBuilder builder{allocation, features, guest};
    builder.Prepare(block.get());
    auto owner = builder.Freeze();
    const auto location = owner->UseLocation(value, store);
    std::weak_ptr<const arm64::FrozenGPRPlan> lifetime = owner;
    auto imported = owner;
    static_assert(std::is_const_v<typename decltype(imported)::element_type>);
    owner.reset();
    REQUIRE_FALSE(lifetime.expired());
    REQUIRE(imported->MatchesFrozenIR());
    REQUIRE(imported->UseLocation(value, store).slot == location.slot);
    REQUIRE_THROWS_AS(builder.ElideZeroExtend(value.Def()), std::logic_error);
    REQUIRE_THROWS_AS(builder.RecordPhysicalWrite(store, 22), std::logic_error);
    allocation.ResetAllocations();
    REQUIRE_FALSE(imported->MatchesFrozenIR());
    REQUIRE_THROWS_AS(imported->UseLocation(value, store), std::logic_error);
}

TEST_CASE("initial GPR homes reserve late replay for spill-dependent arithmetic", "[pin-plan][fixed-gpr]") {
    for (bool pressure : {false, true}) {
        for (unsigned shape : {0u, 2u, 3u}) {
            GPRBenchmarkMetrics metrics;
            const auto code = EmitPlanFixture(shape, pressure, &metrics, true);
            INFO("shape=" << shape << " pressure=" << pressure);
            REQUIRE_FALSE(code.empty());
            REQUIRE(metrics.replays == (pressure && shape == 0 ? 1 : 0));
        }
    }
}

TEST_CASE("early GPR publication cannot pass a later narrow capture", "[pin-plan][regression]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 128; config.stack_alignment = 16;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x9898})};
    const auto initial = block->LoadImm(Imm{swift::u64{0x8877665544332211}}).SetType(ValueType::U64);
    block->SetHostGPR(initial, HostRegIndex(22), Imm{0u});
    const auto next = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
    const auto captured = block->GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(ValueType::U8);
    block->StoreUniform(Uniform{0, ValueType::U8}, captured);
    block->SetHostGPR(next, HostRegIndex(22), Imm{0u});
    const auto current = block->GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{8, ValueType::U64}, current);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    FeatureSet features{};
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE_FALSE(allocation.HasFixedGPRDefinition(next.Id()));
    allocation.CompleteGPRAllocation(module);
    arm64::JitContext context{module, allocation}; arm64::JitTranslator translator{context};
    translator.Translate(block.get()); context.Finish();
#if defined(__aarch64__)
    const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
    REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer); REQUIRE(module->Push(block.get()));
    (void)module->PublishLinkTarget(block->GetStartLocation(), buffer.exec_data, buffer.exec_data);
    space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
    Runtime runtime{&space}; runtime.SetLocation(0x9898);
    REQUIRE(runtime.Run() == HaltReason::CallHost);
    const auto uniform = runtime.GetUniformBuffer();
    REQUIRE(uniform[0] == 0x11);
    swift::u64 value; std::memcpy(&value, uniform.data() + 8, sizeof(value));
    REQUIRE(value == 42);
#endif
}

TEST_CASE("failed GPR finalization rolls back and can retry the same allocator", "[pin-plan][regression]") {
    ScopedVerifyPlan verify_plan;
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x98a0})};
    const auto value = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, Value{value}.SetCastType(ValueType::U8));
    auto* store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    FeatureSet features{};
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE_THROWS_AS(allocation.CompleteGPRAllocation(module), std::logic_error);
    REQUIRE_FALSE(allocation.TargetPlanning());
    REQUIRE_FALSE(allocation.GPRAllocationComplete());
    REQUIRE_FALSE(allocation.GPRLocationsFrozen());
    REQUIRE_FALSE(allocation.GPRPlanFor(block.get()));
    store->SetArg(1, value);
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    allocation.CompleteGPRAllocation(module);
    arm64::JitContext context{module, allocation};
    REQUIRE(allocation.GPRAllocationComplete());
    arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);
    REQUIRE(context.OperandLocation(value, store).kind == RegAlloc::GPR);
    allocation.ResetAllocations();
    REQUIRE_THROWS_AS(context.OperandLocation(value, store), std::logic_error);
}

TEST_CASE("aborting target planning restores fixed publication contracts", "[pin-plan][regression]") {
    GPRSMask pool{}; pool.Mark(20);
    RegAlloc allocation{4, pool, FPRSMask{}, FeatureSet{}};
    allocation.PlanFixedGPRDefinition(0, {.home = 20, .end = 2, .last_use = 2,
            .publication = 2, .owner = 0, .width = 8});
    allocation.MapFixedRegister(0, HostGPR{20});
    allocation.BeginTargetPlanning();
    allocation.PlanFixedGPRDefinition(0, {.home = 20, .end = 3, .last_use = 3,
            .publication = 3, .owner = 0, .width = 8});
    REQUIRE(allocation.NeedsFixedGPRPublication(3));
    allocation.AbortTargetPlanning();
    REQUIRE_FALSE(allocation.TargetPlanning());
    REQUIRE(allocation.FixedGPRDefinitionAt(0)->publication == 2);
    REQUIRE(allocation.NeedsFixedGPRPublication(2));
    REQUIRE_FALSE(allocation.NeedsFixedGPRPublication(3));
    REQUIRE(allocation.Mapping(0).type == RegAlloc::NONE);
}

TEST_CASE("final width verification certifies elisions and rejects mapping drift", "[pin-plan][regression]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x98a8})};
    const auto left = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
    const auto right = block->LoadImm(Imm{swift::u32{7}}).SetType(ValueType::U32);
    const auto producer = block->Add(left, Operand{right}).SetType(ValueType::U32);
    const auto extract = block->BitExtract(producer, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    const auto extended = block->ZeroExtend32To64(extract).SetType(ValueType::U64);
    block->StoreUniform(Uniform{8, ValueType::U64}, extended);
    block->StoreUniform(Uniform{16, ValueType::U32}, extract);
    block->StoreUniform(Uniform{20, ValueType::U32}, producer);
    block->SetTerminal(terminal::ReturnToDispatch{}); block->ReIdInstr();
    FeatureSet features{}; features.ra_width_chain = true; features.ra_coalesce = false;
    RegAlloc allocation{block->MaxInstrId(), GPRSMask{~((1u << 8) - 1u)}, FPRSMask{}, features};
    RegisterAllocTestSupport::RunForWidthChainTest(block.get(), &allocation, true);
    const auto certificates = arm64::VerifyGPRWidthElisions(block.get(), allocation, features, GPRSMask{});
    REQUIRE(certificates.width.size() == 2);
    const auto home = allocation.ValueGPR(producer).id;
    allocation.MapRegister(extract.Id(), HostGPR{static_cast<swift::u16>((home + 1) % 31)});
    REQUIRE_THROWS_AS(arm64::VerifyGPRWidthElisions(block.get(), allocation, features, GPRSMask{}), std::logic_error);
}

TEST_CASE("GPR allocation cannot change during emission", "[pin-plan]") {
    RegAlloc allocation{8, GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.BeginGPRLocationAnalysis();
    allocation.FreezeGPRLocations();
    REQUIRE_THROWS_AS(allocation.AssignGPRValueHome(0, 22), std::logic_error);
    REQUIRE_THROWS_AS(allocation.AssignGPRLowView(0, 23), std::logic_error);
    REQUIRE_THROWS_AS(allocation.RegisterGPRUseHome(0, 1, {22, 4, {}}, true), std::logic_error);
    REQUIRE_THROWS_AS(allocation.MapRegister(0, HostGPR{14}), std::logic_error);
    REQUIRE_THROWS_AS(allocation.MapMemSpill(0, SpillSlot{0}), std::logic_error);
    REQUIRE_THROWS_AS(allocation.SetLiveEnd(0, 7), std::logic_error);
    REQUIRE(allocation.GPRLocationsFrozen());
}

TEST_CASE("RegAlloc completes target planning before an encoder exists", "[pin-plan][target-planner]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 64;
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x98ac})};
    const auto value = block->LoadImm(Imm{swift::u32{42}}).SetType(ValueType::U32);
    const auto sum = block->Add(value, Operand{Imm{swift::u32{7}}}).SetType(ValueType::U32);
    const auto wide = block->ZeroExtend32To64(sum).SetType(ValueType::U64);
    block->SetHostGPR(wide, HostRegIndex(22), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->StoreUniform(Uniform{0, ValueType::U64}, wide);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    FeatureSet features{};
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE_FALSE(allocation.GPRAllocationComplete());
    allocation.CompleteGPRAllocation(module);
    REQUIRE(allocation.GPRAllocationComplete());
    REQUIRE(allocation.GPRLocationsFrozen());
    REQUIRE_FALSE(allocation.TargetPlanning());
    const auto plan = allocation.GPRPlanFor(block.get());
    REQUIRE(plan);
    REQUIRE(plan->pinned->MatchesFrozenIR());
    REQUIRE(plan->pinned->MatchesAllocation());
    REQUIRE(plan->publications.contains(publication));
    const auto generation = allocation.GPRLocationGeneration();
    const auto attempt = allocation.AllocationAttempt();
    allocation.CompleteGPRAllocation(module);
    REQUIRE(allocation.GPRPlanFor(block.get()) == plan);
    REQUIRE(allocation.GPRLocationGeneration() == generation);
    REQUIRE(allocation.AllocationAttempt() == attempt);
    arm64::JitContext context{module, allocation};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);
}

TEST_CASE("pinned GPR plan byte baseline", "[.pin-plan-baseline]") {
    for (bool pressure : {false, true}) {
        for (unsigned shape = 0; shape < 4; ++shape) {
            const auto code = EmitPlanFixture(shape, pressure);
            REQUIRE_FALSE(code.empty());
            // Full bytes are compared between the old and frozen implementations.
            std::printf("pin-plan %u %u ", pressure, shape);
            for (auto byte : code) std::printf("%02x", byte);
            std::printf("\n");
        }
    }
}

TEST_CASE("overwritten GPR versions have allocator-owned register and spill preservation", "[pin-plan][fixed-gpr]") {
    for (bool pressure : {false, true}) {
        for (unsigned publication_width : {8u, 1u, 2u, 4u}) {
            Config config{.loc_start = 0, .loc_end = 1ull << 48,
                          .enable_jit = true, .has_local_operation = false,
                          .backend_isa = kArm64};
            config.uniform_buffer_size = 128;
            config.stack_alignment = 16;
            AddressSpace space{config};
            auto module = space.GetDefaultModule();
            IntrusivePtr<Block> block{new Block(0, Location{0x9900})};
            constexpr swift::u64 old_bits = 0x8877665544332200;
            auto initial = block->LoadImm(Imm{old_bits}).SetType(ValueType::U64);
            block->SetHostGPR(initial, HostRegIndex(20), Imm{0u});
            std::vector<Value> retained;
            if (pressure) {
                for (swift::u64 i = 1; i <= 8; ++i)
                    retained.push_back(block->LoadImm(Imm{i}).SetType(ValueType::U64));
            }
            auto old = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
            auto alias = block->BitCast(old).SetType(ValueType::U64);
            auto early = block->Add(alias, Operand{Imm{swift::u64{7}}}).SetType(ValueType::U64);
            block->StoreUniform(Uniform{0, ValueType::U64}, early);
            auto next = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
            const unsigned byte_offset = publication_width == 1 ? 1 : 0;
            const auto publication_type = publication_width == 1 ? ValueType::U8
                    : publication_width == 2 ? ValueType::U16
                    : publication_width == 4 ? ValueType::U32 : ValueType::U64;
            block->SetHostGPR(Value{next}.SetCastType(publication_type), HostRegIndex(20), Imm{byte_offset});
            auto* publication = &block->GetInstList().back();
            auto late_alias = block->BitCast(alias).SetType(ValueType::U64);
            auto late = block->Add(late_alias, Operand{Imm{swift::u64{9}}}).SetType(ValueType::U64);
            block->StoreUniform(Uniform{8, ValueType::U64}, late);
            auto current = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
            block->StoreUniform(Uniform{16, ValueType::U64}, current);
            if (pressure) {
                auto sum = retained.front();
                for (size_t i = 1; i < retained.size(); ++i)
                    sum = block->Add(sum, Operand{retained[i]}).SetType(ValueType::U64);
                block->StoreUniform(Uniform{24, ValueType::U64}, sum);
            }
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            FeatureSet features{};
            auto pool = pressure ? GPRSMask{~((1u << 6) - 1u)}
                                 : space.GetTrampolines().GetGPRRegs();
            pool.Mark(20);
            RegAlloc allocation{block->MaxInstrId(), pool, space.GetTrampolines().GetFPRRegs(), features};
            RegisterAllocTestSupport::RunForSpillEvictTest(block.get(), &allocation, false);
            const auto* residence = allocation.PreservedGPRDefinitionAt(old.Id());
            INFO("pressure=" << pressure << " reserved20=" << pool.Get(20)
                 << " width=" << publication_width << " olduses=" << old.Def()->GetUses(false));
            REQUIRE(residence != nullptr);
            REQUIRE(residence->transfer == publication->Id());
            REQUIRE(allocation.GPRLocationAt(alias, early.Id()).slot == 20);
            const auto suffix = allocation.GPRLocationAt(late_alias, late.Id());
            REQUIRE(suffix.kind == (pressure ? RegAlloc::MEM : RegAlloc::GPR));
            const auto& copies = allocation.GPRPreservationsBefore(publication->Id());
            REQUIRE(copies.size() == 1);
            REQUIRE(copies.front().source == 20);
            REQUIRE(copies.front().destination_kind == suffix.kind);
            REQUIRE(copies.front().destination == suffix.slot);
            INFO("suffix slot=" << suffix.slot << " reload=" << suffix.reload_register
                 << " new-kind=" << allocation.ValueType(next)
                 << " new-slot=" << allocation.Mapping(allocation.AllocationId(next)).slot);
            arm64::VerifyFixedGPRConstraints(block.get(), allocation, features);
            const std::array<arm64::GPRPhysicalWrite, 1> unsafe_early{{{next.Id(), 20}}};
            REQUIRE_THROWS_AS(arm64::VerifyFixedGPRConstraints(block.get(), allocation,
                                                            features, unsafe_early), std::logic_error);
            allocation.CompleteGPRAllocation(module);
            arm64::JitContext context{module, allocation};
            arm64::JitTranslator translator{context};
            translator.Translate(block.get());
            context.Finish();
#if defined(__aarch64__)
            const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
            REQUIRE(index != INVALID_CACHE_ID);
            context.Flush(buffer);
            REQUIRE(module->Push(block.get()));
            (void)module->PublishLinkTarget(block->GetStartLocation(), buffer.exec_data, buffer.exec_data);
            space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
            Runtime runtime{&space};
            runtime.SetLocation(0x9900);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            const auto uniform = runtime.GetUniformBuffer();
            const auto load = [&](unsigned offset) {
                swift::u64 value;
                std::memcpy(&value, uniform.data() + offset, sizeof(value));
                return value;
            };
            REQUIRE(load(0) == old_bits + 7);
            REQUIRE(load(8) == old_bits + 9);
            const auto expected_current = publication_width >= 4 ? swift::u64{42}
                    : (old_bits & ~(publication_width == 1 ? swift::u64{0xff00} : swift::u64{0xffff})) |
                              (swift::u64{42} << (byte_offset * 8));
            REQUIRE(load(16) == expected_current);
            if (pressure) REQUIRE(load(24) == 36);
#endif
        }
    }
}

TEST_CASE("GPR preservation refuses paths that need a CFG or helper proof", "[pin-plan][fixed-gpr]") {
    for (unsigned shape = 0; shape < 3; ++shape) {
        IntrusivePtr<Block> block{new Block(0, Location{0x9910})};
        const auto old = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
        if (shape == 0) {
            const auto condition = block->LoadImm(Imm{swift::u8{1}}).SetType(ValueType::U8);
            const auto label = block->Goto(BOOL{condition});
            block->BindLabel(label);
        } else if (shape == 1) {
            block->CallLambda(Lambda{Imm{swift::u64{1}}});
        }
        const auto next = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
        block->SetHostGPR(next, HostRegIndex(20), Imm{0u});
        block->StoreUniform(Uniform{0, ValueType::U64}, old);
        if (shape == 2) {
            Vector<terminal::Switch::Case> cases;
            cases.push_back({Imm{swift::u64{0}}, terminal::ReturnToHost{}});
            block->SetTerminal(terminal::Switch{old, cases});
        } else block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        GPRSMask pool{};
        pool.Mark(20);
        RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, FeatureSet{}};
        RegisterAllocPass::Run(block.get(), &allocation, false, FeatureSet{});
        REQUIRE(allocation.PreservedGPRDefinitionAt(old.Id()) != nullptr);
        REQUIRE(allocation.PreservedGPRDefinitionAt(old.Id())->transfer == old.Id());
        REQUIRE(allocation.ValueType(old) == RegAlloc::GPR);
        REQUIRE(allocation.ValueGPR(old).id != 20);
    }
}

TEST_CASE("GPR ties cannot extend a value across a fixed scratch clobber", "[pin-plan][regression]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x9920})};
    const auto input = block->LoadImm(Imm{0xa8816411u}).SetType(ValueType::U32);
    const auto wide = block->ZeroExtend32To64(input).SetType(ValueType::U64);
    const auto captured = block->BitExtract(wide, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    block->SetLocation(Imm{swift::u64{0x12345678}});
    block->StoreUniform(Uniform{0, ValueType::U32}, captured);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    GPRSMask pool{~((1u << 11) | (1u << 14) | (1u << 15) | (1u << 16) | (1u << 17))};
    FeatureSet features{};
    REQUIRE((FixedGPRClobbers(OpCode::SetLocation, features) & (1u << 11)) != 0);
    RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    RegisterAllocTestSupport::RunForIntWidthTieTest(block.get(), &allocation, true);
    REQUIRE(allocation.ValueGPR(input).id == 11);
    REQUIRE(allocation.ValueGPR(wide).id == 11);
    REQUIRE(allocation.ValueType(captured) == RegAlloc::GPR);
    REQUIRE(allocation.ValueGPR(captured).id != 11);
}

TEST_CASE("narrow GPR versions have explicit normalized captures", "[pin-plan][fixed-gpr]") {
    for (auto type : {ValueType::U8, ValueType::U16, ValueType::U32}) {
        for (bool pressure : {false, true}) {
            Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                          .has_local_operation = false, .backend_isa = kArm64};
            config.uniform_buffer_size = 128; config.stack_alignment = 16;
            AddressSpace space{config};
            auto module = space.GetDefaultModule();
            IntrusivePtr<Block> block{new Block(0, Location{0x9930})};
            constexpr swift::u64 original = 0x8877665544332211;
            auto initial = block->LoadImm(Imm{original}).SetType(ValueType::U64);
            block->SetHostGPR(initial, HostRegIndex(20), Imm{0u});
            std::vector<Value> held;
            if (pressure) for (swift::u64 i = 1; i <= 8; ++i) held.push_back(block->LoadImm(Imm{i}).SetType(ValueType::U64));
            const unsigned offset = type == ValueType::U8 ? 1 : 0;
            const auto old = block->GetHostGPR(HostRegIndex(20), Imm{offset}).SetType(type);
            block->StoreUniform(Uniform{0, type}, old);
            const auto next = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
            block->SetHostGPR(next, HostRegIndex(20), Imm{0u});
            block->StoreUniform(Uniform{8, type}, old);
            if (pressure) {
                auto total = held.front();
                for (size_t i = 1; i < held.size(); ++i) total = block->Add(total, Operand{held[i]}).SetType(ValueType::U64);
                block->StoreUniform(Uniform{16, ValueType::U64}, total);
            }
            block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
            auto pool = pressure ? GPRSMask{~((1u << 6) - 1u)} : space.GetTrampolines().GetGPRRegs();
            RegAlloc allocation{block->MaxInstrId(), pool, space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
            RegisterAllocTestSupport::RunForSpillEvictTest(block.get(), &allocation, false);
            const auto* version = allocation.PreservedGPRDefinitionAt(old.Id());
            INFO("type=" << unsigned(type) << " pressure=" << pressure << " id=" << old.Id());
            REQUIRE(version != nullptr); REQUIRE(version->transfer == old.Id());
            const auto& captures = allocation.GPRPreservationsBefore(old.Id());
            REQUIRE(captures.size() == 1); REQUIRE(captures.front().width == GetValueSizeByte(type));
            REQUIRE(captures.front().bit_offset == offset * 8);
            allocation.CompleteGPRAllocation(module);
            arm64::JitContext context{module, allocation};
            REQUIRE(allocation.GPRAllocationComplete());
            REQUIRE(allocation.GPRLocationsFrozen());
            arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
#if defined(__aarch64__)
            const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
            REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer); REQUIRE(module->Push(block.get()));
            space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
            Runtime runtime{&space}; runtime.SetLocation(0x9930); REQUIRE(runtime.Run() == HaltReason::CallHost);
            const auto state = runtime.GetUniformBuffer();
            const auto expected = original >> (offset * 8);
            for (unsigned byte = 0; byte < GetValueSizeByte(type); ++byte) {
                REQUIRE(state[byte] == ((expected >> (byte * 8)) & 0xff));
                REQUIRE(state[8 + byte] == ((expected >> (byte * 8)) & 0xff));
            }
#endif
        }
    }
}

TEST_CASE("GPR verifier rejects stale views and invalid physical residences", "[pin-plan][regression]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x9940})};
    const auto value = block->LoadImm(Imm{swift::u64{1}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, value);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &allocation, false, FeatureSet{});
    arm64::GuestStateMap state; state.Analyze(block.get(), FeatureSet{});
    arm64::GPRPlanBuilder builder{allocation, FeatureSet{}, state}; builder.Prepare(block.get()); auto plan = builder.Freeze();
    const std::array<arm64::GPRPlanVerificationInput, 1> inputs{{{block.get(), plan.get()}}};
    REQUIRE_NOTHROW(arm64::VerifyGPRAllocationPlan(inputs, allocation, FeatureSet{}));
    allocation.ResetAllocations();
    REQUIRE_THROWS_AS(arm64::VerifyGPRAllocationPlan(inputs, allocation, FeatureSet{}), std::logic_error);
}

TEST_CASE("cross-block GPR versions survive loops and validate external entries", "[pin-plan][cfg]") {
    ScopedVerifyPlan verify_plan;
    for (bool external_body : {false, true}) {
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 128; config.stack_alignment = 16;
        config.region_edges = true;
        AddressSpace space{config}; auto module = space.GetDefaultModule();
        FeatureSet features{}; HIRBuilder builder{4, true, features};
        auto* function = builder.AppendFunction(Location{0x9950}, Location{0x9953});
        const auto initial = function->LoadImm(Imm{swift::u64{0x8877665544332211}}).SetType(ValueType::U64);
        function->SetHostGPR(initial, HostRegIndex(20), Imm{0u});
        const auto old = function->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
        auto* body = builder.LinkBlock(terminal::LinkBlock{Location{0x9951}}); builder.SetCurBlock(body);
        const auto next = function->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
        function->SetHostGPR(next, HostRegIndex(20), Imm{0u});
        function->StoreUniform(Uniform{0, ValueType::U64}, old);
        const auto count = function->LoadUniform(Uniform{16, ValueType::U32}).SetType(ValueType::U32);
        const auto remaining = function->Sub(count, Operand{Imm{1u}}).SetType(ValueType::U32);
        function->StoreUniform(Uniform{16, ValueType::U32}, remaining);
        const auto more = function->TestNotZero(remaining);
        auto [exit, backedge] = builder.If(terminal::If{BOOL{more}, terminal::LinkBlock{Location{0x9951}}, terminal::LinkBlock{Location{0x9952}}});
        builder.SetCurBlock(exit);
        function->StoreUniform(Uniform{8, ValueType::U64}, old);
        function->EndBlock(terminal::ReturnToHost{}); function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        if (external_body) function->RegisterExternalEntryRoot(body);
        RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(function, &allocation, false, features);
        REQUIRE(allocation.PreservedGPRDefinitionAt(old.Id()) != nullptr);
        if (external_body) {
            REQUIRE_THROWS_AS(allocation.CompleteGPRAllocation(module), std::logic_error);
            continue;
        }
        allocation.CompleteGPRAllocation(module);
        arm64::JitContext context{module, allocation};
        arm64::JitTranslator translator{context}; translator.Translate(function); context.Finish();
        REQUIRE(allocation.GPRAllocationComplete());
#if defined(__aarch64__)
        const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer);
        space.PushCodeCache(Location{0x9950}, buffer.exec_data + context.GetCodeOffset(0x9950));
        Runtime runtime{&space}; const swift::u32 iterations = 5;
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &iterations, sizeof(iterations));
        runtime.SetLocation(0x9950); REQUIRE(runtime.Run() == HaltReason::CallHost);
        swift::u64 first, final;
        std::memcpy(&first, runtime.GetUniformBuffer().data(), sizeof(first));
        std::memcpy(&final, runtime.GetUniformBuffer().data() + 8, sizeof(final));
        REQUIRE(first == 0x8877665544332211); REQUIRE(final == first);
        swift::u32 remaining_count;
        std::memcpy(&remaining_count, runtime.GetUniformBuffer().data() + 16, sizeof(remaining_count));
        REQUIRE(remaining_count == 0);
#endif
    }
}

TEST_CASE("a captured GPR version survives both paths of a CFG join", "[pin-plan][cfg]") {
    for (swift::u8 selector : {0, 1}) {
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 128; config.stack_alignment = 16; config.region_edges = true;
        AddressSpace space{config}; auto module = space.GetDefaultModule();
        FeatureSet features{}; HIRBuilder builder{5, true, features};
        auto* function = builder.AppendFunction(Location{0x9960}, Location{0x9964});
        const auto initial = function->LoadImm(Imm{swift::u64{0x8877665544332211}}).SetType(ValueType::U64);
        function->SetHostGPR(initial, HostRegIndex(20), Imm{0u});
        const auto old = function->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
        const auto condition = function->LoadUniform(Uniform{16, ValueType::U8}).SetType(ValueType::U8);
        auto [right, left] = builder.If(terminal::If{BOOL{condition}, terminal::LinkBlock{Location{0x9961}}, terminal::LinkBlock{Location{0x9962}}});
        builder.SetCurBlock(left);
        const auto left_value = function->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
        function->SetHostGPR(left_value, HostRegIndex(20), Imm{0u});
        auto* join = builder.LinkBlock(terminal::LinkBlock{Location{0x9963}});
        builder.SetCurBlock(right);
        const auto right_value = function->LoadImm(Imm{swift::u64{99}}).SetType(ValueType::U64);
        function->SetHostGPR(right_value, HostRegIndex(20), Imm{0u});
        builder.LinkBlock(terminal::LinkBlock{Location{0x9963}});
        builder.SetCurBlock(join);
        function->StoreUniform(Uniform{0, ValueType::U64}, old);
        const auto current = function->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
        function->StoreUniform(Uniform{8, ValueType::U64}, current);
        function->EndBlock(terminal::ReturnToHost{}); function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(), space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(function, &allocation, false, features);
        allocation.CompleteGPRAllocation(module);
        arm64::JitContext context{module, allocation};
        arm64::JitTranslator translator{context}; translator.Translate(function); context.Finish();
        REQUIRE(allocation.GPRLocationsFrozen());
#if defined(__aarch64__)
        const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer);
        space.PushCodeCache(Location{0x9960}, buffer.exec_data + context.GetCodeOffset(0x9960));
        Runtime runtime{&space}; runtime.GetUniformBuffer()[16] = selector; runtime.SetLocation(0x9960);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        swift::u64 saved, published;
        std::memcpy(&saved, runtime.GetUniformBuffer().data(), sizeof(saved));
        std::memcpy(&published, runtime.GetUniformBuffer().data() + 8, sizeof(published));
        REQUIRE(saved == 0x8877665544332211); REQUIRE(published == (selector ? 42 : 99));
#endif
    }
}

TEST_CASE("frozen GPR seals are deterministic across identical blocks", "[pin-plan]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48,
                  .enable_jit = true, .has_local_operation = false,
                  .backend_isa = kArm64};
    AddressSpace space{config};
    const FeatureSet features{};
    struct Sealed {
        IntrusivePtr<Block> block;
        Value definition;
        Inst *byte_store{}, *wide_store{};
        RegAlloc* allocation{};
        std::shared_ptr<const arm64::FrozenGPRPlan> plan;
    };
    const auto seal = [&](bool wide_first) {
        Sealed out;
        out.block = IntrusivePtr<Block>{new Block(0, Location{0x99a0})};
        out.definition = out.block->LoadImm(Imm{swift::u64{0x8877665544332211}}).SetType(ValueType::U64);
        auto byte = out.definition;
        byte = byte.SetCastType(ValueType::U8);
        if (wide_first) {
            out.block->StoreUniform(Uniform{8, ValueType::U64}, out.definition);
            out.wide_store = &out.block->GetInstList().back();
        }
        out.block->StoreUniform(Uniform{0, ValueType::U8}, byte);
        out.byte_store = &out.block->GetInstList().back();
        if (!wide_first) {
            out.block->StoreUniform(Uniform{8, ValueType::U64}, out.definition);
            out.wide_store = &out.block->GetInstList().back();
        }
        out.block->SetTerminal(terminal::ReturnToDispatch{});
        out.block->ReIdInstr();
        // The sealed plan borrows its allocation; keep it alive for the case.
        auto* allocation = new RegAlloc{out.block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                                        space.GetTrampolines().GetFPRRegs(), features};
        out.allocation = allocation;
        RegisterAllocPass::Run(out.block.get(), allocation, false, features);
        arm64::GuestStateMap guest;
        guest.Analyze(out.block.get(), features);
        arm64::GPRPlanBuilder builder{*allocation, features, guest};
        builder.Prepare(out.block.get());
        builder.AssignValueHome(out.definition.Def(), 22);
        out.plan = builder.Freeze();
        REQUIRE(out.plan->MatchesFrozenIR());
        REQUIRE(out.plan->MatchesAllocation());
        REQUIRE(out.plan->MatchesFrozenIR());
        return out;
    };
    const auto first = seal(false);
    const auto second = seal(false);
    REQUIRE(first.plan->Operands().size() == second.plan->Operands().size());
    for (size_t i = 0; i < first.plan->Operands().size(); ++i) {
        const auto& a = first.plan->Operands()[i];
        const auto& b = second.plan->Operands()[i];
        REQUIRE(a.argument == b.argument);
        REQUIRE(a.element == b.element);
        REQUIRE(a.type == b.type);
        REQUIRE(a.spill_clobber_valid == b.spill_clobber_valid);
        REQUIRE(a.spill_input == b.spill_input);
        REQUIRE(a.spill_clobbers == b.spill_clobbers);
        REQUIRE(a.spill_consumer == b.spill_consumer);
    }
    REQUIRE(first.plan->UnitScratch() == second.plan->UnitScratch());
    REQUIRE(first.plan->ResultHome(first.definition.Def()) == second.plan->ResultHome(second.definition.Def()));
    const auto swapped = seal(true);
    const auto operand_for = [](const arm64::FrozenGPRPlan& plan, const Inst* consumer) {
        const auto found = std::find_if(plan.Operands().begin(), plan.Operands().end(),
                [&](const auto& use) { return use.consumer == consumer && use.argument == 1; });
        REQUIRE(found != plan.Operands().end());
        return found;
    };
    REQUIRE(operand_for(*swapped.plan, swapped.wide_store)->type == ValueType::U64);
    REQUIRE(operand_for(*swapped.plan, swapped.byte_store)->type == ValueType::U8);
}

TEST_CASE("frozen GPR seals reject foreign consumers and pin scratch preparation inputs", "[pin-plan]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48,
                  .enable_jit = true, .has_local_operation = false,
                  .backend_isa = kArm64};
    AddressSpace space{config};
    const FeatureSet features{};
    IntrusivePtr<Block> block{new Block(0, Location{0x99b0})};
    auto definition = block->LoadImm(Imm{swift::u64{0x8877665544332211}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, definition);
    auto* store = &block->GetInstList().back();
    auto sum = block->Add(definition, Operand{Imm{swift::u64{1}}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{8, ValueType::U64}, sum);
    auto* second_store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    arm64::GuestStateMap guest;
    guest.Analyze(block.get(), features);
    arm64::GPRPlanBuilder builder{allocation, features, guest};
    builder.Prepare(block.get());
    builder.AssignValueHome(definition.Def(), 22);
    const auto plan = builder.Freeze();
    // A standalone builder seal carries no scratch preparation; the prepared
    // view exists only on unit seals, so the input match is false by design.
    REQUIRE_FALSE(plan->HasPreparedScratch());
    REQUIRE_FALSE(plan->MatchesScratchInputs(space.GetTrampolines().GetGPRRegs(), features));
    IntrusivePtr<Block> foreign{new Block(0, Location{0x99b8})};
    auto foreign_value = foreign->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
    foreign->StoreUniform(Uniform{0, ValueType::U64}, foreign_value);
    auto* foreign_store = &foreign->GetInstList().back();
    foreign->SetTerminal(terminal::ReturnToDispatch{});
    foreign->ReIdInstr();
    REQUIRE(plan->SpillConsumerFor(foreign_store) == nullptr);
    REQUIRE(plan->SpillConsumerFor(nullptr) == nullptr);
    REQUIRE(plan->ScratchFor(foreign_store) == nullptr);
    REQUIRE(plan->ScratchFor(nullptr) == nullptr);
    REQUIRE_FALSE(plan->SpillClobberMask(foreign_store).has_value());
    REQUIRE(plan->SpillInputFor(foreign_store, foreign_value.Id()).definition == nullptr);
    REQUIRE(plan->ResultHome(foreign_value.Def()) == std::nullopt);
    std::vector<const Inst*> consumers;
    for (auto& inst : block->GetInstList()) consumers.push_back(&inst);
    size_t cursor = 0;
    for (const auto* consumer : consumers) {
        const auto* via_cursor = plan->ScratchFor(consumer, cursor);
        const auto* direct = plan->ScratchFor(consumer);
        REQUIRE((via_cursor == nullptr) == (direct == nullptr));
        if (via_cursor) {
            REQUIRE(via_cursor->fixed_clobbers == direct->fixed_clobbers);
            REQUIRE(via_cursor->leases == direct->leases);
        }
    }
    size_t skip_cursor = 0;
    REQUIRE(plan->ScratchFor(second_store, skip_cursor) == plan->ScratchFor(second_store));
    (void)store;
    const auto pool = space.GetTrampolines().GetGPRRegs();
    const auto base_capture = arm64::CaptureScratchPreparationInputs(pool, features);
    REQUIRE(base_capture == arm64::CaptureScratchPreparationInputs(pool, features));
    for (unsigned flip = 0; flip < 5; ++flip) {
        auto drifted = features;
        auto drifted_pool = pool;
        switch (flip) {
            case 0: drifted.jit_scratch_xpool = !drifted.jit_scratch_xpool; break;
            case 1: drifted.scratch_precise = !drifted.scratch_precise; break;
            case 2: drifted.fpr_scratch_precise = !drifted.fpr_scratch_precise; break;
            case 3: drifted.sse_nan_coldpath = !drifted.sse_nan_coldpath; break;
            default: drifted_pool = GPRSMask{~((1u << 6) - 1u)}; break;
        }
        REQUIRE_FALSE(arm64::CaptureScratchPreparationInputs(drifted_pool, drifted) == base_capture);
        REQUIRE_FALSE(plan->MatchesScratchInputs(drifted_pool, drifted));
    }
    REQUIRE(arm64::PrepareUnitScratch(pool, features) == arm64::PrepareUnitScratch(pool, features));
    const auto manual = arm64::PrepareSpillInput(store, definition.Id());
    REQUIRE(manual.definition == definition.Def());
    REQUIRE_FALSE(manual.last_use);
    const auto last = arm64::PrepareSpillInput(second_store, sum.Id());
    REQUIRE(last.definition == sum.Def());
    REQUIRE(last.last_use);
}

