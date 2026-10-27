#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/lower_flags.h"
#include "runtime/compile/analysis/block_index.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("token parity demand analysis metrics", "[.token-demand-metrics]") {
    for (unsigned shape = 0; shape < 3; ++shape) for (unsigned length : {2u, 8u, 32u, 128u}) {
        IntrusivePtr<Block> block{new Block(0, Location{0xda00})};
        std::vector<Value> producers;
        std::vector<const Inst*> publications;
        for (unsigned i = 0; i < length; ++i) {
            const auto producer = block->LoadImm(ValueType::U64, Imm{swift::u64{i}});
            producers.push_back(producer);
            if (shape) for (unsigned j = 0; j < (shape == 2 ? 8 : 2); ++j)
                block->SaveFlags(producer, j & 1 ? Flags::Parity : Flags::NZ);
            block->SetHostGPR(producer, HostRegIndex(20), Imm{0u});
            publications.push_back(&block->GetInstList().back());
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        for (auto value : producers) allocation.MapRegister(value.Id(), HostGPR{20});
        for (auto* publication : publications) allocation.MarkHostWriteCoalesced(publication->Id());
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        // Explicit unit-allocation contract; only the last publication has no
        // later definition/publication in its home. IR/index/RA stay outside timers.
        std::vector<swift::u64> times;
        arm64::FlagsTokenActions expected;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto begin = std::chrono::steady_clock::now();
            auto lowered = arm64::LowerFlagsTokens(analysis,
                                                   allocation.Workspace(),
                                                   allocation,
                                                   FeatureSet{},
                                                   publications,
                                                   analysis.PseudoFlags());
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - begin)
                                    .count();
            if (!iteration) expected = lowered;
            REQUIRE(lowered.retentions == expected.retentions);
            REQUIRE(lowered.Retains(producers.back().Id(), 20, producers.back().Id()) ==
                    (shape != 0));
            if (iteration >= 5) times.push_back(ns);
        }
        std::sort(times.begin(), times.end());
        std::printf("token-analysis-metrics shape=%u length=%u retentions=%zu median_ns=%llu\n",
                    shape,
                    length,
                    expected.retentions.size(),
                    (unsigned long long)times[times.size() / 2]);
        std::fflush(stdout);
    }
}
