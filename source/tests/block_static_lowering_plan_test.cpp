#include "runtime/compile/workspace.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/actions.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/backend/arm64/compile/location_preparation.h"
#include "runtime/backend/arm64/compile/lower_edges.h"
#include "runtime/backend/arm64/compile/lower_flags.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/runtime.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/ir/hir_builder.h"

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("terminal publication uses final consumer locations without materializing operands",
          "[terminal-location][ownership]") {
    for (unsigned scenario = 0; scenario < 18; ++scenario) {
        CAPTURE(scenario);
        FeatureSet features{};
        HIRBuilder builder{1, false, features};
        auto* function = builder.AppendFunction(Location{0xe800}, Location{0xe951});
        const auto condition = function->LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
        const auto [left, right] = builder.If(terminal::If{condition,
                                                           terminal::LinkBlock{Location{0xe810}},
                                                           terminal::LinkBlock{Location{0xe820}}});
        const std::array<HIRBlock*, 2> leaves{left, right};
        std::array<Value, 2> values;
        std::array<const Inst*, 2> writes;
        Value earlier_value;
        const Inst* earlier_write{};
        const Inst* transfer{};
        for (unsigned slot = 0; slot < leaves.size(); ++slot) {
            builder.SetCurBlock(leaves[slot]);
            values[slot] = function->LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
            const auto call_return = [&] {
                function->CallReturn(
                        values[slot], Imm{swift::u64{0xea00}}, Lambda{Imm{swift::u64{0xeb00}}});
                return &function->GetCurrentBlock()->GetBlock()->GetInstList().back();
            };
            if (!slot) {
                if (scenario == 2 || scenario == 12) function->PopRSB(Lambda{values[slot]});
                if (scenario == 3 || scenario == 10 || scenario == 16) call_return();
                if (scenario == 10) function->AdvancePC(Imm{swift::u64{1}});
                if (scenario == 9) {
                    earlier_value = values[slot];
                    function->SetLocation(Lambda{earlier_value});
                    earlier_write = &function->GetCurrentBlock()->GetBlock()->GetInstList().back();
                    values[slot] =
                            function->LoadUniform(ValueType::U64, Uniform{16, ValueType::U64});
                }
            }
            function->SetLocation(!slot && scenario == 8 ? Lambda{Imm{swift::u64{0xec00}}}
                                                         : Lambda{values[slot]});
            writes[slot] = &function->GetCurrentBlock()->GetBlock()->GetInstList().back();
            if (!slot) {
                if (scenario == 4) function->PopRSB(Lambda{values[slot]});
                if (scenario == 5 || scenario == 14) transfer = call_return();
                if (scenario == 6) function->AdvancePC(Imm{swift::u64{1}});
            }
            if (!slot && scenario == 7)
                function->EndBlock(terminal::ReturnToHost{});
            else if (!slot && (scenario == 2 || scenario == 4 || scenario == 12 || scenario == 17))
                function->EndBlock(terminal::PopRSBHint{});
            else if (!slot && scenario == 15)
                function->EndBlock(terminal::Invalid{});
            else
                function->EndBlock(terminal::ReturnToDispatch{});
        }
        // Members outside RPO cannot create a repeat for a reachable singleton.
        for (const swift::u64 pc : {0xe900, 0xe940}) {
            function->SetCurBlock(function->AppendBlock(Location{pc}));
            const auto value = function->LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
            function->SetLocation(Lambda{value});
            function->EndBlock(terminal::ReturnToDispatch{});
        }
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        GPRSMask pool{};
        pool.Mark(20);
        pool.Mark(22);
        AllocationFixture allocation{function->MaxInstrCount(), pool, FPRSMask{}, features};
        const swift::u16 right_home =
                scenario == 0 || scenario == 11 || scenario == 15 || scenario == 16 ? 20 : 22;
        allocation.MapRegister(values[0].Id(), HostGPR{20});
        allocation.MapRegister(values[1].Id(), HostGPR{right_home});
        if (earlier_value.Defined()) allocation.MapRegister(earlier_value.Id(), HostGPR{22});
        if (scenario == 11 || scenario == 12 || scenario == 13 || scenario == 16) {
            allocation.MapMemSpill(values[0].Id(), SpillSlot{0});
            if (scenario == 11 || scenario == 16)
                allocation.MapSpillReload(
                        values[0].Id(), writes[0]->Id(), writes[0]->Id(), HostGPR{20});
        }
        if (scenario == 14) {
            REQUIRE(transfer);
            allocation.PlanFixedGPRDefinition(values[0].Id(),
                                              {.home = 22,
                                               .end = transfer->Id() + 1u,
                                               .transfer = transfer->Id() + 1u,
                                               .last_use = transfer->Id(),
                                               .owner = values[0].Id(),
                                               .width = 8,
                                               .preserved = true});
        }
        compile::SemanticFacts facts{function};
        auto disabled = features;
        disabled.indirect_l1 = false;
        REQUIRE(arm64::PrepareDeferredLocations(*function, facts, allocation, disabled).empty());
        auto selected = arm64::PrepareDeferredLocations(*function, facts, allocation, features);
        std::vector<swift::u16> expected;
        // Complete expected choices, independent of the physical grouping code.
        if (scenario == 0 || scenario == 2 || scenario == 3 || scenario == 4 || scenario == 5 ||
            scenario == 10 || scenario == 11 || scenario == 12 || scenario == 14 ||
            scenario == 15 || scenario == 16)
            expected.push_back(writes[0]->Id());
        if (scenario == 0 || scenario == 11 || scenario == 14 || scenario == 15 || scenario == 16)
            expected.push_back(writes[1]->Id());
        std::sort(expected.begin(), expected.end());
        REQUIRE(selected == expected);
        REQUIRE_FALSE(facts.HasSpillUses());
        allocation.BeginTargetPlanning();
        for (const auto& member : function->GetHIRBlocksRPO()) {
            const auto* block = member.GetBlock();
            REQUIRE_FALSE(facts.FindIndex(block));
            allocation.DeclareBlock(block, facts.PrepareSummary(block), {});
        }
        allocation.StoreDeferredLocations(selected);
        std::vector<swift::u16>{}.swap(selected);
        allocation.EndTargetPlanning();
        for (const auto* write : writes)
            REQUIRE(allocation.Actions().DefersTerminalLocation(write) ==
                    std::binary_search(expected.begin(), expected.end(), write->Id()));
        REQUIRE_FALSE(allocation.Actions().DefersTerminalLocation(earlier_write));
        REQUIRE_FALSE(allocation.Actions().DefersTerminalLocation(nullptr));
        REQUIRE_THROWS_AS(allocation.StoreDeferredLocations(expected), std::logic_error);
        allocation.ResetAllocations();
        REQUIRE_FALSE(allocation.Actions().DefersTerminalLocation(writes[0]));
        allocation.BeginTargetPlanning();
        allocation.DeclareBlock(
                writes[0]->GetBlock(), facts.PrepareSummary(writes[0]->GetBlock()), {});
        REQUIRE_FALSE(allocation.Actions().DefersTerminalLocation(writes[0]));
        allocation.StoreDeferredLocations(expected);
        allocation.AbortTargetPlanning();
        REQUIRE_FALSE(allocation.Actions().DefersTerminalLocation(writes[0]));
    }
}

