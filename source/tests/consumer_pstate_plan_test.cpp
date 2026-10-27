#include "runtime/backend/arm64/compile/certificates.h"
#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/compile/workspace.h"
using swift::runtime::compile::BlockAnalysisIndex;
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/lower_consumers.h"
#include "runtime/backend/arm64/compile/lower_predicates.h"
#include "runtime/backend/arm64/jit/function_code_object_emitter.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/context.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/div128.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/ir/hir_builder.h"
#include "support/register_alloc_test_support.h"

using namespace swift::runtime;
using namespace swift::runtime::ir;
using namespace swift::runtime::backend::arm64;

namespace {
const SelectAction* SelectFor(const SelectActions& lowered, swift::u32 id) {
    const auto found = std::ranges::find(lowered.actions, id, &SelectAction::instruction);
    return found == lowered.actions.end() ? nullptr : &*found;
}
}  // namespace

TEST_CASE("named FCmp representation is consistent across internal block edges", "[consumer-plan][fcmp-condition]") {
#if defined(__aarch64__)
    for (unsigned mode : {0u, 1u, 2u}) {
        DYNAMIC_SECTION("publication mode " << mode) {
        const bool publish = mode == 1;
        CAPTURE(mode, publish);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 4096; config.stack_alignment = 16; config.region_edges = true;
        swift::runtime::backend::AddressSpace space{config};
        auto module = space.GetDefaultModule();
        HIRBuilder builder{1, true, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{0xbf90}, Location{0xbfa1});
        const auto left = function->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
        const auto right = function->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
        const auto compared = function->VecFCmp(
                ValueType::U64, left, right, Imm{swift::u32{64}}, Imm{swift::u32{1}});
        if (publish) function->PublishFCmpFlags(compared, Imm{swift::u32{1}});
        auto* second = builder.LinkBlock(terminal::LinkBlock{Location{0xbfa0}});
        builder.SetCurBlock(second);
        if (!publish) {
            const auto one = function->LoadImm(ValueType::U32, Imm{swift::u32{1}});
            const auto result = function->Add(ValueType::U32, one, ir::Operand{one});
            function->SaveFlags(result, Flags::NZCV);
        }
        if (mode == 2) function->PublishFCmpFlags(compared, Imm{swift::u32{1}});
        else {
            const auto condition =
                    function->FCmpCondSet(ValueType::U8, compared, publish ? Cond::VS : Cond::LT);
            function->StoreUniform(Uniform{256, ValueType::U8}, condition);
        }
        function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
        function->ComputeRPO(); function->IdByRPO();
        swift::runtime::test::AllocationFixture allocation{function->MaxInstrCount(),
                                                           space.GetTrampolines().GetGPRRegs(),
                                                           space.GetTrampolines().GetFPRRegs(),
                                                           FeatureSet{}};
        ::swift::runtime::compile::AllocateFunction(
                function,
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        function, allocation, FeatureSet{}));
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        JitContext context{module, allocation, allocation.Actions()};
        JitTranslator translator{context};
        translator.Translate(function); context.Finish();
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != swift::runtime::backend::INVALID_CACHE_ID);
        context.Flush(buffer); REQUIRE(module->Push(function->GetFunction()));
        function->MarkFunctionPublished();
        space.PushCodeCache(function->GetFunction()->GetStartLocation(), buffer.exec_data);
        swift::runtime::Runtime runtime{&space};
        const double first = publish ? std::numeric_limits<double>::quiet_NaN() : 4.0;
        const double second_value = 3.0;
        std::memcpy(runtime.GetUniformBuffer().data(), &first, sizeof(first));
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &second_value, sizeof(second_value));
        runtime.SetLocation(0xbf90);
        REQUIRE(runtime.Run() == swift::runtime::HaltReason::CallHost);
        if (mode == 2) {
            const auto raw_flags = runtime.GetState()->host_cpu_flags;
            REQUIRE((raw_flags & (swift::u64{1} << HostFlagsBit::C)) != 0);
            REQUIRE((raw_flags & (swift::u64{1} << HostFlagsBit::Z)) == 0);
        } else REQUIRE(runtime.GetUniformBuffer()[256] == publish);
        }
    }
#endif
}

TEST_CASE("FCmp condition retains its named relation across later flags producers", "[consumer-plan][fcmp-condition]") {
#if defined(__aarch64__)
    for (unsigned bits : {32u, 64u}) for (bool pressure : {false, true})
    for (bool compact : {false, true}) for (bool publish : {false, true})
    for (unsigned change : {0u, 1u, 2u, 3u, 4u, 5u}) for (unsigned relation = 0; relation < 4; ++relation)
    for (unsigned condition_code = 0; condition_code < 16; ++condition_code) {
        // Publication-only cases have no named condition; one relation row
        // verifies their delayed publication without duplicating Cond rows.
        if (change == 5 && (!publish || condition_code)) continue;
        CAPTURE(bits, pressure, compact, publish, change, relation, condition_code);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 4096; config.stack_alignment = 16;
        swift::runtime::backend::AddressSpace space{config};
        auto module = space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block(0, Location{0xbf60})};
        std::vector<Value> live;
        if (pressure)
            for (unsigned i = 0; i < 36; ++i)
                live.push_back(block->LoadUniform(
                        ValueType::U64,
                        Uniform{static_cast<swift::u16>(512 + i * 8), ValueType::U64}));
        const auto left = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
        const auto right = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
        const auto integer = block->LoadUniform(ValueType::U32, Uniform{32, ValueType::U32});
        const auto one = block->LoadImm(ValueType::U8, Imm{swift::u32{1}});
        const auto zero = block->LoadImm(ValueType::U8, Imm{swift::u32{0}});
        const auto compared = block->VecFCmp(ValueType::U64,
                                             left,
                                             right,
                                             Imm{static_cast<swift::u32>(bits)},
                                             Imm{swift::u32{compact}});
        if (publish && change != 5) block->PublishFCmpFlags(compared, Imm{swift::u32{compact}});
        if (change == 4) {
            const auto early = block->FCmpCondSet(ValueType::U8, compared, Cond::GT);
            block->StoreUniform(Uniform{258, ValueType::U8}, early);
        }
        Value replacement;
        if (change == 1 || change == 2 || change == 4 || change == 5) {
            replacement = block->Add(ValueType::U32, integer, ir::Operand{integer});
            if (change == 1 || change == 4 || change == 5)
                block->SaveFlags(replacement, Flags::NZCV);
        }
        if (change == 5) block->PublishFCmpFlags(compared, Imm{swift::u32{compact}});
        const auto cond = static_cast<Cond>(condition_code);
        const auto condition =
                change == 5 ? one : block->FCmpCondSet(ValueType::U8, compared, cond);
        if (change == 2) block->SaveFlags(replacement, Flags::NZCV);
        if (change == 3) {
            replacement = block->Add(ValueType::U32, integer, ir::Operand{integer});
            block->SaveFlags(replacement, Flags::NZCV);
        }
        const auto selected = block->Select(ValueType::U8, condition, one, zero);
        block->StoreUniform(Uniform{256, ValueType::U8}, selected);
        const auto current_zero = block->CondSet(ValueType::U8, Cond::EQ);
        block->StoreUniform(Uniform{257, ValueType::U8}, current_zero);
        if (publish) {
            const auto carry = block->TestFlags(ValueType::U8, Flags::Carry);
            block->StoreUniform(Uniform{259, ValueType::U8}, carry);
            block->InvertCarry();
            const auto inverted = block->TestFlags(ValueType::U8, Flags::Carry);
            block->StoreUniform(Uniform{260, ValueType::U8}, inverted);
        }
        for (auto value : live) block->StoreUniform(Uniform{1024, ValueType::U64}, value);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        swift::runtime::test::AllocationFixture allocation{block->MaxInstrId(),
                                                           space.GetTrampolines().GetGPRRegs(),
                                                           space.GetTrampolines().GetFPRRegs(),
                                                           FeatureSet{}};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, FeatureSet{}),
                false);
        if (pressure) REQUIRE(allocation.SpillCount() > 0);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        JitContext context{module, allocation, allocation.Actions()};
        JitTranslator translator{context};
        translator.Translate(block.get()); context.Finish();
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != swift::runtime::backend::INVALID_CACHE_ID);
        context.Flush(buffer); REQUIRE(module->Push(block.get()));
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        swift::runtime::Runtime runtime{&space};
        const double first = relation == 0 ? 4.0 : relation == 1 ? 2.0 : relation == 2 ? 3.0 : std::numeric_limits<double>::quiet_NaN();
        const double second = 3.0; const swift::u32 seven = 7;
        if (bits == 32) {
            const float first32 = static_cast<float>(first), second32 = static_cast<float>(second);
            std::memcpy(runtime.GetUniformBuffer().data(), &first32, sizeof(first32));
            std::memcpy(runtime.GetUniformBuffer().data() + 16, &second32, sizeof(second32));
        } else {
            std::memcpy(runtime.GetUniformBuffer().data(), &first, sizeof(first));
            std::memcpy(runtime.GetUniformBuffer().data() + 16, &second, sizeof(second));
        }
        std::memcpy(runtime.GetUniformBuffer().data() + 32, &seven, sizeof(seven));
        runtime.SetLocation(block->GetStartLocation().Value());
        REQUIRE(runtime.Run() == swift::runtime::HaltReason::CallHost);
        const bool n = relation == 1, z = relation == 2, c = relation != 1, v = relation == 3;
        const std::array<bool, 16> expected{z, !z, c, !c, n, !n, v, !v, c && !z, !c || z,
                n == v, n != v, !z && n == v, z || n != v, true, true};
        REQUIRE(runtime.GetUniformBuffer()[256] == (change == 5 ? true : expected[condition_code]));
        if (change) REQUIRE(runtime.GetUniformBuffer()[257] == (change == 5 && (relation == 2 || relation == 3)));
        if (change == 4) REQUIRE(runtime.GetUniformBuffer()[258] == (relation == 0));
        if (publish) {
            const bool carry = change && change != 5 ? false : ((relation == 1 || relation == 3) != compact);
            REQUIRE(runtime.GetUniformBuffer()[259] == carry);
            REQUIRE(runtime.GetUniformBuffer()[260] == !carry);
        }
    }
