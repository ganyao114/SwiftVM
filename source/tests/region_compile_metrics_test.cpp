#include "tests/support/rewrite_fixture.h"
using swift::runtime::test::RewriteFixture;
#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/context.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/svm_config.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/ir/hir_builder.h"
#include "translator/x86/cpu.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
struct Metrics {
    swift::u64 ns{}, hash{1469598103934665603ull}, spills{}, reloads{}, stores{};
    size_t bytes{};
};
Metrics Compile(unsigned shape, unsigned length) {
    constexpr swift::u64 start = 0xd000, hot_start = 0xe000;
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.region_edges = true;
    config.uniform_buffer_size = 64;
    AddressSpace space{config};
    ModuleConfig module_config{};
    module_config.feature_overrides.Set(FeatureId::flags_region_branch, true);
    auto module = space.MapModule(LocationDescriptor{start},
                                  LocationDescriptor{hot_start + length + 1}, module_config);
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{start}, Location{hot_start + length + 1});
    HIRBlock* hot = nullptr;
    for (unsigned source = 0; source < length; ++source) {
        auto condition = function->LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
        if (shape == 0) {
            const auto one = function->LoadImm(ValueType::U8, Imm{swift::u8{1}});
            const auto result = function->Sub(ValueType::U8, condition, Operand{one});
            function->SaveFlags(result, Flags::NZCV);
            function->AdvancePC(Imm{swift::u64{1}});
            condition = function->LocalCondSet(ValueType::U8, Cond::EQ);
        }
        if (shape == 2) {
            auto* next = builder.LinkBlock(terminal::LinkBlock{Location{start + source + 1}});
            builder.SetCurBlock(next);
        } else {
            auto [next, target] = builder.If(terminal::If{condition,
                terminal::LinkBlock{Location{hot_start}},
                terminal::LinkBlock{Location{start + source + 1}}});
            hot = target;
            builder.SetCurBlock(next);
        }
    }
    function->EndBlock(terminal::ReturnToHost{});
    if (hot) {
        builder.SetCurBlock(hot);
        for (unsigned node = 0; node < length; ++node) {
            for (unsigned instruction = 0; instruction < 16; ++instruction)
                function->AdvancePC(Imm{swift::u64{1}});
            if (node + 1 != length) {
                auto* next = builder.LinkBlock(terminal::LinkBlock{Location{hot_start + node + 1}});
                builder.SetCurBlock(next);
            }
        }
        function->ClearFlags(Flags::NZCV);
        function->AdvancePC(Imm{swift::u64{1}});
        function->EndBlock(terminal::ReturnToHost{});
    }
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    AllocationFixture allocation{function->MaxInstrCount(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    const auto begin = std::chrono::steady_clock::now();
    ::swift::runtime::compile::AllocateFunction(
            function,
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, allocation, features));
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    arm64::JitContext context{module, allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
    REQUIRE(translator.Translate(function));
    context.Finish();
    Metrics metrics;
    metrics.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    metrics.bytes = context.CurrentBufferSize();
    metrics.spills = allocation.SpillCount();
    metrics.reloads = context.GetRAShapeCounters().spill_loads;
    metrics.stores = context.GetRAShapeCounters().spill_stores;
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < metrics.bytes; ++i) metrics.hash = (metrics.hash ^ bytes[i]) * 1099511628211ull;
    return metrics;
}
}

TEST_CASE("region isolated compilation metrics", "[.region-compile-metrics]") {
    for (unsigned shape = 0; shape < 3; ++shape) for (unsigned length : {4u, 16u, 64u}) {
        Metrics sample;
        std::vector<swift::u64> times;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = Compile(shape, length);
            REQUIRE(result.bytes > 0);
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("region-compile-metrics shape=%u length=%u bytes=%zu hash=%llu spills=%llu reloads=%llu stores=%llu median_ns=%llu\n",
                shape, length, sample.bytes, (unsigned long long)sample.hash,
                (unsigned long long)sample.spills, (unsigned long long)sample.reloads,
                (unsigned long long)sample.stores,
                (unsigned long long)times[times.size() / 2]);
        std::fflush(stdout);
    }
}

