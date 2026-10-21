#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include "runtime/backend/arm64/alloc/flags_token_plan.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("token parity demand analysis metrics", "[.token-demand-metrics]") {
    for (unsigned shape = 0; shape < 3; ++shape) for (unsigned length : {2u, 8u, 32u, 128u}) {
        IntrusivePtr<Block> block{new Block(0, Location{0xda00})};
        std::vector<Value> producers;
        std::vector<Inst*> publications;
        for (unsigned i = 0; i < length; ++i) {
            const auto producer = block->LoadImm(Imm{swift::u64{i}}).SetType(ValueType::U64);
            producers.push_back(producer);
            if (shape) for (unsigned j = 0; j < (shape == 2 ? 8 : 2); ++j)
                block->SaveFlags(producer, j & 1 ? Flags::Parity : Flags::NZ);
            block->SetHostGPR(producer, HostRegIndex(20), Imm{0u});
            publications.push_back(&block->GetInstList().back());
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        for (auto value : producers) allocation.MapRegister(value.Id(), HostGPR{20});
        for (auto* publication : publications) allocation.MarkHostWriteCoalesced(publication->Id());
        arm64::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        // Explicit unit-allocation contract; only the last publication has no
        // later definition/publication in its home. IR/index/RA stay outside timers.
        std::vector<swift::u64> legacy_times, cached_times;
        arm64::FlagsTokenPlan expected;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            for (unsigned order = 0; order < 2; ++order) {
                const bool cached = (iteration + order) % 2;
                const auto begin = std::chrono::steady_clock::now();
                auto plan = arm64::BuildFlagsTokenPlan(analysis, allocation, FeatureSet{}, nullptr,
                        cached ? &analysis.PseudoFlags() : nullptr);
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
                if (!iteration && !order) expected = plan;
                REQUIRE(plan.homes == expected.homes);
                REQUIRE(plan.point_homes == expected.point_homes);
                REQUIRE(plan.Retains(producers.back().Def(), 20, producers.back().Id()) == (shape != 0));
                if (iteration >= 5) (cached ? cached_times : legacy_times).push_back(ns);
            }
        }
        std::sort(legacy_times.begin(), legacy_times.end());
        std::sort(cached_times.begin(), cached_times.end());
        std::printf("token-analysis-metrics shape=%u length=%u homes=%zu legacy_ns=%llu cached_ns=%llu\n",
                shape, length, expected.homes.size(), (unsigned long long)legacy_times[legacy_times.size() / 2],
                (unsigned long long)cached_times[cached_times.size() / 2]);
        std::fflush(stdout);
    }
}
