#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "support/jit_translator_test_support.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
struct Metrics {
    swift::u64 ns{}, hash{1469598103934665603ull}, spills{}, reloads{}, stores{};
    size_t bytes{}, direct{}, boolean{}, covered{};
};
Metrics Compile(unsigned shape, unsigned length, unsigned pressure) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 4096;
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xbf80})};
    std::vector<Value> live, conditions, constants, selects;
    for (unsigned i = 0; i < pressure; ++i)
        live.push_back(block->LoadUniform(Uniform{static_cast<swift::u16>(i * 8), ValueType::U64})
                .SetType(ValueType::U64));
    for (unsigned i = 0; i < length; ++i) {
        const auto input = block->LoadUniform(Uniform{2048, ValueType::U32}).SetType(ValueType::U32);
        Value condition;
        if (shape < 2) {
            const auto result = block->Sub(input, Operand{input}).SetType(ValueType::U32);
            block->SaveFlags(result, Flags::NZCV);
            condition = block->CondSet(Cond::EQ).SetType(ValueType::U8);
        } else if (shape == 2) {
            condition = block->LoadUniform(Uniform{2056, ValueType::U8}).SetType(ValueType::U8);
        } else condition = block->TestZero(input).SetType(ValueType::U8);
        const auto type = shape == 1 ? ValueType::U64 : ValueType::U8;
        const auto yes = block->LoadImm(Imm{swift::u64{shape == 1 ? 11u : 1u}}).SetType(type);
        const auto no = block->LoadImm(Imm{swift::u64{shape == 1 ? 22u : 0u}}).SetType(type);
        const auto selected = block->Select(condition, yes, no).SetType(type);
        block->StoreUniform(Uniform{2064, type}, selected);
        conditions.push_back(condition);
        constants.push_back(yes); constants.push_back(no);
        selects.push_back(selected);
    }
    for (auto value : live) block->StoreUniform(Uniform{2072, ValueType::U64}, value);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    const auto begin = std::chrono::steady_clock::now();
    RegisterAllocPass::Run(block.get(), &allocation, false, FeatureSet{});
    allocation.CompleteGPRAllocation(module);
    arm64::JitContext context{module, allocation};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    Metrics result;
    result.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count();
    result.bytes = context.CurrentBufferSize();
    result.spills = allocation.SpillCount();
    result.reloads = allocation.RAShape().spill_loads;
    result.stores = allocation.RAShape().spill_stores;
    for (auto condition : conditions)
        result.direct += arm64::JitTranslatorTestSupport::IsInstructionSuppressed(translator, condition.Def());
    size_t constant_coverage = 0;
    for (auto value : constants)
        constant_coverage += arm64::JitTranslatorTestSupport::IsInstructionSuppressed(translator, value.Def());
    result.boolean = constant_coverage / 2;
    result.covered = result.direct + constant_coverage;
    if (const auto plan = allocation.GPRPlanFor(block.get()); plan->boolean_selects) {
        REQUIRE(plan->boolean_selects->actions.size() == selects.size());
        REQUIRE(plan->boolean_selects->covered.size() == result.covered);
    }
    const auto* code = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < result.bytes; ++i) { result.hash ^= code[i]; result.hash *= 1099511628211ull; }
    return result;
}
}

TEST_CASE("boolean select compilation metrics preserve recipes under pressure", "[.boolean-select-metrics]") {
    for (unsigned shape : {0u, 1u, 2u, 3u}) for (unsigned pressure : {0u, 32u})
        for (unsigned length : {2u, 8u, 32u, 128u}) {
            CAPTURE(shape, pressure, length);
            std::vector<swift::u64> times;
            Metrics sample;
            for (unsigned iteration = 0; iteration < 25; ++iteration) {
                const auto result = Compile(shape, length, pressure);
                REQUIRE(result.bytes > 0);
                REQUIRE(result.direct == (shape < 2 ? length : 0));
                REQUIRE(result.boolean == (shape == 0 || shape == 3 ? length : 0));
                REQUIRE(result.covered == result.direct + 2 * result.boolean);
                if (iteration) REQUIRE(result.hash == sample.hash);
                if (iteration >= 5) times.push_back(result.ns);
                sample = result;
            }
            std::sort(times.begin(), times.end());
            std::printf("boolean-metrics shape=%u pressure=%u length=%u bytes=%zu hash=%llu direct=%zu boolean=%zu covered=%zu spills=%llu reloads=%llu stores=%llu median_ns=%llu\n",
                    shape, pressure, length, sample.bytes, (unsigned long long)sample.hash,
                    sample.direct, sample.boolean, sample.covered, (unsigned long long)sample.spills,
                    (unsigned long long)sample.reloads, (unsigned long long)sample.stores,
                    (unsigned long long)times[times.size() / 2]);
            std::fflush(stdout);
        }
}
