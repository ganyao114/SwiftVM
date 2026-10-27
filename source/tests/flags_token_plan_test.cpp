#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/lower_flags.h"
#include "runtime/backend/arm64/compile/lower_predicates.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/compile/workspace.h"
#include "runtime/ir/hir_builder.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("predicate and parity token actions seal unordered block payloads together",
          "[boolean-select][flags-token-plan]") {
    const FeatureSet features;
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0xbca0}, Location{0xbcc1});
    std::array<Value, 2> selected, conditions, producers;
    std::array<const Inst*, 2> publications;
    std::array<const Block*, 2> blocks;
    const auto append = [&](unsigned slot) {
        blocks[slot] = function->GetCurrentBlock()->GetBlock();
        const auto one = function->LoadImm(ValueType::U8, Imm{swift::u64{1}});
        const auto zero = function->LoadImm(ValueType::U8, Imm{swift::u64{0}});
        conditions[slot] = function->CondSet(ValueType::U8, slot ? Cond::NE : Cond::EQ);
        selected[slot] = function->Select(ValueType::U8, conditions[slot], one, zero);
        function->StoreUniform(Uniform{slot, ValueType::U8}, selected[slot]);
        producers[slot] = function->LoadImm(ValueType::U64, Imm{swift::u64{42}});
        function->SaveFlags(producers[slot], Flags::Parity);
        function->SetHostGPR(producers[slot], HostRegIndex(20), Imm{0u});
        publications[slot] = &function->GetCurrentBlock()->GetBlock()->GetInstList().back();
    };
    append(0);
    auto* second = builder.LinkBlock(terminal::LinkBlock{Location{0xbcc0}});
    builder.SetCurBlock(second);
    append(1);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    AllocationFixture allocation{function->MaxInstrCount(), GPRSMask{}, FPRSMask{}, features};
    std::array<compile::BlockAnalysisIndex, 2> indices;
    compile::CompileWorkspace predicate_workspace;
    std::array<arm64::SelectActions, 2> selects;
    std::array<arm64::FlagsTokenActions, 2> tokens;
    for (unsigned slot = 0; slot < 2; ++slot) {
        allocation.MapRegister(producers[slot].Id(), HostGPR{20});
        allocation.MarkHostWriteCoalesced(publications[slot]->Id());
        indices[slot].Build(blocks[slot], nullptr, true);
        selects[slot] = arm64::LowerSelects(
                indices[slot], predicate_workspace, [](const Inst&) { return true; });
        const std::array<const Inst*, 1> candidates{publications[slot]};
        tokens[slot] = arm64::LowerFlagsTokens(indices[slot],
                                               allocation.Workspace(),
                                               allocation,
                                               features,
                                               candidates,
                                               indices[slot].PseudoFlags());
    }
    allocation.BeginTargetPlanning();
    for (const auto slot : {1u, 0u}) {
        allocation.StoreSelects(selects[slot]);
        allocation.StoreFlagsTokens(tokens[slot]);
        REQUIRE(allocation.Actions().Select(selected[slot].Id()));
        REQUIRE(allocation.Actions().RetainsFlagsToken(producers[slot].Id(), 20, UINT32_MAX));
    }
    allocation.EndTargetPlanning();
    for (unsigned slot = 0; slot < 2; ++slot) {
        const auto* action = allocation.Actions().Select(selected[slot].Id());
        REQUIRE(action);
        REQUIRE(action->boolean);
        REQUIRE(action->DirectCondition() == (slot ? Cond::NE : Cond::EQ));
        REQUIRE(allocation.Actions().InstructionCovered(conditions[slot].Id()));
        REQUIRE(allocation.Actions().RetainsFlagsToken(
                producers[slot].Id(), 20, publications[slot]->Id()));
        REQUIRE_FALSE(allocation.Actions().RetainsFlagsToken(
                producers[slot].Id(), 21, publications[slot]->Id()));
    }
    REQUIRE_FALSE(allocation.Actions().RetainsFlagsToken(UINT32_MAX, 20, 0));
    allocation.ResetAllocations();
    REQUIRE_FALSE(allocation.Actions().Select(selected[0].Id()));
    REQUIRE_FALSE(allocation.Actions().InstructionCovered(conditions[1].Id()));
    REQUIRE_FALSE(allocation.Actions().RetainsFlagsToken(producers[1].Id(), 20, producers[1].Id()));
    allocation.BeginTargetPlanning();
    allocation.StoreSelects(selects[0]);
    allocation.StoreFlagsTokens(tokens[0]);
    allocation.AbortTargetPlanning();
    REQUIRE_FALSE(allocation.Actions().Select(selected[0].Id()));
    REQUIRE_FALSE(allocation.Actions().RetainsFlagsToken(producers[0].Id(), 20, producers[0].Id()));
}

