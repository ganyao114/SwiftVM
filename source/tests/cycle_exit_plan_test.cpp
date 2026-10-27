#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/workspace.h"
using swift::runtime::compile::BlockAnalysisIndex;
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <array>
#include <cstdio>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/lower_consumers.h"
#include "runtime/backend/arm64/compile/lower_edges.h"
#include "runtime/backend/arm64/compile/lower_flags.h"
#include "runtime/backend/arm64/compile/vector_preparation.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/ir/hir_builder.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::backend::arm64;
using namespace swift::runtime::ir;

TEST_CASE("prepared cycle candidates preserve terminal shapes and emitted code", "[cycle-exit-plan]") {
#if defined(__aarch64__)
    for (unsigned shape = 0; shape < 6; ++shape) for (bool region : {false, true})
        for (bool links : {false, true}) {
            CAPTURE(shape, region, links);
            Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                          .has_local_operation = false, .backend_isa = kArm64};
            config.uniform_buffer_size = 64; config.region_edges = region;
            config.global_opts = links ? Optimizations::BlockLink : decltype(config.global_opts){};
            AddressSpace space{config}; auto module = space.GetDefaultModule();
            const auto features = ResolveFeatureSet(module->GetModuleConfig());
            HIRBuilder builder{4, true, features};
            auto* function = builder.AppendFunction(Location{0xe000}, Location{0xe040});
            auto* first = &function->EditBlock(function->GetCurrentBlock());
            auto* last = function->AppendBlock(Location{0xe020});
            function->AddEdge(first, last);
            if (shape >= 3) function->AddEdge(last, first);
            function->AdvancePC(Imm{u64{1}});
            function->EndBlock(terminal::LinkBlock{Location{0xe020}});
            function->SetCurBlock(last);
            function->AdvancePC(Imm{u64{1}});
            if (shape == 0) function->EndBlock(terminal::ReturnToHost{});
            else if (shape == 1) function->EndBlock(terminal::LinkBlock{Location{0xd000}});
            else if (shape == 2) function->EndBlock(terminal::Condition{Cond::EQ,
                    terminal::LinkBlock{Location{0xd000}}, terminal::LinkBlockFast{Location{0xd000}}});
            else if (shape == 3) function->EndBlock(terminal::LinkBlock{Location{0xe000}});
            else if (shape == 4) function->EndBlock(terminal::Condition{Cond::EQ,
                    terminal::LinkBlock{Location{0xe000}}, terminal::LinkBlock{Location{0xd000}}});
            else function->EndBlock(terminal::CheckHalt{terminal::Condition{Cond::NE,
                    terminal::LinkBlock{Location{0xd000}}, terminal::LinkBlock{Location{0xc000}}}});
            function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
            AllocationFixture allocation{function->MaxInstrCount(),
                                         space.GetTrampolines().GetGPRRegs(),
                                         space.GetTrampolines().GetFPRRegs(),
                                         features};
            ::swift::runtime::compile::AllocateFunction(
                    function,
                    &allocation,
                    ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                            function, allocation, features));
            REQUIRE(CompleteARM64UnitForEmission(module, allocation, nullptr, function));
            const auto* actions = allocation.Actions().ForBlock(first->GetBlock());
            REQUIRE(actions);
            REQUIRE(allocation.Actions().Certificate());
            const auto& plan = allocation.Actions().Edges();
            const unsigned external = shape == 1 || shape == 2 ? 1 : shape == 5 ? 2 : 0;
            const unsigned internal = shape == 3 || shape == 4 ? 1 : 0;
            const unsigned mixed = shape == 4 ? 1 : 0;
            REQUIRE(plan.backward_terminal_candidates.size() == external + internal + mixed);
            std::vector<std::pair<u64, u64>> expected_backward;
            if (shape == 1 || shape == 2)
                expected_backward.emplace_back(0xe020, 0xd000);
            else if (shape == 3)
                expected_backward.emplace_back(0xe020, 0xe000);
            else if (shape == 4)
                expected_backward = {{0xe020, 0xd000}, {0xe020, 0xe000}};
            else if (shape == 5)
                expected_backward = {{0xe020, 0xc000}, {0xe020, 0xd000}};
            REQUIRE(plan.backward_terminal_candidates == expected_backward);
            REQUIRE(plan.local_cycle_source_count == (region ? internal : 0));
            JitContext context{module, allocation, allocation.Actions()};
            JitTranslator translator{context};
            REQUIRE(translator.Translate(function)); context.Finish();
            const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
            REQUIRE(context.CurrentBufferSize() > 0);
            std::printf("[cycle-exit] shape=%u region=%u links=%u code=", shape, region, links);
            for (size_t i = 0; i < context.CurrentBufferSize(); ++i) std::printf("%02x", bytes[i]);
            std::putchar('\n');
        }
