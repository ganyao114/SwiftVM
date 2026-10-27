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
#include "runtime/backend/arm64/pshufd_direct.h"
#include "runtime/compile/allocate/allocation.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
struct Metrics {
    swift::u64 ns{}, hash{1469598103934665603ull}, spills{}, reloads{}, stores{};
    size_t bytes{}, direct{}, masks{}, ties{};
};
Metrics Compile(unsigned shape, unsigned pressure, unsigned length) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 1024;
    AddressSpace space{config};
    ModuleConfig module_config{};
    module_config.feature_overrides.Set(FeatureId::sse_shufps_imm, true);
    auto module = space.MapModule(LocationDescriptor{0xdd00}, LocationDescriptor{0xde00}, module_config);
    IntrusivePtr<Block> block{new Block(0, Location{0xdd00})};
    std::vector<Value> live;
    if (pressure)
        for (unsigned i = 0; i < 32; ++i)
            live.push_back(
                    block->LoadUniform(ValueType::V128, Uniform{128 + i * 16, ValueType::V128}));
    auto value = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
    const auto [low, high] = arm64::PshufdIndexMask(0x4e);
    Value shared;
    if (shape == 0 || shape == 3)
        shared = block->VecLoadConst(ValueType::V128, Imm{low}, Imm{high});
    for (unsigned i = 0; i < length; ++i) {
        if (shape == 2) {
            value = block->VecShuffle32TwoSrc(ValueType::V128, value, value, Imm{0xe0u});
        } else {
            const auto mask =
                    shape == 1 ? block->VecLoadConst(ValueType::V128, Imm{low}, Imm{high}) : shared;
            value = block->VecShuffle32Indexed(ValueType::V128, value, mask);
            if (shape == 3) {
                const auto other = block->VecTableLookup8(ValueType::V128, value, shared);
                block->StoreUniform(Uniform{48, ValueType::V128}, other);
            }
        }
        block->StoreUniform(Uniform{16, ValueType::V128}, value);
    }
    for (unsigned i = 0; i < live.size(); ++i)
        block->StoreUniform(Uniform{512 + i * 16, ValueType::V128}, live[i]);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    FeatureSet features{};
    features.sse_shufps_imm = true;
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
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    arm64::JitContext context{module, allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    Metrics result;
    result.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    result.bytes = context.CurrentBufferSize();
    result.spills = allocation.SpillCount();
    result.reloads = context.GetRAShapeCounters().spill_loads;
    result.stores = context.GetRAShapeCounters().spill_stores;
    for (const auto& node : block->GetInstList()) {
        if (allocation.IsPshufdDirect(node.Id())) {
            result.direct += node.GetOp() == OpCode::VecShuffle32Indexed;
            result.masks += node.GetOp() == OpCode::VecLoadConst;
        }
        if (node.GetOp() == OpCode::VecShuffle32TwoSrc &&
            allocation.ValueType(Value{&node}) == AllocationFixture::FPR &&
            allocation.ValueType(node.GetArg<Value>(0)) == AllocationFixture::FPR &&
            allocation.ValueFPR(Value{&node}).id == allocation.ValueFPR(node.GetArg<Value>(0)).id)
            ++result.ties;
    }
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < result.bytes; ++i) result.hash = (result.hash ^ bytes[i]) * 1099511628211ull;
    return result;
}
}

TEST_CASE("shuffle plans complete compilation metrics", "[.shuffle-plan-metrics]") {
    for (unsigned shape : {0u, 1u, 2u, 3u}) for (unsigned pressure : {0u, 1u})
        for (unsigned length : {2u, 8u, 32u, 128u}) {
            Metrics sample;
            std::vector<swift::u64> times;
            for (unsigned iteration = 0; iteration < 25; ++iteration) {
                const auto result = Compile(shape, pressure, length);
                REQUIRE(result.bytes > 0);
                REQUIRE(result.direct == (shape < 2 ? length : 0));
                REQUIRE(result.masks == (shape == 0 ? 1 : shape == 1 ? length : 0));
                if (shape == 2 && !pressure) REQUIRE(result.ties == length);
                if (pressure) REQUIRE(result.spills > 0);
                if (iteration) REQUIRE(result.hash == sample.hash);
                if (iteration >= 5) times.push_back(result.ns);
                sample = result;
            }
            std::sort(times.begin(), times.end());
            std::printf("shuffle-metrics shape=%u pressure=%u length=%u bytes=%zu hash=%llu direct=%zu masks=%zu ties=%zu spills=%llu reloads=%llu stores=%llu median_ns=%llu\n",
                    shape, pressure, length, sample.bytes, (unsigned long long)sample.hash,
                    sample.direct, sample.masks, sample.ties, (unsigned long long)sample.spills,
                    (unsigned long long)sample.reloads, (unsigned long long)sample.stores,
                    (unsigned long long)times[times.size()/2]);
            std::fflush(stdout);
        }
}
