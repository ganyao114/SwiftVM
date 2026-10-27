#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/compile/workspace.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/arm64/compile/audit.h"
#include "runtime/backend/arm64/compile/certificates.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/constraints.h"
#include "runtime/backend/arm64/compile/fixed_validation.h"
#include "runtime/backend/arm64/compile/gpr_inputs.h"
#include "runtime/backend/arm64/compile/residence_preparation.h"
#include "runtime/backend/arm64/compile/width_contracts.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/compile/passes/passes.h"

#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/register_state.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/svm_config.h"
#include "runtime/compile/allocate/allocation.h"
#include "support/register_alloc_test_support.h"

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
    swift::u64 ns{}, moves{}, spills{}, reloads{}, stores{}, fpr_retries{};
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
            retained.push_back(block->LoadImm(ValueType::U64, Imm{i}));
    }
    auto old = block->GetHostGPR(ValueType::U64, HostRegIndex(1), Imm{0u});
    if (shape == 0) {
        auto input = block->LoadImm(ValueType::U32, Imm{0x87654321u});
        auto sum = block->Add(ValueType::U32, input, Operand{Imm{1u}});
        auto wide = block->ZeroExtend32To64(ValueType::U64, sum);
        block->SetHostGPR(wide, HostRegIndex(22), Imm{0u});
    } else if (shape == 1) {
        auto low = old.SetCastType(ValueType::U8);
        block->SetHostGPR(low, HostRegIndex(23), Imm{1u});
        block->SetHostGPR(old.SetCastType(ValueType::U16), HostRegIndex(22), Imm{0u});
    } else if (shape == 2) {
        block->SetHostGPR(old, HostRegIndex(23), Imm{0u});
        auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x3000}});
        block->SetHostGPR(replacement, HostRegIndex(1), Imm{0u});
        auto result = block->Add(ValueType::U64, old, Operand{Imm{swift::u64{7}}});
        block->StoreUniform(Uniform{32, ValueType::U64}, result);
    } else if (shape == 3) {
        auto address = block->GetOperand(ValueType::U64, Operand{old});
        auto loaded = block->LoadMemory(ValueType::U64, Operand{address});
        block->SetHostGPR(loaded, HostRegIndex(22), Imm{0u});
    } else {
        auto initial = block->LoadImm(ValueType::U64, Imm{swift::u64{0x8877665544332211}});
        block->SetHostGPR(initial, HostRegIndex(20), Imm{0u});
        auto version = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
        auto first = block->Add(ValueType::U64, version, Operand{Imm{swift::u64{7}}});
        block->StoreUniform(Uniform{16, ValueType::U64}, first);
        auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
        block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
        auto last = block->Add(ValueType::U64, version, Operand{Imm{swift::u64{9}}});
        block->StoreUniform(Uniform{24, ValueType::U64}, last);
    }
    if (pressure) {
        auto total = retained.front();
        for (size_t i = 1; i < retained.size(); ++i)
            total = block->Add(ValueType::U64, total, Operand{retained[i]});
        block->StoreUniform(Uniform{40, ValueType::U64}, total);
    }
    block->StoreUniform(Uniform{48, old.Type()}, old);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    auto gprs = pressure ? GPRSMask{~((1u << 6) - 1u)}
                         : space.GetTrampolines().GetGPRRegs();
    AllocationFixture allocation{
            block->MaxInstrId(), gprs, space.GetTrampolines().GetFPRRegs(), features};
    const auto start = std::chrono::steady_clock::now();
    if (pressure) {
        auto input_features = features;
        input_features.ra_spill_evict = false;
        auto prepared = ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                block.get(), allocation, input_features);
        if (shape < 4 && !use_preplanning) prepared.fixed.clear();
        RegisterAllocTestSupport::RunForSpillEvictTest(
                block.get(), &allocation, std::move(prepared));
        REQUIRE(allocation.SpillCount() > 0);
    } else {
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
    }
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(space.GetDefaultModule(), allocation);
    arm64::JitContext context{space.GetDefaultModule(), allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    if (metrics) {
        metrics->ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        metrics->bytes = context.CurrentBufferSize();
        metrics->spills = allocation.SpillCount();
        metrics->reloads = context.GetRAShapeCounters().spill_loads;
        metrics->stores = context.GetRAShapeCounters().spill_stores;
        metrics->fpr_retries = allocation.FPRContractRetryCount();
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
        std::printf("gpr-metrics pressure=%u shape=%u bytes=%zu moves=%llu spill_defs=%llu reloads=%llu stores=%llu median_ns=%llu fpr_retries=%llu\n",
                    pressure, shape, sample.bytes, static_cast<unsigned long long>(sample.moves),
                    static_cast<unsigned long long>(sample.spills), static_cast<unsigned long long>(sample.reloads),
                    static_cast<unsigned long long>(sample.stores), static_cast<unsigned long long>(times[times.size() / 2]),
                    static_cast<unsigned long long>(sample.fpr_retries));
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
        std::printf("gpr-initial pressure=%u shape=%u bytes=%zu spill_defs=%llu reloads=%llu stores=%llu median_ns=%llu fpr_retries=%llu\n",
                    pressure, shape, sample.bytes, static_cast<unsigned long long>(sample.spills),
                    static_cast<unsigned long long>(sample.reloads), static_cast<unsigned long long>(sample.stores),
                    static_cast<unsigned long long>(times[times.size() / 2]),
                    static_cast<unsigned long long>(sample.fpr_retries));
        }
}

