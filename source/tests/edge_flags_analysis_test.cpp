#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_set>
#include "runtime/backend/arm64/alloc/edge_flags_analysis.h"

using namespace swift::runtime;
using namespace swift::runtime::ir;
using namespace swift::runtime::backend::arm64;

TEST_CASE("edge target facts preserve observation overwrite and commit order", "[edge-flags-plan]") {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xc300})};
        const auto value = block->LoadImm(Imm{7u}).SetType(ValueType::U32);
        if (scenario == 1) (void)block->TestFlags(Flags::Carry);
        if (scenario == 2) (void)block->LoadMemory(Operand{Imm{swift::u64{0}}});
        if (scenario == 3) {
            block->ClearFlags(Flags::CV);
        } else if (scenario == 4) {
            const auto branch = block->Goto(block->CondSet(Cond::EQ));
            block->BindLabel(branch);
        } else block->SaveFlags(value, Flags::NZCV);
        if (scenario != 5) block->AdvancePC(Imm{swift::u64{1}});
        block->ReIdInstr();
        const auto facts = AnalyzeBlockEdgeFlags(*block, [](const Inst& inst) { return inst.GetOp() == OpCode::LoadMemory; });
        REQUIRE(facts.barrier_before_commit == (scenario == 2 || scenario == 4));
        REQUIRE(facts.commits_before_fault == (scenario != 2 && scenario != 4 && scenario != 5));
        REQUIRE(facts.observed_nzcv_mask == (scenario == 1 ? 2 : scenario == 4 ? 15 : 0));
        REQUIRE(facts.overwrite_before_observe == (scenario == 2 || scenario == 4 ? 0 : scenario == 3 ? 0x30000000u : 0xf0000000u));
    }
}

TEST_CASE("token overwrite and transparency facts retain helper and boundary rules", "[edge-flags-plan]") {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xc308})};
        if (scenario == 1) {
            const auto value = block->LoadImm(Imm{7u}).SetType(ValueType::U32);
            (void)block->GetFlags(value, Flags::All);
        }
        if (scenario == 2) (void)block->CallLambda(Lambda{Imm{swift::u64{1}}});
        if (scenario == 3) block->AdvancePC(Imm{swift::u64{1}});
        if (scenario != 4) block->ClearFlags(Flags::Parity | Flags::AuxiliaryCarry);
        block->ReIdInstr();
        const auto overwritten = AnalyzeBlockTokenOverwrite(*block, [](const Inst& inst) { return inst.GetOp() == OpCode::CallLambda; });
        REQUIRE(overwritten == (scenario == 0));
        REQUIRE(AnalyzeBlockFlagsTransparent(*block) == (scenario == 4));
        REQUIRE_FALSE((overwritten && AnalyzeBlockFlagsTransparent(*block)));
    }
}

TEST_CASE("edge acceptance tables preserve early observation and partial overwrites", "[edge-flags-plan]") {
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xc310})};
        if (scenario == 1 || scenario == 3) (void)block->TestFlags(Flags::NZCV);
        if (scenario == 4) block->AdvancePC(Imm{swift::u64{1}});
        if (scenario == 2 || scenario == 3 || scenario == 7) block->ClearFlags(Flags::CV);
        if (scenario == 2 || scenario == 4) (void)block->TestFlags(Flags::NZCV);
        if (scenario == 5 || scenario == 6) {
            const auto value = block->LoadImm(Imm{7u}).SetType(ValueType::U32);
            if (scenario == 5) {
                block->SaveFlags(value, Flags::NZ);
                block->ClearFlags(Flags::CV);
                (void)block->TestFlags(Flags::NZCV);
            } else block->BranchOnlyFlags(value, Flags::NZCV);
        }
        if (scenario == 7) block->InvertCarry();
        constexpr swift::u16 expected[] = {0, 0, 0x000f, 0, 1, 0xffff, 0xffff, 0x000f};
        REQUIRE(AnalyzeBlockEdgeAcceptance(*block) == expected[scenario]);
    }
}

