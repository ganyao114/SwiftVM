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

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
struct Metrics {
    swift::u64 ns{}, hash{1469598103934665603ull}, spills{}, reloads{}, stores{};
    size_t bytes{};
};
Metrics Compile(unsigned shape, unsigned length) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0xd900})};
    auto value = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    for (unsigned i = 0; i < length; ++i) {
        value = block->Add(ValueType::U32, value, Operand{Imm{1u}});
        if (shape == 1) {
            block->SaveFlags(value, Flags::NZCV);
            block->SaveFlags(value, Flags::Parity | Flags::AuxiliaryCarry);
        } else if (shape == 2) {
            for (unsigned j = 0; j < 8; ++j) block->SaveFlags(value, j & 1 ? Flags::NZCV : Flags::Parity | Flags::AuxiliaryCarry);
        } else if (shape == 3)
            block->BranchOnlyFlags(value, Flags::NZCV);
        if (shape == 1 || shape == 2) block->AdvancePC(Imm{swift::u64{1}});
    }
    block->StoreUniform(Uniform{8, ValueType::U32}, value);
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
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    Metrics result;
    result.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    result.bytes = context.CurrentBufferSize();
    result.spills = allocation.SpillCount();
    result.reloads = context.GetRAShapeCounters().spill_loads;
    result.stores = context.GetRAShapeCounters().spill_stores;
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < result.bytes; ++i) result.hash = (result.hash ^ bytes[i]) * 1099511628211ull;
    return result;
}
}

TEST_CASE("pseudo flags isolated compilation metrics", "[.pseudo-flags-metrics]") {
    for (unsigned shape = 0; shape < 4; ++shape) for (unsigned length : {2u, 8u, 32u, 128u}) {
        std::vector<swift::u64> times;
        Metrics sample;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = Compile(shape, length);
            REQUIRE(result.bytes > 0);
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("pseudo-flags-metrics shape=%u length=%u bytes=%zu hash=%llu spills=%llu reloads=%llu stores=%llu median_ns=%llu\n",
                shape, length, sample.bytes, (unsigned long long)sample.hash,
                (unsigned long long)sample.spills, (unsigned long long)sample.reloads,
                (unsigned long long)sample.stores, (unsigned long long)times[times.size() / 2]);
        std::fflush(stdout);
    }
}