TEST_CASE("W-write proofs keep raw aliases and capture roots distinct", "[pin-plan][width]") {
    using arm64::WWriteAliases;
    Block block{0, Location{0x9878}};
    const auto fit = block.LoadImm(ValueType::U64, Imm{swift::u64{0xffffffff}});
    const auto wide = block.LoadImm(ValueType::U64, Imm{swift::u64{0x100000000}});
    const auto input = block.LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    const auto wide_input = block.LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
    const auto byte = block.LoadMemory(ValueType::U8, Operand{fit});
    const auto half = block.LoadMemoryTSO(ValueType::U16, Operand{fit});
    const auto signed_wide = block.SignExtend(ValueType::U64, byte);
    const auto signed_word = block.SignExtend(ValueType::U32, byte);
    const auto capture = block.GetHostGPR(ValueType::U32, HostRegIndex(23), Imm{0u});
    const auto alias = block.BitCast(ValueType::U32, input);
    const auto fit_alias = block.BitCast(ValueType::U64, fit);
    const auto wide_alias = block.BitCast(ValueType::U64, wide);
    const auto extract = block.BitExtract(ValueType::U32, alias, Imm{0u}, Imm{32u});
    const auto extend = block.ZeroExtend32To64(ValueType::U64, extract);
    const auto captured_extend = block.ZeroExtend32To64(ValueType::U64, capture);
    const auto partial = block.BitExtract(ValueType::U32, wide, Imm{8u}, Imm{16u});
    const auto other_extend =
            block.ZeroExtend32To64(ValueType::U64, Value{input}.SetCastType(ValueType::U16));
    block.SetTerminal(terminal::ReturnToHost{});
    block.ReIdInstr();
    struct Case {
        Value value, raw_root, resolved_root;
        bool raw_write, resolved_write;
    };
    // Expected physical facts are independent of the projection walker:
    // aliases are not writes; unsigned narrow loads and real U32 results
    // write W; captures require a separate committed allocation proof.
    const Case cases[] = {
            {{}, {}, {}, false, false},
            {fit, fit, fit, true, true},
            {wide, wide, wide, false, false},
            {input, input, input, true, true},
            {wide_input, wide_input, wide_input, false, false},
            {byte, byte, byte, true, true},
            {half, half, half, true, true},
            {signed_wide, signed_wide, signed_wide, false, false},
            {signed_word, signed_word, signed_word, true, true},
            {capture, capture, capture, false, false},
            {alias, alias, input, false, true},
            {fit_alias, fit_alias, fit, false, true},
            {wide_alias, wide_alias, wide, false, false},
            {extract, alias, input, false, true},
            {extend, alias, input, false, true},
            {captured_extend, capture, capture, false, false},
            {partial, partial, partial, true, true},
            {other_extend, other_extend, other_extend, false, false},
            {Value{fit}.SetCastType(ValueType::U8),
             Value{fit}.SetCastType(ValueType::U8),
             Value{fit}.SetCastType(ValueType::U8),
             true,
             true},
    };
    unsigned index = 0;
    for (const auto& item : cases) {
        CAPTURE(index);
        ++index;
        const auto raw = arm64::WWriteRoot(item.value, WWriteAliases::Raw);
        const auto resolved = arm64::WWriteRoot(item.value, WWriteAliases::Resolved);
        REQUIRE(raw.Def() == item.raw_root.Def());
        REQUIRE(resolved.Def() == item.resolved_root.Def());
        if (raw.Defined()) REQUIRE(raw.Type() == item.raw_root.Type());
        if (resolved.Defined()) REQUIRE(resolved.Type() == item.resolved_root.Type());
        REQUIRE(arm64::HasKnownWWrite(item.value) == item.raw_write);
        REQUIRE(arm64::HasKnownWWrite(item.value, WWriteAliases::Raw) == item.raw_write);
        REQUIRE(arm64::HasKnownWWrite(item.value, WWriteAliases::Resolved) == item.resolved_write);
    }
}

TEST_CASE("prepared GPR facts retain operand views and reject allocation retries", "[pin-plan]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48,
                  .enable_jit = true, .has_local_operation = false,
                  .backend_isa = kArm64};
    AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x9880})};
    auto definition = block->LoadImm(ValueType::U64, Imm{swift::u64{0x8877665544332211}});
    auto byte = definition;
    byte = byte.SetCastType(ValueType::U8);
    block->StoreUniform(Uniform{0, ValueType::U8}, byte);
    auto* byte_store = &block->GetInstList().back();
    block->StoreUniform(Uniform{8, ValueType::U64}, definition);
    auto* wide_store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
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
    arm64::RegisterStatePreparation guest;
    guest.BeginBlock(block.get(), features, allocation.PrepareFacts(block.get()));
    arm64::ResidencePreparation builder{allocation, features, guest};
    allocation.BeginTargetPlanning();
    builder.Prepare(block.get());
    builder.AssignValueHome(definition.Def(), 22);
    RegisterAllocTestSupport::FinishGPRForTest(builder, allocation, features);
    const auto& actions = allocation.Actions();
    REQUIRE(allocation.GPRLocationsFrozen());
    REQUIRE(actions.MatchesAllocation(allocation));
    REQUIRE_THROWS_AS(builder.ElideZeroExtend(definition.Def()), std::logic_error);
    REQUIRE_THROWS_AS(builder.AssignValueHome(definition.Def(), 22), std::logic_error);
    const auto operand = [](const Inst* consumer) { return consumer->GetArg<Value>(1); };
    REQUIRE(operand(byte_store).Type() == ValueType::U8);
    REQUIRE(operand(wide_store).Type() == ValueType::U64);
    REQUIRE(allocation.GPRLocationAt(byte, byte_store->Id()).bits == 32);
    REQUIRE(allocation.GPRLocationAt(byte, byte_store->Id()).slot == 22);
    REQUIRE(allocation.GPRLocationAt(definition, wide_store->Id()).bits == 64);
    const auto generation = allocation.GPRLocationGeneration();
    const auto attempt = allocation.AllocationAttempt();
    allocation.ResetAllocations();
    REQUIRE_FALSE(actions.MatchesAllocation(allocation));
    REQUIRE(allocation.AllocationAttempt() > attempt);
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    guest.BeginBlock(block.get(), features, allocation.PrepareFacts(block.get()));
    allocation.BeginTargetPlanning();
    builder.Prepare(block.get());
    RegisterAllocTestSupport::FinishGPRForTest(builder, allocation, features);
    REQUIRE(allocation.GPRLocationGeneration() > generation);
    REQUIRE(actions.MatchesAllocation(allocation));
}

