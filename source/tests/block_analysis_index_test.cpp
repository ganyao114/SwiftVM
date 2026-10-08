#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/backend/arm64/jit/guest_state_map.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/reg_alloc.h"

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

TEST_CASE("fixed home intervals preserve partial writes, exchanges and exact helper barriers",
          "[block-analysis][pinned-gpr]") {
    using swift::runtime::backend::arm64::GuestStateMap;
    using swift::runtime::backend::arm64::HelperCallContract;
    IntrusivePtr<Block> block{new Block(0, Location{0x3000})};
    auto value = block->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
    for (unsigned i = 0; i < 80; ++i) block->Nop();
    auto* full = block->AppendInst(OpCode::SetHostGPR, value, HostRegIndex(20), Imm{0u});
    block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
    auto* partial = block->AppendInst(OpCode::SetHostGPR, value, HostRegIndex(20), Imm{1u});
    auto* call = block->AppendInst(OpCode::CallLambda, Lambda{Imm{swift::u64{1}}});
    auto* resident = block->AppendInst(OpCode::CallLambda,
            Lambda{DataClass{Imm{swift::u64{1}}},
                   HelperCallTraits{.host_registers = HostRegisterEffect::PreservesPinnedState}});
    auto* leaf = block->AppendInst(OpCode::CallLambda,
            Lambda{DataClass{Imm{swift::u64{1}}},
                   HelperCallTraits{.abi = HelperABI::PreserveAllLeaf}});
    auto left = block->LoadUniform(Uniform{0, ValueType::V128});
    auto right = block->LoadUniform(Uniform{16, ValueType::V128});
    auto* sse = block->Sse42Str(left, right, Imm{swift::u64{0x02}}).Def();
    auto* exchange = block->AppendInst(OpCode::XchgBarrier, Imm{swift::u32{0x10}});
    block->ReIdInstr();
    std::vector<swift::u32> boundaries{0, 1, block->MaxInstrId()};
    for (auto* inst : {full, partial, call, resident, leaf, sse, exchange}) {
        boundaries.push_back(inst->Id() - 1);
        boundaries.push_back(inst->Id());
        boundaries.push_back(inst->Id() + 1);
    }

    GuestStateMap state;
    for (bool leaf_abi : {false, true}) {
        FeatureSet features{}; features.helper_leaf_abi = leaf_abi;
        state.Analyze(block.get(), features);
        // Three full traversals followed by another query exercise the index.
        // The independent forward oracle retains old semantics.
        for (unsigned i = 0; i < 4; ++i)
            REQUIRE(state.FixedHomeSurvives(31, 0, block->MaxInstrId()));
        for (swift::u32 home : {0u, 2u, 7u, 9u, 19u, 20u, 22u, 23u, 29u, 31u}) {
            for (auto after : boundaries) for (auto before : boundaries) {
                bool expected = true;
                for (auto& inst : block->GetInstList()) {
                    if (inst.Id() <= after || inst.Id() >= before) continue;
                    const bool clobbers = inst.GetOp() == OpCode::XchgBarrier
                            ? (swift::runtime::backend::FixedGPRClobbers(inst, features) & (1u << home)) != 0
                            : (inst.GetOp() == OpCode::SetHostGPR && inst.GetArg<Imm>(1).Get() == home) ||
                              (home <= 9 && HelperCallContract::InstructionClobbersGPR(inst, home, features));
                    if (clobbers) { expected = false; break; }
                }
                CHECK(state.FixedHomeSurvives(home, after, before) == expected);
            }
        }
        CHECK_FALSE(state.FixedHomeSurvives(20, partial->Id() - 1, partial->Id() + 1));
        CHECK(state.FixedHomeSurvives(20, partial->Id(), partial->Id() + 1));
        // Function analysis can also change the helper ABI between queries.
        features.helper_leaf_abi = !leaf_abi;
        state.AnalyzeFunction(nullptr, features);
        for (unsigned i = 0; i < 3; ++i)
            REQUIRE(state.FixedHomeSurvives(31, 0, block->MaxInstrId()));
        CHECK(state.FixedHomeSurvives(9, leaf->Id() - 1, leaf->Id() + 1) ==
              !HelperCallContract::InstructionClobbersGPR(*leaf, 9, features));
    }
    IntrusivePtr<Block> next{new Block(0, Location{0x4000})};
    for (unsigned i = 0; i < 100; ++i) next->Nop();
    next->ReIdInstr();
    state.Analyze(next.get(), FeatureSet{});
    for (unsigned i = 0; i < 4; ++i)
        CHECK(state.FixedHomeSurvives(20, 0, next->MaxInstrId()));
}