#endif
}

TEST_CASE("select consumes the condition captured before later flags changes", "[consumer-plan][boolean-select]") {
#if defined(__aarch64__)
    for (bool local : {false, true}) for (bool boolean : {false, true}) for (unsigned change : {0u, 1u, 2u}) {
        CAPTURE(local, boolean, change);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 4096;
        config.stack_alignment = 16;
        swift::runtime::backend::AddressSpace space{config};
        auto module = space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block(0, Location{0xbf40})};
        const auto input = block->LoadImm(ValueType::U32, Imm{swift::u32{7}});
        const auto result = block->Sub(ValueType::U32, input, ir::Operand{input});
        block->SaveFlags(result, Flags::NZCV);
        const auto condition = (local ? block->LocalCondSet(ValueType::U8, Cond::EQ)
                                      : block->CondSet(ValueType::U8, Cond::EQ));
        Value replacement;
        if (change) {
            replacement = block->Add(ValueType::U32, input, ir::Operand{input});
            if (change == 1) block->SaveFlags(replacement, Flags::NZCV);
        }
        const auto type = boolean ? ValueType::U8 : ValueType::U64;
        const auto on_true = block->LoadImm(type, Imm{swift::u64{boolean ? 1u : 11u}});
        const auto on_false = block->LoadImm(type, Imm{swift::u64{boolean ? 0u : 22u}});
        const auto selected = block->Select(type, condition, on_true, on_false);
        if (change == 2) block->SaveFlags(replacement, Flags::NZCV);
        block->StoreUniform(Uniform{256, type}, selected);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        swift::runtime::test::AllocationFixture allocation{block->MaxInstrId(),
                                                           space.GetTrampolines().GetGPRRegs(),
                                                           space.GetTrampolines().GetFPRRegs(),
                                                           FeatureSet{}};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, FeatureSet{}),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        JitContext context{module, allocation, allocation.Actions()};
        JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != swift::runtime::backend::INVALID_CACHE_ID);
        context.Flush(buffer);
        REQUIRE(module->Push(block.get()));
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        swift::runtime::Runtime runtime{&space};
        swift::u64 value = 0;
        std::memcpy(runtime.GetUniformBuffer().data() + 256, &value, sizeof(value));
        runtime.SetLocation(block->GetStartLocation().Value());
        REQUIRE(runtime.Run() == swift::runtime::HaltReason::CallHost);
        std::memcpy(&value, runtime.GetUniformBuffer().data() + 256, sizeof(value));
        REQUIRE(value == (boolean ? 1u : 11u));
    }
#endif
}

namespace {
SVM_HELPER_PRESERVE_ALL SVM_HELPER_GENERAL_REGS_ONLY
HostPairResult PairResultProbe(swift::u64 a, swift::u64 b, swift::u64 c) {
    return {a ^ c, b + c};
}

ConsumerActions Plan(Block& block) {
    block.ReIdInstr();
    compile::SemanticFacts facts{&block};
    auto& analysis = facts.PrepareIndex(&block);
    const auto terminal = block.GetTerminal();
    compile::CompileWorkspace workspace;
    return LowerConsumers(
            analysis,
            workspace,
            [](const Inst&) { return true; },
            [](const Inst&) { return false; },
            &terminal);
}
}  // namespace