namespace {
struct BranchPhases { swift::u64 allocation{}, mapping{}, planning{}, emission{}, finish{}; };
Metrics CompileIntegerBranch(unsigned shape, unsigned length, BranchPhases* phases = nullptr) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0xf000})};
    const auto features = ResolveFeatureSet(space.GetDefaultModule()->GetModuleConfig());
    const auto type = shape == 1 ? ValueType::U8 : shape == 2 ? ValueType::U16 : ValueType::U64;
    const auto base = block->GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
    const auto source =
            shape == 0
                    ? block->LoadMemory(type, Operand{base})
                    : block->BitExtract(type, base, Imm{0u}, Imm{ir::GetValueSizeByte(type) * 8});
    const Inst* publication = nullptr;
    if (shape == 0)
        publication = block->AppendInst(OpCode::SetHostGPR, source, HostRegIndex(22), Imm{0u});
    for (unsigned i = 0; i < length; ++i) {
        auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{i + 17}});
        block->StoreUniform(Uniform{0, ValueType::U64}, value);
    }
    block->AdvancePC(Imm{1u});
    if (shape == 0) {
        const auto result = block->And(type, source, Operand{source});
        block->ClearFlags(Flags::Carry | Flags::Overflow | Flags::AuxiliaryCarry);
        block->SaveFlags(result, Flags::Negate | Flags::Zero | Flags::Parity);
    } else {
        const auto immediate = block->LoadImm(type, Imm{3u});
        const auto result = block->Sub(type, source, Operand{immediate});
        block->SaveFlags(result, Flags::All);
        block->InvertCarry();
    }
    block->AdvancePC(Imm{1u});
    // Exercise the post-producer proof window as well as the long prefix.
    for (unsigned i = 0; i < length; ++i) {
        auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{i + 31}});
        block->StoreUniform(Uniform{8, ValueType::U64}, value);
    }
    block->BranchOnlyEdges();
    const auto condition = block->LocalCondSet(ValueType::U8, Cond::EQ);
    block->SetTerminal(terminal::If{condition,
                                    terminal::LinkBlock{Location{0xf010}},
                                    terminal::LinkBlock{Location{0xf020}}});
    swift::runtime::compile::EliminateFlags(RewriteFixture{block.get(), true, nullptr}.Get(), features);
    REQUIRE(block->HasDeadEdgeIntegerBranchProof());
    block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    const auto begin = std::chrono::steady_clock::now();
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    const auto allocation_end = phases ? std::chrono::steady_clock::now() : begin;
    if (shape == 0) {
        allocation.MapRegister(source.Id(), HostGPR{22});
        allocation.MarkHostWriteCoalesced(publication->Id());
    }
    const auto mapping_end = phases ? std::chrono::steady_clock::now() : begin;
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(space.GetDefaultModule(), allocation);
    arm64::JitContext context{space.GetDefaultModule(), allocation, allocation.Actions()};
    const auto planning_end = phases ? std::chrono::steady_clock::now() : begin;
    arm64::JitTranslator translator{context};
    REQUIRE(translator.Translate(block.get()));
    const auto emission_end = phases ? std::chrono::steady_clock::now() : begin;
    context.Finish();
    const auto finish_end = phases ? std::chrono::steady_clock::now() : begin;
    Metrics metrics;
    metrics.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            (phases ? finish_end : std::chrono::steady_clock::now()) - begin).count();
    if (phases) {
        const auto ns = [](auto d) { return static_cast<swift::u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()); };
        *phases = {ns(allocation_end - begin), ns(mapping_end - allocation_end),
                   ns(planning_end - mapping_end), ns(emission_end - planning_end), ns(finish_end - emission_end)};
    }
    metrics.bytes = context.CurrentBufferSize(); metrics.spills = allocation.SpillCount();
    metrics.reloads = context.GetRAShapeCounters().spill_loads; metrics.stores = context.GetRAShapeCounters().spill_stores;
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < metrics.bytes; ++i) metrics.hash = (metrics.hash ^ bytes[i]) * 1099511628211ull;
    return metrics;
}
}
TEST_CASE("integer branch isolated compilation metrics", "[.integer-branch-metrics]") {
    for (unsigned shape = 0; shape < 4; ++shape) for (unsigned length : {4u, 32u, 128u}) {
        Metrics sample; std::vector<swift::u64> times;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = CompileIntegerBranch(shape, length);
            REQUIRE(result.bytes > 0);
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("integer-branch-metrics shape=%u length=%u bytes=%zu hash=%llu spills=%llu reloads=%llu stores=%llu median_ns=%llu\n",
                shape, length, sample.bytes, (unsigned long long)sample.hash,
                (unsigned long long)sample.spills, (unsigned long long)sample.reloads,
                (unsigned long long)sample.stores, (unsigned long long)times[times.size() / 2]);
        std::fflush(stdout);
    }
}