TEST_CASE("flags token plans preserve publication and suffix ownership contracts", "[flags-token-plan]") {
    for (const bool narrow : {false, true}) {
        for (const unsigned scenario : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
            IntrusivePtr<Block> block{new Block(0, Location{0xbc00})};
            const auto producer =
                    block->LoadImm(narrow ? ValueType::U32 : ValueType::U64, Imm{swift::u64{42}});
            auto stored = producer;
            if (narrow) stored = block->ZeroExtend32To64(ValueType::U64, producer);
            if (scenario != 6) block->SaveFlags(producer, Flags::Parity);
            block->SetHostGPR(stored, HostRegIndex(20), Imm{0u});
            auto* publication = &block->GetInstList().back();
            Value other;
            const Inst* second_publication = nullptr;
            if (scenario == 1) {
                // A second publication to another home is independently safe.
                block->SetHostGPR(stored, HostRegIndex(22), Imm{0u});
                second_publication = &block->GetInstList().back();
            } else if (scenario == 2) {
                block->SetHostGPR(stored, HostRegIndex(20), Imm{0u});
                second_publication = &block->GetInstList().back();
            } else if (scenario == 3) {
                other = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
            } else if (scenario == 4) {
                block->SetHostGPR(stored, HostRegIndex(20), Imm{8u});
            } else if (scenario == 5) {
                // An uncoalesced first publication cannot retain the home.
            }
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
            allocation.MapRegister(producer.Id(), HostGPR{20});
            if (narrow) allocation.MapRegister(stored.Id(), HostGPR{20});
            if (other.Defined()) allocation.MapRegister(other.Id(), HostGPR{20});
            if (scenario != 5) allocation.MarkHostWriteCoalesced(publication->Id());
            if (second_publication) allocation.MarkHostWriteCoalesced(second_publication->Id());
            compile::BlockAnalysisIndex analysis;
            analysis.Build(block.get(), nullptr, true);
            std::vector<const Inst*> publications;
            for (auto& node : block->GetInstList())
                if (node.GetOp() == OpCode::SetHostGPR) publications.push_back(&node);
            const auto plan = arm64::LowerFlagsTokens(analysis,
                                                      allocation.Workspace(),
                                                      allocation,
                                                      FeatureSet{},
                                                      publications,
                                                      analysis.PseudoFlags());
            std::reverse(publications.begin(), publications.end());
            const auto prepared = arm64::LowerFlagsTokens(analysis,
                                                          allocation.Workspace(),
                                                          allocation,
                                                          FeatureSet{},
                                                          publications,
                                                          analysis.PseudoFlags());
            REQUIRE(plan.Retains(producer.Id(), 20, producer.Id()) == (scenario <= 1));
            REQUIRE(plan.Retains(producer.Id(), 22, producer.Id()) == (scenario == 1));
            REQUIRE(prepared.Retains(producer.Id(), 20, producer.Id()) ==
                    plan.Retains(producer.Id(), 20, producer.Id()));
            REQUIRE(prepared.Retains(producer.Id(), 22, producer.Id()) ==
                    plan.Retains(producer.Id(), 22, producer.Id()));
            REQUIRE(prepared.retentions == plan.retentions);
            if (scenario == 0) {
                IntrusivePtr<Block> foreign{new Block(0, Location{0xbc80})};
                foreign->SetHostGPR(stored, HostRegIndex(20), Imm{0u});
                auto* same_id = &foreign->GetInstList().back();
                foreign->SetTerminal(terminal::ReturnToHost{});
                compile::CompileWorkspace editing_workspace;
                compile::RewriteContext editing{foreign.get(), editing_workspace, nullptr, false};
                editing.Change(
                        foreign.get(), same_id, [&](Inst& node) { node.SetId(publication->Id()); });
                const std::array<const Inst*, 1> foreign_publication{same_id};
                const auto rejected = arm64::LowerFlagsTokens(analysis,
                                                              allocation.Workspace(),
                                                              allocation,
                                                              FeatureSet{},
                                                              foreign_publication,
                                                              analysis.PseudoFlags());
                REQUIRE(rejected.retentions.empty());
            }
            allocation.BeginTargetPlanning();
            allocation.StoreFlagsTokens(plan);
            REQUIRE(allocation.Actions().RetainsFlagsToken(producer.Id(), 20, producer.Id()) ==
                    (scenario <= 1));
            allocation.EndTargetPlanning();
            REQUIRE(allocation.Actions().RetainsFlagsToken(producer.Id(), 22, producer.Id()) ==
                    (scenario == 1));
            allocation.ResetAllocations();
            REQUIRE_FALSE(allocation.Actions().RetainsFlagsToken(producer.Id(), 20, producer.Id()));
            if (scenario == 6) REQUIRE(plan.retentions.empty());
            REQUIRE_FALSE(plan.Retains(producer.Id(), 31, producer.Id()));
            REQUIRE_FALSE(plan.Retains(UINT32_MAX, 20, producer.Id()));
        }
    }
}