TEST_CASE("GPR emission facts borrow the allocation owner and reject retries",
          "[pin-plan][regression]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48,
                  .enable_jit = true, .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x9890})};
    auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{123}});
    block->StoreUniform(Uniform{0, ValueType::U64}, value);
    auto* store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
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
    arm64::RegisterStatePreparation guest;
    guest.BeginBlock(block.get(), features, allocation.PrepareFacts(block.get()));
    arm64::ResidencePreparation builder{allocation, features, guest};
    allocation.BeginTargetPlanning();
    builder.Prepare(block.get());
    RegisterAllocTestSupport::FinishGPRForTest(builder, allocation, features);
    const auto read = static_cast<compile::AllocationView>(allocation);
    const auto location = read.GPRLocationAt(value, store->Id());
    const arm64::UnitActions* imported = &allocation.Actions();
    REQUIRE(imported->MatchesAllocation(read));
    REQUIRE(read.GPRLocationAt(value, store->Id()).slot == location.slot);
    REQUIRE(imported->ScratchFor(store));
    REQUIRE_THROWS_AS(builder.ElideZeroExtend(value.Def()), std::logic_error);
    REQUIRE_THROWS_AS(builder.RecordPhysicalWrite(store, 22), std::logic_error);
    allocation.ResetAllocations();
    REQUIRE_FALSE(imported->MatchesAllocation(read));
    REQUIRE(imported->ScratchFor(store) == nullptr);
    REQUIRE(imported->Residence(value.Def()) == nullptr);
}

TEST_CASE("GPR publication completes under pressure without physical retries", "[pin-plan][fixed-gpr]") {
    for (bool pressure : {false, true}) {
        for (unsigned shape : {0u, 2u, 3u}) {
            GPRBenchmarkMetrics metrics;
            const auto code = EmitPlanFixture(shape, pressure, &metrics, true);
            INFO("shape=" << shape << " pressure=" << pressure);
            REQUIRE_FALSE(code.empty());
            REQUIRE(metrics.fpr_retries == 0);
        }
    }
}

TEST_CASE("early GPR publication cannot pass a later narrow capture", "[pin-plan][regression]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 128; config.stack_alignment = 16;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x9898})};
    const auto initial = block->LoadImm(ValueType::U64, Imm{swift::u64{0x8877665544332211}});
    block->SetHostGPR(initial, HostRegIndex(22), Imm{0u});
    const auto next = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
    const auto captured = block->GetHostGPR(ValueType::U8, HostRegIndex(22), Imm{0u});
    block->StoreUniform(Uniform{0, ValueType::U8}, captured);
    block->SetHostGPR(next, HostRegIndex(22), Imm{0u});
    const auto current = block->GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
    block->StoreUniform(Uniform{8, ValueType::U64}, current);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    FeatureSet features{};
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
    REQUIRE_FALSE(allocation.HasFixedGPRDefinition(next.Id()));
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    arm64::JitContext context{module, allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
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
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    ScopedVerifyPlan verify_plan;
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x98a0})};
    const auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
    block->StoreUniform(Uniform{0, ValueType::U64}, Value{value}.SetCastType(ValueType::U8));
    auto* store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    FeatureSet features{};
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
    REQUIRE_THROWS_AS(::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation),
                      std::logic_error);
    REQUIRE_FALSE(allocation.TargetPlanning());
    REQUIRE_FALSE(allocation.Actions().Ready());
    REQUIRE_FALSE(allocation.GPRLocationsFrozen());
    REQUIRE_FALSE(allocation.Actions().ForBlock(block.get()));
    ::swift::runtime::compile::RewriteContext{block.get(), compilation_workspace, nullptr, false}
            .SetArg(block.get(), store, 1, value);
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    arm64::JitContext context{module, allocation, allocation.Actions()};
    REQUIRE(allocation.Actions().Ready());
    arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);
    REQUIRE(context.OperandLocation(value, store).kind == AllocationFixture::GPR);
    allocation.ResetAllocations();
    REQUIRE_THROWS_AS(context.OperandLocation(value, store), std::logic_error);
}

TEST_CASE("aborting target planning discards physical publication contracts", "[pin-plan][regression]") {
    GPRSMask pool{}; pool.Mark(20);
    AllocationFixture allocation{4, pool, FPRSMask{}, FeatureSet{}};
    allocation.PlanFixedGPRDefinition(0, {.home = 20, .end = 2, .last_use = 2,
            .publication = 2, .owner = 0, .width = 8});
    allocation.MapFixedRegister(0, HostGPR{20});
    allocation.BeginTargetPlanning();
    allocation.PlanFixedGPRDefinition(0, {.home = 20, .end = 3, .last_use = 3,
            .publication = 3, .owner = 0, .width = 8});
    REQUIRE(allocation.NeedsFixedGPRPublication(3));
    allocation.AbortTargetPlanning();
    REQUIRE_FALSE(allocation.TargetPlanning());
    REQUIRE_FALSE(allocation.FixedGPRConstraintsReady());
    REQUIRE(allocation.FixedGPRDefinitionAt(0) == nullptr);
    REQUIRE_FALSE(allocation.NeedsFixedGPRPublication(2));
    REQUIRE_FALSE(allocation.NeedsFixedGPRPublication(3));
    REQUIRE(allocation.Mapping(0).type == AllocationFixture::NONE);
}

TEST_CASE("final width verification certifies elisions and rejects mapping drift", "[pin-plan][regression]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x98a8})};
    const auto left = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    const auto right = block->LoadImm(ValueType::U32, Imm{swift::u32{7}});
    const auto producer = block->Add(ValueType::U32, left, Operand{right});
    const auto extract = block->BitExtract(ValueType::U32, producer, Imm{0u}, Imm{32u});
    const auto extended = block->ZeroExtend32To64(ValueType::U64, extract);
    block->StoreUniform(Uniform{8, ValueType::U64}, extended);
    block->StoreUniform(Uniform{16, ValueType::U32}, extract);
    block->StoreUniform(Uniform{20, ValueType::U32}, producer);
    block->SetTerminal(terminal::ReturnToDispatch{}); block->ReIdInstr();
    FeatureSet features{}; features.ra_width_chain = true; features.ra_coalesce = false;
    AllocationFixture allocation{
            block->MaxInstrId(), GPRSMask{~((1u << 8) - 1u)}, FPRSMask{}, features};
    RegisterAllocTestSupport::RunForWidthChainTest(block.get(), &allocation, true);
    auto& analysis = allocation.PrepareFacts(block.get()).PrepareUses(block.get());
    const auto certificates = arm64::PrepareWidthCertificates(
            block.get(), allocation, features, GPRSMask{}, analysis);
    REQUIRE(certificates.width.size() == 2);
    const auto home = allocation.ValueGPR(producer).id;
    allocation.MapRegister(extract.Id(), HostGPR{static_cast<swift::u16>((home + 1) % 31)});
    REQUIRE_THROWS_AS(arm64::PrepareWidthCertificates(
                              block.get(), allocation, features, GPRSMask{}, analysis),
                      std::logic_error);
}