TEST_CASE("integer branch phase diagnostics", "[.integer-branch-phases]") {
    for (unsigned shape = 0; shape < 4; ++shape) {
        std::array<std::vector<swift::u64>, 5> times;
        for (unsigned i = 0; i < 45; ++i) {
            BranchPhases phases; const auto result = CompileIntegerBranch(shape, 128, &phases);
            REQUIRE(result.bytes > 0);
            const std::array values{phases.allocation, phases.mapping, phases.planning, phases.emission, phases.finish};
            if (i >= 5) for (unsigned j = 0; j < 5; ++j) times[j].push_back(values[j]);
        }
        for (auto& phase : times) std::sort(phase.begin(), phase.end());
        std::printf("integer-branch-phases shape=%u length=128 allocation_ns=%llu mapping_ns=%llu planning_ns=%llu emission_ns=%llu finish_ns=%llu\n",
                shape, (unsigned long long)times[0][20], (unsigned long long)times[1][20],
                (unsigned long long)times[2][20], (unsigned long long)times[3][20], (unsigned long long)times[4][20]);
        std::fflush(stdout);
    }
}

namespace {
Metrics CompileNarrowInteger(unsigned shape, unsigned length) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config};
    const auto features = ResolveFeatureSet(space.GetDefaultModule()->GetModuleConfig());
    IntrusivePtr<Block> block{new Block(0, Location{0xfb00})};
    for (unsigned i = 0; i < length; ++i) {
        const auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{i + 31}});
        block->StoreUniform(Uniform{24, ValueType::U64}, value);
    }
    if (shape == 2) {
        const auto base = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
        const auto producer = block->Add(ValueType::U32, base, Operand{Imm{1u}});
        block->SaveFlags(producer, Flags::Carry);
        const auto zero = block->LoadImm(ValueType::U32, Imm{0u});
        const Value test{block->TestFlags(ValueType::U32, Flags::Carry).Def()};
        const auto carry = block->Add(ValueType::U32, zero, Operand{test});
        const auto result = block->Add(ValueType::U32, base, Operand{carry});
        block->StoreUniform(Uniform{8, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
    } else {
        const auto type = shape == 1 ? ValueType::U16 : ValueType::U8;
        const auto source_type = shape == 3 ? ValueType::U32 : type;
        const auto source = block->LoadUniform(source_type, Uniform{0, source_type});
        const auto immediate = block->LoadImm(ValueType::U32, Imm{7u});
        const auto sub = block->Sub(type, source, Operand{immediate});
        block->SaveFlags(sub, Flags::Carry);
    }
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    const auto begin = std::chrono::steady_clock::now();
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(space.GetDefaultModule(), allocation);
    arm64::JitContext context{space.GetDefaultModule(), allocation, allocation.Actions()};
    arm64::JitTranslator translator{context}; REQUIRE(translator.Translate(block.get())); context.Finish();
    Metrics metrics;
    metrics.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    metrics.bytes = context.CurrentBufferSize(); metrics.spills = allocation.SpillCount();
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < metrics.bytes; ++i) metrics.hash = (metrics.hash ^ bytes[i]) * 1099511628211ull;
    return metrics;
}
}
TEST_CASE("narrow integer isolated compilation metrics", "[.narrow-integer-metrics]") {
    for (unsigned shape = 0; shape < 4; ++shape) for (unsigned length : {4u, 32u, 128u}) {
        Metrics sample; std::vector<swift::u64> times;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = CompileNarrowInteger(shape, length);
            REQUIRE(result.bytes > 0);
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("narrow-integer-metrics shape=%u length=%u bytes=%zu hash=%llu spills=%llu median_ns=%llu\n",
                shape, length, sample.bytes, (unsigned long long)sample.hash,
                (unsigned long long)sample.spills, (unsigned long long)times[times.size()/2]);
        std::fflush(stdout);
    }
}