#else
    SUCCEED("native cycle preparation requires AArch64");
#endif
}

TEST_CASE("cycle preparation is order-independent cross-plan stable and empty without candidates", "[cycle-exit-plan]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.global_opts = Optimizations::BlockLink; config.region_edges = true;
    config.arm64_features |= Arm64Features::Atomics;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    const auto build = [&](bool shuffled) {
        HIRBuilder builder{4, true, features};
        auto* function = builder.AppendFunction(Location{0xf000}, Location{0xf060});
        auto* a = &function->EditBlock(function->GetCurrentBlock());
        auto* extra1 = function->AppendBlock(Location{shuffled ? 0xf040u : 0xf020u});
        auto* extra2 = function->AppendBlock(Location{shuffled ? 0xf020u : 0xf040u});
        auto* b = shuffled ? extra2 : extra1;
        auto* c = shuffled ? extra1 : extra2;
        function->AddEdge(a, b); function->AddEdge(a, c); function->AddEdge(b, c);
        function->AddEdge(c, a);
        function->AddEdge(c, b);
        auto zero = function->LoadImm(ValueType::U64, Imm{u64{0}});
        auto loaded = function->LoadMemory(ValueType::U64, ir::Operand{Imm{u64{0x1000}}});
        function->SetHostFPR(loaded, HostRegIndex(17), Imm{0u});
        function->SetHostFPR(zero, HostRegIndex(17), Imm{8u});
        const auto atomic_body = [&](auto* block) {
            function->SetCurBlock(block);
            auto address = function->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
            auto first = function->LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
            auto result = function->AtomicFetchAdd(ValueType::U64, address, first);
            function->StoreUniform(Uniform{24, ValueType::U64}, result);
        };
        atomic_body(a);
        function->EndBlock(terminal::Condition{Cond::EQ, terminal::LinkBlock{Location{0xf020}},
                                             terminal::LinkBlock{Location{0xf040}}});
        function->SetCurBlock(b); function->EndBlock(terminal::LinkBlock{Location{0xf040}});
        function->SetCurBlock(c);
        atomic_body(c);
        function->EndBlock(terminal::Condition{Cond::EQ,
                terminal::LinkBlock{Location{0xf000}}, terminal::LinkBlock{Location{0xf020}}});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        AllocationFixture allocation{function->MaxInstrCount(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateFunction(
                function,
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        function, allocation, features));
        REQUIRE(CompleteARM64UnitForEmission(module, allocation, nullptr, function));
        const auto& plan = allocation.Actions().Edges();
        JitContext context{module, allocation, allocation.Actions()};
        JitTranslator translator{context};
        REQUIRE(translator.Translate(function)); context.Finish();
        const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
        return std::tuple{plan,
                          allocation.Actions().Atomics().instructions.size(),
                          allocation.Actions().Atomics().cohorts.size(),
                          std::vector<u8>(bytes, bytes + context.CurrentBufferSize())};
    };
    const auto [canonical, canonical_sites, canonical_cohorts, canonical_code] = build(false);
    const auto [shuffled, shuffled_sites, shuffled_cohorts, shuffled_code] = build(true);
    REQUIRE(canonical.cycle_edges == shuffled.cycle_edges);
    REQUIRE(canonical.backward_terminal_candidates == shuffled.backward_terminal_candidates);
    REQUIRE(canonical.local_cycle_source_count == shuffled.local_cycle_source_count);
    REQUIRE(canonical_code == shuffled_code);
    REQUIRE(canonical_sites == shuffled_sites);
    REQUIRE(canonical_cohorts == shuffled_cohorts);
    REQUIRE(canonical_sites == 2);
    REQUIRE(canonical_cohorts == 1);
    REQUIRE(canonical.cycle_edges.size() == 2);
    REQUIRE(canonical.local_cycle_source_count == 1);
    unsigned shared_fallback_bodies{};
    for (size_t i = 0; i + 4 <= canonical_code.size(); i += 4) {
        u32 instruction{};
        std::memcpy(&instruction, canonical_code.data() + i, 4);
        shared_fallback_bodies += instruction == 0xd61f01a0u;
    }
    REQUIRE(shared_fallback_bodies == 1);

    HIRBuilder empty_builder{1, true, features};
    auto* empty_function = empty_builder.AppendFunction(Location{0xf100}, Location{0xf104});
    auto* only_block = &empty_function->EditBlock(empty_function->GetCurrentBlock());
    empty_function->EndBlock(terminal::ReturnToHost{});
    empty_function->EndFunction(); empty_function->ComputeRPO(); empty_function->IdByRPO();
    AllocationFixture empty_allocation{empty_function->MaxInstrCount(),
                                       space.GetTrampolines().GetGPRRegs(),
                                       space.GetTrampolines().GetFPRRegs(),
                                       features};
    ::swift::runtime::compile::AllocateFunction(
            empty_function,
            &empty_allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    empty_function, empty_allocation, features));
    REQUIRE(CompleteARM64UnitForEmission(module, empty_allocation, nullptr, empty_function));
    const auto& empty_plan = empty_allocation.Actions().Edges();
    REQUIRE(empty_plan.cycle_edges.empty());
    REQUIRE(empty_plan.backward_terminal_candidates.empty());
    REQUIRE(empty_plan.local_cycle_source_count == 0);
    REQUIRE(empty_allocation.Actions().Atomics().instructions.empty());
    JitContext empty_context{module, empty_allocation, empty_allocation.Actions()};
    JitTranslator empty_translator{empty_context};
    REQUIRE(empty_translator.Translate(empty_function));
    REQUIRE(empty_context.CurrentBufferSize() > 0);
#else
    SUCCEED("native cycle preparation requires AArch64");
#endif
}

