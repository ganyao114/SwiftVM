#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"
#include "support/jit_translator_test_support.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
struct Metrics {
    swift::u64 ns{}, hash{1469598103934665603ull}, spills{}, reloads{}, stores{};
    size_t bytes{}, narrow{}, direct{}, writebacks{}, covered{};
};
struct Phases { swift::u64 allocation{}, mapping{}, planning{}, emission{}, finish{}; };
Metrics Compile(unsigned shape, unsigned length, Phases* phases = nullptr) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 4096;
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xba80})};
    struct Homes { Value address, loaded, result; };
    std::vector<Homes> homes;
    for (unsigned i = 0; i < length; ++i) {
        const auto address = block->LoadUniform(ValueType::U64, Uniform{2048, ValueType::U64});
        const auto loaded = block->LoadMemory(
                shape == 0 || shape == 3 ? ValueType::U16 : ValueType::U64, Operand{address});
        Value result;
        if (shape == 0 || shape == 3) {
            result = block->SignExtend(ValueType::S64, loaded);
            block->StoreUniform(Uniform{2056, ValueType::S64}, result);
            if (shape == 3) block->StoreUniform(Uniform{2064, ValueType::U16}, loaded);
        } else {
            if (shape == 2) block->UniformBarrier();
            result = block->Add(ValueType::U64, address, Operand{Imm{swift::u64{8}}});
            block->StoreUniform(Uniform{2056, ValueType::U64}, loaded);
            block->StoreUniform(Uniform{2064, ValueType::U64}, result);
        }
        homes.push_back({address, loaded, result});
    }
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    auto reserved = space.GetTrampolines().GetGPRRegs();
    for (unsigned reg : {8u, 9u, 13u}) reserved.Mark(reg);
    AllocationFixture allocation{
            block->MaxInstrId(), reserved, space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    const auto begin = std::chrono::steady_clock::now();
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, FeatureSet{}),
            false);
    const auto allocation_end = phases ? std::chrono::steady_clock::now() : begin;
    for (const auto& item : homes) {
        allocation.MapRegister(item.address.Id(), HostGPR{13});
        allocation.MapRegister(item.loaded.Id(), HostGPR{8});
        allocation.MapRegister(item.result.Id(), HostGPR{static_cast<swift::u16>(shape == 0 || shape == 3 ? 9 : 13)});
    }
    GPRSMask active = reserved;
    FPRSMask fprs = space.GetTrampolines().GetFPRRegs();
    for (const auto& node : block->GetInstList()) allocation.SetActiveRegs(node.Id(), active, fprs);
    const auto mapping_end = phases ? std::chrono::steady_clock::now() : begin;
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    arm64::JitContext context{module, allocation, allocation.Actions()};
    const auto planning_end = phases ? std::chrono::steady_clock::now() : begin;
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    const auto emission_end = phases ? std::chrono::steady_clock::now() : begin;
    context.Finish();
    const auto finish_end = phases ? std::chrono::steady_clock::now() : begin;
    Metrics result;
    result.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            (phases ? finish_end : std::chrono::steady_clock::now()) - begin).count();
    if (phases) {
        const auto ns = [](auto interval) { return std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count(); };
        *phases = {static_cast<swift::u64>(ns(allocation_end - begin)),
                   static_cast<swift::u64>(ns(mapping_end - allocation_end)),
                   static_cast<swift::u64>(ns(planning_end - mapping_end)),
                   static_cast<swift::u64>(ns(emission_end - planning_end)),
                   static_cast<swift::u64>(ns(finish_end - emission_end))};
    }
    result.bytes = context.CurrentBufferSize();
    result.spills = allocation.SpillCount();
    result.reloads = context.GetRAShapeCounters().spill_loads;
    result.stores = context.GetRAShapeCounters().spill_stores;
    for (const auto& item : homes) {
        if (!arm64::JitTranslatorTestSupport::IsInstructionSuppressed(translator, item.result.Def())) continue;
        ++result.covered;
        if (shape == 0 || shape == 3) { ++result.narrow; ++result.direct; }
        else ++result.writebacks;
    }
    swift::u64 narrow{}, writebacks{}, covered{};
    for (const auto& item : homes) {
        const auto* action = allocation.Actions().Memory(item.loaded.Id());
        narrow += action && action->Has(arm64::MemoryAction::Narrow);
        writebacks += action && action->Has(arm64::MemoryAction::PostIndex);
        covered += allocation.Actions().InstructionCovered(item.result.Id());
    }
    REQUIRE(result.narrow == narrow);
    REQUIRE(result.writebacks == writebacks);
    REQUIRE(result.covered == covered);
    const auto* code = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < result.bytes; ++i) result.hash = (result.hash ^ code[i]) * 1099511628211ull;
    return result;
}

TEST_CASE("memory compile phase diagnostics", "[.memory-plan-phases]") {
    for (unsigned shape : {0u, 1u, 2u, 3u}) {
        std::array<std::vector<swift::u64>, 5> samples;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            Phases phases;
            const auto result = Compile(shape, 128, &phases);
            REQUIRE(result.bytes > 0);
            if (iteration < 5) continue;
            const std::array values{phases.allocation, phases.mapping, phases.planning, phases.emission, phases.finish};
            for (size_t i = 0; i < samples.size(); ++i) samples[i].push_back(values[i]);
        }
        for (auto& sample : samples) std::sort(sample.begin(), sample.end());
        std::printf("memory-phases shape=%u length=128 allocation_ns=%llu mapping_ns=%llu planning_ns=%llu emission_ns=%llu finish_ns=%llu\n",
                    shape, (unsigned long long)samples[0][10], (unsigned long long)samples[1][10],
                    (unsigned long long)samples[2][10], (unsigned long long)samples[3][10],
                    (unsigned long long)samples[4][10]);
    }
}
}

TEST_CASE("memory plans complete compilation metrics", "[.memory-plan-metrics]") {
    for (unsigned shape : {0u, 1u, 2u, 3u}) for (unsigned length : {2u, 8u, 32u, 128u}) {
        Metrics sample;
        std::vector<swift::u64> times;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = Compile(shape, length);
            REQUIRE(result.bytes > 0);
            REQUIRE(result.narrow == (shape == 0 ? length : 0));
            REQUIRE(result.direct == result.narrow);
            REQUIRE(result.writebacks == (shape == 1 ? length : 0));
            REQUIRE(result.covered == (shape < 2 ? length : 0));
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("memory-metrics shape=%u length=%u bytes=%zu hash=%llu narrow=%zu direct=%zu writebacks=%zu covered=%zu spills=%llu reloads=%llu stores=%llu median_ns=%llu\n",
                    shape, length, sample.bytes, (unsigned long long)sample.hash, sample.narrow, sample.direct,
                    sample.writebacks, sample.covered, (unsigned long long)sample.spills,
                    (unsigned long long)sample.reloads, (unsigned long long)sample.stores,
                    (unsigned long long)times[times.size()/2]);
        std::fflush(stdout);
    }
}