TEST_CASE("predicate preparation builds direct edges only for observed queries",
          "[consumer-plan][boolean-select][facts]") {
    enum Shape {
        Empty,
        UnusedCondition,
        SharedCondition,
        StoredCondition,
        TerminalCondition,
        ForeignUser,
        CompoundClear,
        AuditOnly,
        BooleanSelect,
        DirectSelect,
        FuturePseudo,
        ForeignPseudo,
        OrdinarySelect
    };
    compile::CompileWorkspace workspace;
    for (const auto shape : {Empty,
                             UnusedCondition,
                             SharedCondition,
                             StoredCondition,
                             TerminalCondition,
                             ForeignUser,
                             CompoundClear,
                             AuditOnly,
                             BooleanSelect,
                             DirectSelect,
                             FuturePseudo,
                             ForeignPseudo,
                             OrdinarySelect}) {
        CAPTURE(shape);
        IntrusivePtr<Block> block{new Block(0, Location{0xbfb0})};
        IntrusivePtr<Block> other{new Block(0, Location{0xbfb8})};
        Value condition, selected;
        const Inst* clear{};
        if (shape >= UnusedCondition && shape <= ForeignUser) {
            condition = block->CondSet(ValueType::U8, Cond::EQ);
            if (shape == SharedCondition || shape == StoredCondition)
                block->StoreUniform(Uniform{0, ValueType::U8}, condition);
            if (shape == SharedCondition) block->StoreUniform(Uniform{8, ValueType::U8}, condition);
            if (shape == ForeignUser) other->StoreUniform(Uniform{0, ValueType::U8}, condition);
        } else if (shape == CompoundClear || shape == AuditOnly) {
            const auto input = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
            const auto result = shape == CompoundClear
                                        ? block->Xor(ValueType::U64, input, ir::Operand{input})
                                        : block->Sub(ValueType::U64, input, ir::Operand{Imm{1u}});
            if (shape == CompoundClear) {
                block->ClearFlags(Flags::CV | Flags::AuxiliaryCarry);
                clear = &block->GetInstList().back();
                block->SaveFlags(result, Flags::NZ | Flags::Parity);
            }
            block->StoreUniform(Uniform{8, ValueType::U64}, result);
        } else if (shape >= BooleanSelect) {
            const auto type = shape == OrdinarySelect ? ValueType::U64 : ValueType::U8;
            const auto one = block->LoadImm(type, Imm{swift::u64{1}});
            const auto zero = block->LoadImm(type, Imm{swift::u64{0}});
            if (shape == BooleanSelect)
                condition = one;
            else if (shape == OrdinarySelect)
                condition = block->LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
            else
                condition = block->CondSet(ValueType::U8, Cond::EQ);
            const auto filler = block->LoadImm(ValueType::U8, Imm{swift::u64{3}});
            selected = block->Select(type, condition, one, zero);
            block->StoreUniform(Uniform{8, type}, selected);
            if (shape == FuturePseudo) block->SaveFlags(filler, Flags::Zero);
            if (shape == ForeignPseudo) other->SaveFlags(filler, Flags::Zero);
        }
        if (shape == TerminalCondition)
            block->SetTerminal(
                    terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToHost{}});
        else
            block->SetTerminal(terminal::ReturnToHost{});
        other->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        other->ReIdInstr();
        compile::SemanticFacts facts{block.get()};
        auto& analysis = facts.PrepareIndex(block.get());
        REQUIRE_FALSE(analysis.UsesReady());
        REQUIRE(analysis.PrepareUsesOf(nullptr).empty());
        REQUIRE_FALSE(analysis.PrepareHasPseudoUse(nullptr));
        const auto selects = LowerSelects(analysis, workspace, [](const Inst&) { return true; });
        REQUIRE(analysis.UsesReady() == (shape == FuturePseudo || shape == ForeignPseudo));
        const auto* selection = selected.Defined() ? SelectFor(selects, selected.Id()) : nullptr;
        REQUIRE((selection != nullptr) == (shape >= BooleanSelect && shape != OrdinarySelect));
        if (selection) {
            REQUIRE(selection->boolean);
            REQUIRE(selection->DirectCondition().has_value() ==
                    (shape == DirectSelect || shape == ForeignPseudo));
        }
        const auto terminal_node = block->GetTerminal();
        const auto consumers = LowerConsumers(
                analysis,
                workspace,
                [](const Inst&) { return true; },
                [](const Inst&) { return false; },
                &terminal_node,
                shape == AuditOnly);
        REQUIRE(consumers.valid);
        const bool needs_edges = shape == StoredCondition || shape == TerminalCondition ||
                                 shape == ForeignUser || shape == CompoundClear ||
                                 shape == DirectSelect || shape == FuturePseudo ||
                                 shape == ForeignPseudo;
        REQUIRE(analysis.UsesReady() == needs_edges);
        REQUIRE((facts.FindUses(block.get()) != nullptr) == needs_edges);
        REQUIRE(consumers.audit.size() == (shape == AuditOnly ? 1 : 0));
        if (shape >= UnusedCondition && shape <= ForeignUser) {
            REQUIRE(consumers.Has(condition.Id(), ConsumerFlag::Boolean));
            REQUIRE(consumers.Has(condition.Id(), ConsumerFlag::LocalCondition) ==
                    (shape == TerminalCondition));
        }
        if (clear) REQUIRE(consumers.Has(clear->Id(), ConsumerFlag::CompoundClear));
        if (shape == ForeignUser) REQUIRE(analysis.Uses(condition.Def()).empty());
    }
}

TEST_CASE("pair-result preparation retains owned and explicitly absent secondaries", "[consumer-plan][pair-result]") {
    for (bool cpuid : {false, true}) for (bool secondary : {false, true}) {
        CAPTURE(cpuid, secondary);
        IntrusivePtr<Block> block{new Block(0, Location{0xbfc0})};
        const auto input = block->LoadImm(ValueType::U64, Imm{swift::u64{17}});
        const auto producer =
                (cpuid ? block->Cpuid(ValueType::U64,
                                      input,
                                      input,
                                      Imm{swift::u64{3}},
                                      Imm{swift::u64{0}})
                       : block->Div128(ValueType::U64, input, input, input, Imm{swift::u64{0}}));
        const Inst* selected = nullptr;
        if (secondary)
            selected = (cpuid ? block->CpuidUpper(ValueType::U64, producer)
                              : block->Div128Remainder(ValueType::U64, producer))
                               .Def();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BlockAnalysisIndex index;
        index.Build(block.get());
        REQUIRE(index.HasConsumerDecisions());
        const auto plan = Plan(*block);
        REQUIRE(plan.valid);
        REQUIRE(std::ranges::count_if(plan.actions, [](const auto& row) {
                    return row.Has(ConsumerFlag::Pair);
                }) == 1);
        REQUIRE(plan.Find(producer.Id()));
        REQUIRE(plan.Find(producer.Id())->related == (selected ? selected->Id() : UINT16_MAX));
        REQUIRE(plan.Find(producer.Id())->Has(ConsumerFlag::Pair));
    }
}

TEST_CASE("pair-result preparation rejects foreign duplicate and malformed ownership", "[consumer-plan][pair-result]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    for (bool cpuid : {false, true}) for (unsigned failure = 0; failure < 7; ++failure) {
        CAPTURE(cpuid, failure);
        IntrusivePtr<Block> block{new Block(0, Location{0xbfc8})};
        IntrusivePtr<Block> foreign{new Block(1, Location{0xbfd0})};
        const auto input = block->LoadImm(ValueType::U64, Imm{swift::u64{17}});
        auto producer =
                (cpuid ? block->Cpuid(ValueType::U64,
                                      input,
                                      input,
                                      Imm{swift::u64{3}},
                                      Imm{swift::u64{0}})
                       : block->Div128(ValueType::U64, input, input, input, Imm{swift::u64{0}}));
        if (failure == 4) producer = input;
        auto* owner = failure == 0 ? foreign.get() : block.get();
        const auto secondary = (cpuid ? owner->CpuidUpper(ValueType::U64, producer)
                                      : owner->Div128Remainder(ValueType::U64, producer));
        if (failure == 1) {
            if (cpuid)
                block->CpuidUpper(ValueType::U64, producer);
            else
                block->Div128Remainder(ValueType::U64, producer);
        } else if (failure == 2)
            compile::RewriteContext{owner, compilation_workspace, nullptr, false}.Change(
                    owner, secondary.Def(), [](Inst& target) { target.SetReturn(ValueType::U32); });
        else if (failure == 3) {
            if (cpuid)
                block->Div128Remainder(ValueType::U64, producer);
            else
                block->CpuidUpper(ValueType::U64, producer);
        } else if (failure == 5) {
            compile::RewriteContext{block.get(), compilation_workspace, nullptr, false}.Change(
                    block.get(), secondary.Def(), [&](Inst& target) {
                        block->RemoveInst(&target);
                        block->InsertBefore(&target, producer.Def());
                    });
        } else if (failure == 6)
            compile::RewriteContext{owner, compilation_workspace, nullptr, false}.SetArg(
                    owner, secondary.Def(), 0, producer.SetCastType(ValueType::U32));
        block->SetTerminal(terminal::ReturnToHost{});
        REQUIRE_FALSE(Plan(*block).valid);
    }
}