TEST_CASE("flags regs audit diagnostics never change cycle emitted code", "[cycle-exit-diagnostics]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.global_opts = Optimizations::BlockLink;
    const auto compile = [&](bool region, bool links, bool audit) {
        Config local = config;
        local.region_edges = region;
        local.global_opts = links ? Optimizations::BlockLink : decltype(local.global_opts){};
        AddressSpace local_space{local};
        auto local_module = local_space.GetDefaultModule();
        auto features = ResolveFeatureSet(local_module->GetModuleConfig());
        features.flags_regs_audit = audit;
        HIRBuilder builder{4, true, features};
        auto* function = builder.AppendFunction(Location{0xf400}, Location{0xf440});
        auto* first = &function->EditBlock(function->GetCurrentBlock());
        auto* last = function->AppendBlock(Location{0xf420});
        function->AddEdge(first, last);
        function->AddEdge(last, first);
        const auto left = function->LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
        const auto producer = function->Sub(ValueType::U8, left, ir::Operand{Imm{1u}});
        function->SaveFlags(producer, Flags::All);
        function->AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::LinkBlock{Location{0xf420}});
        function->SetCurBlock(last);
        function->AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::Condition{Cond::EQ,
                terminal::LinkBlock{Location{0xf400}}, terminal::LinkBlock{Location{0xd000}}});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        AllocationFixture allocation{function->MaxInstrCount(),
                                     local_space.GetTrampolines().GetGPRRegs(),
                                     local_space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateFunction(
                function,
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        function, allocation, features));
        REQUIRE(CompleteARM64UnitForEmission(local_module, allocation, nullptr, function));
        JitContext context{local_module, allocation, allocation.Actions()};
        JitTranslator translator{context};
        REQUIRE(translator.Translate(function)); context.Finish();
        const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
        return std::vector<u8>(bytes, bytes + context.CurrentBufferSize());
    };
    for (bool region : {false, true}) for (bool links : {false, true}) {
        CAPTURE(region, links);
        REQUIRE(compile(region, links, false) == compile(region, links, true));
    }
#else
    SUCCEED("native cycle preparation requires AArch64");
#endif
}