TEST_CASE("deferred terminal publication survives builder release and prepared moves",
          "[terminal-location][ownership][production]") {
    Config config{.loc_start = 0,
                  .loc_end = 1ull << 48,
                  .enable_jit = true,
                  .has_local_operation = false,
                  .backend_isa = kArm64,
                  .uniform_buffer_size = 16};
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    std::optional<arm64::Arm64PreparedUnit> pending;
    const Inst* location{};
    {
        HIRBuilder builder{1, false, ResolveFeatureSet(module->GetModuleConfig())};
        auto* function = builder.AppendFunction(Location{0xe980}, Location{0xe981});
        const auto value = function->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        function->SetLocation(Lambda{value});
        location = &function->GetCurrentBlock()->GetBlock()->GetInstList().back();
        function->PopRSB(Lambda{value});
        function->EndBlock(terminal::PopRSBHint{});
        function->EndFunction();
        pending = arm64::PrepareFunction(module, function);
        REQUIRE(pending);
        REQUIRE(pending->Actions().DefersTerminalLocation(location) ==
                ResolveFeatureSet(module->GetModuleConfig()).indirect_l1);
    }
    arm64::Arm64PreparedUnit moved{std::move(*pending)};
    pending.reset();
    REQUIRE(moved.ReadyForEmission(module));
    REQUIRE(moved.Actions().DefersTerminalLocation(location) ==
            ResolveFeatureSet(module->GetModuleConfig()).indirect_l1);
    arm64::JitContext context{module, moved.Allocation(), moved.Actions(), false};
    arm64::JitTranslator translator{context};
    REQUIRE(translator.Translate(moved.Function()));
    context.Finish();
    REQUIRE(context.CurrentBufferSize() != 0);
}

