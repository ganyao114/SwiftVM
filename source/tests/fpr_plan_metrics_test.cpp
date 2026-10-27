#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "support/register_alloc_test_support.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
struct Metrics { swift::u64 ns{}, hash{}, spills{}, reloads{}, stores{}; size_t bytes{}; };
Metrics Compile(unsigned shape, unsigned length) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64,
                  .arm64_features = Arm64Features::AFP, .sse_scalar_insert = true};
    AddressSpace space{config};
    ModuleConfig module_config{};
    module_config.feature_overrides.Set(FeatureId::sse_scalar_tie, true);
    module_config.feature_overrides.Set(FeatureId::ra_aes_chain_tie, true);
    auto module = space.MapModule(LocationDescriptor{0xb100}, LocationDescriptor{0xb200}, module_config);
    IntrusivePtr<Block> block{new Block(0, Location{0xb100})};
    const auto right = block->LoadUniform(Uniform{0, ValueType::V128});
    const auto zero = block->LoadUniform(Uniform{16, ValueType::V128});
    auto value = block->GetHostFPR(ValueType::V128, HostRegIndex(17), Imm{0u});
    for (unsigned i = 0; i < length; ++i) {
        if (shape == 0)
            value = block->VecXor(ValueType::V128, value, right);
        else if (shape == 1)
            value = block->VecFAddScalar64(ValueType::V128, value, right);
        else
            value = (i + 1 == length ? block->VecAesEncLastFast(ValueType::V128, value, right, zero)
                                     : block->VecAesEncFast(ValueType::V128, value, right, zero));
        if (shape != 2 || i + 1 == length) block->SetHostFPR(value, HostRegIndex(17), Imm{0u});
    }
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FPRSMask fprs{}; fprs.Mark(17);
    FeatureSet features{}; features.sse_scalar_tie = true; features.ra_aes_chain_tie = true;
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{~((1u << 8) - 1u)}, fprs, features};
    const auto begin = std::chrono::steady_clock::now();
    if (shape == 0) RegisterAllocTestSupport::RunForXmmResidentTest(block.get(), &allocation, true);
    else if (shape == 1) RegisterAllocTestSupport::RunForScalarFPRTieTest(block.get(), &allocation, true);
    else RegisterAllocTestSupport::RunForAesChainTieTest(block.get(), &allocation, true);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    arm64::JitContext context{module, allocation, allocation.Actions()};
    arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
    Metrics result;
    result.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    result.bytes = context.CurrentBufferSize(); result.spills = allocation.SpillCount();
    result.reloads = context.GetRAShapeCounters().spill_loads; result.stores = context.GetRAShapeCounters().spill_stores;
    result.hash = 1469598103934665603ull;
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < result.bytes; ++i) result.hash = (result.hash ^ bytes[i]) * 1099511628211ull;
    return result;
}
}

TEST_CASE("FPR plan isolated compile metrics", "[.fpr-plan-metrics]") {
    const auto* limit_env = std::getenv("SWIFT_FPR_METRICS_MAX");
    const unsigned limit = limit_env ? std::strtoul(limit_env, nullptr, 10) : 128;
    for (unsigned shape : {0u, 1u, 2u}) for (unsigned length : {2u, 8u, 32u, 128u}) {
        if (length > limit) continue;
        std::vector<swift::u64> times;
        Metrics sample;
        for (unsigned iteration = 0; iteration < 35; ++iteration) {
            const auto result = Compile(shape, length);
            REQUIRE(result.bytes > 0);
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("fpr-metrics shape=%u length=%u bytes=%zu hash=%llu spills=%llu reloads=%llu stores=%llu median_ns=%llu\n",
                    shape, length, sample.bytes, (unsigned long long)sample.hash,
                    (unsigned long long)sample.spills, (unsigned long long)sample.reloads,
                    (unsigned long long)sample.stores, (unsigned long long)times[times.size()/2]);
        std::fflush(stdout);
    }
}