TEST_CASE("flags token suffixes distinguish earlier owners and single wrappers",
          "[flags-token-plan][regression]") {
    struct Trace {
        std::string_view events;
        bool retains;
    };
    // P is the selected publication; Q/p are other full/partial writes to
    // its home. W is a real zero-extension definition, A a BitCast alias,
    // D an unrelated definition in the same GPR, F/R a foreign definition/
    // publication, and X a fixed-home clobber. These complete traces have
    // explicit expected results, independent of the summary representation.
    constexpr Trace traces[]{{"P", true},    {"WP", true},     {"PW", true},     {"AP", true},
                             {"WAP", true},  {"DP", false},    {"PD", false},    {"DWP", false},
                             {"WDP", false}, {"WWP", false},   {"PWW", false},   {"FWP", true},
                             {"WPF", true},  {"RP", true},     {"PR", true},     {"QP", false},
                             {"PQ", false},  {"pP", false},    {"Pp", false},    {"XP", false},
                             {"PX", false},  {"AFWPRA", true}, {"FWDPR", false}, {"DDDP", false}};
    for (const auto trace : traces)
        for (const unsigned prefix : {0u, 3u})
            for (const bool sparse : {false, true}) {
                CAPTURE(trace.events, prefix, sparse);
                IntrusivePtr<Block> block{new Block{0, Location{0xbc28}}};
                std::vector<Value> same_home, foreign;
                for (unsigned i = 0; i < prefix; ++i) {
                    const auto old = block->LoadImm(ValueType::U64, Imm{swift::u64{i}});
                    same_home.push_back(old);
                    block->SetHostGPR(old, HostRegIndex(20), Imm{0u});
                }
                if (prefix) block->XchgBarrier(Imm{3u});
                const auto producer = block->LoadImm(ValueType::U32, Imm{swift::u64{42}});
                same_home.push_back(producer);
                block->SaveFlags(producer, Flags::Parity);
                auto stored = producer;
                const Inst* publication{};
                for (const char event : trace.events) {
                    switch (event) {
                        case 'P':
                        case 'Q':
                        case 'p':
                        case 'R':
                            block->SetHostGPR(stored,
                                              HostRegIndex(event == 'R' ? 22 : 20),
                                              Imm{event == 'p' ? 8u : 0u});
                            if (event == 'P') publication = &block->GetInstList().back();
                            break;
                        case 'W':
                            stored = block->ZeroExtend32To64(ValueType::U64, producer);
                            same_home.push_back(stored);
                            break;
                        case 'A':
                            stored = block->BitCast(ValueType::U64, stored);
                            same_home.push_back(stored);
                            break;
                        case 'D':
                            same_home.push_back(block->LoadImm(ValueType::U64, Imm{swift::u64{7}}));
                            break;
                        case 'F':
                            foreign.push_back(block->LoadImm(ValueType::U64, Imm{swift::u64{9}}));
                            break;
                        case 'X':
                            block->XchgBarrier(Imm{3u});
                            break;
                    }
                }
                block->SetTerminal(terminal::ReturnToHost{});
                block->ReIdInstr();
                if (sparse) {
                    compile::CompileWorkspace workspace;
                    compile::RewriteContext edits{block.get(), workspace, nullptr, false};
                    for (const auto& node : block->GetInstList()) {
                        const auto id = static_cast<swift::u16>(100 + node.Id() * 3);
                        edits.Change(block.get(), &node, [id](Inst& target) { target.SetId(id); });
                    }
                }
                AllocationFixture allocation{block->GetInstList().back().Id() + 1u,
                                             GPRSMask{},
                                             FPRSMask{},
                                             FeatureSet{}};
                for (const auto value : same_home) allocation.MapRegister(value.Id(), HostGPR{20});
                for (const auto value : foreign) allocation.MapRegister(value.Id(), HostGPR{22});
                REQUIRE(publication);
                allocation.MarkHostWriteCoalesced(publication->Id());
                compile::BlockAnalysisIndex analysis;
                analysis.Build(block.get(), nullptr, true);
                // Unselected and partial writes remain relevant even when
                // the caller's candidate span contains only the chosen one.
                const std::array<const Inst*, 1> candidates{publication};
                const auto plan = arm64::LowerFlagsTokens(analysis,
                                                          allocation.Workspace(),
                                                          allocation,
                                                          FeatureSet{},
                                                          candidates,
                                                          analysis.PseudoFlags());
                REQUIRE(plan.Retains(producer.Id(), 20, publication->Id()) == trace.retains);
                REQUIRE(plan.retentions.size() == (trace.retains ? 1u : 0u));
            }
}