TEST_CASE("pseudo flags demand merges order-independently and bounds foreign instructions", "[pseudo-flags-plan]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    const auto build = [](bool reversed) {
        IntrusivePtr<Block> block{new Block(0, Location{0xd900})};
        const auto producer = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
        if (reversed) {
            block->SaveFlags(producer, Flags::Carry);
            block->SaveFlags(producer, Flags::NZ);
        } else {
            block->SaveFlags(producer, Flags::NZ);
            block->SaveFlags(producer, Flags::Carry);
        }
        block->BranchOnlyFlags(producer, Flags::Overflow);
        block->ReIdInstr();
        BlockAnalysisIndex index;
        index.Build(block.get(), nullptr, true);
        return std::pair{block, index.PseudoFlags().For(producer.Def())};
    };
    const auto [forward_block, forward] = build(false);
    const auto [reversed_block, reversed] = build(true);
    (void)forward_block; (void)reversed_block;
    REQUIRE(forward.set == reversed.set);
    REQUIRE(forward.clear == reversed.clear);
    REQUIRE(forward.branch_only == reversed.branch_only);
    REQUIRE(forward.set == (Flags::NZ | Flags::Carry | Flags::Overflow));
    REQUIRE(forward.branch_only);

    IntrusivePtr<Block> block{new Block(0, Location{0xd910})};
    const auto producer = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
    block->SaveFlags(producer, Flags::All);
    unsigned id = 200;
    for (const auto& inst : block->GetInstList()) {
        ::swift::runtime::compile::RewriteContext{
                block.get(), compilation_workspace, nullptr, false}
                .Change(block.get(), &inst, [&](Inst& target) { target.SetId(id); });
        id += 3;
    }
    BlockAnalysisIndex index;
    index.Build(block.get(), nullptr, true);
    const auto& plan = index.PseudoFlags();
    REQUIRE(plan.For(producer.Def()).set == Flags::All);
    IntrusivePtr<Block> foreign{new Block(0, Location{0xd920})};
    const auto foreign_value = foreign->LoadImm(ValueType::U64, Imm{swift::u64{7}});
    foreign->ReIdInstr();
    REQUIRE(plan.For(foreign_value.Def()).Null());
    REQUIRE_FALSE(plan.SavesParity(foreign_value.Def()));
    for (const auto& inst : foreign->GetInstList())
        ::swift::runtime::compile::RewriteContext{
                foreign.get(), compilation_workspace, nullptr, false}
                .Change(foreign.get(), &inst, [](Inst& target) { target.SetId(1000); });
    REQUIRE(plan.For(foreign_value.Def()).Null());
    REQUIRE_FALSE(plan.SavesParity(foreign_value.Def()));
    IntrusivePtr<Block> consuming{new Block(0, Location{0xd930})};
    const auto consumed = consuming->LoadImm(ValueType::U64, Imm{swift::u64{1}});
    (void)consuming->GetFlags(consumed.Type(), consumed, Flags::All);
    consuming->ReIdInstr();
    BlockAnalysisIndex empty_index;
    empty_index.Build(consuming.get(), nullptr, true);
    REQUIRE(empty_index.PseudoFlags().Empty());
    REQUIRE(empty_index.PseudoFlags().For(consumed.Def()).Null());
}