TEST_CASE("final width verification respects preserved owners at transfer boundaries",
          "[pin-plan][regression]") {
    struct Scenario {
        bool preserved, prefix_at_target;
        int transfer;
        bool accepted;
    };
    constexpr Scenario scenarios[] = {{false, false, 0, true},
                                      {false, true, 0, false},
                                      {true, true, -1, true},
                                      {true, true, 0, true},
                                      {true, true, 1, false},
                                      {true, false, -1, false},
                                      {true, false, 0, false},
                                      {true, false, 1, true}};
    for (const swift::u16 home : {0u, 8u, 19u, 22u, 29u}) {
        for (const auto row : scenarios) {
            CAPTURE(home, row.preserved, row.prefix_at_target, row.transfer);
            const swift::u16 away = home == 29 ? 23 : home + 1;
            const auto prefix = row.prefix_at_target ? home : away;
            IntrusivePtr<Block> block{new Block(0, Location{0x98a9})};
            const auto other = block->GetHostGPR(ValueType::U64, HostRegIndex(prefix), Imm{0u});
            const auto source = block->LoadImm(ValueType::U32, Imm{7u});
            const auto short_lived = block->LoadImm(ValueType::U64, Imm{swift::u64{19}});
            block->StoreUniform(Uniform{24, ValueType::U64}, short_lived);
            const auto bridge = block->BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
            block->AdvancePC(Imm{0u});
            block->StoreUniform(Uniform{8, ValueType::U32}, bridge);
            const auto* consumer = &block->GetInstList().back();
            block->StoreUniform(Uniform{16, ValueType::U64}, other);
            const auto* last = &block->GetInstList().back();
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();

            FeatureSet features{};
            features.ra_width_chain = true;
            AllocationFixture allocation{
                    block->MaxInstrId(), GPRSMask{0x7fffffffu}, FPRSMask{}, features};
            for (const auto& inst : block->GetInstList())
                allocation.SetActiveRegs(inst.Id(), GPRSMask{0x7fffffffu}, FPRSMask{});
            allocation.MapRegister(source.Id(), HostGPR{home});
            allocation.MapRegister(bridge.Id(), HostGPR{home});
            // A later-born owner dies before the bridge. It must not hide the
            // earlier owner's longer interval when the prefix is searched.
            allocation.MapRegister(short_lived.Id(), HostGPR{home});
            allocation.MapRegister(
                    other.Id(),
                    HostGPR{row.preserved ? (row.prefix_at_target ? away : home) : prefix});
            allocation.MarkWidthChainCoalesced(bridge.Id(), source.Id());
            if (row.preserved) {
                const swift::u32 transfer = row.transfer < 0    ? bridge.Id() - 1
                                            : row.transfer == 0 ? bridge.Id()
                                                                : consumer->Id() + 1;
                allocation.PlanFixedGPRDefinition(other.Id(),
                                                  {.home = prefix,
                                                   .end = last->Id(),
                                                   .transfer = transfer,
                                                   .last_use = last->Id(),
                                                   .owner = other.Id(),
                                                   .width = 8,
                                                   .preserved = true});
            }
            compile::BlockAnalysisIndex analysis;
            analysis.Build(block.get());
            analysis.PrepareResolvedUses();
            REQUIRE(arm64::CheckWidthContracts(
                            block.get(), allocation, features, GPRSMask{}, analysis) ==
                    row.accepted);
            if (row.accepted) {
                const auto proofs = arm64::PrepareWidthCertificates(
                        block.get(), allocation, features, GPRSMask{}, analysis);
                REQUIRE(proofs.width == std::vector<swift::u32>{bridge.Id()});
            } else {
                REQUIRE_THROWS_AS(arm64::PrepareWidthCertificates(
                                          block.get(), allocation, features, GPRSMask{}, analysis),
                                  std::logic_error);
            }
        }
    }
}