TEST_CASE("edge acceptance resolves fan in external exits and cycles without recursion", "[edge-flags-plan]") {
    const std::vector<EdgeAcceptanceNode> graph{
        {0, 1}, {0, 2}, {0x101, 3}, {0x202, 2}, {0, 1}, {0, UINT32_MAX}, {0, 6},
    };
    const auto result = ResolveEdgeAcceptance(graph);
    REQUIRE(result == std::vector<swift::u16>{0x303, 0x303, 0x303, 0x303, 0x303, 0, 0});

    // Differential oracle is the former per-query visited walk, over arbitrary
    // local acceptance tables and topology (including external successors).
    swift::u32 random = 0x31415926;
    auto next = [&] { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    for (unsigned trial = 0; trial < 64; ++trial) {
        std::vector<EdgeAcceptanceNode> nodes(32);
        for (auto& node : nodes) {
            node.accepted = static_cast<swift::u16>(next());
            node.successor = next() % 35;
        }
        std::vector<swift::u16> expected(nodes.size());
        for (unsigned start = 0; start < nodes.size(); ++start) {
            for (unsigned mask = 0; mask < 16; ++mask) {
                std::vector<bool> visited(nodes.size());
                auto target = start;
                while (target < nodes.size() && !visited[target]) {
                    visited[target] = true;
                    if (nodes[target].accepted & (swift::u16{1} << mask)) {
                        expected[start] |= swift::u16{1} << mask;
                        break;
                    }
                    target = nodes[target].successor;
                }
            }
        }
        CAPTURE(trial);
        REQUIRE(ResolveEdgeAcceptance(nodes) == expected);
    }
    std::vector<EdgeAcceptanceNode> chain(65536);
    for (unsigned i = 0; i + 1 < chain.size(); ++i) chain[i].successor = i + 1;
    chain.back().accepted = 0x8000;
    const auto long_result = ResolveEdgeAcceptance(chain);
    REQUIRE(std::all_of(long_result.begin(), long_result.end(), [](auto value) { return value == 0x8000; }));
}

TEST_CASE("edge acceptance admission retains flag writes and nested conditional terminals", "[edge-flags-plan]") {
    for (unsigned scenario = 0; scenario < 10; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xc318})};
        if (scenario == 2) block->ClearFlags(Flags::Carry);
        if (scenario == 3) block->InvertCarry();
        if (scenario == 7) (void)block->TestFlags(Flags::Carry);
        if (scenario >= 4 && scenario != 9) block->SetCarry(block->LoadImm(Imm{swift::u8{1}}).SetType(ValueType::U8));
        Terminal terminal{terminal::Condition{Cond::EQ,
            terminal::LinkBlock{Location{0xc320}}, terminal::LinkBlock{Location{0xc328}}}};
        if (scenario == 0 || scenario == 6) terminal = terminal::LinkBlock{Location{0xc320}};
        if (scenario == 5) terminal = terminal::CheckHalt{terminal};
        if (scenario >= 8) {
            const auto selector = block->LoadImm(Imm{swift::u32{0}}).SetType(ValueType::U32);
            terminal = terminal::Switch{selector, {{Imm{swift::u32{0}}, terminal}}};
        }
        block->SetTerminal(terminal);
        const bool expected = (scenario >= 2 && scenario <= 5) || scenario == 7 || scenario == 8;
        REQUIRE(MayQueryEdgeAcceptance(*block) == expected);
        bool writes_flags;
        const bool combined = AnalyzeBlockFlagsTransparent(*block, &writes_flags);
        REQUIRE(combined == AnalyzeBlockFlagsTransparent(*block));
        REQUIRE(writes_flags == (scenario >= 2 && scenario != 9));
    }
}