TEST_CASE("spilled terminal targets execute ordinary and deferred publication",
          "[terminal-location][production][spill]") {
#if defined(__aarch64__)
    for (const unsigned shape : {0u, 1u, 2u, 3u}) {
        CAPTURE(shape);
        Config config{.loc_start = 0,
                      .loc_end = 1ull << 48,
                      .enable_jit = true,
                      .has_local_operation = false,
                      .backend_isa = kArm64,
                      .uniform_buffer_size = 16,
                      .static_program = true};
        AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        const auto features = ResolveFeatureSet(module->GetModuleConfig());
        if (!features.indirect_l1) {
            SUCCEED("native terminal transport case requires indirect L1");
            continue;
        }
        constexpr swift::u64 source_pc = 0xed00, target_pc = 0xed40;
        IntrusivePtr<Block> target{new Block{0, Location{target_pc}}};
        target->StoreUniform(Uniform{8, ValueType::U64},
                             target->LoadImm(ValueType::U64, Imm{swift::u64{73}}));
        target->SetTerminal(terminal::ReturnToHost{});
        const auto target_entry = TranslateIR(module, target);
        REQUIRE(target_entry);
        space.PushCodeCache(Location{target_pc}, target_entry);
        HIRBuilder builder{1, false, features};
        auto* function = builder.AppendFunction(Location{source_pc}, Location{source_pc + 1});
        const auto value = function->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        if (shape == 1) function->PopRSB(Lambda{value});
        function->SetLocation(Lambda{value});
        const auto* location = &function->GetCurrentBlock()->GetBlock()->GetInstList().back();
        if (shape >= 2) function->PopRSB(shape == 2 ? Lambda{value} : Lambda{Imm{swift::u64{0}}});
        function->EndBlock(terminal::ReturnToDispatch{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        AllocationFixture allocation{function->MaxInstrCount(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        compile::AllocateFunction(function,
                                  &allocation,
                                  arm64::PrepareAllocationInputs(function, allocation, features),
                                  {});
        REQUIRE(allocation.ValueType(value) == compile::AllocationView::GPR);
        // Keep the real allocation/liveness and final admission, while forcing
        // the operand through ordinary spill transport without a reload home.
        allocation.MapMemSpill(value.Id(), SpillSlot{0});
        arm64::CompleteARM64Allocation(module, allocation);
        REQUIRE(allocation.Actions().DefersTerminalLocation(location) == (shape != 0));
        arm64::JitContext context{module, allocation, allocation.Actions(), false};
        arm64::JitTranslator translator{context};
        REQUIRE(translator.Translate(function));
        context.Finish();
        REQUIRE(std::any_of(translator.GetFaultMetadata().begin(),
                            translator.GetFaultMetadata().end(),
                            [](const auto& fault) { return fault.recovery_offset != 0; }) ==
                (shape != 0));
        const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(),
                                                            context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID);
        context.Flush(buffer);
        space.PushCodeCache(Location{source_pc},
                            buffer.exec_data + context.GetCodeOffset(source_pc));
        Runtime runtime{&space};
        std::memcpy(runtime.GetUniformBuffer().data(), &target_pc, sizeof(target_pc));
        runtime.SetLocation(source_pc);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        swift::u64 answer{};
        std::memcpy(&answer, runtime.GetUniformBuffer().data() + 8, sizeof(answer));
        REQUIRE(answer == 73);
    }
#else
    SUCCEED("native terminal spill transport requires an AArch64 host");
#endif
}

TEST_CASE("block-local scalar plans reject incoming producers", "[flags-token-plan]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    IntrusivePtr<Block> predecessor{new Block(0, Location{0xbcf0})};
    const auto source = predecessor->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
    const auto constant = predecessor->LoadImm(ValueType::U64, Imm{swift::u64{7}});
    const auto sum = predecessor->Add(ValueType::U64, source, Operand{constant});
    predecessor->SaveFlags(sum, Flags::All);
    predecessor->ReIdInstr();
    IntrusivePtr<Block> block{new Block(0, Location{0xbcf8})};
    block->SetHostGPR(sum, HostRegIndex(20), Imm{0u});
    auto* publication = &block->GetInstList().back();
    ::swift::runtime::compile::RewriteContext{block.get(), compilation_workspace, nullptr, false}
            .Change(block.get(), publication, [](Inst& target) { target.SetId(4); });
    AllocationFixture allocation{6, GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.MapRegister(source.Id(), HostGPR{20});
    allocation.MapRegister(constant.Id(), HostGPR{14});
    allocation.MapRegister(sum.Id(), HostGPR{20});
    allocation.MarkHostWriteCoalesced(publication->Id());
    compile::BlockAnalysisIndex analysis;
    analysis.Build(block.get());
    REQUIRE(analysis.Contains(publication));
    REQUIRE_FALSE(analysis.Contains(sum.Def()));
    const std::array<const Inst*, 1> publications{publication};
    const auto flags = arm64::LowerFlagsTokens(analysis,
                                               allocation.Workspace(),
                                               allocation,
                                               FeatureSet{},
                                               publications,
                                               analysis.PseudoFlags());
    REQUIRE(flags.retentions.empty());
}

TEST_CASE("region PF AF plans preserve cold reread inputs", "[region-pfaf-plan]") {
    for (unsigned scenario = 0; scenario < 11; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbd10})};
        const bool uniform = scenario == 1 || scenario == 3 || scenario == 7 || scenario == 8;
        const auto type = scenario == 10 ? ValueType::U32 : ValueType::U16;
        const auto left =
                uniform ? block->LoadUniform(type, Uniform{0, type})
                        : block->GetHostGPR(type, HostRegIndex(20), Imm{scenario == 9 ? 7u : 0u});
        auto right = Operand{Imm{swift::u64{1}}};
        Value sum;
        if (scenario == 6) {
            const auto computed = block->Add(type, left, right);
            sum = block->Sub(type, computed, right);
        } else
            sum = block->Sub(type, left, right);
        block->SaveFlags(sum, scenario == 4 ? Flags::NZCV : Flags::All);
        const auto* save = &block->GetInstList().back();
        if (scenario == 2) block->SetHostGPR(left, HostRegIndex(20), Imm{0u});
        if (scenario == 3 || scenario == 7 || scenario == 8) {
            const auto offset = scenario == 7 ? 2u : scenario == 8 ? 1u : 0u;
            block->StoreUniform(Uniform{offset, ValueType::U8}, Value{left}.SetCastType(ValueType::U8));
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        const auto plan = arm64::LowerPFAF(*block, scenario != 5, sum.Def(), save);
        const bool accepted = scenario == 0 || scenario == 1 || scenario == 7;
        REQUIRE(plan.has_value() == accepted);
        if (plan) {
            REQUIRE(plan->width == 2);
            REQUIRE(plan->producer == sum.Id());
            REQUIRE(plan->right.kind == arm64::PFAFOperand::Kind::Imm);
            REQUIRE(plan->left.kind == (uniform ? arm64::PFAFOperand::Kind::Uniform
                                                : arm64::PFAFOperand::Kind::HostGPR));
        }
    }
}

TEST_CASE("region PF AF plans are independent per producer and reject foreign saves", "[region-pfaf-plan]") {
    using Kind = arm64::PFAFOperand::Kind;
    for (bool interfere : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbd60})};
        const auto left_a = block->GetHostGPR(ValueType::U16, HostRegIndex(20), Imm{0u});
        const auto a = block->Sub(ValueType::U16, left_a, Operand{Imm{swift::u64{1}}});
        block->SaveFlags(a, Flags::All);
        const auto* save_a = &block->GetInstList().back();
        if (interfere) block->SetHostGPR(left_a, HostRegIndex(20), Imm{0u});
        const auto left_b = block->GetHostGPR(ValueType::U16, HostRegIndex(21), Imm{0u});
        const auto b = block->Sub(ValueType::U16, left_b, Operand{Imm{swift::u64{1}}});
        block->SaveFlags(b, Flags::All);
        const auto* save_b = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        const auto plan_a = arm64::LowerPFAF(*block, true, a.Def(), save_a);
        const auto plan_b = arm64::LowerPFAF(*block, true, b.Def(), save_b);
        REQUIRE(plan_a.has_value() == !interfere);
        REQUIRE(plan_b.has_value());
        REQUIRE(plan_b->producer == b.Id());
        REQUIRE(plan_b->width == 2);
        REQUIRE(plan_b->left.kind == Kind::HostGPR);
        REQUIRE(plan_b->left.value == 21);
        REQUIRE(plan_b->right.kind == Kind::Imm);
        REQUIRE(plan_b->right.value == 1);
        if (plan_a) {
            REQUIRE(plan_a->producer == a.Id());
            REQUIRE(plan_a->left.value == 20);
            const auto again = arm64::LowerPFAF(*block, true, a.Def(), save_a);
            REQUIRE(again.has_value());
            REQUIRE(again->width == plan_a->width);
            REQUIRE(again->left.kind == plan_a->left.kind);
            REQUIRE(again->left.value == plan_a->left.value);
            REQUIRE(again->right.kind == plan_a->right.kind);
            REQUIRE(again->right.value == plan_a->right.value);
        }
        REQUIRE_FALSE(arm64::LowerPFAF(*block, true, a.Def(), save_b));
        REQUIRE_FALSE(arm64::LowerPFAF(*block, false, b.Def(), save_b));
        REQUIRE_FALSE(arm64::LowerPFAF(*block, true, nullptr, save_b));
        REQUIRE_FALSE(arm64::LowerPFAF(*block, true, b.Def(), nullptr));
    }
    IntrusivePtr<Block> foreign{new Block(0, Location{0xbd68})};
    const auto foreign_left = foreign->GetHostGPR(ValueType::U16, HostRegIndex(20), Imm{0u});
    const auto foreign_sub =
            foreign->Sub(ValueType::U16, foreign_left, Operand{Imm{swift::u64{1}}});
    foreign->SaveFlags(foreign_sub, Flags::All);
    const auto* foreign_save = &foreign->GetInstList().back();
    foreign->SetTerminal(terminal::ReturnToHost{});
    foreign->ReIdInstr();
    IntrusivePtr<Block> block{new Block(0, Location{0xbd70})};
    const auto left = block->GetHostGPR(ValueType::U16, HostRegIndex(20), Imm{0u});
    const auto sub = block->Sub(ValueType::U16, left, Operand{Imm{swift::u64{1}}});
    block->SaveFlags(sub, Flags::All);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    REQUIRE_FALSE(arm64::LowerPFAF(*block, true, sub.Def(), foreign_save));
    REQUIRE_FALSE(arm64::LowerPFAF(*foreign, true, sub.Def(), foreign_save));
}

