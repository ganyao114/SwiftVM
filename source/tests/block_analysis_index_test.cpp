#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/backend/arm64/jit/guest_state_map.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/reg_alloc.h"

using namespace swift::runtime;
using namespace swift::runtime::ir;
using swift::runtime::backend::arm64::BlockAnalysisIndex;

TEST_CASE("canonical FPR sharing follows aliases and keeps banks distinct", "[block-analysis][register-sharing]") {
    using namespace swift::runtime::backend;
    IntrusivePtr<Block> block{new Block(0, Location{0x10a0})};
    const auto root = block->LoadImm(Imm{swift::u64{1}}).SetType(ValueType::U64);
    const auto alias = block->BitCast(root).SetType(ValueType::U64);
    const auto nested = block->BitCast(alias).SetType(ValueType::U64);
    const auto other = block->LoadImm(Imm{swift::u64{2}}).SetType(ValueType::U64);
    const auto floating = block->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
    block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.MapRegister(root.Id(), HostFPR{14});
    allocation.MapReference(root.Id(), alias.Id());
    allocation.MapReference(alias.Id(), nested.Id());
    allocation.MapRegister(other.Id(), HostFPR{15});
    allocation.MapRegister(floating.Id(), HostGPR{14});
    REQUIRE(allocation.SharesFPR(root, nested));
    REQUIRE(allocation.SharesFPR(nested, Value{root}.SetCastType(ValueType::U8)));
    REQUIRE_FALSE(allocation.SharesFPR(root, other));
    REQUIRE_FALSE(allocation.SharesFPR(root, floating));
    REQUIRE_FALSE(allocation.SharesFPR(floating, root));
    REQUIRE_FALSE(allocation.SharesFPR(Value{}, root));
    REQUIRE_FALSE(allocation.SharesFPR(root, Value{}));
}

TEST_CASE("block membership preserves sparse spans and incoming pointer identity", "[block-analysis]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x1090})};
    IntrusivePtr<Block> parent{new Block(0, Location{0x1098})};
    const auto first = block->LoadImm(Imm{swift::u64{1}}).SetType(ValueType::U64);
    const auto middle = block->LoadImm(Imm{swift::u64{2}}).SetType(ValueType::U64);
    const auto last = block->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
    const auto incoming = parent->LoadImm(Imm{swift::u64{4}}).SetType(ValueType::U64);
    BlockAnalysisIndex index;
    for (unsigned spacing : {1u, 3u}) for (unsigned base : {0u, 1000u}) {
        first.Def()->SetId(base); middle.Def()->SetId(base + spacing); last.Def()->SetId(base + 2 * spacing);
        index.Build(block.get());
        REQUIRE(index.Contains(first.Def())); REQUIRE(index.Contains(middle.Def())); REQUIRE(index.Contains(last.Def()));
        REQUIRE_FALSE(index.Contains(nullptr));
        incoming.Def()->SetId(middle.Id()); REQUIRE_FALSE(index.Contains(incoming.Def()));
        incoming.Def()->SetId(last.Id() + 1); REQUIRE_FALSE(index.Contains(incoming.Def()));
        incoming.Def()->SetId(base + 1);
        REQUIRE_FALSE(index.Contains(incoming.Def()));
    }
}

TEST_CASE("value visitation preserves order multiplicity and operand views", "[block-analysis][value-visitor]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x1080})};
    auto first = block->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
    auto second = block->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
    auto byte = Value{first}.SetCastType(ValueType::U8);
    auto word = Value{second}.SetCastType(ValueType::U32);
    auto direct = block->CallLambda(Lambda{first}, byte, word, byte);
    Params params;
    params.Push(word);
    params.Push(Imm{swift::u64{99}});
    params.Push(byte);
    params.Push(word);
    auto nested = block->CallDynamic(Lambda{first}, params);
    auto immediate = block->CallLambda(Lambda{Imm{swift::u64{1}}}, byte);
    const auto check = [](Inst* inst, std::initializer_list<Value> expected) {
        std::vector<Value> seen;
        inst->ForEachValue([&](Value value) { seen.push_back(value); });
        REQUIRE(seen.size() == expected.size());
        size_t at = 0;
        for (auto value : expected) {
            REQUIRE(seen[at].Def() == value.Def());
            REQUIRE(seen[at].Type() == value.Type());
            ++at;
        }
        // Changing the callback's value must not change an operand's view.
        inst->ForEachValue([](Value value) { value = value.SetCastType(ValueType::U16); });
        const auto copies = inst->GetValues();
        REQUIRE(copies.size() == expected.size());
        at = 0;
        for (auto value : expected) REQUIRE(copies[at++].Type() == value.Type());
    };
    check(direct.Def(), {first, byte, word, byte});
    check(nested.Def(), {first, word, byte, word});
    check(immediate.Def(), {byte});
    check(first.Def(), {});
    block->ReIdInstr();
    for (bool compact : {false, true}) {
        BlockAnalysisIndex::CompactUses scratch;
        BlockAnalysisIndex index;
        index.Build(block.get(), compact ? &scratch : nullptr);
        REQUIRE(index.Uses(first.Def()).size() == 3);
        REQUIRE(index.Uses(first.Def())[0].count == 3);
        REQUIRE(index.Uses(first.Def())[1].count == 2);
        REQUIRE(index.Uses(first.Def())[2].count == 1);
        REQUIRE(index.Uses(second.Def()).size() == 2);
        REQUIRE(index.Uses(second.Def())[0].count == 1);
        REQUIRE(index.Uses(second.Def())[1].count == 2);
    }
}

