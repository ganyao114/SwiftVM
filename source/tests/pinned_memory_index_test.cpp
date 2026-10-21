#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
struct Metrics {
    swift::u64 ns{}, hash{1469598103934665603ull}, spills{}, reloads{}, stores{};
    size_t bytes{}, updates{};
};
Metrics Compile(unsigned scenario, unsigned length) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64, .global_opts = Optimizations::All};
    config.uniform_buffer_size = 128;
    AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0xdb00})};
    for (unsigned i = 0; i < length; ++i) {
        const auto base = block->GetHostGPR(HostRegIndex(23), Imm{0u}).SetType(ValueType::U64);
        const auto loaded = block->LoadMemory(Operand{base, Imm{1u}}).SetType(ValueType::U8);
        const auto widened = block->ZeroExtend32(loaded).SetType(ValueType::U32);
        const auto published = block->ZeroExtend32To64(widened).SetType(ValueType::U64);
        block->SetHostGPR(published, HostRegIndex(29), Imm{0u});
        if (scenario == 3) (void)block->CallLambda(Lambda{Imm{swift::u64{1}}});
        const auto updated = block->Add(base, Operand{Imm{1u}}).SetType(ValueType::U64);
        if (scenario == 2) block->StoreUniform(Uniform{0, ValueType::U64}, updated);
        block->SetHostGPR(updated, HostRegIndex(23), Imm{0u});
        if (scenario == 1) block->StoreUniform(Uniform{0, ValueType::U64}, updated);
        if (scenario == 4) block->StoreUniform(Uniform{0, ValueType::U64}, base);
    }
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    const auto begin = std::chrono::steady_clock::now();
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    allocation.CompleteGPRAllocation(space.GetDefaultModule());
    arm64::JitContext context{space.GetDefaultModule(), allocation};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    Metrics result;
    result.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    result.bytes = context.CurrentBufferSize();
    result.updates = allocation.GPRPlanFor(block.get())->load_updates.size();
    result.spills = allocation.SpillCount();
    result.reloads = allocation.RAShape().spill_loads;
    result.stores = allocation.RAShape().spill_stores;
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < result.bytes; ++i) result.hash = (result.hash ^ bytes[i]) * 1099511628211ull;
    return result;
}
}

TEST_CASE("pinned load update index preserves consumer order and extra-use rejection", "[pinned-memory-index]") {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        CAPTURE(scenario);
        const auto result = Compile(scenario, 1);
        REQUIRE(result.bytes > 0);
        REQUIRE(result.updates == (scenario == 0 ? 1 : 0));
    }
}

TEST_CASE("pinned load update isolated compilation metrics", "[.pinned-memory-metrics]") {
    for (unsigned scenario : {0u, 1u}) for (unsigned length : {2u, 8u, 32u, 128u}) {
        std::vector<swift::u64> times;
        Metrics sample;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = Compile(scenario, length);
            REQUIRE(result.updates == (scenario == 0 ? length : 0));
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("pinned-memory-metrics shape=%u length=%u bytes=%zu hash=%llu updates=%zu spills=%llu reloads=%llu stores=%llu median_ns=%llu\n",
                scenario, length, sample.bytes, (unsigned long long)sample.hash, sample.updates,
                (unsigned long long)sample.spills, (unsigned long long)sample.reloads,
                (unsigned long long)sample.stores, (unsigned long long)times[times.size() / 2]);
        std::fflush(stdout);
    }
}
