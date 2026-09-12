#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/jit/block_analysis_index.h"

using namespace swift::runtime;
using namespace swift::runtime::ir;
using swift::runtime::backend::arm64::BlockAnalysisIndex;

TEST_CASE("block use lists retain operand multiplicity and discard previous contents", "[block-analysis]") {
    IntrusivePtr<Block> first{new Block(0, Location{0x1000})};
    auto value = first->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
    auto sum = first->Add(value, value).SetType(ValueType::U64);
    first->SetHostGPR(sum, HostRegIndex(19), Imm{0u});
    BlockAnalysisIndex index;
    index.Build(first.get());
    const auto uses = index.Uses(value.Def());
    REQUIRE(uses.size() == 1);
    CHECK(uses[0].consumer == sum.Def());
    CHECK(uses[0].count == 2);
    IntrusivePtr<Block> second{new Block(0, Location{0x2000})};
    index.Build(second.get());
    CHECK(index.Uses(value.Def()).empty());
    CHECK(index.Instructions().empty());
}

TEST_CASE("reverse write search agrees with forward search at reads and partial writes", "[block-analysis]") {
    // 64 tiny combinations; no guest execution or timing loops.
    for (unsigned scenario = 0; scenario < 64; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0x1000})};
        auto value = block->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
        for (unsigned i = 0; i < 6; ++i) {
            const unsigned home = 19 + (i & 1);
            block->SetHostGPR(value, HostRegIndex(home), Imm{0u});
            if (scenario & (1u << i)) {
                if (i % 3 == 0) block->GetHostGPR(HostRegIndex(home), Imm{0u}).SetType(ValueType::U64);
                if (i % 3 == 1) block->SetHostGPR(value, HostRegIndex(home), Imm{1u});
                if (i % 3 == 2) block->UniformBarrier();
            }
        }
        BlockAnalysisIndex index;
        index.Build(block.get());
        auto observes = [](Inst& inst) { return inst.GetOp() == OpCode::UniformBarrier; };
        auto full = [](Inst& inst) { return inst.GetArg<Imm>(2).Get() == 0; };
        index.BuildWriteSuccessors(observes, full);
        const auto instructions = index.Instructions();
        for (size_t i = 0; i < instructions.size(); ++i) {
            auto* write = instructions[i];
            if (write->GetOp() != OpCode::SetHostGPR) continue;
            const auto home = write->GetArg<Imm>(1).Get();
            Inst* expected{};
            for (size_t j = i + 1; j < instructions.size(); ++j) {
                auto* later = instructions[j];
                if (observes(*later)) break;
                if (later->GetOp() == OpCode::GetHostGPR && later->GetArg<Imm>(0).Get() == home) break;
                if (later->GetOp() == OpCode::SetHostGPR && later->GetArg<Imm>(1).Get() == home) {
                    if (full(*later)) expected = later;
                    break;
                }
            }
            CHECK(index.FollowingWrite(write) == expected);
        }
    }
}
