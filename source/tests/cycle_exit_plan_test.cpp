#include <array>
#include <cstdio>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/alloc/backedge_flags_plan.h"
#include "runtime/backend/arm64/alloc/consumer_plan.h"
#include "runtime/backend/arm64/alloc/flags_token_plan.h"
#include "runtime/backend/arm64/alloc/scalar_fpr_publication_plan.h"
#include "runtime/backend/arm64/alloc/target_allocation_planner.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/opts/register_alloc_pass.h"

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
            auto* first = function->GetCurrentBlock();
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
            RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                                space.GetTrampolines().GetFPRRegs(), features};
            RegisterAllocPass::Run(function, &allocation, features);
            REQUIRE(CompleteARM64UnitForEmission(module, allocation, nullptr, function));
            const auto* actions = allocation.GPRPlanViewFor(first->GetBlock());
            REQUIRE(actions); REQUIRE(actions->region);
            const auto& plan = *actions->region;
            const unsigned external = shape == 1 || shape == 2 ? 1 : shape == 5 ? 2 : 0;
            const unsigned internal = shape == 3 || shape == 4 ? 1 : 0;
            const unsigned mixed = shape == 4 ? 1 : 0;
            REQUIRE(plan.backward_terminal_candidates.size() == external + internal + mixed);
            REQUIRE(plan.local_cycle_source_count == (region ? internal : 0));
            JitContext context{module, allocation}; JitTranslator translator{context};
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
        auto* a = function->GetCurrentBlock();
        auto* extra1 = function->AppendBlock(Location{shuffled ? 0xf040u : 0xf020u});
        auto* extra2 = function->AppendBlock(Location{shuffled ? 0xf020u : 0xf040u});
        auto* b = shuffled ? extra2 : extra1;
        auto* c = shuffled ? extra1 : extra2;
        function->AddEdge(a, b); function->AddEdge(a, c); function->AddEdge(b, c);
        function->AddEdge(c, a); function->AddEdge(c, b);
        auto zero = function->LoadImm(Imm{u64{0}}).SetType(ValueType::U64);
        auto loaded = function->LoadMemory(ir::Operand{Imm{u64{0x1000}}}).SetType(ValueType::U64);
        function->SetHostFPR(loaded, HostRegIndex(17), Imm{0u});
        function->SetHostFPR(zero, HostRegIndex(17), Imm{8u});
        const auto atomic_body = [&](auto* block) {
            function->SetCurBlock(block);
            auto address = function->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
            auto first = function->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
            auto result = function->AtomicFetchAdd(address, first).SetType(ValueType::U64);
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
        RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(function, &allocation, features);
        REQUIRE(CompleteARM64UnitForEmission(module, allocation, nullptr, function));
        const auto& plan = *allocation.GPRPlanViewFor(a->GetBlock())->region;
        JitContext context{module, allocation}; JitTranslator translator{context};
        REQUIRE(translator.Translate(function)); context.Finish();
        const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
        return std::tuple{plan, std::vector<u8>(bytes, bytes + context.CurrentBufferSize())};
    };
    const auto [canonical, canonical_code] = build(false);
    const auto [shuffled, shuffled_code] = build(true);
    REQUIRE(canonical.cycle_edges == shuffled.cycle_edges);
    REQUIRE(canonical.backward_terminal_candidates == shuffled.backward_terminal_candidates);
    REQUIRE(canonical.local_cycle_source_count == shuffled.local_cycle_source_count);
    REQUIRE(canonical_code == shuffled_code);
    REQUIRE(canonical.unaligned_atomics.keys.size() == 2);
    REQUIRE(canonical.unaligned_atomics.counts.size() == 1);
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
    auto* only_block = empty_function->GetCurrentBlock();
    empty_function->EndBlock(terminal::ReturnToHost{});
    empty_function->EndFunction(); empty_function->ComputeRPO(); empty_function->IdByRPO();
    RegAlloc empty_allocation{empty_function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                              space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(empty_function, &empty_allocation, features);
    REQUIRE(CompleteARM64UnitForEmission(module, empty_allocation, nullptr, empty_function));
    const auto& empty_plan = *empty_allocation.GPRPlanViewFor(only_block->GetBlock())->region;
    REQUIRE(empty_plan.cycle_edges.empty());
    REQUIRE(empty_plan.backward_terminal_candidates.empty());
    REQUIRE(empty_plan.local_cycle_source_count == 0);
    REQUIRE(empty_plan.unaligned_atomics.keys.empty());
    JitContext empty_context{module, empty_allocation}; JitTranslator empty_translator{empty_context};
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
        auto* first = function->GetCurrentBlock();
        auto* last = function->AppendBlock(Location{0xf420});
        function->AddEdge(first, last);
        function->AddEdge(last, first);
        const auto left = function->LoadUniform(Uniform{0, ValueType::U8}).SetType(ValueType::U8);
        const auto producer = function->Sub(left, ir::Operand{Imm{1u}}).SetType(ValueType::U8);
        function->SaveFlags(producer, Flags::All);
        function->AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::LinkBlock{Location{0xf420}});
        function->SetCurBlock(last);
        function->AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::Condition{Cond::EQ,
                terminal::LinkBlock{Location{0xf400}}, terminal::LinkBlock{Location{0xd000}}});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        RegAlloc allocation{function->MaxInstrCount(), local_space.GetTrampolines().GetGPRRegs(),
                            local_space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(function, &allocation, features);
        REQUIRE(CompleteARM64UnitForEmission(local_module, allocation, nullptr, function));
        JitContext context{local_module, allocation}; JitTranslator translator{context};
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
    const auto build = [](bool reversed) {
        IntrusivePtr<Block> block{new Block(0, Location{0xd900})};
        const auto producer = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
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
    const auto producer = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
    block->SaveFlags(producer, Flags::All);
    unsigned id = 200;
    for (auto& inst : block->GetInstList()) { inst.SetId(id); id += 3; }
    BlockAnalysisIndex index;
    index.Build(block.get(), nullptr, true);
    const auto& plan = index.PseudoFlags();
    REQUIRE(plan.For(producer.Def()).set == Flags::All);
    IntrusivePtr<Block> foreign{new Block(0, Location{0xd920})};
    const auto foreign_value = foreign->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
    foreign->ReIdInstr();
    REQUIRE(plan.For(foreign_value.Def()).Null());
    REQUIRE_FALSE(plan.SavesParity(foreign_value.Def()));
    for (auto& inst : foreign->GetInstList()) inst.SetId(1000);
    REQUIRE(plan.For(foreign_value.Def()).Null());
    REQUIRE_FALSE(plan.SavesParity(foreign_value.Def()));
    IntrusivePtr<Block> consuming{new Block(0, Location{0xd930})};
    const auto consumed = consuming->LoadImm(Imm{swift::u64{1}}).SetType(ValueType::U64);
    (void)consuming->GetFlags(consumed, Flags::All);
    consuming->ReIdInstr();
    BlockAnalysisIndex empty_index;
    empty_index.Build(consuming.get(), nullptr, true);
    REQUIRE(empty_index.PseudoFlags().Empty());
    REQUIRE(empty_index.PseudoFlags().For(consumed.Def()).Null());
}

TEST_CASE("consumer plan pair results and alu split facts are deterministic and stale-index safe", "[consumer-plan-cross]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xf400})};
    auto input = block->LoadImm(Imm{u64{45}}).SetType(ValueType::U64);
    const std::array<Value, 4> roots{{
        block->Sub(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64),
        block->And(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64),
        block->Or(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64),
        block->Xor(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64)}};
    for (u16 i = 0; i < roots.size(); ++i) block->StoreUniform(Uniform{u16(i * 8), ValueType::U64}, roots[i]);
    const auto producer = block->Div128(input, input, input, Imm{u64{0}}).SetType(ValueType::U64);
    const auto secondary = block->Div128Remainder(producer).SetType(ValueType::U64);
    block->StoreUniform(Uniform{32, ValueType::U64}, secondary);
    block->ReIdInstr();
    arm64::BlockAnalysisIndex index;
    index.Build(block.get());
    const auto build = [&] {
        return arm64::BuildConsumerPlan(index, [](const Inst&) { return true; },
                                        [](const Inst&) { return false; }, nullptr, true);
    };
    const auto plan = build();
    const auto rebuilt = build();
    REQUIRE(plan.valid);
    REQUIRE(plan.pair_results.size() == 1);
    REQUIRE(plan.pair_results.begin()->first == producer.Def());
    REQUIRE(plan.pair_results.begin()->second.secondary == secondary.Def());
    REQUIRE(plan.pair_results.begin()->second.secondary_opcode == OpCode::Div128Remainder);
    REQUIRE(plan.alu_split.size() == roots.size());
    for (auto root : roots) {
        const auto& facts = plan.alu_split.at(root.Def());
        REQUIRE(facts.value_uses == 1);
        REQUIRE(facts.external_users == 1);
        REQUIRE(facts.family_count == 1);
        REQUIRE(facts.saw_value);
        REQUIRE_FALSE(facts.saw_address);
    }
    REQUIRE(rebuilt.pair_results.size() == plan.pair_results.size());
    REQUIRE(rebuilt.alu_split.size() == plan.alu_split.size());
    for (const auto& [inst, action] : plan.pair_results) {
        const auto other = rebuilt.pair_results.find(inst);
        REQUIRE(other != rebuilt.pair_results.end());
        REQUIRE(other->second.secondary == action.secondary);
        REQUIRE(other->second.secondary_opcode == action.secondary_opcode);
    }
    for (const auto& [inst, facts] : plan.alu_split) {
        const auto other = rebuilt.alu_split.find(inst);
        REQUIRE(other != rebuilt.alu_split.end());
        REQUIRE(other->second.value_uses == facts.value_uses);
        REQUIRE(other->second.external_users == facts.external_users);
        REQUIRE(other->second.family_count == facts.family_count);
    }

    IntrusivePtr<Block> foreign{new Block(0, Location{0xf480})};
    foreign->SetTerminal(terminal::ReturnToHost{});
    foreign->ReIdInstr();
    arm64::BlockAnalysisIndex stale;
    stale.Build(foreign.get());
    const auto stale_plan = arm64::BuildConsumerPlan(stale, [](const Inst&) { return true; },
                                                     [](const Inst&) { return false; }, nullptr, true);
    REQUIRE(stale_plan.valid);
    REQUIRE(stale_plan.pair_results.find(producer.Def()) == stale_plan.pair_results.end());
    for (auto root : roots) REQUIRE(stale_plan.alu_split.find(root.Def()) == stale_plan.alu_split.end());

    IntrusivePtr<Block> plain{new Block(0, Location{0xf4c0})};
    (void)plain->LoadImm(Imm{u64{1}}).SetType(ValueType::U64);
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    arm64::BlockAnalysisIndex plain_index;
    plain_index.Build(plain.get());
    const auto zero = arm64::BuildConsumerPlan(plain_index, [](const Inst&) { return true; },
                                               [](const Inst&) { return false; }, nullptr, true);
    REQUIRE(zero.valid);
    REQUIRE(zero.pair_results.empty());
    REQUIRE(zero.alu_split.empty());
    REQUIRE(zero.booleans.empty());
    REQUIRE(zero.comparisons.empty());
}

TEST_CASE("flags token plans are snapshot-stable order-independent and empty without saves", "[flags-token-cross]") {
    const auto build = [](bool reversed) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbc40})};
        const auto producer = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
        block->SaveFlags(producer, Flags::Parity);
        std::array<Inst*, 2> publications{};
        const auto publish = [&](unsigned slot, u16 home) {
            block->SetHostGPR(producer, HostRegIndex(home), Imm{0u});
            publications[slot] = &block->GetInstList().back();
        };
        if (reversed) { publish(0, 22); publish(1, 20); } else { publish(0, 20); publish(1, 22); }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(producer.Id(), HostGPR{20});
        for (auto* publication : publications) allocation.MarkHostWriteCoalesced(publication->Id());
        arm64::BlockAnalysisIndex analysis;
        analysis.Build(block.get(), nullptr, true);
        return std::tuple{block, producer,
                          arm64::BuildFlagsTokenPlan(analysis, allocation, FeatureSet{}, nullptr, &analysis.PseudoFlags())};
    };
    const auto [forward_block, forward_producer, forward] = build(false);
    const auto [reversed_block, reversed_producer, reversed] = build(true);
    (void)forward_block; (void)reversed_block;
    REQUIRE(forward.homes == reversed.homes);
    REQUIRE(forward.point_homes == reversed.point_homes);
    REQUIRE(forward.Retains(forward_producer.Def(), 20, forward_producer.Id()));
    REQUIRE(forward.Retains(forward_producer.Def(), 22, forward_producer.Id()));
    REQUIRE(reversed.Retains(reversed_producer.Def(), 20, reversed_producer.Id()));
    REQUIRE(reversed.Retains(reversed_producer.Def(), 22, reversed_producer.Id()));
    REQUIRE_FALSE(forward.Retains(forward_producer.Def(), 21, forward_producer.Id()));
    REQUIRE_FALSE(forward.Retains(nullptr, 20, forward_producer.Id()));

    IntrusivePtr<Block> plain{new Block(0, Location{0xbc50})};
    const auto plain_producer = plain->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
    plain->SetHostGPR(plain_producer, HostRegIndex(20), Imm{0u});
    auto* plain_publication = &plain->GetInstList().back();
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    RegAlloc plain_allocation{plain->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    plain_allocation.MapRegister(plain_producer.Id(), HostGPR{20});
    plain_allocation.MarkHostWriteCoalesced(plain_publication->Id());
    arm64::BlockAnalysisIndex plain_analysis;
    plain_analysis.Build(plain.get(), nullptr, true);
    const auto zero = arm64::BuildFlagsTokenPlan(plain_analysis, plain_allocation, FeatureSet{});
    REQUIRE(zero.homes.empty());
    REQUIRE(zero.point_homes.empty());
    REQUIRE_FALSE(zero.Retains(plain_producer.Def(), 20, plain_producer.Id()));
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
        auto* a = function->GetCurrentBlock();
        auto* b = function->AppendBlock(Location{0xf220});
        auto* c = function->AppendBlock(Location{0xf240});
        function->AddEdge(a, b); function->AddEdge(a, c); function->AddEdge(b, c);
        function->AddEdge(c, a); function->AddEdge(c, b);
        auto zero = function->LoadImm(Imm{u64{0}}).SetType(ValueType::U64);
        auto loaded = function->LoadMemory(ir::Operand{Imm{u64{0x1000}}}).SetType(ValueType::U64);
        function->SetHostFPR(loaded, HostRegIndex(17), Imm{0u});
        function->SetHostFPR(zero, HostRegIndex(17), Imm{8u});
        auto address = function->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto first = function->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
        auto result = function->AtomicFetchAdd(address, first).SetType(ValueType::U64);
        function->StoreUniform(Uniform{24, ValueType::U64}, result);
        function->EndBlock(terminal::Condition{Cond::EQ, terminal::LinkBlock{Location{0xf220}},
                                             terminal::LinkBlock{Location{0xf240}}});
        function->SetCurBlock(b); function->EndBlock(terminal::LinkBlock{Location{0xf240}});
        function->SetCurBlock(c); function->EndBlock(terminal::Condition{Cond::EQ,
                terminal::LinkBlock{Location{0xf200}}, terminal::LinkBlock{Location{0xf220}}});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        // Scalar publication eligibility is defined against the bare
        // pre-allocation state (the load must still be fusible), so build it
        // over an unallocated RegAlloc for the same block before the real
        // register allocation pass rewrites the IR, exactly like the scalar
        // family suite does.
        RegAlloc bare{function->MaxInstrCount(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        arm64::BlockAnalysisIndex index;
        index.Build(a->GetBlock());
        const auto scalar = arm64::BuildScalarFPRPublicationPlan(a->GetBlock(), bare, &index);
        const auto scalar_independent = arm64::BuildScalarFPRPublicationPlan(a->GetBlock(), bare);
        RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(function, &allocation, features);
        REQUIRE(CompleteARM64UnitForEmission(module, allocation, nullptr, function));
        JitContext context{module, allocation}; JitTranslator translator{context};
        REQUIRE(translator.Translate(function)); context.Finish();
        const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
        return std::tuple{*allocation.GPRPlanViewFor(a->GetBlock())->region, scalar, scalar_independent,
                          std::vector<u8>(bytes, bytes + context.CurrentBufferSize())};
    };
    const auto [first_plan, first_scalar, first_independent, first_code] = build();
    const auto [second_plan, second_scalar, second_independent, second_code] = build();
    REQUIRE(first_plan.cycle_edges == second_plan.cycle_edges);
    REQUIRE(first_plan.backward_terminal_candidates == second_plan.backward_terminal_candidates);
    REQUIRE(first_code == second_code);
    REQUIRE(first_plan.unaligned_atomics.keys.size() == 1);
    REQUIRE(first_plan.cycle_edges.size() == 2);
    REQUIRE(first_scalar.loads.size() == 1);
    REQUIRE(first_scalar.loads.begin()->second.target == 17);
    REQUIRE(first_scalar.loads.size() == first_independent.loads.size());
    REQUIRE(first_scalar.values.size() == first_independent.values.size());
    REQUIRE(first_scalar.covered == first_independent.covered);

    IntrusivePtr<Block> other{new Block(0, Location{0xf280})};
    other->SetTerminal(terminal::ReturnToHost{});
    other->ReIdInstr();
    arm64::BlockAnalysisIndex stale_index;
    stale_index.Build(other.get());
    RegAlloc empty_allocation{other->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    const auto stale_scalar = arm64::BuildScalarFPRPublicationPlan(other.get(), empty_allocation, &stale_index);
    REQUIRE(stale_scalar.loads.empty());
    REQUIRE(stale_scalar.values.empty());
    REQUIRE(stale_scalar.covered.empty());
    const auto zero_scalar = arm64::BuildScalarFPRPublicationPlan(other.get(), empty_allocation);
    REQUIRE(zero_scalar.loads.empty());
    REQUIRE(zero_scalar.values.empty());
    REQUIRE(zero_scalar.covered.empty());

    IntrusivePtr<Block> backedge{new Block(0, Location{0xf2c0})};
    const auto left = backedge->LoadUniform(Uniform{0, ValueType::U8}).SetType(ValueType::U8);
    const auto producer = backedge->Sub(left, ir::Operand{Imm{1u}}).SetType(ValueType::U8);
    backedge->SaveFlags(producer, Flags::All);
    backedge->InvertCarry();
    backedge->AdvancePC(Imm{u64{1}});
    const auto condition = backedge->LocalCondSet(Cond::EQ);
    backedge->SetTerminal(terminal::If{condition, terminal::LinkBlock{Location{0xf2c0}},
                                        terminal::LinkBlock{Location{0xf2e0}}});
    backedge->ReIdInstr();
    arm64::BackedgeFlagsInputs inputs{features};
    inputs.canonical_carry = true;
    inputs.region_enabled = true;
    REQUIRE_FALSE(arm64::BuildBackedgeFlagsPlan(*backedge, inputs).has_value());
    inputs.self_enabled = true;
    const auto backedge_plan = arm64::BuildBackedgeFlagsPlan(*backedge, inputs);
    REQUIRE(backedge_plan.has_value());
    REQUIRE(backedge_plan->self_target == Location{0xf2c0});
    REQUIRE(backedge_plan->requested == Flags::NZCV);
    IntrusivePtr<Block> plain{new Block(0, Location{0xf300})};
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    REQUIRE_FALSE(arm64::BuildBackedgeFlagsPlan(*plain, inputs).has_value());
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
    auto* a = function->GetCurrentBlock();
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
    RegAlloc allocation{function->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(function, &allocation, features);
    REQUIRE(CompleteARM64UnitForEmission(module, allocation, nullptr, function));
    const auto& prepared = *allocation.GPRPlanViewFor(a->GetBlock())->region;
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
    RegAlloc standalone_allocation{standalone->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                                   space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(standalone.get(), &standalone_allocation, false, features);
    REQUIRE(CompleteARM64UnitForEmission(module, standalone_allocation, standalone.get(), nullptr));
    const auto& single = *standalone_allocation.GPRPlanViewFor(standalone.get())->region;
    REQUIRE(single.local_cycle_source_count == 0);
    REQUIRE(single.backward_terminal_candidates.size() == 1);
    REQUIRE(single.backward_terminal_candidates[0] == std::make_pair(u64{0xf080}, u64{0xf000}));

    // A later mapping changes only live link eligibility; the sealed IR facts
    // and their order remain intact, and an unmap restores the generated code.
    HIRBuilder external_builder{1, true, features};
    auto* external = external_builder.AppendFunction(Location{0xf100}, Location{0xf110});
    auto* external_block = external->GetCurrentBlock()->GetBlock();
    external->EndBlock(terminal::Condition{Cond::EQ, terminal::LinkBlock{Location{0xd000}},
                                          terminal::LinkBlock{Location{0xc000}}});
    external->EndFunction(); external->ComputeRPO(); external->IdByRPO();
    RegAlloc external_allocation{external->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(external, &external_allocation, features);
    REQUIRE(CompleteARM64UnitForEmission(module, external_allocation, nullptr, external));
    const auto plan = external_allocation.GPRPlanFor(external_block)->region;
    REQUIRE(plan->backward_terminal_candidates.size() == 2);
    const auto emit = [&](bool can_link) {
        JitContext context{module, external_allocation}; JitTranslator translator{context};
        REQUIRE(context.CanBypassDispatcher(Location{0xd000}) == can_link);
        REQUIRE(translator.Translate(external)); context.Finish();
        const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
        return std::vector<u8>(bytes, bytes + context.CurrentBufferSize());
    };
    const auto before = emit(true);
    const auto mapped = space.MapModule(0xd000, 0xd100, ModuleConfig{}); REQUIRE(mapped);
    REQUIRE(external_allocation.GPRPlanFor(external_block)->region == plan);
    const auto changed = emit(false); REQUIRE(changed != before);
    space.UnmapModule(0xd000, 0xd100);
    REQUIRE(emit(true) == before);
#else
    SUCCEED("native cycle preparation requires AArch64");
#endif
}