TEST_CASE("pair-result admission rejects missing immutable facts before output", "[consumer-plan][pair-result][pin-plan]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    swift::runtime::backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xbfe0})};
    const auto input = block->LoadImm(ValueType::U64, Imm{swift::u64{17}});
    const auto producer = block->Div128(ValueType::U64, input, input, input, Imm{swift::u64{0}});
    block->StoreUniform(Uniform{0, ValueType::U64}, producer);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    FeatureSet features{};
    swift::runtime::test::AllocationFixture allocation{block->MaxInstrId(),
                                                       space.GetTrampolines().GetGPRRegs(),
                                                       space.GetTrampolines().GetFPRRegs(),
                                                       features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    const auto owner = allocation.Actions().ForBlock(block.get());
    REQUIRE(allocation.Actions().Consumer(producer.Id()));
    REQUIRE(allocation.Actions().Consumer(producer.Id())->Has(ConsumerFlag::Pair));
    REQUIRE(allocation.Actions().Consumer(producer.Id())->related == UINT16_MAX);
    std::vector<ConsumerAction> saved_consumers;
    swift::u32 saved_revision = allocation.Actions().ConsumerRevision() + 1;
    RegisterAllocTestSupport::SwapConsumerActionsForAdmissionTest(
            allocation.Actions(), saved_consumers, saved_revision);
    JitContext context{module, allocation, allocation.Actions()};
    JitTranslator translator{context};
    REQUIRE_FALSE(translator.Translate(block.get()));
    REQUIRE(context.CurrentBufferSize() == 0);
    RegisterAllocTestSupport::SwapConsumerActionsForAdmissionTest(
            allocation.Actions(), saved_consumers, saved_revision);
    REQUIRE(allocation.Actions().Certificate()->MatchesUnit(
            block.get(), nullptr, allocation, allocation.Actions(), config, features));
#endif
}

TEST_CASE("cyclic pair pseudo links reject before early flags preparation", "[consumer-plan][pair-result][pin-plan]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    IntrusivePtr<Block> block{new Block(0, Location{0xbfe8})};
    const auto input = block->LoadImm(ValueType::U64, Imm{swift::u64{17}});
    const auto producer = block->Div128(ValueType::U64, input, input, input, Imm{swift::u64{0}});
    const auto secondary = block->Div128Remainder(ValueType::U64, producer);
    block->SetTerminal(terminal::ReturnToHost{});
    compile::RewriteContext{block.get(), compilation_workspace, nullptr, false}.SetArg(
            block.get(), secondary.Def(), 1, producer);
    // Use accounting and index construction no longer walk the pseudo
    // chain (un-chaining step 3), so a malformed double use cannot
    // hang or corrupt them; the pair relationship itself is still
    // rejected by consumer-plan validation below.
    REQUIRE(producer.Def()->GetUses(false) == 2);
    REQUIRE(producer.Def()->GetUses() == 0);
    BlockAnalysisIndex index;
    index.Build(block.get(), nullptr, true);
    REQUIRE_FALSE(Plan(*block).valid);
}

TEST_CASE("prepared pair results preserve native helper and spilled value oracles", "[consumer-plan][pair-result][pressure]") {
#if defined(__aarch64__)
    for (unsigned shape = 0; shape < 3; ++shape) for (bool sign : {false, true})
    for (bool secondary : {false, true}) for (bool pressure : {false, true}) {
        if (shape == 2 && sign) continue;
        CAPTURE(shape, sign, secondary, pressure);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 4096;
        config.stack_alignment = 16;
        swift::runtime::backend::AddressSpace space{config};
        auto module = space.GetDefaultModule();
        const auto features = ResolveFeatureSet(module->GetModuleConfig());
        IntrusivePtr<Block> block{new Block(0, Location{0xbff0})};
        std::vector<Value> live;
        if (pressure)
            for (unsigned i = 0; i < 36; ++i)
                live.push_back(
                        block->LoadUniform(ValueType::U64, Uniform{512 + i * 8, ValueType::U64}));
        const auto low = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        const auto divisor = block->LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
        auto high = block->LoadImm(ValueType::U64, Imm{swift::u64{0}});
        if (shape == 0 && sign) high = block->AsrImm(ValueType::U64, low, Imm{swift::u8{63}});
        if (shape == 1) high = block->LoadUniform(ValueType::U64, Uniform{16, ValueType::U64});
        const auto producer =
                (shape == 2 ? block->Cpuid(ValueType::U64,
                                           low,
                                           divisor,
                                           Imm{swift::u64{9}},
                                           Imm{reinterpret_cast<swift::u64>(&PairResultProbe)})
                            : block->Div128(
                                      ValueType::U64, high, low, divisor, Imm{swift::u64{sign}}));
        Value other;
        if (secondary)
            other = (shape == 2 ? block->CpuidUpper(ValueType::U64, producer)
                                : block->Div128Remainder(ValueType::U64, producer));
        block->StoreUniform(Uniform{256, ValueType::U64}, producer);
        if (secondary) block->StoreUniform(Uniform{264, ValueType::U64}, other);
        for (unsigned i = 0; i < live.size(); ++i)
            block->StoreUniform(Uniform{2048 + i * 8, ValueType::U64}, live[i]);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        swift::runtime::test::AllocationFixture allocation{block->MaxInstrId(),
                                                           space.GetTrampolines().GetGPRRegs(),
                                                           space.GetTrampolines().GetFPRRegs(),
                                                           features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        const auto plan = allocation.Actions().ForBlock(block.get());
        if (pressure) REQUIRE(allocation.SpillCount() > 0);
        REQUIRE(allocation.Actions().Consumer(producer.Id())->related ==
                (secondary ? other.Id() : UINT16_MAX));
        if (shape != 2) {
            REQUIRE(allocation.Actions().ScratchFor(producer.Def()));
            REQUIRE(allocation.Actions().ScratchFor(producer.Def())->div128_native == (shape == 0));
        }
        JitContext context{module, allocation, allocation.Actions()};
        JitTranslator translator{context};
        REQUIRE(translator.Translate(block.get()));
        context.Finish();
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != swift::runtime::backend::INVALID_CACHE_ID);
        context.Flush(buffer);
        REQUIRE(module->Push(block.get()));
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        swift::runtime::Runtime runtime{&space};
        const auto put = [&](unsigned offset, swift::u64 value) {
            std::memcpy(runtime.GetUniformBuffer().data() + offset, &value, sizeof(value));
        };
        const auto get = [&](unsigned offset) {
            swift::u64 value{};
            std::memcpy(&value, runtime.GetUniformBuffer().data() + offset, sizeof(value));
            return value;
        };
        for (unsigned sample = 0; sample < 3; ++sample) {
            const swift::u64 input = sample == 1 ? static_cast<swift::u64>(-101ll) : 100;
            const swift::u64 d = sample == 2 ? 0 : 7;
            const swift::u64 hi = shape == 1 ? 1 : sign && static_cast<swift::s64>(input) < 0 ? UINT64_MAX : 0;
            std::memset(runtime.GetUniformBuffer().data(), 0xa5, 4096);
            put(0, input); put(8, d); put(16, hi);
            for (unsigned i = 0; i < live.size(); ++i) put(512 + i * 8, 0x1122334455667788ull + i * 17 + sample);
            const auto expected = shape == 2 ? PairResultProbe(input, d, 9)
                    : sign ? DivideSigned128(hi, input, d) : DivideUnsigned128(hi, input, d);
            runtime.SetLocation(block->GetStartLocation().Value());
            REQUIRE(runtime.Run() == swift::runtime::HaltReason::CallHost);
            REQUIRE(get(256) == expected.first);
            REQUIRE(get(264) == (secondary ? expected.second : 0xa5a5a5a5a5a5a5a5ull));
            for (unsigned i = 0; i < live.size(); ++i)
                REQUIRE(get(2048 + i * 8) == 0x1122334455667788ull + i * 17 + sample);
        }
    }
#endif
}

TEST_CASE("a later block missing pair facts rejects the whole function before output", "[consumer-plan][pair-result][region-plan]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.region_edges = true;
    swift::runtime::backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0xc010}, Location{0xc021});
    const auto marker = function->LoadImm(ValueType::U64, Imm{swift::u64{7}});
    function->StoreUniform(Uniform{0, ValueType::U64}, marker);
    auto* later = builder.LinkBlock(terminal::LinkBlock{Location{0xc020}});
    builder.SetCurBlock(later);
    const auto input = function->LoadImm(ValueType::U64, Imm{swift::u64{17}});
    const auto producer = function->Div128(ValueType::U64, input, input, input, Imm{swift::u64{0}});
    function->StoreUniform(Uniform{8, ValueType::U64}, producer);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO(); function->IdByRPO();
    swift::runtime::test::AllocationFixture allocation{function->MaxInstrCount(),
                                                       space.GetTrampolines().GetGPRRegs(),
                                                       space.GetTrampolines().GetFPRRegs(),
                                                       features};
    ::swift::runtime::compile::AllocateFunction(
            function,
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, allocation, features));
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    const auto owner = allocation.Actions().ForBlock(later->GetBlock());
    REQUIRE(allocation.Actions().Consumer(producer.Id()));
    REQUIRE(allocation.Actions().Consumer(producer.Id())->Has(ConsumerFlag::Pair));
    std::vector<ConsumerAction> saved_consumers;
    swift::u32 saved_revision = allocation.Actions().ConsumerRevision() + 1;
    RegisterAllocTestSupport::SwapConsumerActionsForAdmissionTest(
            allocation.Actions(), saved_consumers, saved_revision);
    JitContext context{module, allocation, allocation.Actions()};
    JitTranslator translator{context};
    REQUIRE_FALSE(translator.Translate(function));
    REQUIRE(context.CurrentBufferSize() == 0);