TEST_CASE("final width verification keeps endpoint and clobber families distinct",
          "[pin-plan][regression]") {
    enum class Hazard {
        BridgeUnavailable,
        UseUnavailable,
        AfterUnavailable,
        PublicationSame,
        PublicationOther,
        FixedScratch,
        Helper
    };
    for (const bool copy : {false, true}) {
        for (const swift::u16 home : {0u, 8u, 11u, 19u, 22u, 30u}) {
            for (const auto hazard : {Hazard::BridgeUnavailable,
                                      Hazard::UseUnavailable,
                                      Hazard::AfterUnavailable,
                                      Hazard::PublicationSame,
                                      Hazard::PublicationOther,
                                      Hazard::FixedScratch,
                                      Hazard::Helper}) {
                if (!copy && hazard == Hazard::Helper) continue;
                CAPTURE(copy, home, static_cast<int>(hazard));
                IntrusivePtr<Block> block{new Block(0, Location{0x98aa})};
                const auto source =
                        copy ? block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64})
                             : block->LoadImm(ValueType::U32, Imm{7u});
                const auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{23}});
                const auto bridge = block->BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
                if (hazard == Hazard::PublicationSame || hazard == Hazard::PublicationOther) {
                    const auto target = hazard == Hazard::PublicationSame ? home : (home + 1) % 31;
                    block->SetHostGPR(replacement, HostRegIndex(target), Imm{0u});
                } else if (hazard == Hazard::FixedScratch) {
                    block->CheckMemoryAlignment(source, Imm{8u});
                } else if (hazard == Hazard::Helper) {
                    // Inspect its ABI only; this fixture never invokes the helper.
                    (void)block->CallLambda(ValueType::U64, Lambda{Imm{swift::u64{0}}});
                } else {
                    block->AdvancePC(Imm{0u});
                }
                block->StoreUniform(Uniform{8, ValueType::U32}, bridge);
                const auto* consumer = &block->GetInstList().back();
                block->StoreUniform(Uniform{16, source.Type()}, source);
                const auto* after = &block->GetInstList().back();
                block->SetTerminal(terminal::ReturnToHost{});
                block->ReIdInstr();
                FeatureSet features{};
                features.ra_width_chain = true;
                features.jit_scratch_xpool = true;
                AllocationFixture allocation{
                        block->MaxInstrId(), GPRSMask{0x7fffffffu}, FPRSMask{}, features};
                for (const auto& inst : block->GetInstList())
                    allocation.SetActiveRegs(inst.Id(), GPRSMask{0x7fffffffu}, FPRSMask{});
                allocation.MapRegister(source.Id(), HostGPR{home});
                allocation.MapRegister(bridge.Id(), HostGPR{home});
                if (copy)
                    allocation.MarkLow32CopyCoalesced(bridge.Id(), source.Id());
                else
                    allocation.MarkWidthChainCoalesced(bridge.Id(), source.Id());
                if (hazard == Hazard::BridgeUnavailable || hazard == Hazard::UseUnavailable ||
                    hazard == Hazard::AfterUnavailable) {
                    auto live = GPRSMask{0x7fffffffu};
                    live.Clear(home);
                    const auto point = hazard == Hazard::BridgeUnavailable ? bridge.Id()
                                       : hazard == Hazard::UseUnavailable  ? consumer->Id()
                                                                           : after->Id();
                    allocation.SetActiveRegs(point, live, FPRSMask{});
                }
                const bool accepted = hazard == Hazard::AfterUnavailable ||
                                      hazard == Hazard::PublicationOther ||
                                      (hazard == Hazard::FixedScratch && home != 11) ||
                                      (hazard == Hazard::Helper && home >= 19);
                compile::BlockAnalysisIndex analysis;
                analysis.Build(block.get());
                analysis.PrepareResolvedUses();
                REQUIRE(arm64::CheckWidthContracts(
                                block.get(), allocation, features, GPRSMask{}, analysis) ==
                        accepted);
                if (accepted) {
                    const auto proofs = arm64::PrepareWidthCertificates(
                            block.get(), allocation, features, GPRSMask{}, analysis);
                    REQUIRE((copy ? proofs.low32 : proofs.width) ==
                            std::vector<swift::u32>{bridge.Id()});
                } else {
                    REQUIRE_THROWS_AS(
                            arm64::PrepareWidthCertificates(
                                    block.get(), allocation, features, GPRSMask{}, analysis),
                            std::logic_error);
                }
            }
        }
    }
}

TEST_CASE("GPR allocation cannot change during emission", "[pin-plan]") {
    AllocationFixture allocation{8, GPRSMask{}, FPRSMask{}, FeatureSet{}};
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

TEST_CASE("AllocationFixture completes target planning before an encoder exists",
          "[pin-plan][target-planner]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 64;
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x98ac})};
    const auto value = block->LoadImm(ValueType::U32, Imm{swift::u32{42}});
    const auto sum = block->Add(ValueType::U32, value, Operand{Imm{swift::u32{7}}});
    const auto wide = block->ZeroExtend32To64(ValueType::U64, sum);
    block->SetHostGPR(wide, HostRegIndex(22), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->StoreUniform(Uniform{0, ValueType::U64}, wide);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    FeatureSet features{};
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
    REQUIRE_FALSE(allocation.Actions().Ready());
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    REQUIRE(allocation.Actions().Ready());
    REQUIRE(allocation.GPRLocationsFrozen());
    REQUIRE_FALSE(allocation.TargetPlanning());
    const auto plan = allocation.Actions().ForBlock(block.get());
    REQUIRE(plan);
    REQUIRE(allocation.Actions().MatchesAllocation(allocation));
    REQUIRE(allocation.Actions().MatchesAllocation(allocation));
    REQUIRE(allocation.Actions().Publication(publication->Id()));
    const auto generation = allocation.GPRLocationGeneration();
    const auto attempt = allocation.AllocationAttempt();
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    REQUIRE(allocation.Actions().ForBlock(block.get()) == plan);
    REQUIRE(allocation.GPRLocationGeneration() == generation);
    REQUIRE(allocation.AllocationAttempt() == attempt);
    arm64::JitContext context{module, allocation, allocation.Actions()};
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
            auto initial = block->LoadImm(ValueType::U64, Imm{old_bits});
            block->SetHostGPR(initial, HostRegIndex(20), Imm{0u});
            std::vector<Value> retained;
            if (pressure) {
                for (swift::u64 i = 1; i <= 8; ++i)
                    retained.push_back(block->LoadImm(ValueType::U64, Imm{i}));
            }
            auto old = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
            auto alias = block->BitCast(ValueType::U64, old);
            auto early = block->Add(ValueType::U64, alias, Operand{Imm{swift::u64{7}}});
            block->StoreUniform(Uniform{0, ValueType::U64}, early);
            auto next = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
            const unsigned byte_offset = publication_width == 1 ? 1 : 0;
            const auto publication_type = publication_width == 1 ? ValueType::U8
                    : publication_width == 2 ? ValueType::U16
                    : publication_width == 4 ? ValueType::U32 : ValueType::U64;
            block->SetHostGPR(Value{next}.SetCastType(publication_type), HostRegIndex(20), Imm{byte_offset});
            auto* publication = &block->GetInstList().back();
            auto late_alias = block->BitCast(ValueType::U64, alias);
            auto late = block->Add(ValueType::U64, late_alias, Operand{Imm{swift::u64{9}}});
            block->StoreUniform(Uniform{8, ValueType::U64}, late);
            auto current = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
            block->StoreUniform(Uniform{16, ValueType::U64}, current);
            if (pressure) {
                auto sum = retained.front();
                for (size_t i = 1; i < retained.size(); ++i)
                    sum = block->Add(ValueType::U64, sum, Operand{retained[i]});
                block->StoreUniform(Uniform{24, ValueType::U64}, sum);
            }
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            FeatureSet features{};
            auto pool = pressure ? GPRSMask{~((1u << 6) - 1u)}
                                 : space.GetTrampolines().GetGPRRegs();
            pool.Mark(20);
            AllocationFixture allocation{
                    block->MaxInstrId(), pool, space.GetTrampolines().GetFPRRegs(), features};
            RegisterAllocTestSupport::RunForSpillEvictTest(block.get(), &allocation, false);
            const auto* residence = allocation.PreservedGPRDefinitionAt(old.Id());
            INFO("pressure=" << pressure << " reserved20=" << pool.Get(20)
                 << " width=" << publication_width << " olduses=" << old.Def()->GetUses(false));
            REQUIRE(residence != nullptr);
            REQUIRE(residence->transfer == publication->Id());
            REQUIRE(allocation.GPRLocationAt(alias, early.Id()).slot == 20);
            const auto suffix = allocation.GPRLocationAt(late_alias, late.Id());
            REQUIRE(suffix.kind == (pressure ? AllocationFixture::MEM : AllocationFixture::GPR));
            const auto& copies = allocation.GPRPreservationsBefore(publication->Id());
            REQUIRE(copies.size() == 1);
            REQUIRE(copies.front().source == 20);
            REQUIRE(copies.front().destination_kind == suffix.kind);
            REQUIRE(copies.front().destination == suffix.slot);
            INFO("suffix slot=" << suffix.slot << " reload=" << suffix.reload_register
                 << " new-kind=" << allocation.ValueType(next)
                 << " new-slot=" << allocation.Mapping(allocation.AllocationId(next)).slot);
            auto& check_index = allocation.PrepareFacts(block.get()).PrepareUses(block.get());
            arm64::FixedValidationDescription described{
                    block.get(), allocation, features, check_index};
            compile::VerifyFixedConstraints(block.get(), allocation, described.Inputs());
            const std::array<compile::PhysicalGPRWrite, 1> unsafe_early{{{next.Id(), 20}}};
            REQUIRE_THROWS_AS(compile::VerifyFixedConstraints(
                                      block.get(), allocation, described.Inputs(), unsafe_early),
                              std::logic_error);
            ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
            arm64::JitContext context{module, allocation, allocation.Actions()};
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
        const auto old = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
        if (shape == 0) {
            const auto condition = block->LoadImm(ValueType::U8, Imm{swift::u8{1}});
            const auto label = block->Goto(BOOL{condition});
            block->BindLabel(label);
        } else if (shape == 1) {
            block->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{1}}});
        }
        const auto next = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
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
        AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, FeatureSet{}};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, FeatureSet{}),
                false);
        REQUIRE(allocation.PreservedGPRDefinitionAt(old.Id()) != nullptr);
        REQUIRE(allocation.PreservedGPRDefinitionAt(old.Id())->transfer == old.Id());
        REQUIRE(allocation.ValueType(old) == AllocationFixture::GPR);
        REQUIRE(allocation.ValueGPR(old).id != 20);
    }
}