TEST_CASE("flags token plan rejects fixed home clobbers after publication", "[flags-token-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbc08})};
    const auto producer = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
    block->SaveFlags(producer, Flags::Parity);
    block->SetHostGPR(producer, HostRegIndex(20), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->XchgBarrier(Imm{3u});
    auto* call = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    FeatureSet features{};
    REQUIRE((FixedGPRClobbers(*call, features) & (1u << 20)) != 0);
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, features};
    allocation.MapRegister(producer.Id(), HostGPR{20});
    allocation.MarkHostWriteCoalesced(publication->Id());
    compile::BlockAnalysisIndex analysis;
    analysis.Build(block.get(), nullptr, true);
    const std::array<const Inst*, 1> publications{publication};
    const auto plan = arm64::LowerFlagsTokens(analysis,
                                              allocation.Workspace(),
                                              allocation,
                                              features,
                                              publications,
                                              analysis.PseudoFlags());
    REQUIRE(plan.retentions.empty());
    allocation.BeginTargetPlanning();
    allocation.StoreFlagsTokens(plan);
    allocation.EndTargetPlanning();
    REQUIRE_FALSE(allocation.Actions().RetainsFlagsToken(producer.Id(), 20, producer.Id()));
    REQUIRE_FALSE(plan.Retains(producer.Id(), 20, producer.Id()));
}

TEST_CASE("cached token demand distinguishes saved parity from branch-only parity", "[flags-token-plan][pseudo-flags-plan]") {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xbc18})};
        const auto producer = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
        if (scenario == 1 || scenario == 3) block->SaveFlags(producer, Flags::Parity);
        if (scenario == 2 || scenario == 3) block->BranchOnlyFlags(producer, Flags::Parity);
        if (scenario == 4) {
            block->SaveFlags(producer, Flags::NZCV);
            block->BranchOnlyFlags(producer, Flags::Parity);
        }
        if (scenario == 5) (void)block->GetFlags(producer.Type(), producer, Flags::Parity);
        block->SetHostGPR(producer, HostRegIndex(20), Imm{0u});
        auto* publication = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(producer.Id(), HostGPR{20});
        allocation.MarkHostWriteCoalesced(publication->Id());
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        const auto& demand = analysis.PseudoFlags();
        REQUIRE(demand.SavesParity(producer.Def()) == (scenario == 1 || scenario == 3));
        REQUIRE_FALSE(demand.SavesParity(nullptr));
        const std::array<const Inst*, 1> publications{publication};
        const auto cached = arm64::LowerFlagsTokens(
                analysis, allocation.Workspace(), allocation, FeatureSet{}, publications, demand);
        allocation.BeginTargetPlanning();
        allocation.StoreFlagsTokens(cached);
        allocation.EndTargetPlanning();
        REQUIRE(allocation.Actions().RetainsFlagsToken(producer.Id(), 20, producer.Id()) ==
                (scenario == 1 || scenario == 3));
        REQUIRE(cached.Retains(producer.Id(), 20, producer.Id()) ==
                (scenario == 1 || scenario == 3));
    }
}

