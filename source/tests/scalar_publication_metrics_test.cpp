#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <chrono>
#include <cstdio>
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
    size_t bytes{}, loads{}, values{};
};
Metrics Compile(unsigned shape, unsigned length) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 128;
    AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0xdc00})};
    const auto zero = block->LoadImm(ValueType::U64, Imm{swift::u64{0}});
    const auto address = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    for (unsigned i = 0; i < length; ++i) {
        auto low = shape == 1 ? block->LoadUniform(ValueType::U64, Uniform{8, ValueType::U64})
                              : block->LoadMemory(shape == 2 ? ValueType::U32 : ValueType::U64,
                                                  Operand{address, Imm{swift::u64{i * 8}}});
        if (shape == 2) low = block->ZeroExtend64(ValueType::U64, low);
        if (shape == 3) block->UniformBarrier();
        block->SetHostFPR(low, HostRegIndex(17), Imm{0u});
        block->SetHostFPR(zero, HostRegIndex(17), Imm{8u});
    }
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    FeatureSet features{};
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
    Metrics result;
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    result.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    for (const auto& inst : block->GetInstList()) {
        result.loads += allocation.Actions().ScalarFPRLoad(inst.Id()) != nullptr;
        result.values += allocation.Actions().ScalarFPRValue(inst.Id()) != nullptr;
    }
    result.bytes = context.CurrentBufferSize();
    result.spills = allocation.SpillCount();
    result.reloads = context.GetRAShapeCounters().spill_loads;
    result.stores = context.GetRAShapeCounters().spill_stores;
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < result.bytes; ++i) result.hash = (result.hash ^ bytes[i]) * 1099511628211ull;
    return result;
}
}

TEST_CASE("scalar publication isolated compilation metrics", "[.scalar-publication-metrics]") {
#if defined(__aarch64__)
    swift::u64 counter_hz;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(counter_hz));
    std::printf("scalar-publication-counter-hz=%llu\n", (unsigned long long)counter_hz);
#endif
    for (unsigned shape = 0; shape < 4; ++shape) for (unsigned length : {2u, 8u, 32u, 128u}) {
        std::vector<swift::u64> times;
        Metrics sample;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = Compile(shape, length);
            REQUIRE(result.bytes > 0);
            REQUIRE(result.loads == (shape == 0 || shape == 2 ? length : 0));
            REQUIRE(result.values == (shape == 1 || shape == 3 ? length : 0));
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("scalar-publication-metrics shape=%u length=%u bytes=%zu hash=%llu loads=%zu values=%zu spills=%llu reloads=%llu stores=%llu median_ns=%llu\n",
                shape, length, sample.bytes, (unsigned long long)sample.hash, sample.loads, sample.values,
                (unsigned long long)sample.spills, (unsigned long long)sample.reloads,
                (unsigned long long)sample.stores, (unsigned long long)times[times.size() / 2]);
        std::fflush(stdout);
    }
}