#endif
}

TEST_CASE("a later code-object region missing pair facts rejects before any output", "[consumer-plan][pair-result][region-plan]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.region_edges = true;
    swift::runtime::backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{2, true, features};
    const auto make = [&](swift::u64 start, bool pair) {
        auto* function = builder.AppendFunction(Location{start}, Location{start + 16});
        const auto input = function->LoadImm(ValueType::U64, Imm{swift::u64{17}});
        const auto result =
                pair ? function->Div128(ValueType::U64, input, input, input, Imm{swift::u64{0}})
                     : input;
        function->StoreUniform(Uniform{pair ? 8u : 0u, ValueType::U64}, result);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        return function;
    };
    auto* first = make(0xc040, false);
    auto* second = make(0xc060, true);
    const std::array<HIRFunction*, 2> inputs{first, second};
    auto prepared = PrepareFunctions(module, inputs);
    REQUIRE(prepared.has_value());
    const auto& second_actions = prepared->at(1).Actions();
    auto* second_block = second->GetHIRBlocksRPO().front().GetBlock();
    const auto owner = second_actions.ForBlock(second_block);
    const Inst* pair{};
    for (const auto& inst : second_block->GetInstList())
        if (inst.GetOp() == OpCode::Div128) pair = &inst;
    REQUIRE(pair);
    REQUIRE(second_actions.Consumer(pair->Id()));
    REQUIRE(second_actions.Consumer(pair->Id())->Has(ConsumerFlag::Pair));
    std::vector<ConsumerAction> saved_consumers;
    swift::u32 saved_revision = second_actions.ConsumerRevision() + 1;
    RegisterAllocTestSupport::SwapConsumerActionsForAdmissionTest(
            second_actions, saved_consumers, saved_revision);
    FunctionCodeObjectEmitter emitter{module, *prepared};
    REQUIRE_FALSE(emitter.Emit(false));
    REQUIRE(emitter.CurrentBufferSize() == 0);
#endif
}

TEST_CASE("local captured condition plans verify through direct and terminal users", "[consumer-plan][fcmp-condition]") {
    for (bool local : {false, true}) for (bool terminal : {false, true})
    for (unsigned change = 0; change < 4; ++change) {
        CAPTURE(local, terminal, change);
        IntrusivePtr<Block> block{new Block(0, Location{0xbf68})};
        const auto vector = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
        const auto compared = block->VecFCmp(
                ValueType::U64, vector, vector, Imm{swift::u32{64}}, Imm{swift::u32{0}});
        const auto condition = (local ? block->LocalCondSet(ValueType::U8, Cond::EQ)
                                      : block->FCmpCondSet(ValueType::U8, compared, Cond::EQ));
        if (change) {
            const auto value = block->LoadImm(ValueType::U32, Imm{swift::u32{1}});
            const auto added = block->Add(ValueType::U32, value, ir::Operand{value});
            if (change == 1) block->SaveFlags(added, Flags::NZCV);
            if (change == 2) block->BranchOnlyFlags(added, Flags::NZCV);
            if (change == 3) block->SaveFlags(added, Flags::Parity);
        }
        if (terminal) block->SetTerminal(terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToHost{}});
        else { (void)block->Goto(condition); block->SetTerminal(terminal::ReturnToHost{}); }
        const auto plan = Plan(*block);
        // The shared conservative window contract also excludes PF-only
        // pseudos: qualification does not guess at their eventual encoding.
        REQUIRE(plan.Has(condition.Id(), ConsumerFlag::LocalCondition) == (change == 0));
    }
}

TEST_CASE("compact publication plans preserve named comparison before publication", "[consumer-plan][fcmp-condition]") {
    for (unsigned change = 0; change < 4; ++change) {
        CAPTURE(change);
        IntrusivePtr<Block> block{new Block(0, Location{0xbf70})};
        const auto vector = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
        const auto compared = block->VecFCmp(
                ValueType::U64, vector, vector, Imm{swift::u32{64}}, Imm{swift::u32{1}});
        if (change) {
            const auto value = block->LoadImm(ValueType::U32, Imm{swift::u32{1}});
            const auto added = block->Add(ValueType::U32, value, ir::Operand{value});
            if (change == 1) block->SaveFlags(added, Flags::NZCV);
            if (change == 2) block->BranchOnlyFlags(added, Flags::NZCV);
            if (change == 3) block->SaveFlags(added, Flags::Parity);
        }
        block->PublishFCmpFlags(compared, Imm{swift::u32{1}});
        block->SetTerminal(terminal::ReturnToHost{});
        const auto plan = Plan(*block);
        REQUIRE(plan.Has(compared.Id(), ConsumerFlag::PackedRelation) == (change != 0));
    }
}