TEST_CASE("GPR ties cannot extend a value across a fixed scratch clobber", "[pin-plan][regression]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x9920})};
    const auto input = block->LoadImm(ValueType::U32, Imm{0xa8816411u});
    const auto wide = block->ZeroExtend32To64(ValueType::U64, input);
    const auto captured = block->BitExtract(ValueType::U32, wide, Imm{0u}, Imm{32u});
    block->SetLocation(Lambda{Imm{swift::u64{0x12345678}}});
    block->StoreUniform(Uniform{0, ValueType::U32}, captured);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    GPRSMask pool{~((1u << 11) | (1u << 14) | (1u << 15) | (1u << 16) | (1u << 17))};
    FeatureSet features{};
    REQUIRE((FixedGPRClobbers(OpCode::SetLocation, features) & (1u << 11)) != 0);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    RegisterAllocTestSupport::RunForIntWidthTieTest(block.get(), &allocation, true);
    REQUIRE(allocation.ValueGPR(input).id == 11);
    REQUIRE(allocation.ValueGPR(wide).id == 11);
    REQUIRE(allocation.ValueType(captured) == AllocationFixture::GPR);
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
            auto initial = block->LoadImm(ValueType::U64, Imm{original});
            block->SetHostGPR(initial, HostRegIndex(20), Imm{0u});
            std::vector<Value> held;
            if (pressure)
                for (swift::u64 i = 1; i <= 8; ++i)
                    held.push_back(block->LoadImm(ValueType::U64, Imm{i}));
            const unsigned offset = type == ValueType::U8 ? 1 : 0;
            const auto old = block->GetHostGPR(type, HostRegIndex(20), Imm{offset});
            block->StoreUniform(Uniform{0, type}, old);
            const auto next = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
            block->SetHostGPR(next, HostRegIndex(20), Imm{0u});
            block->StoreUniform(Uniform{8, type}, old);
            if (pressure) {
                auto total = held.front();
                for (size_t i = 1; i < held.size(); ++i)
                    total = block->Add(ValueType::U64, total, Operand{held[i]});
                block->StoreUniform(Uniform{16, ValueType::U64}, total);
            }
            block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
            auto pool = pressure ? GPRSMask{~((1u << 6) - 1u)} : space.GetTrampolines().GetGPRRegs();
            AllocationFixture allocation{
                    block->MaxInstrId(), pool, space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
            RegisterAllocTestSupport::RunForSpillEvictTest(block.get(), &allocation, false);
            const auto* version = allocation.PreservedGPRDefinitionAt(old.Id());
            INFO("type=" << unsigned(type) << " pressure=" << pressure << " id=" << old.Id());
            REQUIRE(version != nullptr); REQUIRE(version->transfer == old.Id());
            const auto& captures = allocation.GPRPreservationsBefore(old.Id());
            REQUIRE(captures.size() == 1); REQUIRE(captures.front().width == GetValueSizeByte(type));
            REQUIRE(captures.front().bit_offset == offset * 8);
            ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
            arm64::JitContext context{module, allocation, allocation.Actions()};
            REQUIRE(allocation.Actions().Ready());
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
    const FeatureSet features{};
    IntrusivePtr<Block> block{new Block(0, Location{0x9940})};
    const auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{1}});
    block->StoreUniform(Uniform{0, ValueType::U64}, value);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    arm64::RegisterStatePreparation state;
    state.BeginBlock(block.get(), features, allocation.PrepareFacts(block.get()));
    arm64::ResidencePreparation builder{allocation, features, state};
    REQUIRE_FALSE(allocation.Actions().MatchesPreparation(allocation));
    allocation.BeginTargetPlanning();
    REQUIRE(allocation.Actions().MatchesPreparation(allocation));
    builder.Prepare(block.get());
    RegisterAllocTestSupport::FinishGPRForTest(builder, allocation, features);
    auto& analysis = allocation.PrepareFacts(block.get()).PrepareUses(block.get());
    const std::array<arm64::EmissionCertificateInput, 1> inputs{{{block.get(), &analysis, {}}}};
    arm64::FixedValidationDescription fixed{block.get(), allocation, features, analysis};
    REQUIRE_FALSE(allocation.Actions().MatchesPreparation(allocation));
    REQUIRE_NOTHROW((void)arm64::PrepareEmissionCertificates(
            inputs, allocation, allocation.Actions(), fixed.Inputs(), features, GPRSMask{}));
    REQUIRE_NOTHROW(arm64::AuditGPRAllocation(inputs, allocation, allocation.Actions(), features));
    allocation.ResetAllocations();
    REQUIRE_THROWS_AS(
            (void)arm64::PrepareEmissionCertificates(
                    inputs, allocation, allocation.Actions(), fixed.Inputs(), features, GPRSMask{}),
            std::logic_error);
    REQUIRE_THROWS_AS(arm64::AuditGPRAllocation(inputs, allocation, allocation.Actions(), features),
                      std::logic_error);
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
        const auto initial = function->LoadImm(ValueType::U64, Imm{swift::u64{0x8877665544332211}});
        function->SetHostGPR(initial, HostRegIndex(20), Imm{0u});
        const auto old = function->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
        auto* body = builder.LinkBlock(terminal::LinkBlock{Location{0x9951}});
        builder.SetCurBlock(body);
        const auto next = function->LoadImm(ValueType::U64, Imm{swift::u64{42}});
        function->SetHostGPR(next, HostRegIndex(20), Imm{0u});
        function->StoreUniform(Uniform{0, ValueType::U64}, old);
        const auto count = function->LoadUniform(ValueType::U32, Uniform{16, ValueType::U32});
        const auto remaining = function->Sub(ValueType::U32, count, Operand{Imm{1u}});
        function->StoreUniform(Uniform{16, ValueType::U32}, remaining);
        const auto more = function->TestNotZero(remaining);
        auto [exit, backedge] = builder.If(terminal::If{BOOL{more}, terminal::LinkBlock{Location{0x9951}}, terminal::LinkBlock{Location{0x9952}}});
        builder.SetCurBlock(exit);
        function->StoreUniform(Uniform{8, ValueType::U64}, old);
        function->EndBlock(terminal::ReturnToHost{}); function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        if (external_body) function->RegisterExternalEntryRoot(body);
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
        REQUIRE(allocation.PreservedGPRDefinitionAt(old.Id()) != nullptr);
        if (external_body) {
            REQUIRE_THROWS_AS(
                    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation),
                    std::logic_error);
            continue;
        }
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context}; translator.Translate(function); context.Finish();
        REQUIRE(allocation.Actions().Ready());
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
        const auto initial = function->LoadImm(ValueType::U64, Imm{swift::u64{0x8877665544332211}});
        function->SetHostGPR(initial, HostRegIndex(20), Imm{0u});
        const auto old = function->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
        const auto condition = function->LoadUniform(ValueType::U8, Uniform{16, ValueType::U8});
        auto [right, left] = builder.If(terminal::If{BOOL{condition},
                                                     terminal::LinkBlock{Location{0x9961}},
                                                     terminal::LinkBlock{Location{0x9962}}});
        builder.SetCurBlock(left);
        const auto left_value = function->LoadImm(ValueType::U64, Imm{swift::u64{42}});
        function->SetHostGPR(left_value, HostRegIndex(20), Imm{0u});
        auto* join = builder.LinkBlock(terminal::LinkBlock{Location{0x9963}});
        builder.SetCurBlock(right);
        const auto right_value = function->LoadImm(ValueType::U64, Imm{swift::u64{99}});
        function->SetHostGPR(right_value, HostRegIndex(20), Imm{0u});
        builder.LinkBlock(terminal::LinkBlock{Location{0x9963}});
        builder.SetCurBlock(join);
        function->StoreUniform(Uniform{0, ValueType::U64}, old);
        const auto current = function->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
        function->StoreUniform(Uniform{8, ValueType::U64}, current);
        function->EndBlock(terminal::ReturnToHost{}); function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
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
        arm64::JitContext context{module, allocation, allocation.Actions()};
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

