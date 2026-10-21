#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "runtime/ir/opts/flags_elimination_pass.h"
#include "runtime/common/svm_config.h"
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
    FeatureSet features{};
    features.flags_region_branch = true;
    AddressSpace space{config};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{start}, Location{hot_start + length + 1});
    HIRBlock* hot = nullptr;
    for (unsigned source = 0; source < length; ++source) {
        auto condition = function->LoadUniform(Uniform{0, ValueType::U8}).SetType(ValueType::U8);
        if (shape == 0) {
            const auto one = function->LoadImm(Imm{swift::u8{1}}).SetType(ValueType::U8);
            const auto result = function->Sub(condition, Operand{one}).SetType(ValueType::U8);
            function->SaveFlags(result, Flags::NZCV);
            function->AdvancePC(Imm{swift::u64{1}});
            condition = function->LocalCondSet(Cond::EQ).SetType(ValueType::U8);
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
    RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    const auto begin = std::chrono::steady_clock::now();
    RegisterAllocPass::Run(function, &allocation, features);
    allocation.CompleteGPRAllocation(space.GetDefaultModule());
    arm64::JitContext context{space.GetDefaultModule(), allocation};
    arm64::JitTranslator translator{context};
    translator.Translate(function);
    context.Finish();
    Metrics metrics;
    metrics.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    metrics.bytes = context.CurrentBufferSize();
    metrics.spills = allocation.SpillCount();
    metrics.reloads = allocation.RAShape().spill_loads;
    metrics.stores = allocation.RAShape().spill_stores;
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
    FeatureSet features{};
    const auto type = shape == 1 ? ValueType::U8
                    : shape == 2 ? ValueType::U16 : ValueType::U64;
    const auto base = block->GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(ValueType::U64);
    const auto source = shape == 0 ? block->LoadMemory(Operand{base}).SetType(type)
                                   : block->BitExtract(base, Imm{0u}, Imm{ir::GetValueSizeByte(type) * 8}).SetType(type);
    Inst* publication = nullptr;
    if (shape == 0) publication = block->AppendInst(
            OpCode::SetHostGPR, source, HostRegIndex(22), Imm{0u});
    for (unsigned i = 0; i < length; ++i) {
        auto value = block->LoadImm(Imm{swift::u64{i + 17}}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{0, ValueType::U64}, value);
    }
    block->AdvancePC(Imm{1u});
    if (shape == 0) {
        const auto result = block->And(source, Operand{source}).SetType(type);
        block->ClearFlags(Flags::Carry | Flags::Overflow | Flags::AuxiliaryCarry);
        block->SaveFlags(result, Flags::Negate | Flags::Zero | Flags::Parity);
    } else {
        const auto immediate = block->LoadImm(Imm{3u}).SetType(type);
        const auto result = block->Sub(source, Operand{immediate}).SetType(type);
        block->SaveFlags(result, Flags::All);
        block->InvertCarry();
    }
    block->AdvancePC(Imm{1u});
    // Exercise the post-producer proof window as well as the long prefix.
    for (unsigned i = 0; i < length; ++i) {
        auto value = block->LoadImm(Imm{swift::u64{i + 31}}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{8, ValueType::U64}, value);
    }
    block->BranchOnlyEdges();
    const auto condition = block->LocalCondSet(Cond::EQ).SetType(ValueType::U8);
    block->SetTerminal(terminal::If{condition,
            terminal::LinkBlock{Location{0xf010}}, terminal::LinkBlock{Location{0xf020}}});
    FlagsEliminationPass::Run(block.get(), nullptr, features);
    REQUIRE(block->HasDeadEdgeIntegerBranchProof());
    block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    const auto begin = std::chrono::steady_clock::now();
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    const auto allocation_end = phases ? std::chrono::steady_clock::now() : begin;
    if (shape == 0) {
        allocation.MapRegister(source.Id(), HostGPR{22});
        allocation.MarkHostWriteCoalesced(publication->Id());
    }
    const auto mapping_end = phases ? std::chrono::steady_clock::now() : begin;
    allocation.CompleteGPRAllocation(space.GetDefaultModule());
    arm64::JitContext context{space.GetDefaultModule(), allocation};
    const auto planning_end = phases ? std::chrono::steady_clock::now() : begin;
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
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
    metrics.reloads = allocation.RAShape().spill_loads; metrics.stores = allocation.RAShape().spill_stores;
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
    AddressSpace space{config}; FeatureSet features{};
    IntrusivePtr<Block> block{new Block(0, Location{0xfb00})};
    for (unsigned i = 0; i < length; ++i) {
        const auto value = block->LoadImm(Imm{swift::u64{i + 31}}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{24, ValueType::U64}, value);
    }
    if (shape == 2) {
        const auto base = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
        const auto producer = block->Add(base, Operand{Imm{1u}}).SetType(ValueType::U32);
        block->SaveFlags(producer, Flags::Carry);
        const auto zero = block->LoadImm(Imm{0u}).SetType(ValueType::U32);
        const auto test = block->TestFlags(Flags::Carry).SetType(ValueType::U32);
        const auto carry = block->Add(zero, Operand{test}).SetType(ValueType::U32);
        const auto result = block->Add(base, Operand{carry}).SetType(ValueType::U32);
        block->StoreUniform(Uniform{8, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
    } else {
        const auto type = shape == 1 ? ValueType::U16 : ValueType::U8;
        const auto source_type = shape == 3 ? ValueType::U32 : type;
        const auto source = block->LoadUniform(Uniform{0, source_type}).SetType(source_type);
        const auto immediate = block->LoadImm(Imm{7u}).SetType(ValueType::U32);
        const auto sub = block->Sub(source, Operand{immediate}).SetType(type);
        block->SaveFlags(sub, Flags::Carry);
    }
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    const auto begin = std::chrono::steady_clock::now();
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    allocation.CompleteGPRAllocation(space.GetDefaultModule());
    arm64::JitContext context{space.GetDefaultModule(), allocation};
    arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
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
    AddressSpace space{config}; FeatureSet features{};
    IntrusivePtr<Block> block{new Block(0, Location{0xbf80})};
    const auto address = block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    for (unsigned i = 0; i < length; ++i) {
        if (shape == 0) {
            const auto input = block->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
            const auto left = block->BitExtract(input, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
            const auto right = block->BitExtract(input, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
            const auto result = block->Xor(left, Operand{right}).SetType(ValueType::U32);
            block->SaveFlags(result, Flags::Zero | Flags::Negate | Flags::Parity);
            block->StoreUniform(Uniform{16, ValueType::U32}, result);
        } else if (shape == 1) {
            const auto input = block->LoadUniform(Uniform{8, ValueType::U32}).SetType(ValueType::U32);
            const auto comparison = block->Sub(input, Operand{Imm{7u}}).SetType(ValueType::U32);
            block->SaveFlags(comparison, Flags::Carry | Flags::Zero);
            block->InvertCarry(); block->AdvancePC(Imm{1u});
            const auto carry = block->TestFlags(Flags::Carry).SetType(ValueType::U8);
            const auto no_carry = block->TestZero(carry).SetType(ValueType::U8);
            const auto nonzero = block->CondSet(Cond::NE).SetType(ValueType::U8);
            const auto condition = block->And(no_carry, Operand{nonzero}).SetType(ValueType::U8);
            const auto alternative = block->LoadUniform(Uniform{24, ValueType::U32}).SetType(ValueType::U32);
            const auto selected = block->Select(BOOL{condition}, input, alternative).SetType(ValueType::U32);
            block->StoreUniform(Uniform{16, ValueType::U32}, selected);
        } else if (shape == 2) {
            const auto value = block->GetHostFPR(HostRegIndex(17), Imm{0u}).SetType(ValueType::U64);
            block->StoreMemory(Operand{address, Imm{swift::u64{i * 8}}}, value);
        } else {
            const auto old = block->GetHostFPR(HostRegIndex(17), Imm{0u}).SetType(ValueType::V128);
            const auto zero = block->VecXor(old, old).SetType(ValueType::V128);
            block->SetHostFPR(zero, HostRegIndex(17), Imm{0u});
            const auto input = block->LoadUniform(Uniform{8, ValueType::U32}).SetType(ValueType::U32);
            const auto converted = block->VecFCvtIntToFloat(input, Imm{32u}, Imm{64u}).SetType(ValueType::U64);
            block->SetHostFPR(converted, HostRegIndex(17), Imm{0u});
        }
    }
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    const auto begin = std::chrono::steady_clock::now();
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    allocation.CompleteGPRAllocation(space.GetDefaultModule());
    arm64::JitContext context{space.GetDefaultModule(), allocation};
    arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
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
    AddressSpace space{config}; FeatureSet features{};
    IntrusivePtr<Block> block{new Block(0, Location{0xbf90})};
    for (unsigned i = 0; i < length; ++i) {
        if (shape < 3) {
            const auto type = shape == 2 ? ValueType::U8 : ValueType::U32;
            const auto input = block->LoadUniform(Uniform{0, type}).SetType(type);
            const auto extract = block->BitExtract(input, Imm{0u}, Imm{8u}).SetType(ValueType::U8);
            Value result;
            if (shape == 2) result = block->And(extract, Operand{Imm{15u}}).SetType(ValueType::U8);
            else {
                result = block->ZeroExtend32(extract).SetType(ValueType::U8);
                if (shape == 1) result = block->LsrImm(result, Imm{1u}).SetType(ValueType::U32);
            }
            block->StoreUniform(Uniform{16, result.Type()}, result);
        } else if (shape == 3) {
            const auto input = block->LoadUniform(Uniform{0, ValueType::U32}).SetType(ValueType::U32);
            const auto shifted = block->LsrImm(input, Imm{8u}).SetType(ValueType::U32);
            const auto result = block->And(shifted, Operand{Imm{1u}}).SetType(ValueType::U32);
            block->StoreUniform(Uniform{16, ValueType::U32}, result);
        } else {
            const auto low = block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
            const auto high = block->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
            const auto right = block->LsrImm(low, Imm{32u}).SetType(ValueType::U64);
            const auto left = block->LslImm(high, Imm{32u}).SetType(ValueType::U64);
            const auto result = block->Or(right, Operand{left}).SetType(ValueType::U64);
            if (shape == 5) block->StoreUniform(Uniform{24, ValueType::U64}, right);
            block->StoreUniform(Uniform{16, ValueType::U64}, result);
        }
    }
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    const auto begin = std::chrono::steady_clock::now();
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    allocation.CompleteGPRAllocation(space.GetDefaultModule());
    arm64::JitContext context{space.GetDefaultModule(), allocation};
    arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
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
    auto* source = function->GetCurrentBlock();
    auto* hot = function->AppendBlock(Location{hot_start}, Location{hot_start + 16});
    auto* cold = function->AppendBlock(Location{cold_start}, Location{cold_start + 16});
    function->AddEdge(source, hot); function->AddEdge(source, cold);
    builder.SetCurBlock(source);
    for (unsigned i = 0; i < length; ++i) function->AdvancePC(Imm{swift::u64{1}});
    const auto left = function->LoadUniform(Uniform{0, ValueType::U16}).SetType(ValueType::U16);
    const auto sub = function->Sub(left, Operand{Imm{swift::u64{1}}}).SetType(ValueType::U16);
    function->SaveFlags(sub, Flags::All);
    Value condition;
    if (shape == 2) condition = function->LocalCondSet(Cond::EQ).SetType(ValueType::U8);
    if (shape == 1) {
        const auto zero = function->LoadImm(Imm{swift::u8{0}}).SetType(ValueType::U8);
        function->StoreUniform(Uniform{0, ValueType::U8}, zero);
    } else if (shape == 2) {
        // Compile-only opaque helper address: deterministic across all control ELFs.
        (void)function->CallLambda(Lambda{Imm{swift::u64{0x1234}}});
    } else if (shape == 3) {
        const auto address = function->LoadUniform(Uniform{16, ValueType::U64}).SetType(ValueType::U64);
        const auto loaded = function->LoadMemory(Operand{address}).SetType(ValueType::U32);
        function->StoreUniform(Uniform{32, ValueType::U32}, loaded);
    }
    if (GetSvmConfig().flags_cfinv && True(config.arm64_features & Arm64Features::FlagM)) {
        function->InvertCarry();
    } else {
        const auto polarity = function->LoadImm(Imm{swift::u8{0}}).SetType(ValueType::U8);
        function->StoreUniform(Uniform{offsetof(swift::x86::ThreadContext64, carry_inverted), ValueType::U8}, polarity);
    }
    function->AdvancePC(Imm{swift::u64{1}});
    if (shape != 2) condition = function->LocalCondSet(Cond::EQ).SetType(ValueType::U8);
    function->EndBlock(terminal::If{condition, terminal::LinkBlock{Location{hot_start}}, terminal::LinkBlock{Location{cold_start}}});
    builder.SetCurBlock(hot);
    const auto two = function->LoadImm(Imm{swift::u16{2}}).SetType(ValueType::U16);
    const auto next = function->Sub(two, Operand{Imm{swift::u64{1}}}).SetType(ValueType::U16);
    function->SaveFlags(next, Flags::All); function->AdvancePC(Imm{swift::u64{1}});
    function->EndBlock(terminal::ReturnToHost{});
    builder.SetCurBlock(cold);
    const auto pf = function->TestFlags(Flags::Parity).SetType(ValueType::U32);
    const auto af = function->TestFlags(Flags::AuxiliaryCarry).SetType(ValueType::U32);
    function->StoreUniform(Uniform{24, ValueType::U32}, pf);
    function->StoreUniform(Uniform{28, ValueType::U32}, af);
    function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
    function->ComputeRPO(); function->IdByRPO();
    RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(), space.GetTrampolines().GetFPRRegs(), features};
    const auto begin = std::chrono::steady_clock::now();
    RegisterAllocPass::Run(function, &allocation, false, features);
    []<class Context>(auto& allocation, const auto& module) {
        if constexpr (requires { typename Context::AssemblerOnlyTag; }) allocation.CompleteGPRAllocation(module);
    }.template operator()<arm64::JitContext>(allocation, module);
    arm64::JitContext context{module, allocation}; arm64::JitTranslator translator{context};
    REQUIRE(translator.Translate(function)); context.Finish();
    Metrics metrics;
    metrics.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    // Prove this family actually selects its intended actions, outside the clock.
    // The complete original architecture has no frozen Backedge action API.
    [](const auto& allocation, auto* source, unsigned shape) {
        const auto actions = allocation.GPRPlanFor(source->GetBlock());
        if constexpr (requires { actions->backedge_flags; }) {
            REQUIRE(actions);
            if (shape < 2) {
                REQUIRE(actions->backedge_flags);
                REQUIRE(actions->backedge_flags->dead_successor);
                REQUIRE(actions->backedge_flags->pfaf.has_value() == (shape == 0));
            } else REQUIRE_FALSE(actions->backedge_flags);
        }
    }(allocation, source, shape);
    metrics.bytes = context.CurrentBufferSize(); metrics.spills = allocation.SpillCount();
    metrics.reloads = allocation.RAShape().spill_loads; metrics.stores = allocation.RAShape().spill_stores;
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