namespace {
unsigned LegacyMask(Flags flags) {
    return (True(flags & Flags::Negate) ? 8 : 0) |
           (True(flags & Flags::Zero) ? 4 : 0) |
           (True(flags & Flags::Carry) ? 2 : 0) |
           (True(flags & Flags::Overflow) ? 1 : 0);
}
bool LegacyAccepts(const std::vector<IntrusivePtr<Block>>& blocks,
                   const std::vector<EdgeAcceptanceNode>& topology,
                   unsigned target, unsigned mask) {
    std::unordered_set<unsigned> visited;
    while (target < blocks.size() && visited.insert(target).second) {
        unsigned needed = mask;
        for (const auto& inst : blocks[target]->GetInstList()) {
            switch (inst.GetOp()) {
                case OpCode::GetFlags: case OpCode::CallLambda: case OpCode::CallLocation:
                case OpCode::CallDynamic: case OpCode::X87Op: case OpCode::TestFlags:
                case OpCode::TestNotFlags: case OpCode::Adc: case OpCode::Sbb:
                case OpCode::CondSelect: case OpCode::CondSet: return false;
                case OpCode::ClearFlags: needed &= ~LegacyMask(inst.GetArg<Flags>(0)); break;
                case OpCode::SaveFlags: needed &= ~LegacyMask(inst.GetArg<Flags>(1)); break;
                case OpCode::BranchOnlyFlags: return true;
                default: break;
            }
            if (!needed) return true;
        }
        target = topology[target].successor;
    }
    return false;
}
}

TEST_CASE("region edge acceptance preparation and query metrics", "[.edge-acceptance-metrics]") {
    // IR construction, topology and the already-cached transparency are outside
    // both timers. The new timer includes table preparation and chain resolution.
    for (unsigned shape = 0; shape < 3; ++shape) for (unsigned length : {8u, 32u, 128u}) {
        std::vector<IntrusivePtr<Block>> blocks;
        std::vector<EdgeAcceptanceNode> topology(length);
        for (unsigned i = 0; i < length; ++i) {
            blocks.emplace_back(new Block(0, Location{0xc400 + i}));
            for (unsigned instruction = 0; instruction < 32; ++instruction)
                blocks.back()->AdvancePC(Imm{swift::u64{1}});
            if (i + 1 == length) {
                if (shape == 1) (void)blocks.back()->TestFlags(Flags::NZCV);
                else blocks.back()->ClearFlags(Flags::CV);
            }
            topology[i].successor = shape == 2 ? length - 1 : i + 1;
            if (!AnalyzeBlockFlagsTransparent(*blocks.back())) topology[i].successor = UINT32_MAX;
        }
        if (shape == 1) topology[length / 2].successor = 0; // an unresolved transparent cycle
        std::vector<swift::u64> legacy_times, prepared_times;
        swift::u64 expected = 0;
        for (unsigned iteration = 0; iteration < 10; ++iteration) {
            for (unsigned order = 0; order < 2; ++order) {
                const bool prepared = (iteration + order) % 2;
                const auto begin = std::chrono::steady_clock::now();
                swift::u64 checksum = 0;
                std::vector<swift::u16> accepted;
                if (prepared) {
                    auto nodes = topology;
                    for (unsigned i = 0; i < length; ++i) nodes[i].accepted = AnalyzeBlockEdgeAcceptance(*blocks[i]);
                    accepted = ResolveEdgeAcceptance(nodes);
                }
                for (unsigned start = 0; start < length; ++start) for (unsigned mask = 1; mask < 16; ++mask) {
                    const bool result = prepared ? (accepted[start] & (swift::u16{1} << mask)) != 0
                                                 : LegacyAccepts(blocks, topology, start, mask);
                    checksum = checksum * 131 + result;
                }
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
                if (!iteration && !order) expected = checksum;
                REQUIRE(checksum == expected);
                if (iteration >= 2) (prepared ? prepared_times : legacy_times).push_back(ns);
            }
        }
        std::sort(legacy_times.begin(), legacy_times.end());
        std::sort(prepared_times.begin(), prepared_times.end());
        std::printf("edge-acceptance-metrics shape=%u length=%u hash=%llu legacy_ns=%llu prepared_ns=%llu\n",
                    shape, length, (unsigned long long)expected,
                    (unsigned long long)legacy_times[legacy_times.size() / 2],
                    (unsigned long long)prepared_times[prepared_times.size() / 2]);
        std::fflush(stdout);
    }
}