TEST_CASE("prepared GPR emission facts are deterministic across identical blocks", "[pin-plan]") {
    Config config{.loc_start = 0,
                  .loc_end = 1ull << 48,
                  .enable_jit = true,
                  .has_local_operation = false,
                  .backend_isa = kArm64};
    AddressSpace space{config};
    const FeatureSet features{};
    struct Sealed {
        IntrusivePtr<Block> block;
        Value definition;
        const Inst *byte_store{}, *wide_store{};
        std::unique_ptr<AllocationFixture> allocation;
    };
    const auto seal = [&](bool wide_first) {
        Sealed out;
        out.block = IntrusivePtr<Block>{new Block(0, Location{0x99a0})};
        out.definition = out.block->LoadImm(ValueType::U64, Imm{swift::u64{0x8877665544332211}});
        const auto byte = Value{out.definition}.SetCastType(ValueType::U8);
        const auto store = [&](bool wide) {
            out.block->StoreUniform(Uniform{wide ? 8u : 0u, wide ? ValueType::U64 : ValueType::U8},
                                    wide ? out.definition : byte);
            (wide ? out.wide_store : out.byte_store) = &out.block->GetInstList().back();
        };
        store(wide_first);
        store(!wide_first);
        out.block->SetTerminal(terminal::ReturnToDispatch{});
        out.block->ReIdInstr();
        out.allocation = std::make_unique<AllocationFixture>(out.block->MaxInstrId(),
                                                             space.GetTrampolines().GetGPRRegs(),
                                                             space.GetTrampolines().GetFPRRegs(),
                                                             features);
        compile::AllocateBlock(
                out.block.get(),
                out.allocation.get(),
                arm64::PrepareAllocationInputs(out.block.get(), *out.allocation, features),
                false);
        arm64::RegisterStatePreparation guest;
        guest.BeginBlock(out.block.get(), features, out.allocation->PrepareFacts(out.block.get()));
        arm64::ResidencePreparation builder{*out.allocation, features, guest};
        out.allocation->BeginTargetPlanning();
        builder.Prepare(out.block.get());
        builder.AssignValueHome(out.definition.Def(), 22);
        RegisterAllocTestSupport::FinishGPRForTest(builder, *out.allocation, features);
        REQUIRE(out.allocation->Actions().MatchesAllocation(*out.allocation));
        return out;
    };
    const auto first = seal(false), second = seal(false);
    REQUIRE(first.allocation->Actions().UnitScratch() ==
            second.allocation->Actions().UnitScratch());
    auto a = first.block->GetInstList().begin(), b = second.block->GetInstList().begin();
    for (; a != first.block->GetInstList().end(); ++a, ++b) {
        const auto* left = first.allocation->Actions().ScratchFor(&*a);
        const auto* right = second.allocation->Actions().ScratchFor(&*b);
        REQUIRE(left);
        REQUIRE(right);
        REQUIRE(*left == *right);
    }
    REQUIRE(b == second.block->GetInstList().end());
    for (const auto* member : {&first, &second}) {
        const auto byte = member->byte_store->GetArg<Value>(1);
        const auto wide = member->wide_store->GetArg<Value>(1);
        REQUIRE(byte.Type() == ValueType::U8);
        REQUIRE(wide.Type() == ValueType::U64);
        REQUIRE(member->allocation->GPRLocationAt(byte, member->byte_store->Id()).bits == 32);
        REQUIRE(member->allocation->GPRLocationAt(wide, member->wide_store->Id()).bits == 64);
    }
    const auto swapped = seal(true);
    REQUIRE(swapped.wide_store->GetArg<Value>(1).Type() == ValueType::U64);
    REQUIRE(swapped.byte_store->GetArg<Value>(1).Type() == ValueType::U8);
}