namespace {
struct FCmpMetrics {
    swift::u64 ns{}, hash{1469598103934665603ull}, spills{};
    size_t bytes{}, raw{}, carrier{};
};
FCmpMetrics CompileFCmp(unsigned shape, unsigned length, unsigned pressure) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 4096;
    swift::runtime::backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xbf78})};
    std::vector<Value> live, comparisons;
    for (unsigned i = 0; i < pressure; ++i)
        live.push_back(block->LoadUniform(
                ValueType::U64, Uniform{static_cast<swift::u16>(512 + i * 8), ValueType::U64}));
    for (unsigned i = 0; i < length; ++i) {
        const auto left = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
        const auto right = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
        const bool compact = shape == 1;
        const auto compared = block->VecFCmp(
                ValueType::U64, left, right, Imm{swift::u32{64}}, Imm{swift::u32{compact}});
        if (shape) block->PublishFCmpFlags(compared, Imm{swift::u32{compact}});
        const auto condition = block->FCmpCondSet(ValueType::U8, compared, Cond::GT);
        block->StoreUniform(Uniform{256, ValueType::U8}, condition);
        if (shape == 3) {
            // A neutral reader and a long intervening window must not force
            // the next named reader to reconstruct an unchanged relation.
            for (unsigned gap = 0; gap < 8; ++gap)
                (void)block->LoadUniform(ValueType::U64, Uniform{128, ValueType::U64});
            const auto another = block->FCmpCondSet(ValueType::U8, compared, Cond::LT);
            block->StoreUniform(Uniform{257, ValueType::U8}, another);
        }
        comparisons.push_back(compared);
    }
    for (auto value : live) block->StoreUniform(Uniform{1024, ValueType::U64}, value);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    swift::runtime::test::AllocationFixture allocation{block->MaxInstrId(),
                                                       space.GetTrampolines().GetGPRRegs(),
                                                       space.GetTrampolines().GetFPRRegs(),
                                                       FeatureSet{}};
    const auto begin = std::chrono::steady_clock::now();
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, FeatureSet{}),
            false);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    JitContext context{module, allocation, allocation.Actions()};
    JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    FCmpMetrics result;
    result.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    result.bytes = context.CurrentBufferSize(); result.spills = allocation.SpillCount();
    const auto plan = allocation.Actions().ForBlock(block.get());
    for (auto compared : comparisons) {
        const auto* action = allocation.Actions().Consumer(compared.Id());
        REQUIRE(action);
        result.raw += action->Has(ConsumerFlag::RawSingle);
        result.carrier += action->Has(ConsumerFlag::CompactCarrier);
    }
    const auto* code = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    for (size_t i = 0; i < result.bytes; ++i) { result.hash ^= code[i]; result.hash *= 1099511628211ull; }
    return result;
}
}
TEST_CASE("FCmp condition compilation metrics retain safe target selections", "[.fcmp-condition-metrics]") {
    for (unsigned shape : {0u, 1u, 2u, 3u}) for (unsigned pressure : {0u, 36u})
    for (unsigned length : {2u, 8u, 32u, 128u}) {
        // The historical full-publication path aborts under this pressure.
        // Keep its comparable timing cells separate from correctness pressure.
        if (shape >= 2 && pressure) continue;
        CAPTURE(shape, pressure, length);
        std::vector<swift::u64> times; FCmpMetrics sample;
        for (unsigned iteration = 0; iteration < 25; ++iteration) {
            const auto result = CompileFCmp(shape, length, pressure);
            REQUIRE(result.raw == (shape == 0 ? length : 0));
            REQUIRE(result.carrier == (shape == 1 ? length : 0));
            if (iteration) REQUIRE(result.hash == sample.hash);
            if (iteration >= 5) times.push_back(result.ns);
            sample = result;
        }
        std::sort(times.begin(), times.end());
        std::printf("fcmp-metrics shape=%u pressure=%u length=%u bytes=%zu hash=%llu raw=%zu carrier=%zu spills=%llu median_ns=%llu\n",
                shape, pressure, length, sample.bytes, (unsigned long long)sample.hash, sample.raw, sample.carrier,
                (unsigned long long)sample.spills, (unsigned long long)times[times.size() / 2]);
        std::fflush(stdout);
    }
}
TEST_CASE("boolean select plans preserve flags windows and exclusive local coverage", "[consumer-plan][boolean-select]") {
    compile::CompileWorkspace predicate_workspace;
    for (unsigned scenario = 0; scenario < 20; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xbf48})};
        IntrusivePtr<Block> parent{new Block(0, Location{0xbf50})};
        auto condition = (scenario == 4 || scenario == 17 ? parent.get() : block.get())
                                 ->CondSet(ValueType::U8, Cond::EQ);
        const auto select_type = scenario == 8 ? ValueType::U64 : ValueType::U8;
        auto on_true = (scenario == 13 ? parent.get() : block.get())
                               ->LoadImm(select_type, Imm{swift::u64{scenario == 7 ? 2u : 1u}});
        const auto on_false = block->LoadImm(select_type, Imm{swift::u64{0}});
        if (scenario == 18) condition = on_true;
        if (scenario == 19) condition = on_false;
        if (scenario == 1) block->ClearFlags(Flags::Zero);
        if (scenario == 2) (void)block->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{1}}});
        if (scenario == 3) (void)block->Goto(on_false);
        if (scenario == 5) block->StoreUniform(Uniform{0, ValueType::U8}, condition);
        Value future_flags;
        if (scenario == 15) future_flags = block->Add(ValueType::U8, on_true, ir::Operand{on_true});
        if (scenario == 9 || scenario == 10)
            for (unsigned depth = 0; depth < (scenario == 9 ? 3u : 4u); ++depth)
                condition = block->And(ValueType::U8, condition, ir::Operand{on_true});
        const auto selected = block->Select(select_type, condition, on_true, on_false);
        if (scenario == 6) block->StoreUniform(Uniform{8, ValueType::U8}, on_true);
        if (scenario == 11) block->SetTerminal(terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToHost{}});
        else
            block->SetTerminal(terminal::ReturnToHost{});
        if (scenario == 12) (void)block->Select(ValueType::U8, condition, on_true, on_false);
        if (scenario == 14) block->SaveFlags(on_true, Flags::Zero);
        if (scenario == 15) block->SaveFlags(future_flags, Flags::Zero);
        parent->ReIdInstr(); block->ReIdInstr();
        if (scenario >= 16) {
            compile::CompileWorkspace workspace;
            for (auto* owner : {block.get(), parent.get()}) {
                compile::RewriteContext editing{owner, workspace, nullptr, false};
                for (const auto& inst : owner->GetInstList())
                    editing.Change(
                            owner, &inst, [](Inst& node) { node.SetId(100 + node.Id() * 3); });
            }
        }
        BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        REQUIRE(analysis.HasBooleanSelects());
        REQUIRE(analysis.DenseInstructionIDs() == (scenario < 16));
        const auto plan = LowerSelects(analysis, predicate_workspace, [](const Inst& inst) {
            return inst.GetOp() != OpCode::ClearFlags && inst.GetOp() != OpCode::Goto &&
                   inst.GetOp() != OpCode::CallLambda;
        });
        const bool boolean = scenario != 7 && scenario != 8 && scenario != 10;
        const bool direct = scenario == 0 || scenario == 6 || scenario == 7 || scenario == 8 ||
                            scenario == 13 || scenario == 16;
        REQUIRE((SelectFor(plan, selected.Id()) != nullptr) == (boolean || direct));
        if (boolean || direct) {
            REQUIRE(SelectFor(plan, selected.Id())->boolean == boolean);
            REQUIRE(SelectFor(plan, selected.Id())->DirectCondition().has_value() == direct);
        }
        REQUIRE(SelectFor(plan, UINT32_MAX) == nullptr);
        const auto covered = [&](const Inst* node) {
            return analysis.Contains(node) &&
                   std::find(plan.covered.begin(), plan.covered.end(), node->Id()) !=
                           plan.covered.end();
        };
        REQUIRE(covered(condition.Def()) == direct);
        REQUIRE(covered(on_true.Def()) ==
                (boolean && scenario != 6 && scenario != 9 && scenario != 13 && scenario != 14 &&
                 scenario != 15 && scenario != 18));
        REQUIRE(covered(on_false.Def()) == (boolean && scenario != 3 && scenario != 19));
        if (scenario == 12) REQUIRE(plan.actions.size() == 2);
        parent->ReIdInstr();
        analysis.Build(parent.get());
        REQUIRE_FALSE(analysis.HasBooleanSelects());
    }
}
TEST_CASE("local condition eligibility preserves direct and terminal use contracts", "[consumer-plan]") {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbf00})};
        const auto condition = block->CondSet(Cond::EQ);
        if (scenario == 0) (void)block->Goto(condition);
        if (scenario == 1) block->StoreUniform(Uniform{0, ValueType::U8}, condition);
        if (scenario == 2) { (void)block->Goto(condition); block->StoreUniform(Uniform{0, ValueType::U8}, condition); }
        if (scenario >= 3) {
            Terminal terminal{terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToHost{}}};
            if (scenario == 4) terminal = terminal::CheckHalt{terminal};
            block->SetTerminal(terminal);
        } else block->SetTerminal(terminal::ReturnToHost{});
        const auto plan = Plan(*block);
        REQUIRE(plan.Has(condition.Id(), ConsumerFlag::LocalCondition) ==
                (scenario == 0 || scenario >= 3));
    }
}
TEST_CASE("flag prefix snapshots preserve polarity clears and single-user multiplicity", "[consumer-plan]") {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbf08})};
        const auto value = block->LoadImm(ValueType::U32, Imm{7u});
        const auto producer = (scenario == 1 || scenario == 2
                                       ? block->Sub(ValueType::U32, value, ir::Operand{Imm{1u}})
                                       : block->Add(ValueType::U32, value, ir::Operand{Imm{1u}}));
        block->SaveFlags(producer, Flags::Carry | Flags::Zero | Flags::Overflow);
        if (scenario == 2) block->InvertCarry();
        if (scenario == 3) block->ClearFlags(Flags::Carry | Flags::Zero);
        if (scenario == 4) (void)block->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{1}}});
        const auto test = block->TestFlags(ValueType::U32, Flags::Carry);
        (void)block->Add(ValueType::U32, test, ir::Operand{scenario == 5 ? test : value});
        block->SetTerminal(terminal::ReturnToHost{});
        const auto plan = Plan(*block);
        REQUIRE(plan.pstate.has_value());
        const auto flags = plan.pstate->At(test.Def());
        REQUIRE(True(flags & Flags::Carry) == (scenario == 0 || scenario == 2 || scenario == 5));
        REQUIRE(True(flags & Flags::Zero) == (scenario != 3 && scenario != 4));
    }
}
TEST_CASE("later PSTATE demand includes nested terminals and ignores earlier readers", "[consumer-plan]") {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbf10})};
        (void)block->CondSet(Cond::EQ);
        const auto test = block->TestFlags(Flags::NZCV);
        if (scenario == 1) (void)block->CondSet(Cond::NE);
        if (scenario == 2) block->SetTerminal(terminal::CheckHalt{
                terminal::Condition{Cond::EQ, terminal::ReturnToHost{}, terminal::ReturnToHost{}}});
        else block->SetTerminal(terminal::ReturnToHost{});
        const auto plan = Plan(*block);
        REQUIRE(plan.Has(test.Id(), ConsumerFlag::LaterPstate) == (scenario != 0));
    }
}