TEST_CASE("consumer plan pair results and alu split facts are deterministic and stale-index safe", "[consumer-plan-cross]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xf400})};
    auto input = block->LoadImm(ValueType::U64, Imm{u64{45}});
    const std::array<Value, 4> roots{{block->Sub(ValueType::U64, input, ir::Operand{Imm{u64{7}}}),
                                      block->And(ValueType::U64, input, ir::Operand{Imm{u64{7}}}),
                                      block->Or(ValueType::U64, input, ir::Operand{Imm{u64{7}}}),
                                      block->Xor(ValueType::U64, input, ir::Operand{Imm{u64{7}}})}};
    for (u16 i = 0; i < roots.size(); ++i)
        block->StoreUniform(Uniform{u16(i * 8), ValueType::U64}, roots[i]);
    const auto producer = block->Div128(ValueType::U64, input, input, input, Imm{u64{0}});
    const auto secondary = block->Div128Remainder(ValueType::U64, producer);
    block->StoreUniform(Uniform{32, ValueType::U64}, secondary);
    block->ReIdInstr();
    compile::BlockAnalysisIndex index;
    index.Build(block.get());
    compile::CompileWorkspace workspace;
    const auto build = [&] {
        return arm64::LowerConsumers(
                index,
                workspace,
                [](const Inst&) { return true; },
                [](const Inst&) { return false; },
                nullptr,
                true);
    };
    const auto plan = build();
    const auto rebuilt = build();
    REQUIRE(plan.valid);
    REQUIRE(plan.Find(producer.Id()));
    REQUIRE(plan.Has(producer.Id(), arm64::ConsumerFlag::Pair));
    REQUIRE(plan.Find(producer.Id())->related == secondary.Id());
    REQUIRE(plan.audit.size() == roots.size());
    for (auto root : roots) {
        const auto* facts = plan.Audit(root.Id());
        REQUIRE(facts);
        REQUIRE(facts->value_uses == 1);
        REQUIRE(facts->external_users == 1);
        REQUIRE(facts->family_count == 1);
        REQUIRE(facts->saw_value);
        REQUIRE_FALSE(facts->saw_address);
    }
    REQUIRE(rebuilt.actions == plan.actions);
    REQUIRE(rebuilt.audit == plan.audit);

    IntrusivePtr<Block> foreign{new Block(0, Location{0xf480})};
    foreign->SetTerminal(terminal::ReturnToHost{});
    foreign->ReIdInstr();
    compile::BlockAnalysisIndex stale;
    stale.Build(foreign.get());
    const auto stale_plan = arm64::LowerConsumers(
            stale,
            workspace,
            [](const Inst&) { return true; },
            [](const Inst&) { return false; },
            nullptr,
            true);
    REQUIRE(stale_plan.valid);
    REQUIRE(stale_plan.Find(producer.Id()) == nullptr);
    for (auto root : roots) REQUIRE(stale_plan.Audit(root.Id()) == nullptr);

    IntrusivePtr<Block> plain{new Block(0, Location{0xf4c0})};
    (void)plain->LoadImm(ValueType::U64, Imm{u64{1}});
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    compile::BlockAnalysisIndex plain_index;
    plain_index.Build(plain.get());
    const auto zero = arm64::LowerConsumers(
            plain_index,
            workspace,
            [](const Inst&) { return true; },
            [](const Inst&) { return false; },
            nullptr,
            true);
    REQUIRE(zero.valid);
    REQUIRE(zero.actions.empty());
    REQUIRE(zero.audit.empty());
    const auto condition = plain->CondSet(ValueType::U8, Cond::NE);
    const auto rebuild = [&] {
        plain->ReIdInstr();
        plain_index.Build(plain.get());
        return arm64::LowerConsumers(
                plain_index,
                workspace,
                [](const Inst&) { return true; },
                [](const Inst&) { return false; });
    };
    const auto tail = rebuild();
    REQUIRE(tail.Find(condition.Id()) != nullptr);
    REQUIRE(tail.Find(condition.Id())->Has(ConsumerFlag::Last));
    plain->AdvancePC(Imm{u64{1}});
    const auto followed = rebuild();
    REQUIRE(followed.Find(condition.Id()) != nullptr);
    REQUIRE_FALSE(followed.Find(condition.Id())->Has(ConsumerFlag::Last));
    (void)plain->Goto(condition);
    const auto controlled = rebuild();
    REQUIRE(controlled.Find(condition.Id()) != nullptr);
    REQUIRE(controlled.Find(condition.Id())->Has(ConsumerFlag::LaterControl));
    REQUIRE_FALSE(controlled.Find(condition.Id())->Has(ConsumerFlag::Last));
    REQUIRE(tail.Find(condition.Id())->Has(ConsumerFlag::Last));
    REQUIRE_FALSE(followed.Find(condition.Id())->Has(ConsumerFlag::LaterControl));
}