namespace {
Metrics CompileScalarPreparation(unsigned shape, unsigned length) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 64; config.stack_alignment = 16;
    config.arm64_features = Arm64Features::FlagM;
    AddressSpace space{config};
    const auto features = ResolveFeatureSet(space.GetDefaultModule()->GetModuleConfig());
    IntrusivePtr<Block> block{new Block(0, Location{0xbf80})};
    const auto address = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    for (unsigned i = 0; i < length; ++i) {
        if (shape == 0) {
            const auto input = block->LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
            const auto left = block->BitExtract(ValueType::U32, input, Imm{0u}, Imm{32u});
            const auto right = block->BitExtract(ValueType::U32, input, Imm{0u}, Imm{32u});
            const auto result = block->Xor(ValueType::U32, left, Operand{right});
            block->SaveFlags(result, Flags::Zero | Flags::Negate | Flags::Parity);
            block->StoreUniform(Uniform{16, ValueType::U32}, result);
        } else if (shape == 1) {
            const auto input = block->LoadUniform(ValueType::U32, Uniform{8, ValueType::U32});
            const auto comparison = block->Sub(ValueType::U32, input, Operand{Imm{7u}});
            block->SaveFlags(comparison, Flags::Carry | Flags::Zero);
            block->InvertCarry();
            block->AdvancePC(Imm{1u});
            const auto carry = block->TestFlags(ValueType::U8, Flags::Carry);
            const auto no_carry = block->TestZero(ValueType::U8, carry);
            const auto nonzero = block->CondSet(ValueType::U8, Cond::NE);
            const auto condition = block->And(ValueType::U8, no_carry, Operand{nonzero});
            const auto alternative =
                    block->LoadUniform(ValueType::U32, Uniform{24, ValueType::U32});
            const auto selected =
                    block->Select(ValueType::U32, BOOL{condition}, input, alternative);
            block->StoreUniform(Uniform{16, ValueType::U32}, selected);
        } else if (shape == 2) {
            const auto value = block->GetHostFPR(ValueType::U64, HostRegIndex(17), Imm{0u});
            block->StoreMemory(Operand{address, Imm{swift::u64{i * 8}}}, value);
        } else {
            const auto old = block->GetHostFPR(ValueType::V128, HostRegIndex(17), Imm{0u});
            const auto zero = block->VecXor(ValueType::V128, old, old);
            block->SetHostFPR(zero, HostRegIndex(17), Imm{0u});
            const auto input = block->LoadUniform(ValueType::U32, Uniform{8, ValueType::U32});
            const auto converted =
                    block->VecFCvtIntToFloat(ValueType::U64, input, Imm{32u}, Imm{64u});
            block->SetHostFPR(converted, HostRegIndex(17), Imm{0u});
        }
    }
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    const auto begin = std::chrono::steady_clock::now();
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(space.GetDefaultModule(), allocation);
    arm64::JitContext context{space.GetDefaultModule(), allocation, allocation.Actions()};
    arm64::JitTranslator translator{context}; REQUIRE(translator.Translate(block.get())); context.Finish();
    Metrics result;
    result.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    result.bytes = context.CurrentBufferSize(); result.spills = allocation.SpillCount();
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < result.bytes; ++i) result.hash = (result.hash ^ bytes[i]) * 1099511628211ull;
    return result;
}
}