TEST_CASE("prepared GPR facts reject foreign consumers and pin scratch preparation inputs",
          "[pin-plan]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48,
                  .enable_jit = true, .has_local_operation = false,
                  .backend_isa = kArm64};
    AddressSpace space{config};
    const FeatureSet features{};
    IntrusivePtr<Block> block{new Block(0, Location{0x99b0})};
    auto definition = block->LoadImm(ValueType::U64, Imm{swift::u64{0x8877665544332211}});
    block->StoreUniform(Uniform{0, ValueType::U64}, definition);
    auto* store = &block->GetInstList().back();
    auto sum = block->Add(ValueType::U64, definition, Operand{Imm{swift::u64{1}}});
    block->StoreUniform(Uniform{8, ValueType::U64}, sum);
    auto* second_store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
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
    arm64::RegisterStatePreparation guest;
    guest.BeginBlock(block.get(), features, allocation.PrepareFacts(block.get()));
    arm64::ResidencePreparation builder{allocation, features, guest};
    allocation.BeginTargetPlanning();
    builder.Prepare(block.get());
    builder.AssignValueHome(definition.Def(), 22);
    RegisterAllocTestSupport::FinishGPRForTest(builder, allocation, features);
    const auto& actions = allocation.Actions();
    const auto scratch = actions.ScratchForBlock(block.get());
    // Standalone facts have scratch data, but cannot authorize target input
    // matching without the complete unit's region/admission certificate.
    REQUIRE(actions.HasPreparedScratch(block.get()));
    REQUIRE_FALSE(actions.MatchesScratchInputs(block.get(), allocation.GetGprs(), features));
    IntrusivePtr<Block> foreign{new Block(0, Location{0x99b8})};
    auto foreign_value = foreign->LoadImm(ValueType::U64, Imm{swift::u64{3}});
    foreign->StoreUniform(Uniform{0, ValueType::U64}, foreign_value);
    auto* foreign_store = &foreign->GetInstList().back();
    foreign->SetTerminal(terminal::ReturnToDispatch{});
    foreign->ReIdInstr();
    REQUIRE(actions.ScratchFor(foreign_store) == nullptr);
    REQUIRE(actions.ScratchFor(nullptr) == nullptr);
    REQUIRE(actions.SpillInputFor(foreign_store, foreign_value.Id()).definition == nullptr);
    REQUIRE(actions.Residence(foreign_value.Def()) == nullptr);
    std::vector<const Inst*> consumers;
    for (auto& inst : block->GetInstList()) consumers.push_back(&inst);
    size_t cursor = 0;
    for (const auto* consumer : consumers) {
        const auto* via_cursor = scratch.For(consumer, cursor);
        const auto* direct = actions.ScratchFor(consumer);
        REQUIRE((via_cursor == nullptr) == (direct == nullptr));
        if (via_cursor) {
            REQUIRE(via_cursor->fixed_clobbers == direct->fixed_clobbers);
            REQUIRE(via_cursor->leases == direct->leases);
        }
    }
    size_t skip_cursor = 0;
    REQUIRE(scratch.For(second_store, skip_cursor) == actions.ScratchFor(second_store));
    (void)store;
    const auto pool = space.GetTrampolines().GetGPRRegs();
    const auto base_capture = arm64::CaptureScratchPreparationInputs(pool, features);
    REQUIRE(base_capture == arm64::CaptureScratchPreparationInputs(pool, features));
    for (unsigned flip = 0; flip < 4; ++flip) {
        auto drifted = features;
        auto drifted_pool = pool;
        switch (flip) {
            case 0: drifted.jit_scratch_xpool = !drifted.jit_scratch_xpool; break;
            case 1: drifted.scratch_precise = !drifted.scratch_precise; break;
            case 2: drifted.fpr_scratch_precise = !drifted.fpr_scratch_precise; break;
            default: drifted_pool = GPRSMask{~((1u << 6) - 1u)}; break;
        }
        REQUIRE_FALSE(arm64::CaptureScratchPreparationInputs(drifted_pool, drifted) == base_capture);
        REQUIRE_FALSE(actions.MatchesScratchInputs(block.get(), drifted_pool, drifted));
    }
    REQUIRE(actions.UnitScratch() == arm64::PrepareUnitScratch(allocation.GetGprs(), features));
    const auto manual = arm64::PrepareSpillInput(store, definition.Id());
    REQUIRE(manual.definition == definition.Def());
    REQUIRE_FALSE(manual.last_use);
    const auto last = arm64::PrepareSpillInput(second_store, sum.Id());
    REQUIRE(last.definition == sum.Def());
    REQUIRE(last.last_use);
}