TEST_CASE("flags token plans are snapshot-stable order-independent and empty without saves", "[flags-token-cross]") {
    const auto build = [](bool reversed) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbc40})};
        const auto producer = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
        block->SaveFlags(producer, Flags::Parity);
        std::array<const Inst*, 2> publications{};
        const auto publish = [&](unsigned slot, u16 home) {
            block->SetHostGPR(producer, HostRegIndex(home), Imm{0u});
            publications[slot] = &block->GetInstList().back();
        };
        if (reversed) {
            publish(0, 22);
            publish(1, 20);
        } else {
            publish(0, 20);
            publish(1, 22);
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(producer.Id(), HostGPR{20});
        for (auto* publication : publications) allocation.MarkHostWriteCoalesced(publication->Id());
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        return std::tuple{block,
                          producer,
                          arm64::LowerFlagsTokens(analysis,
                                                  allocation.Workspace(),
                                                  allocation,
                                                  FeatureSet{},
                                                  publications,
                                                  analysis.PseudoFlags())};
    };
    const auto [forward_block, forward_producer, forward] = build(false);
    const auto [reversed_block, reversed_producer, reversed] = build(true);
    (void)forward_block; (void)reversed_block;
    REQUIRE(forward.retentions == reversed.retentions);
    REQUIRE(forward.Retains(forward_producer.Id(), 20, forward_producer.Id()));
    REQUIRE(forward.Retains(forward_producer.Id(), 22, forward_producer.Id()));
    REQUIRE(reversed.Retains(reversed_producer.Id(), 20, reversed_producer.Id()));
    REQUIRE(reversed.Retains(reversed_producer.Id(), 22, reversed_producer.Id()));
    REQUIRE_FALSE(forward.Retains(forward_producer.Id(), 21, forward_producer.Id()));
    REQUIRE_FALSE(forward.Retains(UINT32_MAX, 20, forward_producer.Id()));

    IntrusivePtr<Block> plain{new Block(0, Location{0xbc50})};
    const auto plain_producer = plain->LoadImm(ValueType::U64, Imm{swift::u64{7}});
    plain->SetHostGPR(plain_producer, HostRegIndex(20), Imm{0u});
    auto* plain_publication = &plain->GetInstList().back();
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    AllocationFixture plain_allocation{plain->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    plain_allocation.MapRegister(plain_producer.Id(), HostGPR{20});
    plain_allocation.MarkHostWriteCoalesced(plain_publication->Id());
    compile::BlockAnalysisIndex plain_analysis;
    plain_analysis.Build(plain.get(), nullptr, true);
    const std::array<const Inst*, 1> plain_publications{plain_publication};
    const auto zero = arm64::LowerFlagsTokens(plain_analysis,
                                              plain_allocation.Workspace(),
                                              plain_allocation,
                                              FeatureSet{},
                                              plain_publications,
                                              plain_analysis.PseudoFlags());
    REQUIRE(zero.retentions.empty());
    REQUIRE_FALSE(zero.Retains(plain_producer.Id(), 20, plain_producer.Id()));
}