TEST_CASE("scalar preparation isolated compilation metrics", "[.scalar-preparation-metrics]") {
    for (unsigned shape = 0; shape < 4; ++shape) for (unsigned length : {4u, 32u, 128u}) {
        Metrics sample; std::vector<swift::u64> times;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = CompileScalarPreparation(shape, length);
            REQUIRE(result.bytes > 0);
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("scalar-preparation-metrics shape=%u length=%u bytes=%zu hash=%llu spills=%llu median_ns=%llu\n",
                shape, length, sample.bytes, (unsigned long long)sample.hash,
                (unsigned long long)sample.spills, (unsigned long long)times[times.size()/2]);
        std::fflush(stdout);
    }
}

namespace {
Metrics CompileBitFunnel(unsigned shape, unsigned length) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 64; config.stack_alignment = 16;
    AddressSpace space{config};
    const auto features = ResolveFeatureSet(space.GetDefaultModule()->GetModuleConfig());
    IntrusivePtr<Block> block{new Block(0, Location{0xbf90})};
    for (unsigned i = 0; i < length; ++i) {
        if (shape < 3) {
            const auto type = shape == 2 ? ValueType::U8 : ValueType::U32;
            const auto input = block->LoadUniform(type, Uniform{0, type});
            const auto extract = block->BitExtract(ValueType::U8, input, Imm{0u}, Imm{8u});
            Value result;
            if (shape == 2)
                result = block->And(ValueType::U8, extract, Operand{Imm{15u}});
            else {
                result = block->ZeroExtend32(ValueType::U8, extract);
                if (shape == 1) result = block->LsrImm(ValueType::U32, result, Imm{1u});
            }
            block->StoreUniform(Uniform{16, result.Type()}, result);
        } else if (shape == 3) {
            const auto input = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
            const auto shifted = block->LsrImm(ValueType::U32, input, Imm{8u});
            const auto result = block->And(ValueType::U32, shifted, Operand{Imm{1u}});
            block->StoreUniform(Uniform{16, ValueType::U32}, result);
        } else {
            const auto low = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
            const auto high = block->LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
            const auto right = block->LsrImm(ValueType::U64, low, Imm{32u});
            const auto left = block->LslImm(ValueType::U64, high, Imm{32u});
            const auto result = block->Or(ValueType::U64, right, Operand{left});
            if (shape == 5) block->StoreUniform(Uniform{24, ValueType::U64}, right);
            block->StoreUniform(Uniform{16, ValueType::U64}, result);
        }
    }
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    const auto begin = std::chrono::steady_clock::now();
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(space.GetDefaultModule(), allocation);
    arm64::JitContext context{space.GetDefaultModule(), allocation, allocation.Actions()};
    arm64::JitTranslator translator{context}; REQUIRE(translator.Translate(block.get())); context.Finish();
    Metrics result;
    result.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    result.bytes = context.CurrentBufferSize(); result.spills = allocation.SpillCount();
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < result.bytes; ++i) result.hash = (result.hash ^ bytes[i]) * 1099511628211ull;
    return result;
}
}