TEST_CASE("compound clear plans require exact logical shape and flags contract", "[consumer-plan][compound-flags]") {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xbf18})};
        const auto input = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
        const auto producer =
                (scenario == 1 ? block->Add(ValueType::U32, input, ir::Operand{input})
                               : block->Xor(ValueType::U32, input, ir::Operand{input}));
        if (scenario == 2) (void)block->LoadImm(Imm{1u});
        block->ClearFlags(scenario == 3 ? Flags::Carry : Flags::CV | Flags::AuxiliaryCarry);
        auto* clear = &block->GetInstList().back();
        block->SaveFlags(producer, scenario == 4 ? Flags::NZ : Flags::NZ | Flags::Parity);
        if (scenario == 5) block->BranchOnlyFlags(producer, Flags::NZCV);
        block->SetTerminal(terminal::ReturnToHost{});
        const auto plan = Plan(*block);
        REQUIRE(plan.Has(clear->Id(), ConsumerFlag::CompoundClear) == (scenario == 0));
        if (scenario == 0) REQUIRE(plan.Find(clear->Id())->related == producer.Id());
    }
}
TEST_CASE("boolean select plans are order-independent over independent selects", "[boolean-select]") {
    compile::CompileWorkspace predicate_workspace;
    struct Facts {
        bool a_boolean{}, a_direct{}, b_boolean{}, b_direct{};
        Cond b_cond{Cond::NE};
        std::vector<swift::u16> covered;
        swift::u16 select_a{}, select_b{}, cond_b{}, one{}, zero{};
        size_t actions{};
    };
    const auto build = [&predicate_workspace](bool second_first) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbf60})};
        const auto one = block->LoadImm(ValueType::U8, Imm{swift::u64{1}});
        const auto zero = block->LoadImm(ValueType::U8, Imm{swift::u64{0}});
        Value cond_a, cond_b, select_a, select_b;
        const auto make_a = [&] {
            cond_a = block->CondSet(ValueType::U8, Cond::NE);
            block->StoreUniform(Uniform{0, ValueType::U8}, cond_a);
            select_a = block->Select(ValueType::U8, cond_a, one, zero);
            block->StoreUniform(Uniform{8, ValueType::U8}, select_a);
        };
        const auto make_b = [&] {
            cond_b = block->CondSet(ValueType::U8, Cond::EQ);
            select_b = block->Select(ValueType::U8, cond_b, one, zero);
            block->StoreUniform(Uniform{16, ValueType::U8}, select_b);
        };
        if (second_first) {
            make_b();
            make_a();
        } else {
            make_a();
            make_b();
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        const auto plan =
                LowerSelects(analysis, predicate_workspace, [](const Inst&) { return true; });
        Facts facts;
        facts.actions = plan.actions.size();
        facts.select_a = select_a.Id();
        facts.select_b = select_b.Id();
        facts.cond_b = cond_b.Id();
        facts.one = one.Id();
        facts.zero = zero.Id();
        const auto* a = SelectFor(plan, select_a.Id());
        const auto* b = SelectFor(plan, select_b.Id());
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        facts.a_boolean = a->boolean;
        facts.a_direct = a->DirectCondition().has_value();
        facts.b_boolean = b->boolean;
        facts.b_direct = b->DirectCondition().has_value();
        if (b->DirectCondition()) facts.b_cond = *b->DirectCondition();
        facts.covered.assign(plan.covered.begin(), plan.covered.end());
        REQUIRE(SelectFor(plan, cond_a.Id()) == nullptr);
        REQUIRE(SelectFor(plan, UINT32_MAX) == nullptr);
        return facts;
    };
    const auto forward = build(false);
    REQUIRE(forward.actions == 2);
    REQUIRE(forward.a_boolean);
    REQUIRE_FALSE(forward.a_direct);
    REQUIRE(forward.b_boolean);
    REQUIRE(forward.b_direct);
    REQUIRE(forward.b_cond == Cond::EQ);
    const auto covered_has = [](const Facts& facts, swift::u16 id) {
        return std::find(facts.covered.begin(), facts.covered.end(), id) != facts.covered.end();
    };
    REQUIRE(covered_has(forward, forward.cond_b));
    REQUIRE(covered_has(forward, forward.one));
    REQUIRE(covered_has(forward, forward.zero));
    REQUIRE(forward.covered.size() == 3);
    const auto swapped = build(true);
    REQUIRE(swapped.actions == forward.actions);
    REQUIRE(swapped.a_boolean == forward.a_boolean);
    REQUIRE(swapped.a_direct == forward.a_direct);
    REQUIRE(swapped.b_boolean == forward.b_boolean);
    REQUIRE(swapped.b_direct == forward.b_direct);
    REQUIRE(swapped.b_cond == forward.b_cond);
    REQUIRE(swapped.covered.size() == forward.covered.size());
    REQUIRE(covered_has(swapped, swapped.cond_b));
    REQUIRE(covered_has(swapped, swapped.one));
    REQUIRE(covered_has(swapped, swapped.zero));
}
TEST_CASE(
        "boolean select plans consume one indexed roster and gate direct arms on the defer "
        "predicate",
        "[boolean-select]") {
    compile::CompileWorkspace predicate_workspace;
    IntrusivePtr<Block> block{new Block(0, Location{0xbf68})};
    const auto one = block->LoadImm(ValueType::U8, Imm{swift::u64{1}});
    const auto zero = block->LoadImm(ValueType::U8, Imm{swift::u64{0}});
    const auto condition = block->CondSet(ValueType::U8, Cond::EQ);
    const auto filler = block->LoadImm(ValueType::U8, Imm{swift::u64{3}});
    block->StoreUniform(Uniform{24, ValueType::U8}, filler);
    const auto selected = block->Select(ValueType::U8, condition, one, zero);
    block->StoreUniform(Uniform{0, ValueType::U8}, selected);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    IntrusivePtr<Block> other{new Block(0, Location{0xbf70})};
    other->LoadUniform(Uniform{0, ValueType::U64});
    other->SetTerminal(terminal::ReturnToHost{});
    other->ReIdInstr();
    BlockAnalysisIndex stale;
    stale.Build(other.get());
    const auto foreign = LowerSelects(stale, predicate_workspace, [](const Inst&) { return true; });
    REQUIRE(foreign.actions.empty());
    REQUIRE(foreign.covered.empty());
    REQUIRE(SelectFor(foreign, selected.Id()) == nullptr);
    BlockAnalysisIndex analysis;
    analysis.Build(block.get());
    const auto gated =
            LowerSelects(analysis, predicate_workspace, [](const Inst&) { return false; });
    const auto* gated_action = SelectFor(gated, selected.Id());
    REQUIRE(gated_action != nullptr);
    REQUIRE(gated_action->boolean);
    REQUIRE_FALSE(gated_action->DirectCondition().has_value());
    REQUIRE(std::find(gated.covered.begin(), gated.covered.end(), condition.Id()) ==
            gated.covered.end());
    const auto open = LowerSelects(analysis, predicate_workspace, [](const Inst&) { return true; });
    const auto* open_action = SelectFor(open, selected.Id());
    REQUIRE(open_action != nullptr);
    REQUIRE(open_action->boolean);
    REQUIRE(open_action->DirectCondition() == Cond::EQ);
    REQUIRE(std::find(open.covered.begin(), open.covered.end(), condition.Id()) !=
            open.covered.end());
    AllocationFixture allocation{
            block->MaxInstrId(), backend::GPRSMask{}, backend::FPRSMask{}, FeatureSet{}};
    allocation.BeginTargetPlanning();
    allocation.StoreSelects(open);
    REQUIRE(allocation.Actions().Select(selected.Id()));
    REQUIRE(allocation.Actions().InstructionCovered(condition.Id()));
    allocation.EndTargetPlanning();
    REQUIRE(allocation.Actions().Select(selected.Id())->DirectCondition() == Cond::EQ);
    compile::Allocation other_allocation{
            block->MaxInstrId(), backend::GPRSMask{}, backend::FPRSMask{}, FeatureSet{}};
    REQUIRE_FALSE(allocation.Actions().MatchesAllocation(other_allocation.View()));
    allocation.ResetAllocations();
    REQUIRE_FALSE(allocation.Actions().Select(selected.Id()));
    REQUIRE_FALSE(allocation.Actions().InstructionCovered(condition.Id()));
    IntrusivePtr<Block> plain{new Block(0, Location{0xbf80})};
    const auto plain_condition = plain->CondSet(ValueType::U8, Cond::EQ);
    plain->StoreUniform(Uniform{0, ValueType::U8}, plain_condition);
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    BlockAnalysisIndex plain_analysis;
    plain_analysis.Build(plain.get());
    const auto zero_plan =
            LowerSelects(plain_analysis, predicate_workspace, [](const Inst&) { return true; });
    REQUIRE(zero_plan.actions.empty());
    REQUIRE(zero_plan.covered.empty());
    REQUIRE(SelectFor(zero_plan, plain_condition.Id()) == nullptr);
}