TEST_CASE("forwarded flags captures use their planned preservation point", "[flags-token-plan]") {
    for (const unsigned extra : {0u, 32u})
        for (const bool sparse : {false, true}) {
            CAPTURE(extra, sparse);
            IntrusivePtr<Block> block{new Block(0, Location{0xbc10})};
            const auto input = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
            const auto producer = block->Or(ValueType::U64, input, Operand{Imm{swift::u64{0}}});
            const auto other = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
            std::vector<Value> captures{
                    block->Or(ValueType::U64, producer, Operand{Imm{swift::u64{0}}})};
            block->SaveFlags(captures.front(), Flags::Parity);
            if (extra) {
                const auto left =
                        block->And(ValueType::U64, producer, Operand{Imm{~swift::u64{0}}});
                const auto right =
                        block->And(ValueType::U64, producer, Operand{Imm{~swift::u64{0}}});
                // Both AND paths reach the same capture. Its certificate is unique.
                captures.push_back(block->Or(ValueType::U64, left, Operand{right}));
                block->SaveFlags(captures.back(), Flags::Parity);
                for (unsigned i = 0; i < extra; ++i) {
                    captures.push_back(
                            block->Or(ValueType::U64, producer, Operand{Imm{swift::u64{0}}}));
                    block->SaveFlags(captures.back(), Flags::Parity);
                }
            }
            // An independent candidate must survive scratch growth for the first one.
            const auto independent = block->Or(ValueType::U64, input, Operand{Imm{swift::u64{0}}});
            block->SaveFlags(independent, Flags::Parity);
            block->SetHostGPR(producer, HostRegIndex(22), Imm{0u});
            const auto* publication = &block->GetInstList().back();
            block->SetHostGPR(independent, HostRegIndex(23), Imm{0u});
            const auto* second_publication = &block->GetInstList().back();
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            if (sparse) {
                compile::CompileWorkspace editing_workspace;
                compile::RewriteContext editing{block.get(), editing_workspace, nullptr, false};
                for (const auto& node : block->GetInstList())
                    editing.Change(block.get(), &node, [](Inst& target) {
                        target.SetId(100 + target.Id() * 3);
                    });
            }
            GPRSMask pool{};
            pool.Mark(20);
            pool.Mark(22);
            pool.Mark(23);
            AllocationFixture allocation{
                    block->GetInstList().back().Id() + 1u, pool, FPRSMask{}, FeatureSet{}};
            allocation.MapRegister(producer.Id(), HostGPR{22});
            allocation.MapRegister(other.Id(), HostGPR{20});
            allocation.MapRegister(independent.Id(), HostGPR{23});
            allocation.PlanFixedGPRDefinition(producer.Id(),
                                              {.home = 20,
                                               .end = captures.front().Id(),
                                               .transfer = captures.front().Id(),
                                               .last_use = publication->Id(),
                                               .owner = producer.Id(),
                                               .width = 8,
                                               .preserved = true});
            allocation.MarkHostWriteCoalesced(publication->Id());
            allocation.MarkHostWriteCoalesced(second_publication->Id());
            compile::BlockAnalysisIndex analysis;
            analysis.Build(block.get(), nullptr, true);
            std::array<const Inst*, 2> publications{publication, second_publication};
            const auto plan = arm64::LowerFlagsTokens(analysis,
                                                      allocation.Workspace(),
                                                      allocation,
                                                      FeatureSet{},
                                                      publications,
                                                      analysis.PseudoFlags());
            REQUIRE(plan.retentions.size() == captures.size() + 1);
            REQUIRE(std::ranges::all_of(plan.retentions,
                                        [](const auto& row) { return row.point != UINT16_MAX; }));
            for (const auto capture : captures) {
                REQUIRE(plan.Retains(producer.Id(), 22, capture.Id()));
                REQUIRE_FALSE(plan.Retains(producer.Id(), 20, capture.Id()));
            }
            REQUIRE(plan.Retains(independent.Id(), 23, independent.Id()));
            std::reverse(publications.begin(), publications.end());
            const auto reversed = arm64::LowerFlagsTokens(analysis,
                                                          allocation.Workspace(),
                                                          allocation,
                                                          FeatureSet{},
                                                          publications,
                                                          analysis.PseudoFlags());
            REQUIRE(reversed.retentions == plan.retentions);
            allocation.BeginTargetPlanning();
            allocation.StoreFlagsTokens(plan);
            REQUIRE_FALSE(allocation.Actions().RetainsFlagsToken(producer.Id(), 22, producer.Id()));
            REQUIRE(allocation.Actions().RetainsFlagsToken(
                    producer.Id(), 22, captures.front().Id()));
            allocation.EndTargetPlanning();
            REQUIRE(allocation.Actions().RetainsFlagsToken(
                    producer.Id(), 22, captures.front().Id()));
            REQUIRE_FALSE(
                    allocation.Actions().RetainsFlagsToken(producer.Id(), 22, publication->Id()));
            allocation.ResetAllocations();
            REQUIRE_FALSE(allocation.Actions().RetainsFlagsToken(
                    producer.Id(), 22, captures.front().Id()));
            REQUIRE_FALSE(plan.Retains(producer.Id(), 22, producer.Id()));
            REQUIRE(plan.Retains(producer.Id(), 22, captures.front().Id()));
            REQUIRE_FALSE(plan.Retains(producer.Id(), 20, captures.front().Id()));
        }
}