TEST_CASE("resolved consumers combine direct and nested cast operands", "[block-analysis]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x1000})};
    auto source = block->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
    auto first = block->BitCast(source).SetType(ValueType::U64);
    auto second = block->BitCast(first).SetType(ValueType::U64);
    auto sum = block->Add(source, second).SetType(ValueType::U64);
    block->ReIdInstr();
    for (bool compact : {false, true}) {
        BlockAnalysisIndex::CompactUses scratch;
        BlockAnalysisIndex index; index.Build(block.get(), compact ? &scratch : nullptr);
        const auto uses = index.ResolvedUses(source.Def());
        REQUIRE(uses.size() == 3);
        REQUIRE(uses.back().consumer == sum.Def()); REQUIRE(uses.back().count == 2);
        REQUIRE(index.LastInstructionUse(source.Def()) == sum.Id());
        REQUIRE(index.ResolvedUses(first.Def()).empty());
    }
}

TEST_CASE("indexed last use agrees with resolved operand scans at proof boundaries", "[block-analysis][publication]") {
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> parent{new Block(0, Location{0x1100})};
        IntrusivePtr<Block> block{new Block(0, Location{0x1108})};
        auto root = (scenario & 1 ? parent.get() : block.get())->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
        auto alias = root;
        if (scenario & 2) {
            alias = block->BitCast(root).SetType(ValueType::U64);
            alias = block->BitCast(alias).SetType(ValueType::U64);
        }
        const auto consumer = block->Add(root, alias).SetType(ValueType::U64);
        if (scenario & 4) block->SaveFlags(alias, Flags::NZCV);
        block->SetHostGPR(consumer, HostRegIndex(20), Imm{0u});
        parent->ReIdInstr();
        block->ReIdInstr();
        if (scenario & 1) root.Def()->SetId(500); // incoming IDs can exceed local uses
        const auto resolve = [](Value value) {
            while (value.Def() && value.Def()->IsBitCastOperation()) value = value.Def()->GetArg<Value>(0);
            return value.Def();
        };
        unsigned expected = root.Id();
        for (auto& node : block->GetInstList()) for (auto use : node.GetValues())
            if (resolve(use) == root.Def()) expected = std::max<unsigned>(expected, node.Id());
        for (bool compact : {false, true}) {
            BlockAnalysisIndex::CompactUses scratch;
            BlockAnalysisIndex index;
            index.Build(block.get(), compact ? &scratch : nullptr);
            REQUIRE(index.LastInstructionUse(root.Def()) == expected);
            const auto window = index.Between(consumer.Id(), block->GetInstList().back().Id());
            REQUIRE(window.size() == ((scenario & 4) ? 1 : 0));
            if (!window.empty()) REQUIRE(window.front()->GetOp() == OpCode::SaveFlags);
        }
    }
}

TEST_CASE("resolved block uses follow incoming bitcasts and reset to direct operands", "[block-analysis]") {
    IntrusivePtr<Block> first{new Block(0, Location{0x1000})};
    auto source = first->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
    auto cast = first->BitCast(source).SetType(ValueType::U64);
    first->ReIdInstr();
    IntrusivePtr<Block> second{new Block(0, Location{0x2000})};
    auto sum = second->Add(cast, cast).SetType(ValueType::U64);
    second->ReIdInstr();
    BlockAnalysisIndex index; index.Build(second.get());
    REQUIRE(index.ResolvedUses(source.Def()).size() == 1);
    REQUIRE(index.ResolvedUses(source.Def()).front().consumer == sum.Def());
    REQUIRE(index.ResolvedUses(source.Def()).front().count == 2);
    REQUIRE(index.Uses(source.Def()).empty());
    IntrusivePtr<Block> direct{new Block(0, Location{0x3000})};
    auto value = direct->LoadImm(Imm{swift::u64{5}}).SetType(ValueType::U64);
    auto result = direct->Add(value, value).SetType(ValueType::U64);
    direct->ReIdInstr(); index.Build(direct.get());
    REQUIRE(index.ResolvedUses(source.Def()).empty());
    REQUIRE(index.ResolvedUses(value.Def()).front().consumer == result.Def());
    REQUIRE(index.ResolvedUses(value.Def()).front().count == 2);
}

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

TEST_CASE("compact consumers retain multiplicity through capacity fallback", "[block-analysis]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x1000})};
    auto left = block->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
    auto right = block->LoadImm(Imm{swift::u64{4}}).SetType(ValueType::U64);
    for (unsigned i = 0; i < 64; ++i) {
        auto sum = block->Add(left, left).SetType(ValueType::U64);
        block->Add(right, sum).SetType(ValueType::U64);
    }
    block->ReIdInstr();
    BlockAnalysisIndex::CompactUses scratch;
    BlockAnalysisIndex index; index.Build(block.get(), &scratch);
    REQUIRE(index.DirectUseCount(left.Def()) == 128);
    REQUIRE(index.DirectUseCount(right.Def()) == 64);
    REQUIRE(index.ResolvedUses(left.Def()).size() == 64);
    for (const auto& use : index.ResolvedUses(left.Def())) REQUIRE(use.count == 2);
    index.Build(block.get(), &scratch);
    REQUIRE(index.DirectUseCount(left.Def()) == 128);
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