TEST_CASE("bit and funnel preparation isolated compilation metrics", "[.bit-funnel-metrics]") {
    for (unsigned shape = 0; shape < 6; ++shape) for (unsigned length : {4u, 32u, 128u}) {
        Metrics sample; std::vector<swift::u64> times;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = CompileBitFunnel(shape, length);
            REQUIRE(result.bytes > 0);
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("bit-funnel-metrics shape=%u length=%u bytes=%zu hash=%llu spills=%llu median_ns=%llu\n",
                shape, length, sample.bytes, (unsigned long long)sample.hash,
                (unsigned long long)sample.spills, (unsigned long long)times[times.size()/2]);
        std::fflush(stdout);
    }
}

namespace {
Metrics CompileBackedge(unsigned shape, unsigned length, bool native_probe = false) {
    constexpr swift::u64 start = 0xfa00, hot_start = 0xfb00, cold_start = 0xfc00;
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.region_edges = true;
    config.uniform_buffer_size = sizeof(swift::x86::ThreadContext64) + 64;
    AddressSpace space{config}; ModuleConfig module_config{};
    module_config.feature_overrides.Set(FeatureId::flags_region_branch, true);
    auto module = space.MapModule(LocationDescriptor{start}, LocationDescriptor{cold_start + 16}, module_config);
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{3, true, features};
    auto* function = builder.AppendFunction(Location{start}, Location{cold_start + 16});
    auto* source = &function->EditBlock(function->GetCurrentBlock());
    auto* hot = function->AppendBlock(Location{hot_start}, Location{hot_start + 16});
    auto* cold = function->AppendBlock(Location{cold_start}, Location{cold_start + 16});
    function->AddEdge(source, hot); function->AddEdge(source, cold);
    builder.SetCurBlock(source);
    for (unsigned i = 0; i < length; ++i) function->AdvancePC(Imm{swift::u64{1}});
    const auto left = function->LoadUniform(ValueType::U16, Uniform{0, ValueType::U16});
    const auto sub = function->Sub(ValueType::U16, left, Operand{Imm{swift::u64{1}}});
    function->SaveFlags(sub, Flags::All);
    Value condition;
    if (shape == 2) condition = function->LocalCondSet(ValueType::U8, Cond::EQ);
    if (shape == 1) {
        const auto zero = function->LoadImm(ValueType::U8, Imm{swift::u8{0}});
        function->StoreUniform(Uniform{0, ValueType::U8}, zero);
    } else if (shape == 2) {
        // Compile-only opaque helper address: deterministic across all control ELFs.
        (void)function->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{0x1234}}});
    } else if (shape == 3) {
        const auto address = function->LoadUniform(ValueType::U64, Uniform{16, ValueType::U64});
        const auto loaded = function->LoadMemory(ValueType::U32, Operand{address});
        function->StoreUniform(Uniform{32, ValueType::U32}, loaded);
    }
    if (GetSvmConfig().flags_cfinv && True(config.arm64_features & Arm64Features::FlagM)) {
        function->InvertCarry();
    } else {
        const auto polarity = function->LoadImm(ValueType::U8, Imm{swift::u8{0}});
        function->StoreUniform(
                Uniform{offsetof(swift::x86::ThreadContext64, carry_inverted), ValueType::U8},
                polarity);
    }
    function->AdvancePC(Imm{swift::u64{1}});
    if (shape != 2) condition = function->LocalCondSet(ValueType::U8, Cond::EQ);
    function->EndBlock(terminal::If{condition,
                                    terminal::LinkBlock{Location{hot_start}},
                                    terminal::LinkBlock{Location{cold_start}}});
    builder.SetCurBlock(hot);
    const auto two = function->LoadImm(ValueType::U16, Imm{swift::u16{2}});
    const auto next = function->Sub(ValueType::U16, two, Operand{Imm{swift::u64{1}}});
    function->SaveFlags(next, Flags::All);
    function->AdvancePC(Imm{swift::u64{1}});
    function->EndBlock(terminal::ReturnToHost{});
    builder.SetCurBlock(cold);
    // BOOL returns an U8 use view; these state slots consume the declared U32 result.
    const Value pf{function->TestFlags(ValueType::U32, Flags::Parity).Def()};
    const Value af{function->TestFlags(ValueType::U32, Flags::AuxiliaryCarry).Def()};
    function->StoreUniform(Uniform{24, ValueType::U32}, pf);
    function->StoreUniform(Uniform{28, ValueType::U32}, af);
    function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
    function->ComputeRPO(); function->IdByRPO();
    AllocationFixture allocation{function->MaxInstrCount(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    const auto begin = std::chrono::steady_clock::now();
    ::swift::runtime::compile::AllocateFunction(
            function,
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, allocation, features),
            {.block_intervals = false});
    arm64::CompleteARM64Allocation(module, allocation);
    arm64::JitContext context{module, allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
    REQUIRE(translator.Translate(function)); context.Finish();
    Metrics metrics;
    metrics.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    // Prove this family actually selects its intended actions, outside the clock.
    REQUIRE(allocation.Actions().ForBlock(source->GetBlock()));
    const auto* backedge = allocation.Actions().BackedgeFor(source->GetBlock());
    if (shape < 2) {
        REQUIRE(backedge);
        REQUIRE(backedge->dead_successor);
        REQUIRE((allocation.Actions().PFAFFor(source->GetBlock()) != nullptr) == (shape == 0));
    } else
        REQUIRE_FALSE(backedge);
    metrics.bytes = context.CurrentBufferSize(); metrics.spills = allocation.SpillCount();
    metrics.reloads = context.GetRAShapeCounters().spill_loads; metrics.stores = context.GetRAShapeCounters().spill_stores;
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < metrics.bytes; ++i) metrics.hash = (metrics.hash ^ bytes[i]) * 1099511628211ull;
#if defined(__aarch64__)
    if (native_probe) {
        REQUIRE(shape < 2);
        const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer);
        space.PushCodeCache(Location{start}, buffer.exec_data + context.GetCodeOffset(start));
        for (const swift::u16 initial : {swift::u16{0}, swift::u16{2}, swift::u16{128}}) {
            CAPTURE(shape, length, initial);
            Runtime runtime{&space};
            std::memcpy(runtime.GetUniformBuffer().data(), &initial, sizeof(initial));
            runtime.SetLocation(start); REQUIRE(runtime.Run() == HaltReason::CallHost);
            swift::u32 pf{}, af{};
            std::memcpy(&pf, runtime.GetUniformBuffer().data() + 24, sizeof(pf));
            std::memcpy(&af, runtime.GetUniformBuffer().data() + 28, sizeof(af));
            REQUIRE(pf == (initial == 0 ? 1u : 0u));
            REQUIRE(af == (initial == 2 ? 0u : 1u));
            REQUIRE((runtime.GetState()->host_cpu_flags & 0xff) == static_cast<swift::u8>(initial - 1));
        }
    }
#else
    (void)native_probe;
#endif
    return metrics;
}
}
TEST_CASE("backedge isolated whole compilation metrics", "[.backedge-compile-metrics]") {
    for (unsigned shape = 0; shape < 4; ++shape) for (unsigned length : {4u, 16u, 64u}) {
        Metrics sample; std::vector<swift::u64> times;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = CompileBackedge(shape, length);
            REQUIRE(result.bytes > 0); if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns); sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("backedge-compile-metrics shape=%u length=%u bytes=%zu hash=%llu spills=%llu reloads=%llu stores=%llu median_ns=%llu\n",
                    shape, length, sample.bytes, (unsigned long long)sample.hash, (unsigned long long)sample.spills,
                    (unsigned long long)sample.reloads, (unsigned long long)sample.stores,
                    (unsigned long long)times[times.size() / 2]);
        std::fflush(stdout);
    }
}

TEST_CASE("backedge metrics code executes selected cold flag paths", "[.backedge-native-smoke]") {
    for (unsigned shape : {0u, 1u}) for (unsigned length : {4u, 64u}) {
        const auto result = CompileBackedge(shape, length, true);
        REQUIRE(result.bytes > 0);
    }
}