TEST_CASE("consumer actions keep inversion windows and receiving-edge ownership",
          "[consumer-plan][regression]") {
    for (unsigned count = 0; count < 4; ++count) {
        CAPTURE(count);
        IntrusivePtr<Block> block{new Block(0, Location{0xc200})};
        block->InvertCarry();
        const auto* before = &block->GetInstList().back();
        const auto vector = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
        const auto compared = block->VecFCmp(ValueType::U64, vector, vector, Imm{64u}, Imm{1u});
        block->PublishFCmpFlags(compared, Imm{1u});
        std::vector<const Inst*> inversions;
        for (unsigned i = 0; i < count; ++i) {
            block->InvertCarry();
            inversions.push_back(&block->GetInstList().back());
        }
        const auto reader = block->FCmpCondSet(ValueType::U8, compared, Cond::LT);
        block->InvertCarry();
        const auto* after = &block->GetInstList().back();
        block->StoreUniform(Uniform{16, ValueType::U8}, reader);
        block->SetTerminal(terminal::ReturnToHost{});
        const auto lowered = Plan(*block);
        REQUIRE(lowered.valid);
        const auto* row = lowered.Find(reader.Id());
        std::vector<const Inst*> pending{nullptr, before, after};
        pending.insert(pending.end(), inversions.begin(), inversions.end());
        for (const auto* skipped : pending) {
            bool expected = true;
            for (const auto* inversion : inversions)
                if (inversion != skipped) expected = !expected;
            const bool actual =
                    row ? row->CarryInverted(skipped ? skipped->Id() : UINT32_MAX) : true;
            REQUIRE(actual == expected);
        }
        if (count) {
            REQUIRE(row);
            REQUIRE(row->related == compared.Id());
        }
    }

    std::array<IntrusivePtr<Block>, 2> prefixes{
            IntrusivePtr<Block>{new Block(0, Location{0xc204})},
            IntrusivePtr<Block>{new Block(0, Location{0xc208})}};
    std::array<const Inst*, 2> flag_tests{};
    for (unsigned i = 0; i < prefixes.size(); ++i) {
        auto& prefix = prefixes[i];
        const auto value = prefix->LoadImm(ValueType::U32, Imm{1u});
        const auto added = prefix->Add(ValueType::U32, value, ir::Operand{value});
        prefix->SaveFlags(added, Flags::Carry | Flags::Zero);
        flag_tests[i] = prefix->TestFlags(ValueType::U8, Flags::Carry).Def();
        prefix->SetTerminal(terminal::ReturnToHost{});
        prefix->ReIdInstr();
    }
    BlockAnalysisIndex prefix_index;
    prefix_index.Build(prefixes[0].get());
    const auto pstate = PreparePstateEligibility(prefix_index);
    REQUIRE(flag_tests[0]->Id() == flag_tests[1]->Id());
    REQUIRE(True(pstate.At(flag_tests[0]) & Flags::Carry));
    REQUIRE(pstate.At(flag_tests[1]) == Flags::None);

    IntrusivePtr<Block> foreign{new Block(0, Location{0xc210})};
    const auto foreign_vector = foreign->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
    const auto foreign_comparison =
            foreign->VecFCmp(ValueType::U64, foreign_vector, foreign_vector, Imm{64u}, Imm{1u});
    const auto zero = foreign->LoadImm(ValueType::U64, Imm{swift::u64{0}});
    foreign->SetTerminal(terminal::ReturnToHost{});
    foreign->ReIdInstr();
    IntrusivePtr<Block> block{new Block(0, Location{0xc220})};
    const auto vector = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
    const auto compared = block->VecFCmp(ValueType::U64, vector, vector, Imm{64u}, Imm{1u});
    const auto local = block->FCmpCondSet(ValueType::U8, compared, Cond::LT);
    block->StoreUniform(Uniform{16, ValueType::U8}, local);
    const auto incoming = block->FCmpCondSet(ValueType::U8, foreign_comparison, Cond::LT);
    block->StoreUniform(Uniform{24, ValueType::U8}, incoming);
    block->StoreUniform(Uniform{32, ValueType::U64}, zero);
    const auto* store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    REQUIRE(compared.Id() == foreign_comparison.Id());
    REQUIRE(local.Id() == zero.Id());
    BlockAnalysisIndex index;
    index.Build(block.get());
    const auto terminal_node = block->GetTerminal();
    compile::CompileWorkspace workspace;
    auto lowered = LowerConsumers(
            index,
            workspace,
            [](const Inst&) { return true; },
            [](const Inst&) { return false; },
            &terminal_node);
    REQUIRE(lowered.valid);
    REQUIRE(lowered.Has(compared.Id(), ConsumerFlag::RawSingle));
    REQUIRE_FALSE(lowered.Has(compared.Id(), ConsumerFlag::PackedRelation));
    REQUIRE(lowered.Has(local.Id(), ConsumerFlag::RawReader));
    REQUIRE(lowered.Has(incoming.Id(), ConsumerFlag::IncomingPacked));
    REQUIRE_FALSE(lowered.Has(incoming.Id(), ConsumerFlag::RawReader));
    swift::runtime::test::AllocationFixture allocation{block->MaxInstrId(),
                                                       swift::runtime::backend::GPRSMask{},
                                                       swift::runtime::backend::FPRSMask{},
                                                       FeatureSet{}};
    allocation.MapRegister(zero.Id(), HostGPR{13});
    std::vector<const Inst*> incoming_zeros;
    (void)ProveZeroStores(index, allocation, &incoming_zeros);
    REQUIRE(incoming_zeros == std::vector<const Inst*>{zero.Def()});
    lowered.AddIncomingZeros(index, incoming_zeros);
    const auto* use = lowered.Find(store->Id());
    REQUIRE(use);
    REQUIRE(use->MatchesZeroInput(*store, zero));
    REQUIRE_FALSE(use->MatchesZeroInput(*store, local));
    allocation.BeginTargetPlanning();
    allocation.StoreConsumers(lowered);
    allocation.EndTargetPlanning();
    const auto& actions = allocation.Actions();
    REQUIRE(actions.Ready());
    REQUIRE(actions.Consumer(store->Id())->MatchesZeroInput(*store, zero));
    REQUIRE_FALSE(actions.Consumer(store->Id())->MatchesZeroInput(*store, local));
    allocation.ResetAllocations();
    REQUIRE_FALSE(actions.Consumer(store->Id()));
    REQUIRE_FALSE(actions.Consumer(compared.Id()));
}