TEST_CASE("cycle scalar and backedge plans coexist reject stale inputs and stay empty", "[cycle-exit-cross-plan]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.global_opts = Optimizations::BlockLink; config.region_edges = true;
    config.arm64_features |= Arm64Features::Atomics;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    const auto build = [&] {
        HIRBuilder builder{4, true, features};
        auto* function = builder.AppendFunction(Location{0xf200}, Location{0xf260});
        auto* a = &function->EditBlock(function->GetCurrentBlock());
        auto* b = function->AppendBlock(Location{0xf220});
        auto* c = function->AppendBlock(Location{0xf240});
        function->AddEdge(a, b); function->AddEdge(a, c); function->AddEdge(b, c);
        function->AddEdge(c, a);
        function->AddEdge(c, b);
        auto zero = function->LoadImm(ValueType::U64, Imm{u64{0}});
        auto loaded = function->LoadMemory(ValueType::U64, ir::Operand{Imm{u64{0x1000}}});
        function->SetHostFPR(loaded, HostRegIndex(17), Imm{0u});
        function->SetHostFPR(zero, HostRegIndex(17), Imm{8u});
        auto address = function->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        auto first = function->LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
        auto result = function->AtomicFetchAdd(ValueType::U64, address, first);
        function->StoreUniform(Uniform{24, ValueType::U64}, result);
        function->EndBlock(terminal::Condition{Cond::EQ, terminal::LinkBlock{Location{0xf220}},
                                             terminal::LinkBlock{Location{0xf240}}});
        function->SetCurBlock(b); function->EndBlock(terminal::LinkBlock{Location{0xf240}});
        function->SetCurBlock(c); function->EndBlock(terminal::Condition{Cond::EQ,
                terminal::LinkBlock{Location{0xf200}}, terminal::LinkBlock{Location{0xf220}}});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        // Exercise scalar lowering against a known bare physical state, then
        // allocate the function separately without changing numbered IR.
        AllocationFixture bare{function->MaxInstrCount(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        compile::BlockAnalysisIndex index;
        index.Build(a->GetBlock());
        const auto scalar =
                arm64::PrepareVector(a->GetBlock(), bare, index, bare.Workspace()).publications;
        compile::BlockAnalysisIndex separate;
        separate.Build(a->GetBlock());
        const auto scalar_independent =
                arm64::PrepareVector(a->GetBlock(), bare, separate, bare.Workspace()).publications;
        AllocationFixture allocation{function->MaxInstrCount(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateFunction(
                function,
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        function, allocation, features));
        REQUIRE(CompleteARM64UnitForEmission(module, allocation, nullptr, function));
        JitContext context{module, allocation, allocation.Actions()};
        JitTranslator translator{context};
        REQUIRE(translator.Translate(function)); context.Finish();
        const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
        return std::tuple{allocation.Actions().Edges(),
                          allocation.Actions().Atomics().instructions.size(),
                          scalar,
                          scalar_independent,
                          std::vector<u8>(bytes, bytes + context.CurrentBufferSize())};
    };
    const auto [first_plan, first_atomic_sites, first_scalar, first_independent, first_code] =
            build();
    const auto [second_plan, second_atomic_sites, second_scalar, second_independent, second_code] =
            build();
    REQUIRE(first_plan.cycle_edges == second_plan.cycle_edges);
    REQUIRE(first_plan.backward_terminal_candidates == second_plan.backward_terminal_candidates);
    REQUIRE(first_code == second_code);
    REQUIRE(first_atomic_sites == second_atomic_sites);
    REQUIRE(first_atomic_sites == 1);
    REQUIRE(first_plan.cycle_edges.size() == 2);
    REQUIRE(first_scalar.actions.size() == 1);
    REQUIRE(first_scalar.actions.front().target == 17);
    REQUIRE(first_scalar.actions.front().kind == arm64::ScalarFPRAction::Load64);
    REQUIRE(first_scalar.actions == first_independent.actions);
    REQUIRE(first_scalar.covered == first_independent.covered);

    IntrusivePtr<Block> other{new Block(0, Location{0xf280})};
    other->SetTerminal(terminal::ReturnToHost{});
    other->ReIdInstr();
    compile::BlockAnalysisIndex stale_index;
    stale_index.Build(other.get());
    AllocationFixture empty_allocation{other->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    const auto stale_scalar =
            arm64::PrepareVector(
                    other.get(), empty_allocation, stale_index, empty_allocation.Workspace())
                    .publications;
    REQUIRE(stale_scalar.actions.empty());
    REQUIRE(stale_scalar.covered.empty());
    compile::BlockAnalysisIndex empty_index;
    empty_index.Build(other.get());
    const auto zero_scalar =
            arm64::PrepareVector(
                    other.get(), empty_allocation, empty_index, empty_allocation.Workspace())
                    .publications;
    REQUIRE(zero_scalar.actions.empty());
    REQUIRE(zero_scalar.covered.empty());

    IntrusivePtr<Block> backedge{new Block(0, Location{0xf2c0})};
    const auto left = backedge->LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
    const auto producer = backedge->Sub(ValueType::U8, left, ir::Operand{Imm{1u}});
    backedge->SaveFlags(producer, Flags::All);
    backedge->InvertCarry();
    backedge->AdvancePC(Imm{u64{1}});
    const auto condition = backedge->LocalCondSet(Cond::EQ);
    backedge->SetTerminal(terminal::If{condition, terminal::LinkBlock{Location{0xf2c0}},
                                        terminal::LinkBlock{Location{0xf2e0}}});
    backedge->ReIdInstr();
    arm64::BackedgeInputs inputs{features};
    inputs.canonical_carry = true;
    inputs.region_enabled = true;
    REQUIRE_FALSE(arm64::LowerBackedge(*backedge, inputs).has_value());
    inputs.self_enabled = true;
    const auto backedge_plan = arm64::LowerBackedge(*backedge, inputs);
    REQUIRE(backedge_plan.has_value());
    REQUIRE(backedge_plan->action.self_target == Location{0xf2c0});
    REQUIRE(backedge_plan->action.Required() == Flags::NZCV);
    IntrusivePtr<Block> plain{new Block(0, Location{0xf300})};
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    REQUIRE_FALSE(arm64::LowerBackedge(*plain, inputs).has_value());
#else
    SUCCEED("native cycle preparation requires AArch64");
#endif
}

TEST_CASE("cycle preparation preserves candidate multiplicity and live module admission", "[cycle-exit-facts]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.global_opts = Optimizations::BlockLink; config.region_edges = true;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{4, true, features};
    auto* function = builder.AppendFunction(Location{0xf000}, Location{0xf060});
    auto* a = &function->EditBlock(function->GetCurrentBlock());
    auto* b = function->AppendBlock(Location{0xf020});
    auto* c = function->AppendBlock(Location{0xf040});
    function->AddEdge(a, b); function->AddEdge(a, c); function->AddEdge(b, c);
    function->AddEdge(c, a); function->AddEdge(c, b);
    function->EndBlock(terminal::Condition{Cond::EQ, terminal::LinkBlock{Location{0xf020}},
                                         terminal::LinkBlock{Location{0xf040}}});
    function->SetCurBlock(b); function->EndBlock(terminal::LinkBlock{Location{0xf040}});
    function->SetCurBlock(c); function->EndBlock(terminal::Condition{Cond::EQ,
            terminal::LinkBlock{Location{0xf000}}, terminal::LinkBlock{Location{0xf020}}});
    function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
    AllocationFixture allocation{function->MaxInstrCount(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    ::swift::runtime::compile::AllocateFunction(
            function,
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, allocation, features));
    REQUIRE(CompleteARM64UnitForEmission(module, allocation, nullptr, function));
    const auto& prepared = allocation.Actions().Edges();
    REQUIRE(prepared.cycle_edges.size() == 2);
    REQUIRE(prepared.local_cycle_source_count == 1);
    REQUIRE(prepared.backward_terminal_candidates.size() == 2);
    for (const auto& edge : prepared.cycle_edges) REQUIRE(edge.first == 0xf040);

    IntrusivePtr<Block> standalone{new Block(0, Location{0xf080})};
    standalone->SetTerminal(terminal::Condition{Cond::EQ,
        terminal::LinkBlock{Location{0xf000}}, terminal::CheckHalt{
            terminal::Condition{Cond::NE, terminal::LinkBlockFast{Location{0xf000}},
                                         terminal::LinkBlock{Location{0xf080}}}}});
    standalone->ReIdInstr();
    AllocationFixture standalone_allocation{standalone->MaxInstrId(),
                                            space.GetTrampolines().GetGPRRegs(),
                                            space.GetTrampolines().GetFPRRegs(),
                                            features};
    ::swift::runtime::compile::AllocateBlock(
            standalone.get(),
            &standalone_allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    standalone.get(), standalone_allocation, features),
            false);
    REQUIRE(CompleteARM64UnitForEmission(module, standalone_allocation, standalone.get(), nullptr));
    const auto& single = standalone_allocation.Actions().Edges();
    REQUIRE(single.local_cycle_source_count == 0);
    REQUIRE(single.backward_terminal_candidates.size() == 1);
    REQUIRE(single.backward_terminal_candidates[0] == std::make_pair(u64{0xf080}, u64{0xf000}));

    // A later mapping changes only live link eligibility; the sealed IR facts
    // and their order remain intact, and an unmap restores the generated code.
    HIRBuilder external_builder{1, true, features};
    auto* external = external_builder.AppendFunction(Location{0xf100}, Location{0xf110});
    auto* external_block = external->EditBlock(external->GetCurrentBlock()).GetBlock();
    external->EndBlock(terminal::Condition{Cond::EQ, terminal::LinkBlock{Location{0xd000}},
                                          terminal::LinkBlock{Location{0xc000}}});
    external->EndFunction(); external->ComputeRPO(); external->IdByRPO();
    AllocationFixture external_allocation{external->MaxInstrCount(),
                                          space.GetTrampolines().GetGPRRegs(),
                                          space.GetTrampolines().GetFPRRegs(),
                                          features};
    ::swift::runtime::compile::AllocateFunction(
            external,
            &external_allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    external, external_allocation, features));
    REQUIRE(CompleteARM64UnitForEmission(module, external_allocation, nullptr, external));
    const auto* plan = &external_allocation.Actions().Edges();
    REQUIRE(plan->backward_terminal_candidates.size() == 2);
    const auto emit = [&](bool can_link) {
        JitContext context{module, external_allocation, external_allocation.Actions()};
        JitTranslator translator{context};
        REQUIRE(context.CanBypassDispatcher(Location{0xd000}) == can_link);
        REQUIRE(translator.Translate(external));
        context.Finish();
        const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
        return std::vector<u8>(bytes, bytes + context.CurrentBufferSize());
    };
    const auto before = emit(true);
    const auto mapped = space.MapModule(0xd000, 0xd100, ModuleConfig{}); REQUIRE(mapped);
    REQUIRE(&external_allocation.Actions().Edges() == plan);
    const auto changed = emit(false); REQUIRE(changed != before);
    space.UnmapModule(0xd000, 0xd100);
    REQUIRE(emit(true) == before);
#else
    SUCCEED("native cycle preparation requires AArch64");
#endif
}
