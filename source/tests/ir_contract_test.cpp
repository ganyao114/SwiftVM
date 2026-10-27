#include "tests/support/rewrite_fixture.h"
using swift::runtime::test::RewriteFixture;
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/compile/analysis/flag_flow.h"
#include "runtime/compile/analysis/terminal_uses.h"
#include "runtime/compile/analysis/value_bits.h"
#include "runtime/compile/audit/verify.h"
#include "runtime/compile/workspace.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#include "base/logging.h"
#include "base/scope_exit.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/allocation_preparation.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/constraints.h"
#include "runtime/backend/arm64/compile/fixed_validation.h"
#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/backend/arm64/compile/lower_integer_flags.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/backend/arm64/compile/residence_preparation.h"
#include "runtime/backend/arm64/compile/target.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/arm64/jit/function_code_object_emitter.h"
#include "runtime/backend/context.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/runtime.h"
#include "runtime/compile/allocate/controller.h"
#include "runtime/compile/allocate/scratch.h"
#include "runtime/compile/allocate/transaction.h"
#include "runtime/compile/allocate/writer.h"
#include "runtime/compile/analysis/facts.h"
#include "runtime/compile/analysis/effects.h"
#include "runtime/compile/analysis/guest_state.h"
#include "runtime/compile/analysis/uses.h"
#include "runtime/compile/builder/ir_builder.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/compile/passes/value_worklist.h"
#include "runtime/compile/pipeline.h"
#include "runtime/compile/unit.h"
#include "runtime/ir/atomic_rmw.h"
#include "runtime/ir/block.h"
#include "runtime/ir/hir_builder.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::ir;

// Independent complete reference: retain every raw root demand, including
// aliases, nested arguments and implicit roots, before selecting declarations.
static void CheckRegisterReadUses(compile::SemanticFacts& facts,
                                  const Block* block,
                                  const HIRFunction* function = nullptr) {
    struct Use {
        u32 point{}, width{};
    };
    std::unordered_map<u32, Use> expected;
    std::vector<u32> declarations;
    const auto visit = [&](const Block& current) {
        const auto add = [&](Value value, u32 point) {
            if (!value.Defined()) return;
            const auto width = value.Type() == ValueType::VOID ? 0 : GetValueSizeByte(value.Type());
            while (value.Def()->IsBitCastOperation()) value = value.Def()->GetArg<Value>(0);
            auto& use = expected[value.Id()];
            use.point = std::max(use.point, point);
            use.width = std::max<u32>(use.width, width);
        };
        for (const auto& inst : current.GetInstList()) {
            if (inst.GetOp() == OpCode::GetHostGPR) declarations.push_back(inst.Id());
            inst.ForEachValue([&](Value value) { add(value, inst.Id()); });
        }
        compile::VisitImplicitUses(
                current,
                current.GetInstList().empty() ? 0 : current.GetInstList().back().Id(),
                add);
    };
    if (function)
        for (const auto& member : function->GetHIRBlocksRPO()) visit(*member.GetBlock());
    else
        visit(*block);
    const auto& actual = facts.PrepareRegisterReads();
    REQUIRE(facts.FindRegisterReads() == &actual);
    REQUIRE(&facts.PrepareRegisterReads() == &actual);
    for (const auto id : declarations) {
        const auto wanted = expected.find(id);
        const auto use = actual.Find(id);
        REQUIRE(use.has_value() == (wanted != expected.end()));
        if (use && wanted != expected.end()) {
            REQUIRE(use->last_use == wanted->second.point);
            REQUIRE(use->max_width == wanted->second.width);
        }
    }
    REQUIRE_FALSE(actual.Find(UINT32_MAX));
}

TEST_CASE("common allocation preserves caller pools and outlives its writer",
          "[allocation][ownership]") {
    ::swift::runtime::compile::CompileWorkspace rejected_workspace;
    IntrusivePtr<Block> block{new Block(0, Location{0xf7f0})};
    const auto input = block->LoadImm(ValueType::U64, Imm{u64{41}});
    const auto result = block->Add(ValueType::U64, input, Operand{Imm{u64{7}}});
    block->StoreUniform(Uniform{0, ValueType::U64}, result);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    const backend::GPRSMask gprs{~0x3ffffu};
    const backend::FPRSMask fprs{~0xffffu};
    const FeatureSet features{};
    compile::Allocation physical{block->MaxInstrId(), gprs, fprs, features};
    compile::Allocation independent{block->MaxInstrId(), gprs, fprs, features};
    compile::SemanticFacts independent_facts{block.get()};
    const auto view = physical.View();
    compile::SemanticFacts facts{block.get()};
    compile::CompileWorkspace preparation_workspace;
    {
        compile::AllocationWriter writer{physical, facts, preparation_workspace};
        REQUIRE(&writer.Workspace() == &preparation_workspace);
        REQUIRE_THROWS_AS((compile::AllocationWriter{physical, facts, preparation_workspace}),
                          std::logic_error);
        REQUIRE_THROWS_AS(
                (compile::AllocationWriter{independent, independent_facts, preparation_workspace}),
                std::logic_error);
        writer.SetAllocationUnit(block.get(), nullptr);
        compile::AllocateBlock(block.get(),
                               &writer,
                               backend::arm64::DescribeAllocationInputs(
                                       block.get(), writer, features, facts, writer.Workspace()));
        REQUIRE(writer.GetGprs() == gprs);
        REQUIRE(writer.GetFprs() == fprs);
        REQUIRE(facts.FindIndex(block.get()) != nullptr);
        REQUIRE(facts.FindUses(block.get()) == nullptr);
        writer.FreezeGPRLocations();
    }
    {
        compile::AllocationWriter recovered{independent, independent_facts, preparation_workspace};
        REQUIRE(&recovered.Workspace() == &preparation_workspace);
    }
    REQUIRE(view.GPRLocationsFrozen());
    REQUIRE(view.ValueType(result) == compile::Allocation::GPR);
    REQUIRE(view.ValueGPR(result).id < 18);
    REQUIRE_THROWS_AS((compile::AllocationWriter{physical, facts, rejected_workspace}),
                      std::logic_error);
    // Keep the original caller-pool contract above. The complete target path
    // uses its own real module configuration, pools and analysis owner.
    Config config{.loc_start = 0,
                  .loc_end = 1ull << 48,
                  .enable_jit = true,
                  .has_local_operation = false,
                  .backend_isa = kArm64};
    config.uniform_buffer_size = 128;
    config.stack_alignment = 16;
    backend::AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    const auto module_features = ResolveFeatureSet(module->GetModuleConfig());
    compile::CompileWorkspace target_workspace;
    compile::SemanticFacts target_facts{block.get()};
    compile::Allocation target{block->MaxInstrId(),
                               space.GetTrampolines().GetGPRRegs(),
                               space.GetTrampolines().GetFPRRegs(),
                               module_features};
    backend::arm64::UnitActions actions{target.View()};
    backend::arm64::AllocationPreparation writer{target, target_facts, target_workspace, actions};
    compile::AllocateBlock(
            block.get(),
            &writer,
            backend::arm64::PrepareAllocationInputs(block.get(), writer, module_features));
    REQUIRE(target_facts.FindUses(block.get()) == nullptr);
    backend::arm64::CompleteARM64Allocation(module, writer);
    REQUIRE(actions.ForBlock(block.get()) != nullptr);
    const auto& settings = GetSvmConfig();
    if (!settings.verify_plan && !(settings.density_prof && settings.ra_hot_coalesce_all))
        REQUIRE(target_facts.FindUses(block.get()) == nullptr);
    REQUIRE(target.View().GPRLocationsFrozen());
}

static_assert(!std::is_default_constructible_v<compile::CompileUnit>);
static_assert(!std::is_default_constructible_v<compile::AllocationInputs>);
static_assert(!std::is_copy_constructible_v<compile::AllocationInputs>);
static_assert(std::is_nothrow_move_constructible_v<compile::AllocationInputs>);
static_assert(!std::is_default_constructible_v<compile::OptimizedUnit>);
static_assert(!std::is_copy_constructible_v<compile::OptimizedUnit>);
static_assert(std::is_nothrow_move_constructible_v<compile::OptimizedUnit>);
static_assert(!std::is_default_constructible_v<backend::arm64::Arm64PreparedUnit>);
static_assert(!std::is_copy_constructible_v<backend::arm64::Arm64PreparedUnit>);
static_assert(std::is_nothrow_move_constructible_v<backend::arm64::Arm64PreparedUnit>);
static_assert(
        std::is_same_v<decltype(std::declval<backend::arm64::Arm64PreparedUnit&>().Allocation()),
                       compile::AllocationView>);
static_assert(std::is_same_v<decltype(std::declval<const Block&>().GetInstList()), const InstList&>);
static_assert(std::is_same_v<decltype(std::declval<Block&>().GetInstList()), const InstList&>);
static_assert(std::is_same_v<decltype(std::declval<HIRBlock&>().GetInstList()), const InstList&>);
static_assert(std::is_same_v<decltype(*std::declval<const InstList&>().begin()), const Inst&>);
static_assert(std::is_same_v<decltype(std::declval<const Block&>().GetInstList().front()), const Inst&>);
static_assert(std::is_same_v<Inst::Pseudos::value_type, const Inst*>);
static_assert(std::is_same_v<decltype(std::declval<Value>().Def()), const Inst*>);
template <typename T>
concept WritableDefinition = requires(T value) { value.Def()->SetReturn(ValueType::U64); };
template <typename T>
concept PublicUseBookkeeping = requires(T value) { value.Use(); value.UnUse(); };
static_assert(!WritableDefinition<Value>);
static_assert(!PublicUseBookkeeping<Value>);
static_assert(std::is_same_v<decltype(LoopHoistMetadata::anchors)::element_type, const Inst* const>);
static_assert(
        std::is_same_v<decltype(std::declval<const Inst&>().GetArg<Params>(0)), Params::View>);
static_assert(std::is_same_v<decltype(std::declval<const Arg&>().Get<Params>()), Params::View>);
static_assert(std::is_same_v<decltype(*std::declval<const Params&>().begin()), Params::Param>);
static_assert(std::is_same_v<decltype(*std::declval<Params&>().begin()), Params::Param>);
static_assert(!std::is_copy_constructible_v<Params> && !std::is_copy_assignable_v<Params>);
static_assert(std::is_nothrow_move_constructible_v<Params> && std::is_nothrow_move_assignable_v<Params>);
static_assert(!std::is_constructible_v<Arg, Params> && !std::is_constructible_v<ArgClass, Params>);
template<class Iterator> concept ExposesParamNode = requires(Iterator iterator) { iterator.operator->(); };
static_assert(!ExposesParamNode<Params::Iterator>);
static_assert(!std::is_convertible_v<Params::View, Params>);
static_assert(std::is_same_v<decltype(std::declval<const Lambda&>().GetValue()), const Value&>);
static_assert(std::is_same_v<decltype(std::declval<const Lambda&>().GetImm()), const Imm&>);
static_assert(!std::is_copy_constructible_v<Arg>);
static_assert(std::is_same_v<decltype(std::declval<const HIRFunction&>().GetEntryBlock()),
                             const HIRBlock*>);
static_assert(
        std::is_same_v<decltype(std::declval<HIRFunction&>().GetEntryBlock()), const HIRBlock*>);
static_assert(
        std::is_same_v<decltype(std::declval<HIRFunction&>().GetCurrentBlock()), const HIRBlock*>);
static_assert(std::is_same_v<decltype(std::declval<HIRFunction&>().GetHIRBlockList()),
                             const HIRBlockList&>);
static_assert(std::is_same_v<decltype(std::declval<HIRFunction&>().GetHIRBlocksRPO()),
                             const HIRBlockList&>);
static_assert(
        std::is_same_v<decltype(std::declval<const HIRFunction&>().GetHIRBlocks()), HIRBlockView>);
static_assert(std::is_same_v<decltype(std::declval<const HIRFunction&>().GetExternalEntryRoots()),
                             HIRBlockView>);
static_assert(
        std::is_same_v<decltype(*std::declval<const HIRFunction&>().GetHIRBlocksRPO().begin()),
                       const HIRBlock&>);
static_assert(
        std::is_same_v<decltype(std::declval<const HIRFunction&>().GetHIRValues())::element_type,
                       const HIRValue* const>);
static_assert(std::is_same_v<decltype(std::declval<const HIRFunction&>().GetHIRValue(Value{})),
                             const HIRValue*>);
static_assert(std::is_same_v<decltype(std::declval<HIRFunction&>().GetHIRValue(Value{})),
                             const HIRValue*>);
static_assert(std::is_same_v<decltype(std::declval<HIRFunction&>().GetHIRValues())::element_type,
                             const HIRValue* const>);
static_assert(std::is_const_v<decltype(HIRValue::value)>);
static_assert(std::is_same_v<decltype(std::declval<const HIRFunction&>().GetFunction()),
                             const Function*>);
static_assert(std::is_same_v<decltype(std::declval<const HIRBlock&>().GetBlock()), const Block*>);
static_assert(std::is_same_v<decltype(std::declval<Function&>().GetBlocks()), const BlockMap&>);
static_assert(std::is_same_v<decltype(*std::declval<const BlockMap&>().find(AddressNode{})),
                             const AddressNode&>);
static_assert(std::is_same_v<decltype(*std::declval<const BlockMap&>().lower_bound(AddressNode{})),
                             const AddressNode&>);
static_assert(std::is_same_v<decltype(*std::declval<const BlockMap&>().upper_bound(AddressNode{})),
                             const AddressNode&>);
template <typename T>
concept WritableAddressStart = requires(T node) { node.location = Location{0}; };
template <typename T>
concept WritableAddressSize = requires(T node) { node.node_size = 0; };
template <typename T>
concept WritableAddressKind = requires(T node) { node.node_type = AddressNode::Block; };
static_assert(!WritableAddressStart<AddressNode> && !WritableAddressSize<AddressNode> &&
              !WritableAddressKind<AddressNode>);
static_assert(!std::is_copy_constructible_v<AddressNode>);
static_assert(std::is_same_v<decltype(std::declval<const HIRBlock&>().GetCallReturnBlock()),
                             const HIRBlock*>);
static_assert(
        std::is_same_v<decltype(std::declval<const HIRBlock&>().GetPredecessors()), HIRBlockView>);
static_assert(
        std::is_same_v<decltype(std::declval<const HIRBlock&>().GetSuccessors()), HIRBlockView>);
static_assert(std::is_same_v<decltype(std::declval<HIRBlock&>().GetPredecessors()), HIRBlockView>);
static_assert(std::is_same_v<decltype(std::declval<HIRBlock&>().GetSuccessors()), HIRBlockView>);
static_assert(std::is_same_v<decltype(*std::declval<HIRBlock&>().GetOutgoingEdges().begin()),
                             const Edge&>);
static_assert(std::is_same_v<decltype(*std::declval<const HIRBlock&>().GetOutgoingEdges().begin()),
                             const Edge&>);
static_assert(
        std::is_same_v<decltype(std::declval<const Edge&>().GetSourceBlock()), const HIRBlock*>);
static_assert(
        std::is_same_v<decltype(std::declval<const Edge&>().GetTargetBlock()), const HIRBlock*>);
static_assert(
        std::is_same_v<decltype(std::declval<const HIRValue&>().GetBlock()), const HIRBlock*>);
static_assert(std::is_same_v<decltype(*std::declval<const HIRValue&>().GetUses().begin()),
                             const HIRUse&>);

TEST_CASE("prepared IR rejects retained writers and foreign rewrite identities",
          "[ir][phase][ownership]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    for (bool function_mode : {false, true}) {
        CAPTURE(function_mode);
        IntrusivePtr<Block> standalone{new Block{0, Location{0x5900}}};
        Block foreign{0, Location{0x5980}};
        HIRBuilder builder{1, true, FeatureSet{}};
        auto* function = function_mode ? builder.AppendFunction(Location{0x5900}) : nullptr;
        auto* hir = function ? &function->EditBlock(function->GetCurrentBlock()) : nullptr;
        auto* block = hir ? hir->GetBlock() : standalone.get();
        if (function) {
            REQUIRE_THROWS_AS(compile::CompileUnit{function}, std::logic_error);
            REQUIRE_FALSE(function->GetFunction()->IsIRReadOnly());
            REQUIRE_FALSE(block->IsIRReadOnly());
        }
        auto* source = function ? function->AppendInst(OpCode::LoadImm, ValueType::U64, Imm{u64{7}})
                                : block->AppendInst(OpCode::LoadImm, ValueType::U64, Imm{u64{7}});
        auto* sum =
                function
                        ? function->AppendInst(
                                  OpCode::Add, ValueType::U64, Value{source}, Operand{Imm{u64{1}}})
                        : block->AppendInst(
                                  OpCode::Add, ValueType::U64, Value{source}, Operand{Imm{u64{1}}});
        if (function) {
            function->StoreUniform(Uniform{0, ValueType::U64}, Value{sum});
            function->EndBlock(terminal::ReturnToHost{});
            function->EndFunction();
            function->ComputeRPO();
            function->IdByRPO();
        } else {
            block->StoreUniform(Uniform{0, ValueType::U64}, Value{sum});
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
        }
        auto rewrite =
                function ? compile::RewriteContext{function, compilation_workspace, false}
                         : compile::RewriteContext{block, compilation_workspace, nullptr, false};
        auto* outsider = foreign.AppendInst(OpCode::LoadImm, ValueType::U64, Imm{u64{9}});
        outsider->SetId(source->Id());
        if (function) REQUIRE(function->GetHIRValue(Value{outsider}) == nullptr);
        bool edited{};
        REQUIRE_THROWS_AS(rewrite.Change(block, outsider, [&](Inst&) { edited = true; }),
                          std::logic_error);
        REQUIRE_THROWS_AS(rewrite.Change(&foreign, outsider, [&](Inst&) { edited = true; }),
                          std::logic_error);
        REQUIRE_FALSE(edited);
        const auto uses = source->GetUses(false);
        const auto hir_uses = function ? function->GetHIRValue(Value{source})->GetUses().size() : 0;
        if (function) {
            HIRBuilder other_builder{1, true, FeatureSet{}};
            auto* other = other_builder.AppendFunction(Location{0x5980});
            const auto* foreign_hir = other->GetCurrentBlock();
            const auto outgoing = hir->GetOutgoingEdges().size();
            const auto incoming = foreign_hir->GetIncomingEdges().size();
            REQUIRE_THROWS_AS(function->EditBlock(foreign_hir), std::logic_error);
            REQUIRE_THROWS_AS(function->AddEdge(hir, foreign_hir), std::logic_error);
            REQUIRE(hir->GetOutgoingEdges().size() == outgoing);
            REQUIRE(foreign_hir->GetIncomingEdges().size() == incoming);
            {
                compile::CompileUnit standalone_scope{hir};
                standalone_scope.SealIR();
                REQUIRE(function->GetFunction()->IsIRReadOnly());
                REQUIRE_THROWS_AS(function->GetFunction()->SetEndLocation(Location{0x5940}),
                                  std::logic_error);
                const auto* entry = function->GetEntryBlock();
                const auto entry_outgoing = entry->GetOutgoingEdges().size();
                REQUIRE_THROWS_AS(function->EditBlock(hir), std::logic_error);
                REQUIRE_THROWS_AS(function->AddEdge(entry, hir), std::logic_error);
                REQUIRE_THROWS_AS(function->GetFunction()->DetachBlock(block), std::logic_error);
                REQUIRE(function->GetFunction()->FindBlock(block->GetStartLocation()) == block);
                REQUIRE(entry->GetOutgoingEdges().size() == entry_outgoing);
                REQUIRE(hir->GetIncomingEdges().size() == 1);
            }
            REQUIRE_FALSE(block->IsIRReadOnly());
            REQUIRE_FALSE(function->GetFunction()->IsIRReadOnly());
            {
                Config config{.loc_start = 0,
                              .loc_end = 0x10000,
                              .enable_jit = true,
                              .has_local_operation = false,
                              .backend_isa = kArm64};
                backend::AddressSpace space{config};
                REQUIRE(space.GetDefaultModule()->Push(function->GetFunction()));
                function->MarkFunctionPublished();
            }
            // HIR remains an actual owner after the publishing module is gone.
            REQUIRE(function->GetFunction()->FindBlock(block->GetStartLocation()) == block);
            REQUIRE_FALSE(block->GetInstList().empty());
        }
        std::optional<compile::CompileUnit> retained;
        {
            auto unit =
                    function ? compile::CompileUnit{function} : compile::CompileUnit{standalone};
            unit.SealIR();
            retained.emplace(std::move(unit));
            REQUIRE(retained->IsSealed());
            if (function)
                REQUIRE_THROWS_AS(retained->EditFunction(), std::logic_error);
            else
                REQUIRE_THROWS_AS(retained->EditBlock(), std::logic_error);
            REQUIRE(block->IsIRReadOnly());
            const auto end = block->GetEndLocation();
            REQUIRE_THROWS_AS(static_cast<AddressNode*>(block)->SetEndLocation(Location{0x5920}),
                              std::logic_error);
            REQUIRE(block->GetEndLocation() == end);
            REQUIRE_THROWS_AS(source->SetReturn(ValueType::U32), std::logic_error);
            REQUIRE_THROWS_AS(source->SetId(100), std::logic_error);
            REQUIRE_THROWS_AS(sum->SetArgFresh(0, Value{source}), std::logic_error);
            REQUIRE_THROWS_AS(sum->Reset(), std::logic_error);
            REQUIRE_THROWS_AS(rewrite.SetArg(block, sum, 1, Operand{Imm{u64{2}}}),
                              std::logic_error);
            REQUIRE_THROWS_AS(block->RemoveInst(sum), std::logic_error);
            REQUIRE_THROWS_AS(block->DestroyInstrs(), std::logic_error);
            REQUIRE_THROWS_AS(block->SetTerminal(terminal::ReturnToDispatch{}), std::logic_error);
            REQUIRE_THROWS_AS(block->AppendInst(OpCode::LoadImm, ValueType::U64, Imm{u64{1}}),
                              std::logic_error);
            auto unattached = std::make_unique<Inst>(OpCode::BitCast, ValueType::U64);
            REQUIRE_THROWS_AS(unattached->SetArgFresh(0, Value{source}), std::logic_error);
            REQUIRE(unattached->ArgAt(0).IsVoid());
            if (function) {
                auto* persistent = function->GetFunction();
                REQUIRE(persistent->IsIRReadOnly());
                REQUIRE_THROWS_AS(
                        static_cast<AddressNode*>(persistent)->SetEndLocation(Location{0x5920}),
                        std::logic_error);
                REQUIRE_THROWS_AS(persistent->DetachBlock(block), std::logic_error);
                REQUIRE_THROWS_AS(persistent->DestroyInstrs(), std::logic_error);
                Function donor{Location{0x59c0}};
                donor.AddBlock(new Block{Location{0x59c0}});
                REQUIRE_THROWS_AS(persistent->TakeBlocksFrom(donor), std::logic_error);
                REQUIRE(donor.GetBlocks().size() == 1);
                REQUIRE(persistent->FindBlock(block->GetStartLocation()) == block);
                const auto incoming = hir->GetIncomingEdges().size();
                const auto outgoing = hir->GetOutgoingEdges().size();
                REQUIRE_THROWS_AS(function->EditBlock(hir), std::logic_error);
                REQUIRE_THROWS_AS(function->SetCurBlock(hir), std::logic_error);
                REQUIRE_THROWS_AS(function->AddEdge(hir, hir), std::logic_error);
                REQUIRE_THROWS_AS(hir->BeginStandalonePublication(), std::logic_error);
                REQUIRE_THROWS_AS(hir->LoadImm<U64>(Imm{u64{3}}), std::logic_error);
                REQUIRE_THROWS_AS(function->IdByRPO(), std::logic_error);
                REQUIRE_THROWS_AS(function->AppendBlock(Location{0x5940}), std::logic_error);
                REQUIRE(hir->GetIncomingEdges().size() == incoming);
                REQUIRE(hir->GetOutgoingEdges().size() == outgoing);
                REQUIRE(function->GetHIRValue(Value{source})->GetUses().size() == hir_uses);
            }
            REQUIRE(source->GetUses(false) == uses);
            REQUIRE(source->ReturnType() == ValueType::U64);
            REQUIRE(sum->GetBlock() == block);
        }
        REQUIRE(block->IsIRReadOnly());
        retained.reset();
        REQUIRE_FALSE(block->IsIRReadOnly());
        if (function) REQUIRE_FALSE(function->GetFunction()->IsIRReadOnly());
        block->SetEndLocation(Location{0x5920});
        REQUIRE(block->GetEndLocation() == Location{0x5920});
        rewrite.SetArg(block, sum, 1, Operand{Imm{u64{2}}});
        REQUIRE(sum->GetArg<Operand>(1).GetLeft().imm.Get() == 2);
        REQUIRE(source->GetUses(false) == uses);
    }
}

TEST_CASE("merged regions retain their actual Function seal until the last consumer",
          "[ir][phase][ownership]") {
    for (const bool release_primary_first : {false, true}) {
        CAPTURE(release_primary_first);
        HIRBuilder builder{2, true, FeatureSet{}};
        const auto finish = [&](u64 address) {
            auto* function = builder.AppendFunction(Location{address});
            const auto value = function->LoadImm(ValueType::U64, Imm{address});
            function->StoreUniform(Uniform{0, ValueType::U64}, value);
            function->EndBlock(terminal::ReturnToHost{});
            function->EndFunction();
            function->ComputeRPO();
            function->IdByRPO();
            return function;
        };
        auto* first = finish(0x5a00);
        auto* second = finish(0x5a40);
        auto* owner = first->GetFunction();
        std::optional<compile::CompileUnit> primary, secondary;
        primary.emplace(first);
        secondary.emplace(second);
        REQUIRE_THROWS_AS(owner->TakeBlocksFrom(*second->GetFunction()), std::logic_error);
        const std::array<compile::CompileUnit*, 2> units{&*primary, &*secondary};
        REQUIRE(compile::CompileUnit::MergeFunctions(units));
        REQUIRE(second->GetFunction()->GetBlocks().empty());
        primary->SealIR();
        secondary->SealIR();
        REQUIRE(owner->IsIRReadOnly());
        REQUIRE(second->GetFunction()->IsIRReadOnly());
        auto* first_block = first->GetHIRBlocks()[1]->GetBlock();
        auto* second_block = second->GetHIRBlocks()[1]->GetBlock();
        auto& released = release_primary_first ? primary : secondary;
        auto& remaining = release_primary_first ? secondary : primary;
        released.reset();
        REQUIRE(owner->IsIRReadOnly());
        REQUIRE((release_primary_first ? second_block : first_block)->IsIRReadOnly());
        const auto end = owner->GetEndLocation();
        auto added = std::make_unique<Block>(Location{0x5a80});
        REQUIRE_THROWS_AS(owner->SetEndLocation(Location{0x5ac0}), std::logic_error);
        REQUIRE_THROWS_AS(owner->AddBlock(added.get()), std::logic_error);
        REQUIRE_THROWS_AS(owner->DestroyInstrs(), std::logic_error);
        REQUIRE(owner->GetEndLocation() == end);
        REQUIRE(owner->GetBlocks().size() == 2);
        remaining.reset();
        REQUIRE_FALSE(owner->IsIRReadOnly());
        REQUIRE_FALSE(second->GetFunction()->IsIRReadOnly());
        owner->SetEndLocation(Location{0x5ac0});
        owner->AddBlock(added.get());
        added.release();
        REQUIRE(owner->GetBlocks().size() == 3);
    }
}

TEST_CASE("preparing ownership merges reject collisions without moving earlier regions",
          "[ir][phase][ownership]") {
    HIRBuilder builder{3, true, FeatureSet{}};
    const auto finish = [&](u64 address) {
        auto* function = builder.AppendFunction(Location{address});
        function->StoreUniform(Uniform{0, ValueType::U64}, function->LoadImm<U64>(Imm{address}));
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        return function;
    };
    auto* first = finish(0x5b00);
    auto* second = finish(0x5b40);
    auto* conflicting = finish(0x5b40);
    compile::CompileUnit a{first}, b{second}, c{conflicting};
    REQUIRE_FALSE(a.IsSealed());
    REQUIRE(a.EditFunction() == first);
    REQUIRE_THROWS_AS(compile::CompileUnit{first}, std::logic_error);
    REQUIRE_THROWS_AS(first->GetFunction()->DetachBlock(first->GetFunction()->EntryBlock()),
                      std::logic_error);
    const auto* anchor = &first->GetHIRBlocks()[1]->GetBlock()->GetInstList().front();
    const std::array<const Inst*, 1> anchors{anchor};
    auto* foreign = conflicting->EditBlock(conflicting->GetHIRBlocks()[1]).GetBlock();
    REQUIRE_THROWS_AS(a.SetLoopHoistMetadata(foreign, {anchor, anchors, 0, 1}), std::logic_error);
    REQUIRE_THROWS_AS(a.ClearLoopHoistMetadata(foreign), std::logic_error);
    const std::array<compile::CompileUnit*, 3> colliding{&a, &b, &c};
    REQUIRE_FALSE(compile::CompileUnit::MergeFunctions(colliding));
    REQUIRE(first->GetFunction()->GetBlocks().size() == 1);
    REQUIRE(second->GetFunction()->GetBlocks().size() == 1);
    REQUIRE(conflicting->GetFunction()->GetBlocks().size() == 1);
    const std::array<compile::CompileUnit*, 2> duplicate{&a, &a};
    REQUIRE_FALSE(compile::CompileUnit::MergeFunctions(duplicate));
    const std::array<compile::CompileUnit*, 2> valid{&a, &b};
    REQUIRE(compile::CompileUnit::MergeFunctions(valid));
    REQUIRE(first->GetFunction()->GetBlocks().size() == 2);
    REQUIRE(second->GetFunction()->GetBlocks().empty());
    REQUIRE(conflicting->GetFunction()->GetBlocks().size() == 1);
    a.SealIR();
    b.SealIR();
    REQUIRE_THROWS_AS(a.EditFunction(), std::logic_error);
    REQUIRE_THROWS_AS(b.EditFunction(), std::logic_error);
}

TEST_CASE("shared rewrites maintain HIR uses and typed Lambda and Params edges",
          "[ir][rewrite][uses]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    FeatureSet features{};
    features.const_cse = true;
    HIRBuilder builder{1, false, features};
    auto* function = builder.AppendFunction(Location{0x5a00}, Location{0x5b00});
    compile::IrBuilder build{&builder};
    const auto byte = build.LoadImm(ValueType::U8, Imm{0x80u});
    const auto extended = build.SignExtend(ValueType::S64, byte);
    REQUIRE(extended.Def()->GetOp() == OpCode::LoadImm);
    REQUIRE(function->GetHIRValue(byte)->GetUses().empty());
    const auto source = function->LoadImm<U64>(Imm{u64{42}});
    function->StoreUniform(Uniform{0, ValueType::U64}, source);
    const auto duplicate = function->LoadImm<U64>(Imm{u64{42}});
    Params params;
    params.Push(Value{duplicate}.SetCastType(ValueType::U16));
    params.Push(Imm{7u});
    params.Push(Value{duplicate}.SetCastType(ValueType::U16));
    function->CallDynamic(ValueType::VOID, Lambda{Value{duplicate}.SetCastType(ValueType::U32)}, std::move(params));
    auto* call = &function->GetCurrentBlock()->GetInstList().back();
    function->EndBlock(terminal::ReturnToDispatch{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    const auto duplicate_id = duplicate.Id();
    REQUIRE(function->GetHIRValue(duplicate)->GetUses().size() == 3);
    compile::RewriteContext rewrite{function, compilation_workspace};
    // Constant-only cleanup needs no mutable identity/use storage. Keeping
    // that borrow occupied also proves CSE still maintains real HIR uses.
    compile::ValueQueueScope unused_value_storage{compilation_workspace};
    compile::FoldValues(rewrite, features);
    rewrite.CollectDead();
    REQUIRE(function->GetHIRValues()[duplicate_id] == nullptr);
    REQUIRE(function->GetHIRValue(source)->GetUses().size() == 4);
    REQUIRE(source.Def()->GetUses(false) == 4);
    const auto lambda = call->GetArg<Lambda>(0).GetValue();
    REQUIRE(lambda.Def() == source.Def());
    REQUIRE(lambda.Type() == ValueType::U32);
    u32 values = 0;
    for (const auto& param : call->GetArg<Params>(1)) {
        if (!param.data.IsValue()) {
            REQUIRE(param.data.imm.Get() == 7);
            continue;
        }
        REQUIRE(param.data.value.Def() == source.Def());
        REQUIRE(param.data.value.Type() == ValueType::U16);
        ++values;
    }
    REQUIRE(values == 2);
    REQUIRE(source.Type() == ValueType::U64);
}

TEST_CASE("value worklist propagates edits and survives DCE in Block and Function",
          "[ir][rewrite][worklist]") {
    FeatureSet features{};
    features.int_imm_fold = true;
    features.const_cse = false;
    const compile::BuilderCapabilities target{};
    for (const bool function_mode : {false, true}) {
        CAPTURE(function_mode);
        compile::CompileWorkspace workspace;
        HIRBuilder builder{2, false, features};
        auto* function = builder.AppendFunction(Location{0x5b00}, Location{0x5b80});
        auto* first = function->EditBlock(function->GetCurrentBlock()).GetBlock();
        const auto address = function->LoadImm<U64>(Imm{u64{0x400000}});
        const auto faulting = function->LoadMemory<U64>(Operand{address});
        const auto masked = function->And<U64>(faulting, Operand{Imm{UINT64_MAX}});
        if (function_mode) {
            auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x5b40}});
            builder.SetCurBlock(next);
        }
        const auto view = function->BitCast<U64>(masked);
        const auto result = function->BitExtract<U32>(view, Imm{u32{32}}, Imm{u32{32}});
        function->StoreUniform(Uniform{0, ValueType::U32}, result);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        auto rewrite = function_mode ? compile::RewriteContext{function, workspace}
                                     : compile::RewriteContext{first, workspace, function};
        compile::ValueWorklist work{rewrite, features, target, compile::ValueCleanup::All};
        REQUIRE(work.Pending());
        REQUIRE(result.Def()->GetArg<Value>(0).Def() == view.Def());
        work.Run();
        REQUIRE(result.Def()->GetOp() == OpCode::BitExtract);
        rewrite.SetArg(first, masked.Def(), 1, Operand{Imm{u64{0xffff}}});
        REQUIRE(work.Pending());
        work.Drain();
        REQUIRE(result.Def()->GetOp() == OpCode::LoadImm);
        REQUIRE(result.Def()->GetArg<Imm>(0).Get() == 0);
        rewrite.CollectDead();
        work.Drain();
        REQUIRE_FALSE(work.Pending());
        REQUIRE(faulting.Def()->GetOp() == OpCode::LoadMemory);
        REQUIRE(faulting.Def()->HasSideEffects());
        REQUIRE(work.SingleConsumer(masked.Def()) == nullptr);
        REQUIRE(work.SingleConsumer(view.Def()) == nullptr);
        REQUIRE(function->GetHIRValue(result)->GetUses().size() == 1);
    }
}

TEST_CASE("value worklist observes removed flag outputs before narrow immediate folding",
          "[ir][rewrite][worklist][flags]") {
    compile::CompileWorkspace workspace;
    FeatureSet features{};
    features.int_imm_fold = true;
    const auto target = backend::arm64::BuilderTarget();
    Block block{0, Location{0x5bc0}};
    const auto input = block.GetHostGPR<U8>(Imm{u32{20}}, Imm{u32{0}});
    const auto constant = block.LoadImm<U8>(Imm{u32{1}});
    const auto sum = block.Add<U8>(input, Operand{constant});
    block.SaveFlags(sum, Flags::NZCV);
    const auto* flags = &block.GetInstList().back();
    block.StoreUniform(Uniform{0, ValueType::U8}, sum);
    block.SetTerminal(terminal::ReturnToHost{});
    compile::RewriteContext rewrite{&block, workspace};
    compile::ValueWorklist work{rewrite, features, target, compile::ValueCleanup::All};
    REQUIRE(work.Pending());
    REQUIRE(sum.Def()->GetArg<Operand>(1).GetLeft().IsValue());
    work.Run();
    REQUIRE(sum.Def()->GetArg<Operand>(1).GetLeft().IsValue());
    REQUIRE(work.RequestedFlags(sum.Def()) == Flags::NZCV);
    rewrite.Erase(&block, flags);
    work.Drain();
    REQUIRE(work.RequestedFlags(sum.Def()) == Flags::None);
    REQUIRE(sum.Def()->GetArg<Operand>(1).IsImm());
    REQUIRE(sum.Def()->GetArg<Operand>(1).GetLeft().imm.Get() == 1);
    rewrite.CollectDead();
    work.Drain();
    REQUIRE(sum.Def()->ReturnType() == ValueType::U8);
    REQUIRE_FALSE(work.Pending());
}

TEST_CASE("shared DCE retains hoist roots and faults until their owner releases them",
          "[ir][rewrite][lifetime]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    IntrusivePtr<Block> owner{new Block{0, Location{0x5c00}}};
    auto& block = *owner;
    const auto anchor = block.LoadImm<U64>(Imm{u64{42}});
    block.Add<U64>(anchor, Operand{Imm{1u}});
    const auto address = block.LoadImm<U64>(Imm{u64{0x400000}});
    const auto load = block.LoadMemory<U64>(Operand{address});
    block.SetTerminal(terminal::ReturnToHost{});
    compile::CompileUnit unit{owner};
    std::array<const Inst*, 1> anchors{anchor.Def()};
    unit.SetLoopHoistMetadata(&block, {anchor.Def(), anchors, 0, 1});
    compile::RewriteContext rewrite{&block, compilation_workspace};
    rewrite.CollectDead();
    REQUIRE(block.GetInstList().size() == 3);
    REQUIRE(anchor.Def()->GetUses(false) == 0);
    REQUIRE(load.Def()->HasSideEffects());
    unit.ClearLoopHoistMetadata(&block);
    rewrite.CollectDead();
    REQUIRE(block.GetInstList().size() == 2);
    REQUIRE(block.GetInstList().back().GetOp() == OpCode::LoadMemory);
    const auto zero = block.And(ValueType::U64, load, Operand{Imm{u64{0}}});
    const auto result = block.BitExtract(ValueType::U32, zero, Imm{u32{32}}, Imm{u32{32}});
    block.StoreUniform(Uniform{0, ValueType::U32}, result);
    auto options = FeatureSet{};
    options.int_imm_fold = true;
    compile::FoldValues(RewriteFixture{&block, false}.Get(), options);
    REQUIRE(result.Def()->GetOp() == OpCode::LoadImm);
    REQUIRE(result.Def()->GetArg<Imm>(0).Get() == 0);
    rewrite.CollectDead();
    REQUIRE(std::count_if(
                    block.GetInstList().begin(), block.GetInstList().end(), [](const Inst& inst) {
                        return inst.GetOp() == OpCode::LoadMemory;
                    }) == 1);
}

TEST_CASE("dead flag readers release earlier writers in Block and Function cleanup",
          "[ir][rewrite][flags]") {
    FeatureSet features{};
    features.flag_carry_elim = true;
    features.flags_branch_only = false;
    for (bool function_mode : {false, true}) {
        CAPTURE(function_mode);
        HIRBuilder builder{1, false, features};
        auto* function = builder.AppendFunction(Location{0x5d00}, Location{0x5f00});
        auto* first = function->EditBlock(function->GetCurrentBlock()).GetBlock();
        const auto input = function->LoadImm<U64>(Imm{u64{1}});
        const auto old = function->Add<U64>(input, Operand{Imm{1u}});
        function->SaveFlags(old, Flags::All);
        if (function_mode) {
            auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x5e00}});
            builder.SetCurBlock(next);
        }
        const auto reader = function->TestFlags(Flags::Carry);
        function->Add<U64>(reader, Operand{Imm{1u}});
        const auto last_input = function->LoadImm<U64>(Imm{u64{2}});
        const auto last = function->Add<U64>(last_input, Operand{Imm{1u}});
        function->SaveFlags(last, Flags::All);
        function->EndBlock(terminal::ReturnToDispatch{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        if (function_mode) compile::EliminateFlags(RewriteFixture{function}.Get(), features);
        else compile::EliminateFlags(RewriteFixture{first, true, function}.Get(), features);
        u32 saves = 0;
        for (auto& current : function->GetHIRBlocksRPO()) {
            for (const auto& inst : current.GetInstList()) {
                REQUIRE(inst.GetOp() != OpCode::TestFlags);
                saves += inst.GetOp() == OpCode::SaveFlags;
            }
        }
        REQUIRE(saves == 1);
    }
}

TEST_CASE("function flag summaries preserve paths that skip a local write",
          "[ir][rewrite][flags][cfg]") {
    FeatureSet features{};
    features.flags_branch_only = false;
    HIRBuilder builder{1, false, features};
    auto* function = builder.AppendFunction(Location{0x6000}, Location{0x6200});
    auto* first = function->EditBlock(function->GetCurrentBlock()).GetBlock();
    const auto input = function->LoadImm<U64>(Imm{u64{1}});
    const auto sum = function->Add<U64>(input, Operand{Imm{1u}});
    function->SaveFlags(sum, Flags::All);
    auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x6100}});
    builder.SetCurBlock(next);
    const auto condition = function->LoadImm<BOOL>(Imm{1u});
    const auto skip = function->Goto(condition);
    function->ClearFlags(Flags::All);
    function->BindLabel(skip);
    const auto read = function->TestFlags(Flags::Carry);
    function->StoreUniform(Uniform{0, ValueType::U8}, read);
    function->EndBlock(terminal::ReturnToDispatch{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    compile::EliminateFlags(RewriteFixture{function}.Get(), features);
    REQUIRE(std::count_if(first->GetInstList().begin(), first->GetInstList().end(),
                         [](const Inst& inst) { return inst.GetOp() == OpCode::SaveFlags; }) == 1);
}

namespace {
struct LocalFlagEvent {
    enum Kind { Read, Kill, Jump, Bind } kind;
    Flags mask{Flags::None};
    u32 label{};
};

std::vector<std::vector<LocalFlagEvent>> LocalFlagPrograms() {
    using E = LocalFlagEvent;
    const auto read = [](Flags mask) { return E{E::Read, mask}; };
    const auto kill = [](Flags mask) { return E{E::Kill, mask}; };
    const auto jump = [](u32 id) { return E{E::Jump, Flags::None, id}; };
    const auto bind = [](u32 id) { return E{E::Bind, Flags::None, id}; };
    std::vector<std::vector<E>> cases{
        {}, {read(Flags::Carry)}, {kill(Flags::Carry), read(Flags::Carry)},
        {jump(0), kill(Flags::Carry), bind(0), read(Flags::Carry)},
        {kill(Flags::Carry), jump(0), kill(Flags::All), bind(0), read(Flags::Carry)},
        {jump(0), read(Flags::Parity), bind(0), kill(Flags::All)},
        {jump(0), kill(Flags::Carry), jump(1), read(Flags::Overflow), bind(1),
         bind(0), read(Flags::Carry | Flags::Parity)},
        {jump(0), jump(1), kill(Flags::All), bind(0), read(Flags::Zero), bind(1),
         read(Flags::Carry), kill(Flags::Carry)},
        {jump(0), kill(Flags::All), bind(0), jump(1), read(Flags::Negate), bind(1),
         read(Flags::Overflow)},
        // An unseen binding is an unknown exit, independently of a later kill.
        {jump(0), kill(Flags::All)},
        {kill(Flags::Parity), jump(0), read(Flags::Parity)},
    };
    std::vector<E> nested;
    for (u32 i = 0; i < 12; ++i) {
        nested.push_back(jump(i));
        nested.push_back(read(i & 1 ? Flags::Carry : Flags::Parity));
        nested.push_back(kill(i & 1 ? Flags::Zero : Flags::Overflow));
    }
    for (u32 i = 12; i--;) {
        nested.push_back(bind(i));
        nested.push_back(read(i & 1 ? Flags::Overflow : Flags::Zero));
    }
    cases.push_back(std::move(nested));
    return cases;
}

Flags LocalFlagPathDemand(const std::vector<LocalFlagEvent>& events) {
    std::array<size_t, 16> targets;
    targets.fill(events.size());
    for (size_t i = 0; i < events.size(); ++i)
        if (events[i].kind == LocalFlagEvent::Bind) targets[events[i].label] = i;
    // Enumerate paths, carrying the bits overwritten on each path. This
    // reference does not use reverse transfer summaries or label snapshots.
    std::vector<std::pair<size_t, Flags>> paths{{0, Flags::None}};
    Flags needed{Flags::None};
    while (!paths.empty()) {
        auto [i, killed] = paths.back();
        paths.pop_back();
        for (; i < events.size(); ++i) {
            const auto& event = events[i];
            if (event.kind == LocalFlagEvent::Read) needed |= event.mask & ~killed;
            if (event.kind == LocalFlagEvent::Kill) killed |= event.mask;
            if (event.kind == LocalFlagEvent::Jump) {
                const auto target = targets[event.label];
                if (target == events.size() || target <= i) needed |= Flags::All & ~killed;
                else paths.emplace_back(target + 1, killed);
            }
        }
    }
    return needed;
}

std::vector<const Inst*> BuildLocalFlagProgram(compile::IrBuilder& factory,
                                              const std::vector<LocalFlagEvent>& events) {
    const auto condition = factory.LoadUniform(Uniform{0, ValueType::U8});
    std::array<Value, 16> targets{};
    std::vector<const Inst*> readers;
    u32 offset = 32;
    for (const auto& event : events) {
        switch (event.kind) {
            case LocalFlagEvent::Read: {
                const auto value = factory.TestFlags(event.mask);
                readers.push_back(value.Def());
                factory.StoreUniform(Uniform{offset, ValueType::U8}, value);
                offset += 8;
                break;
            }
            case LocalFlagEvent::Kill: factory.ClearFlags(event.mask); break;
            case LocalFlagEvent::Jump:
                targets[event.label] = Value{factory.AppendInst(
                        event.label & 1 ? OpCode::NotGoto : OpCode::Goto, ValueType::U8, condition)};
                break;
            case LocalFlagEvent::Bind: factory.BindLabel(targets[event.label]); break;
        }
    }
    return readers;
}
}  // namespace

TEST_CASE("local flag flow matches forward paths and refreshes edited demand",
          "[ir][flags][cfg][labels]") {
    auto cases = LocalFlagPrograms();
    // Positive is a legal named bit outside Flags::All. The demand analysis
    // must preserve it rather than reducing every read to Flags::All.
    cases.push_back({{LocalFlagEvent::Jump}, {LocalFlagEvent::Kill, Flags::Carry},
                     {LocalFlagEvent::Bind}, {LocalFlagEvent::Read, Flags::Positive}});
    for (const bool fast : {false, true})
    for (size_t scenario = 0; scenario < cases.size(); ++scenario) {
        CAPTURE(fast, scenario);
        auto events = cases[scenario];
        HIRBuilder builder{1, false, fast, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{0x7900}, Location{0x7a00});
        auto* root = function->GetCurrentBlock();
        auto* body = function->EditBlock(root).GetBlock();
        compile::IrBuilder factory{&builder};
        const auto readers = BuildLocalFlagProgram(factory, events);
        auto* exit = function->AppendBlock(Location{0x7910});
        function->EndBlock(terminal::LinkBlock{Location{0x7910}});
        function->AddEdge(root, exit);
        builder.SetCurBlock(exit);
        builder.ClearFlags(Flags::All);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO(); function->IdByRPO();
        compile::FlagFlow flow{function};
        flow.ComputeLiveIn();
        REQUIRE(flow.LiveIn(root));
        REQUIRE(*flow.LiveIn(root) == LocalFlagPathDemand(events));
        if (!readers.empty()) {
            auto it = std::find_if(events.begin(), events.end(), [](const auto& e) {
                return e.kind == LocalFlagEvent::Read;
            });
            it->mask ^= Flags::Parity;
            compile::CompileWorkspace workspace;
            compile::RewriteContext rewrite{body, workspace, function, false};
            rewrite.SetArg(body, readers.front(), 0, it->mask);
            flow.ComputeLiveIn();
            REQUIRE(*flow.LiveIn(root) == LocalFlagPathDemand(events));
        }
        HIRBuilder foreign{1, false, FeatureSet{}};
        auto* other = foreign.AppendFunction(Location{0x7b00}, Location{0x7b01});
        auto* same_order = other->GetCurrentBlock();
        const auto selector = other->LoadImm(Imm{u64{0}});
        other->EndBlock(terminal::Switch{selector, {}}); other->EndFunction();
        REQUIRE_FALSE(flow.LiveIn(same_order));
        REQUIRE(flow.LiveOut(same_order) == Flags::All);
        REQUIRE(flow.LiveOut(nullptr) == Flags::All);
    }
}

TEST_CASE("local flag cleanup preserves all path demands in Block and Function",
          "[ir][flags][rewrite][labels]") {
    const auto cases = LocalFlagPrograms();
    for (const bool fast : {false, true})
    for (const bool function_mode : {false, true})
    for (size_t scenario = 0; scenario < cases.size(); ++scenario) {
        // A raw Block follows the process IR_FAST setting; the two IR-suite
        // executions cover it. The HIR constructor has its own explicit mode.
        if (!function_mode && !fast) continue;
        CAPTURE(fast, function_mode, scenario);
        FeatureSet features{};
        features.flags_branch_only = false;
        features.flag_carry_elim = true;
        features.flag_full_elim = true;
        features.flag_narrow = true;
        HIRBuilder builder{1, false, fast, features};
        auto* function = function_mode ? builder.AppendFunction(Location{0x7c00}, Location{0x7d00}) : nullptr;
        IntrusivePtr<Block> block;
        if (!function) block = new Block{0, Location{0x7c00}};
        auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
        compile::IrBuilder factory = function ? compile::IrBuilder{&builder} : compile::IrBuilder{body};
        const auto source = factory.LoadUniform(Uniform{8, ValueType::U64});
        const auto sum = factory.Add(ValueType::U64, source, Operand{Imm{1u}});
        factory.SaveFlags(sum, Flags::All);
        const auto* publication = &body->GetInstList().back();
        BuildLocalFlagProgram(factory, cases[scenario]);
        if (function) {
            auto* root = function->GetCurrentBlock();
            auto* exit = function->AppendBlock(Location{0x7c10});
            function->EndBlock(terminal::LinkBlock{Location{0x7c10}});
            function->AddEdge(root, exit);
            builder.SetCurBlock(exit); builder.ClearFlags(Flags::All);
            function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
            function->ComputeRPO(); function->IdByRPO();
            compile::EliminateFlags(RewriteFixture{function}.Get(), features);
        } else {
            factory.ReturnToHost(); body->ReIdInstr();
            compile::EliminateFlags(RewriteFixture{body, true, nullptr}.Get(), features, Flags::None);
        }
        const auto expected = LocalFlagPathDemand(cases[scenario]) & Flags::All;
        const auto found = std::find_if(body->GetInstList().begin(), body->GetInstList().end(),
                [&](const Inst& inst) { return &inst == publication; });
        REQUIRE((found != body->GetInstList().end()) == (expected != Flags::None));
        if (found != body->GetInstList().end()) REQUIRE(found->GetArg<Flags>(1) == expected);
    }
}

TEST_CASE("carry barriers discard previously seen local flag snapshots",
          "[ir][flags][carry][labels]") {
    FeatureSet features{};
    features.flags_branch_only = false;
    features.flag_carry_elim = true;
    features.flag_full_elim = true;
    IntrusivePtr<Block> block{new Block{0, Location{0x7e00}}};
    compile::IrBuilder factory{block.get()};
    const auto input = factory.LoadUniform(Uniform{0, ValueType::U64});
    const auto value = factory.Add(ValueType::U64, input, Operand{Imm{1u}});
    factory.SaveFlags(value, Flags::All);
    const auto* publication = &block->GetInstList().back();
    const auto branch = Value{factory.AppendInst(OpCode::Goto, ValueType::U8, input)};
    factory.ClearFlags(Flags::All);
    block->AdvancePC(Imm{1u});
    factory.ClearFlags(Flags::All);
    block->AdvancePC(Imm{1u});
    const auto carry = factory.Adc(ValueType::U64, input, Operand{Imm{0u}});
    factory.StoreUniform(Uniform{16, ValueType::U64}, carry);
    block->AdvancePC(Imm{1u});
    factory.BindLabel(branch);
    factory.ClearFlags(Flags::All);
    factory.ReturnToHost(); block->ReIdInstr();
    compile::EliminateFlags(RewriteFixture{block.get(), true, nullptr}.Get(), features, Flags::None);
    const auto found = std::find_if(block->GetInstList().begin(), block->GetInstList().end(),
            [&](const Inst& inst) { return &inst == publication; });
    REQUIRE(found != block->GetInstList().end());
    REQUIRE(found->GetArg<Flags>(1) == Flags::All);
}

TEST_CASE("Function flags CFG demand matches paths to reads and observation boundaries",
          "[ir][rewrite][flags][cfg]") {
    struct Node {
        bool read{}, kill{}, empty{};
        std::vector<int> next;
    };
    struct Scenario {
        std::vector<Node> nodes;
        std::vector<int> roots;
        unsigned terminal{};
        bool unknown_root{};
    };
    const Node kill{false, true, false, {}};
    const Node reader{true, true, false, {}};
    const Node forward{false, false, false, {1}};
    const std::vector<Scenario> scenarios{
            {{kill}, {0}, 0},
            {{reader}, {0}, 1},
            {{forward, kill}, {0}, 0},
            {{forward, reader}, {0}, 0},
            {{{false, false, false, {0}}}, {0}, 0},
            {{{true, false, false, {0}}}, {0}, 0},
            {{forward, {false, false, false, {0}}}, {0}, 0},
            {{forward, {true, false, false, {0}}}, {0}, 0},
            {{{false, false, true, {1}}, kill}, {0}, 0},
            {{{false, false, false, {-1}}}, {0}, 0},
            {{{false, false, false, {}}}, {0}, 0},
            {{kill, reader}, {0, 1}, 2},
            {{kill, kill}, {0, 1}, 3},
            {{kill, reader}, {0, 1}, 3},
            {{kill}, {0}, 4, true},
            {{kill}, {0}, 5, true},
            {{kill}, {0}, 6, true},
            {{kill}, {-1}, 0},
            {{kill}, {}, 2},
            {{kill}, {0, 0}, 2},
    };
    for (bool fast : {false, true}) {
        for (size_t scenario = 0; scenario < scenarios.size(); ++scenario) {
            CAPTURE(fast, scenario);
            const auto& spec = scenarios[scenario];
            // Independent reachability oracle: follow transparent nodes until
            // a read, a complete overwrite, or an unknown observation. A seen
            // closed cycle with no read cannot demand the incoming flags.
            bool needed = spec.unknown_root;
            std::vector<int> pending = spec.roots;
            std::vector<bool> seen(spec.nodes.size());
            while (!pending.empty() && !needed) {
                const int next = pending.back();
                pending.pop_back();
                if (next < 0) {
                    needed = true;
                    break;
                }
                if (seen[next]) continue;
                seen[next] = true;
                const auto& node = spec.nodes[next];
                if (node.empty || node.read) {
                    needed = true;
                    break;
                }
                if (node.kill) continue;
                if (node.next.empty()) {
                    needed = true;
                    break;
                }
                pending.insert(pending.end(), node.next.begin(), node.next.end());
            }
            FeatureSet features{};
            features.flags_branch_only = false;
            features.flag_carry_elim = true;
            HIRBuilder builder{1, false, fast, features};
            auto* function = builder.AppendFunction(Location{0x7000}, Location{0x8000});
            auto* root = function->GetCurrentBlock();
            const auto input = builder.LoadUniform(Uniform{0, ValueType::U64});
            const auto sum = builder.Add(ValueType::U64, input, Operand{Imm{1u}});
            builder.SaveFlags(sum, Flags::All);
            const auto* publication = &root->GetInstList().back();
            const auto condition = builder.LoadImm(ValueType::U8, Imm{1u});
            const auto selector = builder.LoadImm(ValueType::U64, Imm{u64{0}});
            std::vector<HIRBlock*> blocks;
            for (size_t i = 0; i < spec.nodes.size(); ++i)
                blocks.push_back(function->AppendBlock(Location{0x7100 + i * 0x10}));
            const auto link = [&](int index) {
                return terminal::LinkBlock{index < 0
                                                   ? Location{0x7f00}
                                                   : blocks[index]->GetBlock()->GetStartLocation()};
            };
            const auto make_switch = [&](const std::vector<int>& targets) {
                Vector<terminal::Switch::Case> arms;
                for (size_t i = 0; i < targets.size(); ++i)
                    arms.push_back({Imm{u64{i}}, link(targets[i])});
                return terminal::Switch{selector, arms};
            };
            const auto edges = [&](const HIRBlock* from, const std::vector<int>& targets) {
                for (int target : targets)
                    if (target >= 0) function->AddEdge(from, blocks[target]);
            };
            Terminal root_terminal = terminal::ReturnToHost{};
            switch (spec.terminal) {
                case 0:
                    root_terminal = link(spec.roots.front());
                    break;
                case 1:
                    root_terminal = terminal::LinkBlockFast{link(spec.roots.front()).next};
                    break;
                case 2:
                    root_terminal = make_switch(spec.roots);
                    break;
                case 3:
                    root_terminal = terminal::If{
                            BOOL{condition},
                            link(spec.roots[0]),
                            make_switch({spec.roots[1], spec.roots[0], spec.roots[1]})};
                    break;
                case 4:
                    root_terminal = terminal::If{
                            BOOL{condition}, link(spec.roots[0]), terminal::ReturnToHost{}};
                    break;
                case 5:
                    root_terminal =
                            terminal::Condition{Cond::EQ, link(spec.roots[0]), link(spec.roots[0])};
                    break;
                case 6:
                    root_terminal = terminal::CheckHalt{link(spec.roots[0])};
                    break;
            }
            function->EndBlock(std::move(root_terminal));
            edges(root, spec.roots);
            for (size_t i = 0; i < spec.nodes.size(); ++i) {
                const auto& node = spec.nodes[i];
                builder.SetCurBlock(blocks[i]);
                if (!node.empty) {
                    builder.AdvancePC(Imm{1u});
                    if (node.read) {
                        const auto flags = builder.TestFlags(Flags::Carry);
                        builder.StoreUniform(Uniform{16, ValueType::U8}, flags);
                    }
                    if (node.kill) builder.ClearFlags(Flags::All);
                }
                function->EndBlock(node.next.empty() ? Terminal{terminal::ReturnToHost{}}
                                                     : Terminal{link(node.next.front())});
                edges(blocks[i], node.next);
            }
            function->EndFunction();
            function->ComputeRPO();
            function->IdByRPO();
            compile::EliminateFlags(RewriteFixture{function}.Get(), features);
            const auto& list = root->GetInstList();
            REQUIRE(std::any_of(list.begin(), list.end(), [&](const Inst& inst) {
                        return &inst == publication;
                    }) == needed);
        }
    }
}

TEST_CASE("carry protection follows preceding writers across empty instruction regions",
          "[ir][rewrite][flags][carry]") {
    for (unsigned pattern = 0; pattern != 256; ++pattern) {
        for (bool subtract : {false, true}) {
            for (bool enabled : {false, true}) {
                CAPTURE(pattern, subtract, enabled);
                // Four real regions, with an empty region on entry and two
                // on exit. The oracle asks whether a consumer is reachable
                // before an intervening writer, independently for each region.
                std::array<bool, 7> writes{}, consumes{}, protected_regions{};
                for (size_t i = 1; i != 5; ++i) {
                    writes[i] = pattern & (1u << (2 * (i - 1)));
                    consumes[i] = pattern & (2u << (2 * (i - 1)));
                }
                for (size_t from = 0; from != consumes.size(); ++from) {
                    for (size_t use = from; use != consumes.size(); ++use) {
                        if (!consumes[use]) continue;
                        bool killed{};
                        for (size_t between = from + 1; between < use; ++between)
                            killed |= writes[between];
                        protected_regions[from] |= !killed;
                    }
                }
                Block block{0, Location{0x8700}};
                const auto left = block.LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
                const auto right = block.LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
                const auto producer = block.Add(ValueType::U64, left, Operand{right});
                const auto carry_value = block.LoadImm(ValueType::U8, Imm{1u});
                std::array<const Inst*, 4> parity_writes{};
                for (size_t region = 0; region != consumes.size(); ++region) {
                    if (region != 0 && region < 5) {
                        block.ClearFlags(Flags::Parity);
                        parity_writes[region - 1] = &block.GetInstList().back();
                        if (writes[region]) {
                            if (region == 1)
                                block.ClearFlags(Flags::Carry);
                            else if (region == 2)
                                block.SetCarry(carry_value);
                            else
                                block.SaveFlags(producer, Flags::Carry);
                        }
                        if (consumes[region]) {
                            const auto result =
                                    subtract ? block.Sbb(ValueType::U64, left, Operand{right})
                                             : block.Adc(ValueType::U64, left, Operand{right});
                            block.StoreUniform(Uniform{16 + u32(region - 1) * 8, ValueType::U64},
                                               result);
                        }
                    }
                    if (region + 1 != consumes.size()) block.AdvancePC(Imm{1u});
                }
                block.SetTerminal(terminal::ReturnToHost{});
                FeatureSet features{};
                features.flags_branch_only = false;
                features.flag_carry_elim = enabled;
                compile::CompileWorkspace workspace;
                compile::RewriteContext rewrite{&block, workspace, nullptr, false};
                compile::EliminateFlags(rewrite, features, Flags::None);
                const bool legacy = !enabled && std::any_of(consumes.begin(),
                                                            consumes.end(),
                                                            [](bool value) { return value; });
                for (size_t region = 1; region != 5; ++region) {
                    // Protected regions conservatively retain every flag
                    // write. The preceding unprotected region supplies PF
                    // across that observation boundary as in the old pass.
                    const bool expected =
                            legacy || protected_regions[region] || protected_regions[region + 1];
                    REQUIRE(std::any_of(block.GetInstList().begin(),
                                        block.GetInstList().end(),
                                        [&](const Inst& inst) {
                                            return &inst == parity_writes[region - 1];
                                        }) == expected);
                }
            }
        }
    }
}

TEST_CASE("Function branch-only shares demand from before its first rewrite",
          "[ir][rewrite][flags][cfg]") {
    for (bool fast : {false, true}) {
        for (bool enabled : {false, true}) {
            for (bool read : {false, true}) {
                for (unsigned shape : {0u, 1u, 2u}) {
                    CAPTURE(fast, enabled, read, shape);
                    FeatureSet features{};
                    features.flags_branch_only = enabled;
                    HIRBuilder builder{1, false, fast, features};
                    auto* function = builder.AppendFunction(Location{0x8100}, Location{0x8600});
                    auto* root = function->GetCurrentBlock();
                    auto* middle = function->AppendBlock(Location{0x8200});
                    auto* observer = function->AppendBlock(Location{0x8300});
                    auto* bypass = function->AppendBlock(Location{0x8400});
                    auto* other = function->AppendBlock(Location{0x8500});
                    builder.SetCurBlock(root);
                    Value condition;
                    if (shape == 1) {
                        const auto left = builder.LoadUniform(Uniform{0, ValueType::V128});
                        const auto right = builder.LoadUniform(Uniform{16, ValueType::V128});
                        const auto relation =
                                builder.VecFCmp(ValueType::U64, left, right, Imm{64u}, Imm{1u});
                        builder.PublishFCmpFlags(relation, Imm{1u});
                        builder.AdvancePC(Imm{4u});
                        condition = builder.FCmpCondSet(ValueType::U8, relation, Cond::LT);
                    } else {
                        const auto input = builder.LoadUniform(Uniform{0, ValueType::U64});
                        const auto value = builder.And(ValueType::U64, input, Operand{Imm{0xffu}});
                        builder.SaveFlags(value, Flags::All);
                        builder.AdvancePC(Imm{4u});
                        condition = builder.LocalCondSet(ValueType::U8, Cond::NE);
                    }
                    builder.BranchOnlyEdges();
                    if (shape == 2) builder.BranchOnlyEdges();
                    function->EndBlock(terminal::If{BOOL{condition},
                                                    terminal::LinkBlock{Location{0x8200}},
                                                    terminal::LinkBlock{Location{0x8400}}});
                    function->AddEdge(root, middle);
                    function->AddEdge(root, bypass);
                    builder.SetCurBlock(middle);
                    const auto input = builder.LoadUniform(Uniform{0, ValueType::U64});
                    const auto value = builder.And(ValueType::U64, input, Operand{Imm{0xffu}});
                    builder.SaveFlags(value, Flags::All);
                    builder.AdvancePC(Imm{4u});
                    const auto test = builder.LocalCondSet(ValueType::U8, Cond::NE);
                    function->EndBlock(terminal::If{BOOL{test},
                                                    terminal::LinkBlock{Location{0x8300}},
                                                    terminal::LinkBlock{Location{0x8500}}});
                    function->AddEdge(middle, observer);
                    function->AddEdge(middle, other);
                    for (auto* leaf : {observer, bypass, other}) {
                        builder.SetCurBlock(leaf);
                        builder.AdvancePC(Imm{1u});
                        if (leaf == observer && read) {
                            const auto flags = builder.TestFlags(Flags::Carry);
                            builder.StoreUniform(Uniform{32, ValueType::U8}, flags);
                        }
                        builder.ClearFlags(Flags::All);
                        function->EndBlock(terminal::ReturnToHost{});
                    }
                    function->EndFunction();
                    function->ComputeRPO();
                    function->IdByRPO();
                    compile::CompileWorkspace workspace;
                    compile::RewriteContext rewrite{function, workspace, false};
                    for (unsigned iteration = 0; iteration != 2; ++iteration) {
                        compile::EliminateFlags(rewrite, features);
                        const auto count = [](const HIRBlock* block, OpCode op) {
                            return std::count_if(
                                    block->GetInstList().begin(),
                                    block->GetInstList().end(),
                                    [&](const Inst& inst) { return inst.GetOp() == op; });
                        };
                        REQUIRE(count(root, OpCode::BranchOnlyFlags) ==
                                (enabled && shape == 0 ? 1 : 0));
                        REQUIRE(count(middle, OpCode::BranchOnlyFlags) ==
                                (enabled && !read ? 1 : 0));
                        REQUIRE(count(root, OpCode::BranchOnlyEdges) == 0);
                    }
                }
            }
        }
    }
}

TEST_CASE("branch-only arithmetic retains writes observed by a later fault",
          "[ir][rewrite][flags][cfg][fault]") {
    for (const bool fast : {false, true})
    for (const bool read_before : {false, true}) {
        CAPTURE(fast, read_before);
        FeatureSet features{};
        features.flags_branch_only = true;
        HIRBuilder builder{1, false, fast, features};
        auto* function = builder.AppendFunction(Location{0x8610}, Location{0x8650});
        auto* source = function->GetCurrentBlock();
        auto* then_block = function->AppendBlock(Location{0x8620});
        auto* else_block = function->AppendBlock(Location{0x8630});
        builder.SetCurBlock(source);
        const auto address = builder.LoadUniform(Uniform{0, ValueType::U64});
        auto input = builder.LoadUniform(Uniform{8, ValueType::U64});
        if (read_before) {
            // This fault precedes the arithmetic and cannot observe its flags.
            input = builder.LoadMemory(ValueType::U64, Operand{address});
        }
        const auto produced = builder.Add(ValueType::U64, input, Operand{Imm{u64{1}}});
        builder.SaveFlags(produced, Flags::All);
        if (!read_before) {
            // SaveFlags is an explicit IR write before this possible fault.
            // This tests IR order, not x86 instruction rollback on a store fault.
            builder.StoreMemory(Operand{address}, produced);
        }
        // Keep the arithmetic result live independently of its flag pseudo.
        builder.StoreUniform(Uniform{16, ValueType::U64}, produced);
        builder.AdvancePC(Imm{4u});
        const auto condition = builder.LocalCondSet(ValueType::U8, Cond::NE);
        function->EndBlock(terminal::If{BOOL{condition},
                                       terminal::LinkBlock{Location{0x8620}},
                                       terminal::LinkBlock{Location{0x8630}}});
        function->AddEdge(source, then_block);
        function->AddEdge(source, else_block);
        for (auto* successor : {then_block, else_block}) {
            builder.SetCurBlock(successor);
            builder.ClearFlags(Flags::All);
            function->EndBlock(terminal::ReturnToHost{});
        }
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        const auto* arithmetic = produced.Def();
        compile::EliminateFlags(RewriteFixture{function}.Get(), features);
        const auto& instructions = source->GetInstList();
        REQUIRE(std::any_of(instructions.begin(), instructions.end(),
                            [&](const Inst& inst) { return &inst == arithmetic; }));
        const auto saves = CollectPseudoOperations(*source->GetBlock(), arithmetic,
                                                   OpCode::SaveFlags);
        const auto branch_only = CollectPseudoOperations(*source->GetBlock(), arithmetic,
                                                         OpCode::BranchOnlyFlags);
        REQUIRE(saves.size() == size_t{read_before ? 0u : 1u});
        REQUIRE(branch_only.size() == size_t{read_before ? 1u : 0u});
        if (read_before) REQUIRE(branch_only.front()->GetArg<Flags>(1) == Flags::Zero);
        else REQUIRE(saves.front()->GetArg<Flags>(1) == Flags::All);
    }
}

TEST_CASE("terminal views borrow read-only storage while explicit snapshots remain independent",
          "[ir][terminal][lifetime]") {
    Block block{0, Location{0x6800}};
    const auto condition = block.LoadImm(ValueType::U8, Imm{1u});
    const auto selector = block.LoadImm(ValueType::U64, Imm{u64{2}});
    Vector<terminal::Switch::Case> cases{{Imm{2u}, terminal::LinkBlock{Location{0x6900}}}};
    block.SetTerminal(terminal::If{BOOL{condition},
                                   terminal::Switch{selector, cases},
                                   terminal::CheckHalt{terminal::ReturnToHost{}}});
    const auto& first = block.GetTerminal();
    const auto& second = std::as_const(block).GetTerminal();
    REQUIRE(&first == &second);
    std::vector<const Inst*> values;
    compile::VisitTerminalValues(first, [&](Value value) { values.push_back(value.Def()); });
    REQUIRE(values == std::vector<const Inst*>{condition.Def(), selector.Def()});
    Terminal snapshot = first;
    block.SetTerminal(terminal::ReturnToDispatch{});
    REQUIRE(boost::get<terminal::ReturnToDispatch>(&block.GetTerminal()));
    const auto* saved = boost::get<terminal::If>(&snapshot);
    REQUIRE(saved);
    const auto* selected = boost::get<terminal::Switch>(&saved->then_);
    REQUIRE(selected);
    REQUIRE(selected->cases.size() == 1);
    REQUIRE(boost::get<terminal::LinkBlock>(&selected->cases.front().then)->next ==
            Location{0x6900});
}

TEST_CASE("terminal membership keeps raw aliases, resolved roots and implicit anchors distinct",
          "[ir][terminal][facts]") {
    using compile::TerminalValueUses;
    for (u32 count : {0u, 1u, 8u, 40u}) {
        CAPTURE(count);
        IntrusivePtr<Block> block{new Block(0, Location{0x6d00})};
        Block foreign{0, Location{0x6e00}};
        const auto outside = foreign.LoadImm(ValueType::U64, Imm{u64{0}});
        std::vector<Value> roots, aliases;
        Terminal tail = terminal::ReturnToHost{};
        for (u32 i = 0; i < count; ++i) {
            roots.push_back(block->LoadImm(ValueType::U64, Imm{u64{i}}));
            aliases.push_back(block->BitCast(ValueType::U64, roots.back()));
            const auto alias = aliases.back();
            Vector<terminal::Switch::Case> cases{
                    {Imm{0u}, terminal::CheckHalt{std::move(tail)}},
                    {Imm{1u}, terminal::Condition{Cond::EQ,
                                                 terminal::ReturnToHost{},
                                                 terminal::ReturnToDispatch{}}}};
            tail = terminal::If{BOOL{Value{alias}.SetCastType(ValueType::U8)},
                                 terminal::Switch{alias, cases},
                                 terminal::ReturnToHost{}};
        }
        const auto anchor = block->LoadImm(ValueType::U64, Imm{u64{73}});
        block->SetTerminal(std::move(tail));
        std::array<const Inst*, 1> anchors{anchor.Def()};
        compile::CompileUnit unit{block.get()};
        unit.SetLoopHoistMetadata(block.get(), {anchor.Def(), anchors, 0, 1});
        const TerminalValueUses raw{block.get(), TerminalValueUses::Aliases::Raw};
        const TerminalValueUses resolved{block.get(), TerminalValueUses::Aliases::Resolved};
        for (u32 i = 0; i < count; ++i) {
            REQUIRE(raw.Contains(aliases[i].Def()));
            REQUIRE_FALSE(raw.Contains(roots[i].Def()));
            REQUIRE(resolved.Contains(roots[i].Def()));
            REQUIRE_FALSE(resolved.Contains(aliases[i].Def()));
        }
        REQUIRE_FALSE(raw.Contains(anchor.Def()));
        REQUIRE_FALSE(resolved.Contains(anchor.Def()));
        REQUIRE_FALSE(raw.Contains(outside.Def()));
        REQUIRE_FALSE(resolved.Contains(outside.Def()));
        REQUIRE_FALSE(raw.Contains(nullptr));
        REQUIRE_FALSE(resolved.Contains(nullptr));
    }
}

TEST_CASE("terminal membership keeps independent Blocks and Function RPO scope separate",
          "[ir][terminal][facts]") {
    using compile::TerminalValueUses;
    HIRBuilder builder{1, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{0x7100}, Location{0x7400});
    auto* root = function->GetCurrentBlock();
    const auto condition = function->GetHostGPR<BOOL>(Imm{2u}, Imm{0u});
    auto* next = function->AppendBlock(Location{0x7200});
    function->EndBlock(terminal::If{condition,
                                    terminal::LinkBlock{Location{0x7200}},
                                    terminal::ReturnToHost{}});
    function->AddEdge(root, next);
    function->SetCurBlock(next);
    const auto source = function->LoadImm<U64>(Imm{u64{91}});
    const auto alias = function->BitCast<U64>(source);
    function->EndBlock(terminal::Switch{
            alias, {{Imm{0u}, terminal::CheckHalt{terminal::ReturnToHost{}}}}});
    auto* disconnected = function->AppendBlock(Location{0x7300});
    function->SetCurBlock(disconnected);
    const auto dead = function->GetHostGPR<BOOL>(Imm{3u}, Imm{0u});
    function->EndBlock(terminal::If{
            dead, terminal::ReturnToHost{}, terminal::ReturnToDispatch{}});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    const TerminalValueUses raw{function, TerminalValueUses::Aliases::Raw};
    const TerminalValueUses resolved{function, TerminalValueUses::Aliases::Resolved};
    REQUIRE(raw.Contains(condition.Def()));
    REQUIRE(raw.Contains(alias.Def()));
    REQUIRE_FALSE(raw.Contains(source.Def()));
    REQUIRE_FALSE(raw.Contains(dead.Def()));
    REQUIRE(resolved.Contains(condition.Def()));
    REQUIRE(resolved.Contains(source.Def()));
    REQUIRE_FALSE(resolved.Contains(alias.Def()));
    REQUIRE_FALSE(resolved.Contains(dead.Def()));
    const TerminalValueUses root_only{root->GetBlock(), TerminalValueUses::Aliases::Raw};
    const TerminalValueUses next_only{next->GetBlock(), TerminalValueUses::Aliases::Resolved};
    const TerminalValueUses dead_only{disconnected->GetBlock(), TerminalValueUses::Aliases::Raw};
    REQUIRE(root_only.Contains(condition.Def()));
    REQUIRE_FALSE(root_only.Contains(alias.Def()));
    REQUIRE(next_only.Contains(source.Def()));
    REQUIRE_FALSE(next_only.Contains(condition.Def()));
    REQUIRE(dead_only.Contains(dead.Def()));
}

TEST_CASE("branch-only qualification preserves guards and erases retained ranges safely",
          "[ir][rewrite][flags]") {
    for (bool floating : {false, true}) {
        for (unsigned marker : {0u, 1u, 2u}) {
            for (unsigned shape : {0u, 1u, 2u, 3u, 4u}) {
                CAPTURE(floating, marker, shape);
                Block block{0, Location{0x6a00}};
                if (marker == 0) block.BranchOnlyEdges();
                Value producer;
                if (floating) {
                    const auto left =
                            block.LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
                    const auto right =
                            block.LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
                    producer = block.VecFCmp(ValueType::U64, left, right, Imm{64u}, Imm{1u});
                    if (shape != 3) block.PublishFCmpFlags(producer, Imm{1u});
                    block.InvertCarry();
                } else {
                    const auto input =
                            block.LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
                    producer = block.And(ValueType::U64, input, Operand{Imm{u64{0xff}}});
                    block.SaveFlags(producer, Flags::NZCV);
                    block.ClearFlags(Flags::Parity | Flags::AuxiliaryCarry);
                }
                const auto polarity = block.LoadImm(ValueType::U8, Imm{0u});
                block.StoreUniform(Uniform{48, ValueType::U8}, polarity);
                if (!floating && shape == 3)
                    block.StoreUniform(Uniform{49, ValueType::U8}, polarity);
                if (marker == 1) block.BranchOnlyEdges();
                if (shape != 2) block.AdvancePC(Imm{4u});
                const auto condition =
                        floating ? block.FCmpCondSet(ValueType::U8, producer, Cond::LT)
                                 : block.LocalCondSet(ValueType::U8, Cond::NE);
                if (marker == 2) block.BranchOnlyEdges();
                if (shape == 1) block.BranchOnlyEdges();
                if (shape == 4) block.StoreUniform(Uniform{56, ValueType::U8}, condition);
                block.SetTerminal(terminal::If{BOOL{condition},
                                               terminal::LinkBlock{Location{0x6b00}},
                                               terminal::LinkBlock{Location{0x6c00}}});
                FeatureSet features{};
                features.flags_branch_only = true;
                compile::CompileWorkspace workspace;
                compile::RewriteContext rewrite{&block, workspace, nullptr, false};
                compile::EliminateFlags(rewrite, features);
                const bool accepted = shape == 0;
                const auto count = [&](OpCode op) {
                    return std::count_if(block.GetInstList().begin(),
                                         block.GetInstList().end(),
                                         [&](const Inst& inst) { return inst.GetOp() == op; });
                };
                REQUIRE(count(OpCode::BranchOnlyEdges) == 0);
                if (floating) {
                    REQUIRE(count(OpCode::PublishFCmpFlags) == (shape == 3 || accepted ? 0 : 1));
                    REQUIRE(count(OpCode::InvertCarry) == (accepted ? 0 : 1));
                } else {
                    REQUIRE(count(OpCode::BranchOnlyFlags) == (accepted ? 1 : 0));
                }
                const auto stores =
                        std::count_if(block.GetInstList().begin(),
                                      block.GetInstList().end(),
                                      [](const Inst& inst) {
                                          return inst.GetOp() == OpCode::StoreUniform &&
                                                 inst.GetArg<Uniform>(0).GetOffset() == 48;
                                      });
                REQUIRE(stores == (accepted ? 0 : 1));
            }
        }
    }
}

TEST_CASE("state forwarding registers its new HIR operand use",
          "[ir][rewrite][state]") {
    FeatureSet features{};
    features.uniform_dse = false;
    features.xmm_fault_sink = false;
    HIRBuilder builder{1, false, features};
    auto* function = builder.AppendFunction(Location{0x6300}, Location{0x6400});
    const auto source = function->LoadImm<U64>(Imm{u64{42}});
    function->StoreUniform(Uniform{0, ValueType::U64}, source);
    const auto load = function->LoadUniform(Uniform{0, ValueType::U64});
    function->StoreUniform(Uniform{8, ValueType::U64}, load);
    function->EndBlock(terminal::ReturnToDispatch{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    swift::runtime::compile::GuestStateLayout info{};
    info.uniform_size = 16;
    swift::runtime::compile::CleanupState(RewriteFixture{function, false}.Get(), info, features);
    REQUIRE(load.Def()->GetOp() == OpCode::BitCast);
    REQUIRE(load.Def()->GetArg<Value>(0).Def() == source.Def());
    REQUIRE(std::count_if(function->GetHIRValue(source)->GetUses().begin(),
                          function->GetHIRValue(source)->GetUses().end(),
                          [&](const HIRUse& use) { return use.inst == load.Def(); }) == 1);

    // The shared byte rules must not turn a logical narrow view into an alias
    // that leaks its producer's high bits, nor join unrelated byte sources.
    using compile::GuestStateByteFact;
    using compile::GuestStateReadKind;
    using compile::GuestStateReadPolicy;
    Block values{0, Location{0x6500}};
    const auto wide = values.LoadImm<U64>(Imm{u64{0xfedcba9876543210}});
    const auto other = values.LoadImm<U64>(Imm{u64{0x0123456789abcdef}});
    const auto vector = values.LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
    std::array<GuestStateByteFact, 16> bytes{};
    for (u8 byte = 0; byte < 8; ++byte) bytes[byte] = {wide, byte};
    const auto full = compile::ReadGuestStateBytes(
            bytes, Uniform{0, ValueType::U64}, GuestStateReadPolicy::Forward);
    REQUIRE(full.kind == GuestStateReadKind::Alias);
    REQUIRE(full.value.Def() == wide.Def());
    const auto partial = compile::ReadGuestStateBytes(
            bytes, Uniform{2, ValueType::U32}, GuestStateReadPolicy::Forward);
    REQUIRE(partial.kind == GuestStateReadKind::Extract);
    REQUIRE(partial.offset == 2);
    for (const auto type : {ValueType::U8, ValueType::U16, ValueType::U32}) {
        CAPTURE(type);
        const auto view = Value{wide.Def()}.SetCastType(type);
        const auto read = compile::ClassifyGuestStateRead(
                view, 0, type, GuestStateReadPolicy::Forward);
        REQUIRE(read.kind == GuestStateReadKind::Extract);
        REQUIRE(read.value.Def() == wide.Def());
    }
    // Byte identity is the definition and source offset, not the edge's view.
    bytes[2].value = Value{wide.Def()}.SetCastType(ValueType::U32);
    REQUIRE(compile::ReadGuestStateBytes(bytes, Uniform{0, ValueType::U64},
                                        GuestStateReadPolicy::Forward).Found());
    bytes[2].offset = 3;
    REQUIRE_FALSE(compile::ReadGuestStateBytes(bytes, Uniform{0, ValueType::U64},
                                              GuestStateReadPolicy::Forward).Found());
    bytes[2] = {};
    REQUIRE_FALSE(compile::ReadGuestStateBytes(bytes, Uniform{0, ValueType::U64},
                                              GuestStateReadPolicy::Forward).Found());
    REQUIRE_FALSE(compile::ReadGuestStateBytes(bytes, Uniform{14, ValueType::U32},
                                              GuestStateReadPolicy::Forward).Found());
    REQUIRE_FALSE(compile::ClassifyGuestStateRead(
            wide, std::numeric_limits<u32>::max(), ValueType::U8,
            GuestStateReadPolicy::Forward).Found());

    for (u8 byte = 0; byte < bytes.size(); ++byte) bytes[byte] = {vector, byte};
    REQUIRE_FALSE(compile::ReadGuestStateBytes(bytes, Uniform{0, ValueType::V64},
                                              GuestStateReadPolicy::Forward).Found());
    REQUIRE(compile::ReadGuestStateBytes(bytes, Uniform{0, ValueType::V64},
                                        GuestStateReadPolicy::ForwardNarrowFPR).kind ==
            GuestStateReadKind::Alias);
    REQUIRE_FALSE(compile::ReadGuestStateBytes(bytes, Uniform{8, ValueType::V64},
                                              GuestStateReadPolicy::ForwardNarrowFPR).Found());
    REQUIRE_FALSE(compile::ReadGuestStateBytes(bytes, Uniform{8, ValueType::U64},
                                              GuestStateReadPolicy::ForwardNarrowFPR).Found());
    for (const u32 offset : {0u, 8u}) {
        const auto lane = compile::ClassifyGuestStateRead(
                vector, offset, ValueType::U64, GuestStateReadPolicy::PendingStore);
        REQUIRE(lane.kind == GuestStateReadKind::VectorLane64);
        REQUIRE(lane.offset == offset);
    }

    std::array<GuestStateByteFact, 4> fallthrough{{{wide, 0}, {wide, 1}, {wide, 2}, {wide, 3}}};
    auto taken = fallthrough;
    taken[0].value = Value{wide.Def()}.SetCastType(ValueType::U32);
    taken[1].offset = 2;
    taken[2].value = other;
    taken[3] = {};
    REQUIRE(compile::MeetGuestStateBytes(fallthrough, taken) == 1);
    REQUIRE(fallthrough[0].value.Def() == wide.Def());
    for (size_t byte = 1; byte < fallthrough.size(); ++byte) {
        REQUIRE_FALSE(fallthrough[byte].Defined());
    }

    // Layout eligibility is shared, while decode still decides whether a
    // partial mapped store must wait for the residual pass's byte context.
    using compile::GuestStateAccessKind;
    compile::GuestStateLayout layout{.uniform_size = 64};
    const compile::GuestRegisterBinding gpr{
            Uniform{16, ValueType::U64}, HostReg{.gpr = {20}, .is_fpr = false}};
    const compile::GuestRegisterBinding fpr{
            Uniform{32, ValueType::V128}, HostReg{.fpr = {4}, .is_fpr = true}};
    layout.uniform_regs_map.Map(16, 24, gpr);
    layout.uniform_regs_map.Map(32, 48, fpr);
    const auto access = [&](u32 offset, ValueType type, bool defer_fpr = false) {
        return compile::ResolveGuestStateAccess(layout, offset, type, defer_fpr);
    };
    REQUIRE(access(0, ValueType::U64).kind == GuestStateAccessKind::Memory);
    REQUIRE(access(12, ValueType::U64).kind == GuestStateAccessKind::Invalid);
    REQUIRE(access(16, ValueType::V64).kind == GuestStateAccessKind::Invalid);
    REQUIRE(access(20, ValueType::U32).kind == GuestStateAccessKind::Register);
    REQUIRE(access(20, ValueType::U32).binding.host_reg.gpr.id == 20);
    REQUIRE(access(20, ValueType::U64).kind == GuestStateAccessKind::Invalid);
    REQUIRE(access(32, ValueType::V128).kind == GuestStateAccessKind::Register);
    REQUIRE(access(40, ValueType::U64).kind == GuestStateAccessKind::Register);
    REQUIRE(access(40, ValueType::U64).binding.host_reg.fpr.id == 4);
    REQUIRE(access(44, ValueType::V128).kind == GuestStateAccessKind::Invalid);
    REQUIRE(access(44, ValueType::V128, true).kind == GuestStateAccessKind::Memory);
    REQUIRE(access(60, ValueType::U64).kind == GuestStateAccessKind::Invalid);
    REQUIRE(access(UINT32_MAX, ValueType::U64).kind == GuestStateAccessKind::Invalid);
    REQUIRE(access(0, ValueType::VOID).kind == GuestStateAccessKind::Invalid);
    layout.uniform_regs_map.Unmap(18, 20);
    REQUIRE(access(16, ValueType::U64).kind == GuestStateAccessKind::Invalid);

    compile::GuestStateForwarding forwarding;
    using Event = compile::GuestStateForwardEvent;
    const auto condition = values.LoadImm<BOOL>(Imm{1u});
    const auto first_edge = values.Goto(condition);
    const auto second_edge = values.NotGoto(condition);
    values.BindLabel(first_edge);
    const auto* first_label = &values.GetInstList().back();
    values.BindLabel(second_edge);
    const auto* second_label = &values.GetInstList().back();
    const std::array<GuestStateByteFact, 4> entry{{{wide, 0}, {wide, 1}, {wide, 2}, {wide, 3}}};
    auto current = entry;
    REQUIRE(forwarding.Observe(current, *first_edge.Def(), false, true).event == Event::Branch);
    REQUIRE(forwarding.Observe(current, *second_edge.Def(), false, true).event == Event::Branch);
    current[0].value = Value{wide.Def()}.SetCastType(ValueType::U32);
    current[1].value = other;
    current[2].offset = 3;
    current[3] = {};
    const auto first_merge = forwarding.Observe(current, *first_label, false, true);
    REQUIRE(first_merge.event == Event::Merge);
    REQUIRE(first_merge.merged_bytes == 1);
    REQUIRE(current[0].value.Def() == wide.Def());
    REQUIRE(forwarding.Observe(current, *second_label, false, true).merged_bytes == 1);
    REQUIRE(forwarding.Observe(current, *first_label, false, true).event ==
            Event::FullInvalidation);
    REQUIRE(std::none_of(current.begin(), current.end(),
                         [](const auto& byte) { return byte.Defined(); }));

    // An existing edge with an empty meet differs from a missing edge, which
    // invalidates the entire table. Reset at block exit must forget both.
    current = entry;
    (void)forwarding.Observe(current, *first_edge.Def(), false, true);
    for (auto& byte : current) byte = {other, 0};
    const auto empty_meet = forwarding.Observe(current, *first_label, false, true);
    REQUIRE(empty_meet.event == Event::Merge);
    REQUIRE(empty_meet.merged_bytes == 0);
    current = entry;
    (void)forwarding.Observe(current, *first_edge.Def(), false, true);
    forwarding.Reset();
    REQUIRE(forwarding.Observe(current, *first_label, false, true).event ==
            Event::FullInvalidation);
    REQUIRE_FALSE(current[0].Defined());

}

TEST_CASE("typed construction and residual folding agree after result typing",
          "[ir][builder][fold]") {
    struct Case {
        OpCode op;
        ValueType input, result;
        u64 bits, expected;
    };
    const Case cases[]{
            {OpCode::SignExtend, ValueType::U8, ValueType::S64, 0x80, 0xffffffffffffff80},
            {OpCode::SignExtend, ValueType::U16, ValueType::S32, 0x8000, 0xffff8000},
            {OpCode::ZeroExtend32To64, ValueType::U64, ValueType::U64,
             0xdeadbeef87654321, 0x87654321},
            {OpCode::BitExtract, ValueType::U64, ValueType::U32,
             0x123456789abcdef0, 0xbcde},
    };
    for (const auto& item : cases) {
        for (bool typed : {false, true}) {
            CAPTURE(item.op, item.input, item.result, typed);
            Block block{0, Location{0x5700}};
            compile::IrBuilder builder{&block};
            const auto input = builder.LoadImm(item.input, Imm{item.bits});
            Value result;
            switch (item.op) {
                case OpCode::SignExtend:
                    result = typed ? builder.SignExtend(item.result, input)
                                   : block.SignExtend(item.result, input);
                    break;
                case OpCode::ZeroExtend32To64:
                    result = typed ? builder.ZeroExtend32To64(item.result, input)
                                   : block.ZeroExtend32To64(item.result, input);
                    break;
                case OpCode::BitExtract:
                    result = typed ? builder.BitExtract(item.result, input, Imm{8u}, Imm{16u})
                                   : block.BitExtract(item.result, input, Imm{8u}, Imm{16u});
                    break;
                default: FAIL("unexpected width operation");
            }
            REQUIRE(result.Def()->GetOp() == (typed ? OpCode::LoadImm : item.op));
            FeatureSet features{};
            features.int_imm_fold = true;
            compile::FoldValues(RewriteFixture{&block, false}.Get(), features);
            REQUIRE(result.Def()->GetOp() == OpCode::LoadImm);
            REQUIRE(result.Type() == item.result);
            REQUIRE(result.Def()->GetArg<Imm>(0).Get() == item.expected);
            REQUIRE(input.Def()->ReturnType() == item.input);
        }
    }
    for (unsigned shape = 0; shape < 14; ++shape) {
        for (const bool typed : {false, true}) {
            CAPTURE(shape, typed);
            Block block{0, Location{0x57a0}};
            compile::IrBuilder builder{&block};
            const auto unknown = block.GetHostGPR(ValueType::U64, Imm{u32{20}}, Imm{u32{0}});
            Value source;
            OpCode op = OpCode::BitExtract;
            ValueType type = ValueType::U32;
            swift::u64 left = 32, count = 32, expected = 0;
            if (shape == 0) {
                source = block.Xor(ValueType::U64, unknown, Operand{unknown});
                source = source.SetCastType(ValueType::U8);
                op = OpCode::SignExtend;
                type = ValueType::U64;
            } else if (shape == 1) {
                source = block.And(ValueType::U64, unknown, Operand{Imm{u64{0xffff}}});
            } else if (shape == 2) {
                source = block.Or(ValueType::U64, unknown, Operand{Imm{UINT64_MAX}});
                op = OpCode::ZeroExtend32To64;
                type = ValueType::U64;
                expected = UINT32_MAX;
            } else if (shape == 3) {
                const auto byte = Value{unknown}.SetCastType(ValueType::U8);
                source = block.SignExtend(ValueType::S64, byte);
                REQUIRE(compile::AnalyzeIntegerValue(source).SignExtendedFrom() == 8);
                left = 8;
                count = 8;
                type = ValueType::U8;
            } else if (shape == 4) {
                source = block.GetHostGPR(ValueType::U32, Imm{u32{21}}, Imm{u32{0}});
                REQUIRE(compile::AnalyzeIntegerValue(source).KnownZero() == 0);
                source = source.SetCastType(ValueType::U64);
            } else if (shape == 5) {
                source = block.BitCast(ValueType::U32, unknown);
                source = block.BitCast(ValueType::U64, source);
            } else if (shape == 6) {
                const auto low = block.And(ValueType::U64, unknown, Operand{Imm{0xfu}});
                source = block.Add(ValueType::U64, low, Operand{Imm{1u}});
            } else if (shape == 7) {
                source = block.Sub(ValueType::U64, unknown, Operand{unknown});
            } else if (shape == 8) {
                const auto zero = block.Xor(ValueType::U64, unknown, Operand{unknown});
                source = block.Neg(ValueType::U64, zero);
            } else if (shape == 9) {
                source = block.AndNot(ValueType::U64, unknown, Operand{unknown});
            } else if (shape == 10) {
                const auto low = block.And(ValueType::U64, unknown, Operand{Imm{0xffu}});
                source = block.LslImm(ValueType::U64, low, Imm{32u});
                left = 48;
                count = 16;
            } else if (shape == 11) {
                const auto low = block.And(ValueType::U64, unknown, Operand{Imm{0xffffu}});
                source = block.LsrImm(ValueType::U64, low, Imm{16u});
                op = OpCode::ZeroExtend32To64;
                type = ValueType::U64;
            } else if (shape == 12) {
                const auto negative = block.Or(ValueType::U64, unknown,
                                                Operand{Imm{u64{1} << 63}});
                source = block.AsrImm(ValueType::U64, negative, Imm{63u});
                left = 0;
                expected = UINT32_MAX;
            } else {
                const auto marked = block.Or(ValueType::U64, unknown, Operand{Imm{0xff00u}});
                source = block.RorImm(ValueType::U64, marked, Imm{8u});
                left = 0;
                count = 8;
                expected = 0xff;
            }
            const auto result =
                    op == OpCode::SignExtend ? (typed ? builder.SignExtend(type, source)
                                                      : block.SignExtend(type, source))
                    : op == OpCode::ZeroExtend32To64
                            ? (typed ? builder.ZeroExtend32To64(type, source)
                                     : block.ZeroExtend32To64(type, source))
                            : (typed ? builder.BitExtract(type, source, Imm{left}, Imm{count})
                                     : block.BitExtract(type, source, Imm{left}, Imm{count}));
            auto options = FeatureSet{};
            options.int_imm_fold = true;
            compile::FoldValues(RewriteFixture{&block, false}.Get(), options);
            const bool constant = shape < 3 || shape >= 6;
            REQUIRE((result.Def()->GetOp() == OpCode::LoadImm) == constant);
            if (constant) REQUIRE(result.Def()->GetArg<Imm>(0).Get() == expected);
        }
    }
    // Compare literal transformations against a bit-by-bit semantic reference.
    // This covers truncation, signed fill and extracts crossing the input end.
    for (const auto input_type : {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64})
        for (const auto output_type :
             {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64})
            for (const auto op : {OpCode::ZeroExtend32,
                                  OpCode::ZeroExtend32To64,
                                  OpCode::ZeroExtend64,
                                  OpCode::SignExtend,
                                  OpCode::BitExtract})
                for (const auto literal : {u64{0}, u64{0x80ff8001}, UINT64_MAX}) {
                    Block block{0, Location{0x57a8}};
                    const auto value = block.LoadImm(input_type, Imm{literal});
                    const auto input_bits = GetValueSizeByte(input_type) * 8;
                    const auto output_bits = GetValueSizeByte(output_type) * 8;
                    const swift::u64 left = op == OpCode::BitExtract ? 7 : 0;
                    const swift::u64 count = 64;
                    swift::u64 expected = 0;
                    for (u32 bit = 0; bit < output_bits; ++bit) {
                        swift::u64 source_bit = bit;
                        bool copy = true;
                        if (op == OpCode::SignExtend)
                            source_bit = std::min<u32>(bit, input_bits - 1);
                        else if (op == OpCode::ZeroExtend32 || op == OpCode::ZeroExtend32To64)
                            copy = bit < 32;
                        else if (op == OpCode::BitExtract) {
                            source_bit += left;
                            copy = bit < count;
                        }
                        if (copy && source_bit < input_bits && ((literal >> source_bit) & 1))
                            expected |= swift::u64{1} << bit;
                    }
                    const auto folded = compile::FoldWidthValue(
                            op, output_type, value, compile::FlagsIntent::None(), left, count);
                    REQUIRE(folded);
                    REQUIRE(folded->Get() == expected);
                }
    // Independent finite-domain checks: every reported known bit and sign-copy
    // span must hold for all byte inputs, rather than mirroring the analyser.
    Block domain{0, Location{0x57b0}};
    const auto input = domain.GetHostGPR(ValueType::U8, Imm{u32{20}}, Imm{u32{0}});
    const auto masked = domain.And(ValueType::U8, input, Operand{Imm{u32{0xf0}}});
    const auto marked = domain.Or(ValueType::U8, masked, Operand{Imm{u32{0x80}}});
    const auto flipped = domain.Xor(ValueType::U8, marked, Operand{Imm{u32{0x3f}}});
    const auto signed_value = domain.SignExtend(ValueType::S32, flipped);
    const auto zero_value = domain.ZeroExtend32To64(ValueType::U64, flipped);
    const auto extracted = domain.BitExtract(ValueType::U8, signed_value, Imm{u32{8}}, Imm{u32{8}});
    const std::array facts_for_value{compile::AnalyzeIntegerValue(masked),
                                     compile::AnalyzeIntegerValue(marked),
                                     compile::AnalyzeIntegerValue(flipped),
                                     compile::AnalyzeIntegerValue(signed_value),
                                     compile::AnalyzeIntegerValue(zero_value),
                                     compile::AnalyzeIntegerValue(extracted)};
    for (unsigned byte = 0; byte <= UINT8_MAX; ++byte) {
        const swift::u64 m = byte & 0xf0, o = m | 0x80, x = o ^ 0x3f;
        const swift::u64 signed_bits = x & 0x80 ? (x | 0xffffff00u) : x;
        const std::array<swift::u64, 6> expected{
                m, o, x, signed_bits, x, (signed_bits >> 8) & 0xff};
        for (size_t i = 0; i < facts_for_value.size(); ++i) {
            const auto& facts = facts_for_value[i];
            const auto bits = facts.Width();
            const swift::u64 mask = bits == 64 ? UINT64_MAX : (swift::u64{1} << bits) - 1;
            REQUIRE((expected[i] & facts.KnownZero()) == 0);
            REQUIRE(((~expected[i]) & facts.KnownOne()) == 0);
            if (const auto constant = facts.Constant()) REQUIRE(*constant == expected[i]);
            if (const auto from = facts.SignExtendedFrom()) {
                const swift::u64 low_mask = (swift::u64{1} << from) - 1;
                const auto low = expected[i] & low_mask;
                const auto sign_extended =
                        low & (swift::u64{1} << (from - 1)) ? low | (mask & ~low_mask) : low;
                REQUIRE(sign_extended == expected[i]);
            }
        }
    }
    Value chain = domain.LoadImm(ValueType::U64, Imm{u64{7}});
    for (u32 i = 0; i <= compile::kIntegerValueVisitLimit; ++i)
        chain = domain.BitCast(ValueType::U64, chain);
    REQUIRE_FALSE(compile::AnalyzeIntegerValue(chain).Constant());
    REQUIRE(compile::AnalyzeIntegerValue(chain).Width() == 0);
    REQUIRE_FALSE(compile::FoldWidthValue(OpCode::BitExtract,
                                          ValueType::U32,
                                          chain.SetCastType(ValueType::U32),
                                          compile::FlagsIntent::None(),
                                          32,
                                          32));
    REQUIRE(compile::AnalyzeIntegerValue(Value{}).Width() == 0);
    REQUIRE_FALSE(compile::ApplyIntegerWidth(OpCode::BitExtract,
                                             ValueType::U64,
                                             compile::IntegerValueBits::Constant(64, UINT64_MAX),
                                             64,
                                             1));
}

TEST_CASE("integer arithmetic and shift facts hold over independent input domains",
          "[ir][analysis][width]") {
    const auto check = [](const compile::IntegerValueBits& facts, u64 expected) {
        REQUIRE(facts.Width() != 0);
        const auto mask = facts.Width() == 64 ? UINT64_MAX : (u64{1} << facts.Width()) - 1;
        expected &= mask;
        REQUIRE((expected & facts.KnownZero()) == 0);
        REQUIRE(((~expected) & facts.KnownOne()) == 0);
        if (const auto constant = facts.Constant()) REQUIRE(*constant == expected);
        if (const auto from = facts.SignExtendedFrom()) {
            const auto low_mask = (u64{1} << from) - 1;
            const auto low = expected & low_mask;
            const auto extended = low & (u64{1} << (from - 1)) ? low | (mask & ~low_mask) : low;
            REQUIRE(extended == expected);
        }
    };
    for (const auto type : {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64}) {
        Block block{0, Location{0x57c0}};
        const auto x = block.GetHostGPR(type, Imm{20u}, Imm{0u});
        const auto y = block.GetHostGPR(type, Imm{21u}, Imm{0u});
        const auto a = block.And(type, x, Operand{Imm{0xfu}});
        const auto b = block.And(type, y, Operand{Imm{0xfu}});
        const auto biased = block.Or(type, b, Operand{Imm{0x80u}});
        const std::array values{block.Add(type, a, Operand{b}),
                                block.Sub(type, a, Operand{b}),
                                block.AndNot(type, a, Operand{b}),
                                block.Add(type, a, Operand{biased}),
                                block.Sub(type, a, Operand{biased})};
        std::array<compile::IntegerValueBits, 5> facts{
                compile::AnalyzeIntegerValue(values[0]), compile::AnalyzeIntegerValue(values[1]),
                compile::AnalyzeIntegerValue(values[2]), compile::AnalyzeIntegerValue(values[3]),
                compile::AnalyzeIntegerValue(values[4])};
        const auto negated = compile::AnalyzeIntegerValue(block.Neg(type, a));
        REQUIRE((facts[0].KnownZero() & 0xe0) == 0xe0);
        for (u64 left = 0; left < 16; ++left) {
            check(negated, u64{0} - left);
            for (u64 right = 0; right < 16; ++right) {
                const std::array expected{left + right, left - right, left & ~right,
                                          left + (right | 0x80), left - (right | 0x80)};
                for (size_t i = 0; i < facts.size(); ++i) check(facts[i], expected[i]);
            }
        }
    }
    for (const auto type : {ValueType::U32, ValueType::U64}) {
        Block block{0, Location{0x57c8}};
        const u32 width = GetValueSizeByte(type) * 8;
        const auto x = block.GetHostGPR(type, Imm{20u}, Imm{0u});
        const auto y = block.GetHostGPR(type, Imm{21u}, Imm{0u});
        const auto a = block.And(type, x, Operand{Imm{0xfu}});
        const auto b = block.And(type, y, Operand{Imm{0xfu}});
        const auto high = block.LslImm(type, b, Imm{width - 4});
        const auto input = block.Or(type, a, Operand{high});
        for (const u32 amount : {0u, 1u, width / 2, width - 1}) {
            const std::array values{block.LslImm(type, input, Imm{amount}),
                                    block.LsrImm(type, input, Imm{amount}),
                                    block.AsrImm(type, input, Imm{amount}),
                                    block.RorImm(type, input, Imm{amount})};
            const std::array facts{
                    compile::AnalyzeIntegerValue(values[0]), compile::AnalyzeIntegerValue(values[1]),
                    compile::AnalyzeIntegerValue(values[2]), compile::AnalyzeIntegerValue(values[3])};
            for (u64 low = 0; low < 16; ++low)
                for (u64 high_bits = 0; high_bits < 16; ++high_bits) {
                    const u64 bits = low | (high_bits << (width - 4));
                    for (u32 op = 0; op < facts.size(); ++op) {
                        // Move individual bits, including signed fill. This
                        // reference never calls the analysed shift or adder.
                        u64 expected = 0;
                        for (u32 bit = 0; bit < width; ++bit) {
                            const u32 source = op == 0 ? (bit >= amount ? bit - amount : width)
                                               : op == 1 ? bit + amount
                                               : op == 2 ? std::min(bit + amount, width - 1)
                                                         : (bit + amount) % width;
                            if (source < width && ((bits >> source) & 1))
                                expected |= u64{1} << bit;
                        }
                        check(facts[op], expected);
                    }
                }
        }
        const auto wrapped = compile::AnalyzeIntegerValue(block.LsrImm(type, input, Imm{width}));
        const auto original = compile::AnalyzeIntegerValue(input);
        REQUIRE(wrapped.Width() == width);
        REQUIRE(wrapped.KnownZero() == original.KnownZero());
        REQUIRE(wrapped.KnownOne() == original.KnownOne());
        const auto narrow = Value{x}.SetCastType(ValueType::U8);
        const auto mixed = block.AsrImm(type, narrow, Imm{1u});
        REQUIRE_FALSE(compile::AnalyzeIntegerValue(mixed).Constant());
        REQUIRE(compile::AnalyzeIntegerValue(mixed).KnownZero() == 0);
        REQUIRE(compile::AnalyzeIntegerValue(mixed).KnownOne() == 0);
        REQUIRE(compile::AnalyzeIntegerValue(mixed).SignExtendedFrom() == 7);
    }
}

TEST_CASE("value feedback crosses arithmetic and immediate shift definitions",
          "[ir][rewrite][width]") {
    Block block{0, Location{0x57d0}};
    const auto source = block.GetHostGPR(ValueType::U32, Imm{20u}, Imm{0u});
    const auto difference = block.Sub(ValueType::U32, source, Operand{Imm{1u}});
    const auto shifted = block.LsrImm(ValueType::U32, difference, Imm{16u});
    const auto result = block.ZeroExtend32To64(ValueType::U64, shifted);
    block.StoreUniform(Uniform{0, ValueType::U64}, result);
    FeatureSet features{};
    features.int_imm_fold = true;
    compile::CompileWorkspace workspace;
    compile::RewriteContext rewrite{&block, workspace, nullptr, false};
    const compile::BuilderCapabilities target{};
    compile::ValueWorklist work{rewrite, features, target, compile::ValueCleanup::Values};
    work.Drain();
    REQUIRE(result.Def()->GetOp() == OpCode::ZeroExtend32To64);
    rewrite.Change(&block, source.Def(), [](Inst& writer) {
        writer.DestroyArgs();
        writer.SetInst(OpCode::LoadImm, Imm{1u});
        writer.SetReturn(ValueType::U32);
    });
    work.Drain();
    REQUIRE(result.Def()->GetOp() == OpCode::LoadImm);
    REQUIRE(result.Def()->GetArg<Imm>(0).Get() == 0);
    REQUIRE(result.Type() == ValueType::U64);

    Block variable{0, Location{0x57dc}};
    const auto data = variable.GetHostGPR(ValueType::U64, Imm{20u}, Imm{0u});
    const auto count = variable.GetHostGPR(ValueType::U64, Imm{21u}, Imm{0u});
    const auto shift = variable.LsrValue(ValueType::U64, Value{data}.SetCastType(ValueType::U8), count);
    const auto widened = variable.ZeroExtend64(shift);
    variable.StoreUniform(Uniform{0, ValueType::U64}, widened);
    compile::CompileWorkspace variable_workspace;
    compile::RewriteContext variable_rewrite{&variable, variable_workspace, nullptr, false};
    compile::ValueWorklist variable_work{variable_rewrite, features, target, compile::ValueCleanup::Values};
    variable_work.Drain();
    REQUIRE(widened.Def()->GetOp() == OpCode::ZeroExtend64);
    variable_rewrite.Change(&variable, count.Def(), [](Inst& writer) {
        writer.DestroyArgs();
        writer.SetInst(OpCode::LoadImm, Imm{u64{8}});
        writer.SetReturn(ValueType::U64);
    });
    variable_work.Drain();
    REQUIRE(widened.Def()->GetOp() == OpCode::LoadImm);
    REQUIRE(widened.Def()->GetArg<Imm>(0).Get() == 0);
}

TEST_CASE("integer shift facts normalize views and demand only count bits", "[ir][types][bits]") {
    static_assert(ClassifyIntegerShift(OpCode::LsrValue) == IntegerShiftKind::Right);
    static_assert(ClassifyIntegerShift(OpCode::VecShiftRight) == IntegerShiftKind::None);
    static_assert(IntegerShiftInputBits(64, 8) == 8);
    static_assert(IntegerShiftInputBits(0, 32) == 0);
    static_assert(NormalizeIntegerShiftCount(UINT64_MAX, 64) == 63);
    for (const auto source_type : {ValueType::U8, ValueType::S16, ValueType::U32, ValueType::S64})
    for (const auto result_type : {ValueType::S8, ValueType::U16, ValueType::S32, ValueType::U64})
    for (const auto op : {OpCode::LslImm, OpCode::LsrImm, OpCode::AsrImm, OpCode::RorImm,
                          OpCode::LslValue, OpCode::LsrValue, OpCode::AsrValue, OpCode::RorValue})
    for (const bool sign : {false, true}) {
        const u32 source_bits = GetValueSizeByte(source_type) * 8;
        const u32 bits = GetValueSizeByte(result_type) * 8;
        const u32 copied = std::min(source_bits, bits);
        const u64 pattern = sign ? u64{1} << (copied - 1)
                                 : source_bits > bits ? u64{1} << bits : 0;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        for (const u64 count : {u64{0}, u64{1}, u64{bits - 1}, u64{bits}, u64{bits + 1},
                                u64{0x100000001}, u64{UINT64_MAX}}) {
            CAPTURE(source_type, result_type, op, sign, count);
            Block block{0, Location{0x57e0}};
            const auto data = block.GetHostGPR(ValueType::U64, Imm{20u}, Imm{0u});
            const auto low = block.And(source_type, Value{data}.SetCastType(source_type), Operand{Imm{u64{15}}});
            const auto input = block.Or(source_type, low, Operand{Imm{pattern}});
            const bool immediate = op == OpCode::LslImm || op == OpCode::LsrImm ||
                                   op == OpCode::AsrImm || op == OpCode::RorImm;
            Value shifted;
            if (immediate) shifted = Value{block.AppendInst(op, result_type, input, Imm{count})};
            else {
                const auto unknown = block.GetHostGPR(ValueType::U64, Imm{21u}, Imm{0u});
                const auto high = block.And(ValueType::U64, unknown, Operand{Imm{~u64{bits - 1}}});
                const auto partial_count = block.Or(ValueType::U64, high, Operand{Imm{count}});
                shifted = Value{block.AppendInst(op, result_type, input, partial_count)};
            }
            const auto facts = compile::AnalyzeIntegerValue(shifted);
            u64 zeros = mask, ones = mask;
            for (u64 low_bits = 0; low_bits < 16; ++low_bits) {
                const u64 raw = pattern | low_bits;
                const u32 amount = count & (bits - 1);
                const bool arithmetic = op == OpCode::AsrImm || op == OpCode::AsrValue;
                u64 expected{};
                for (u32 bit = 0; bit < bits; ++bit) {
                    u32 source{};
                    if (op == OpCode::LslImm || op == OpCode::LslValue)
                        source = bit >= amount ? bit - amount : bits;
                    else if (op == OpCode::RorImm || op == OpCode::RorValue)
                        source = (bit + amount) % bits;
                    else source = arithmetic ? std::min(bit + amount, copied - 1) : bit + amount;
                    if (source < copied && ((raw >> source) & 1)) expected |= u64{1} << bit;
                }
                zeros &= ~expected;
                ones &= expected;
            }
            REQUIRE(facts.Width() == bits);
            REQUIRE(facts.KnownZero() == zeros);
            REQUIRE(facts.KnownOne() == ones);
        }
    }
    Block block{0, Location{0x57e8}};
    compile::IrBuilder builder{&block};
    FeatureSet features{};
    features.int_imm_fold = true;
    builder.ConfigureGuestState(nullptr, features, backend::arm64::BuilderTarget());
    const auto data = builder.LoadUniform(Uniform{0, ValueType::U64});
    const auto count = builder.LoadImm(Imm{u64{8}});
    const auto shifted = builder.LsrValue(ValueType::U64, Value{data}.SetCastType(ValueType::U8), count);
    const auto folded = builder.ZeroExtend64(ValueType::U64, compile::FlagsIntent::None(), shifted);
    REQUIRE(folded.Def()->GetOp() == OpCode::LoadImm);
    REQUIRE(folded.Def()->GetArg<Imm>(0).Get() == 0);
}

TEST_CASE("integer predicate facts and decode folds observe logical views", "[ir][types][bits]") {
    for (const auto source_type : {ValueType::U8, ValueType::S16, ValueType::U32, ValueType::S64})
    for (const auto result_type : {ValueType::S8, ValueType::U16, ValueType::S32, ValueType::U64})
    for (const auto op : {OpCode::TestZero, OpCode::TestNotZero, OpCode::Not, OpCode::TestBit})
    for (const bool alias : {false, true}) {
        const u32 source_bits = GetValueSizeByte(source_type) * 8;
        const u32 result_bits = GetValueSizeByte(result_type) * 8;
        const u64 source_mask = source_bits == 64 ? UINT64_MAX : (u64{1} << source_bits) - 1;
        const u64 result_mask = result_bits == 64 ? UINT64_MAX : (u64{1} << result_bits) - 1;
        const std::array<u64, 5> positions{0, 7, 15, 31, 63};
        for (const u64 pattern : {u64{0}, u64{0x100}, u64{1} << (source_bits - 1)})
        for (u32 variant = 0; variant < (op == OpCode::TestBit ? positions.size() : 1); ++variant) {
            CAPTURE(source_type, result_type, op, alias, pattern, variant);
            Block block{0, Location{0x57f0}};
            const auto raw = block.GetHostGPR(ValueType::U64, Imm{20u}, Imm{0u});
            const auto low = block.And(ValueType::U64, raw, Operand{Imm{u64{15}}});
            const auto mixed = block.Or(ValueType::U64, low, Operand{Imm{pattern}});
            const auto input = alias ? block.BitCast(source_type, mixed)
                                     : Value{mixed}.SetCastType(source_type);
            const auto result = op == OpCode::TestBit
                    ? Value{block.AppendInst(op, result_type, input, Imm{positions[variant]})}
                    : Value{block.AppendInst(op, result_type, input)};
            const auto facts = compile::AnalyzeIntegerValue(result);
            u64 zeros = result_mask, ones = result_mask;
            for (u64 nibble = 0; nibble < 16; ++nibble) {
                const u64 value = (pattern | nibble) & source_mask;
                const u64 expected = op == OpCode::TestBit ? (value >> positions[variant]) & 1
                        : op == OpCode::TestNotZero ? value != 0 : value == 0;
                zeros &= ~expected;
                ones &= expected;
            }
            REQUIRE(facts.Width() == result_bits);
            REQUIRE(facts.KnownZero() == zeros);
            REQUIRE(facts.KnownOne() == ones);
            REQUIRE(result.Def()->HasBooleanResult());
        }
        for (const u64 raw : {u64{0}, u64{0x100}, u64{1}, u64{0x100000000}, UINT64_MAX})
        for (u32 variant = 0; variant < (op == OpCode::TestBit ? positions.size() : 1); ++variant) {
            Block block{0, Location{0x57f4}};
            const auto literal = block.LoadImm(Imm{raw});
            const auto input = alias ? block.BitCast(source_type, literal)
                                     : Value{literal}.SetCastType(source_type);
            Inst inst{op, result_type};
            if (op == OpCode::TestBit) inst.SetArgs(input, Imm{positions[variant]});
            else inst.SetArgs(input);
            const u64 value = raw & source_mask;
            const u64 expected = op == OpCode::TestBit ? (value >> positions[variant]) & 1
                    : op == OpCode::TestNotZero ? value != 0 : value == 0;
            const auto folded = compile::FoldConditionalValue(inst, compile::FlagsIntent::None());
            REQUIRE(folded);
            REQUIRE(folded->Get() == expected);
            REQUIRE_FALSE(compile::FoldConditionalValue(inst, compile::FlagsIntent::Unknown()));
        }
    }
    Block block{0, Location{0x57f8}};
    compile::IrBuilder builder{&block};
    const auto raw = builder.LoadImm(Imm{u64{0x100}});
    const auto byte = Value{raw}.SetCastType(ValueType::U8);
    const auto zero = builder.TestZero(byte);
    const auto bit = builder.TestBit(byte, Imm{u64{8}});
    const auto selected = builder.Select(ValueType::U64, zero, raw, builder.LoadImm(Imm{u64{5}}));
    REQUIRE(zero.Def()->GetOp() == OpCode::LoadImm);
    REQUIRE(zero.Def()->GetArg<Imm>(0).Get() == 1);
    REQUIRE(bit.Def()->GetOp() == OpCode::LoadImm);
    REQUIRE(bit.Def()->GetArg<Imm>(0).Get() == 0);
    REQUIRE(selected.Def()->GetOp() == OpCode::LoadImm);
    REQUIRE(selected.Def()->GetArg<Imm>(0).Get() == 0x100);
}

TEST_CASE("conditional value worklist follows predicate and selector edits", "[ir][rewrite][bits]") {
    for (const bool function_mode : {false, true}) {
        HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode ? hir.AppendFunction(Location{0x5800}, Location{0x5801}) : nullptr;
        IntrusivePtr<Block> block;
        if (!function) block = new Block{0, Location{0x5800}};
        compile::IrBuilder factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{block.get()};
        const auto raw = factory.LoadUniform(Uniform{0, ValueType::U64});
        const auto condition = Value{factory.AppendInst(OpCode::TestZero, ValueType::U64,
                                                       Value{raw}.SetCastType(ValueType::U8))};
        const auto yes = factory.LoadImm(Imm{u64{0xbeef}});
        const auto no = factory.LoadImm(Imm{u64{0xfeed}});
        const auto selected = Value{factory.AppendInst(OpCode::Select, ValueType::U64,
                                                       condition, yes, no)};
        factory.StoreUniform(Uniform{8, ValueType::U64}, condition);
        factory.StoreUniform(Uniform{16, ValueType::U64}, selected);
        auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
        factory.ReturnToHost();
        if (function) { function->ComputeRPO(); function->IdByRPO(); }
        else block->ReIdInstr();
        compile::CompileWorkspace workspace;
        compile::RewriteContext rewrite{body, workspace, function, false};
        FeatureSet features{};
        features.int_imm_fold = true;
        std::optional<compile::ValueWorklist> work;
        compile::BeginValueCleanup(work, rewrite, features, {}, compile::ValueCleanup::Values);
        REQUIRE(work);
        work->Drain();
        REQUIRE(condition.Def()->GetOp() == OpCode::TestZero);
        REQUIRE(selected.Def()->GetOp() == OpCode::Select);
        rewrite.Change(body, raw.Def(), [](Inst& writer) {
            writer.DestroyArgs();
            writer.SetInst(OpCode::LoadImm, Imm{u64{0x100}});
            writer.SetReturn(ValueType::U64);
        });
        work->Drain();
        REQUIRE(condition.Def()->GetOp() == OpCode::LoadImm);
        REQUIRE(condition.Def()->GetArg<Imm>(0).Get() == 1);
        REQUIRE(selected.Def()->GetOp() == OpCode::LoadImm);
        REQUIRE(selected.Def()->GetArg<Imm>(0).Get() == 0xbeef);
    }
}

TEST_CASE("integer predicate and condition domains reject before cleanup", "[ir][types][admission]") {
    for (const bool function_mode : {false, true})
    for (const auto op : {OpCode::TestZero, OpCode::TestNotZero, OpCode::Not, OpCode::TestBit,
                          OpCode::Select, OpCode::SelectZero, OpCode::Goto, OpCode::NotGoto}) {
        const bool predicate = op == OpCode::TestZero || op == OpCode::TestNotZero ||
                               op == OpCode::Not || op == OpCode::TestBit;
        for (u32 role = 0; role < (op == OpCode::TestBit ? 6u : predicate ? 2u : 1u); ++role) {
            CAPTURE(function_mode, op, role);
            HIRBuilder hir{1, false, FeatureSet{}};
            auto* function = function_mode ? hir.AppendFunction(Location{0x5810}, Location{0x5811}) : nullptr;
            IntrusivePtr<Block> block;
            if (!function) block = new Block{0, Location{0x5810}};
            compile::IrBuilder factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{block.get()};
            // Keep the predicate dynamic until malformed IR is injected.
            const auto raw = factory.LoadUniform(Uniform{0, ValueType::U64});
            const auto* invalid = op == OpCode::TestBit
                    ? factory.AppendInst(op, ValueType::U32, raw, Imm{u64{0}})
                    : op == OpCode::Select || op == OpCode::SelectZero
                    ? factory.AppendInst(op, ValueType::U32, raw, raw, raw)
                    : factory.AppendInst(op, ValueType::U32, raw);
            if (op == OpCode::Goto || op == OpCode::NotGoto) factory.BindLabel(Value{invalid});
            auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
            factory.ReturnToHost();
            if (function) { function->ComputeRPO(); function->IdByRPO(); }
            else block->ReIdInstr();
            compile::CompileWorkspace workspace;
            compile::RewriteContext rewrite{body, workspace};
            if (role == 0) rewrite.SetArg(body, invalid, 0, Value{raw}.SetCastType(ValueType::V64));
            else if (role == 1)
                rewrite.Change(body, invalid, [](Inst& writer) { writer.SetReturn(ValueType::V64); });
            else rewrite.SetArg(body, invalid, 1, role == 2 ? Imm{u64{0}, ValueType::V32}
                    : Imm{role == 3 ? u64{64} : role == 4 ? u64{0x100000001} : UINT64_MAX});
            REQUIRE_FALSE(invalid->HasValidArguments());
            REQUIRE_FALSE(compile::FoldConditionalValue(*invalid, compile::FlagsIntent::None()));
            REQUIRE_FALSE(backend::arm64::SupportsIR(body));
            compile::SemanticFacts facts{body};
            REQUIRE_FALSE(facts.PrepareSSAValidation());
            REQUIRE(facts.FindFunctionCFG() == nullptr);
            alignas(backend::State) std::array<u8, sizeof(backend::State) + 64> storage{};
            auto* state = new (storage.data()) backend::State{};
            backend::interp::Interpreter interpreter{*state, body};
            REQUIRE(interpreter.Run() == HaltReason::IllegalCode);
            Config config{.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
                          .backend_isa = kArm64, .uniform_buffer_size = 64};
            config.global_opts = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove;
            config.stack_alignment = 16;
            backend::AddressSpace space{config};
            const auto prepared = function ? backend::arm64::PrepareFunction(space.GetDefaultModule(), function)
                                           : backend::arm64::PrepareBlock(space.GetDefaultModule(), block);
            REQUIRE_FALSE(prepared);
            REQUIRE(space.GetCodeCache(Location{0x5810}) == nullptr);
        }
    }
}

TEST_CASE("integer shifts reject noninteger domains before optimization", "[ir][types][admission]") {
    for (const bool function_mode : {false, true})
    for (const auto op : {OpCode::LslImm, OpCode::LsrImm, OpCode::AsrImm, OpCode::RorImm,
                          OpCode::LslValue, OpCode::LsrValue, OpCode::AsrValue, OpCode::RorValue})
    for (const u32 invalid_role : {0u, 1u, 2u}) {
        CAPTURE(function_mode, op, invalid_role);
        HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode ? hir.AppendFunction(Location{0x7bc0}, Location{0x7bc1}) : nullptr;
        IntrusivePtr<Block> block;
        if (!function) block = new Block{0, Location{0x7bc0}};
        compile::IrBuilder factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{block.get()};
        const auto raw = factory.LoadImm(Imm{u64{0x180}});
        const auto input = Value{raw}.SetCastType(ValueType::U32);
        const bool immediate = op == OpCode::LslImm || op == OpCode::LsrImm ||
                               op == OpCode::AsrImm || op == OpCode::RorImm;
        const auto* invalid = immediate
                ? factory.AppendInst(op, ValueType::U32, input, Imm{u64{1}})
                : factory.AppendInst(op, ValueType::U32, input, raw);
        auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
        factory.ReturnToHost();
        if (function) { function->ComputeRPO(); function->IdByRPO(); }
        else body->ReIdInstr();
        // Checked construction rejects invalid domains immediately. Inject
        // imported/rewritten IR through the mutable-phase API instead.
        compile::CompileWorkspace workspace;
        compile::RewriteContext rewrite{body, workspace};
        if (invalid_role == 0)
            rewrite.SetArg(body, invalid, 0, Value{raw}.SetCastType(ValueType::V64));
        else if (invalid_role == 1)
            rewrite.Change(body, invalid, [](Inst& writer) { writer.SetReturn(ValueType::V64); });
        else if (immediate)
            rewrite.SetArg(body, invalid, 1, Imm{u64{1}, ValueType::V32});
        else rewrite.SetArg(body, invalid, 1, Value{raw}.SetCastType(ValueType::V32));
        REQUIRE_FALSE(invalid->HasValidArguments());
        REQUIRE_FALSE(backend::arm64::SupportsIR(body));
        compile::SemanticFacts facts{body};
        REQUIRE_FALSE(facts.PrepareSSAValidation());
        REQUIRE(facts.FindFunctionCFG() == nullptr);
        alignas(backend::State) std::array<u8, sizeof(backend::State) + 64> storage{};
        auto* state = new (storage.data()) backend::State{};
        backend::interp::Interpreter interpreter{*state, body};
        REQUIRE(interpreter.Run() == HaltReason::IllegalCode);
        Config config{.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
                      .backend_isa = kArm64, .uniform_buffer_size = 64};
        config.global_opts = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove;
        config.stack_alignment = 16;
        backend::AddressSpace space{config};
        const auto prepared = function ? backend::arm64::PrepareFunction(space.GetDefaultModule(), function)
                                       : backend::arm64::PrepareBlock(space.GetDefaultModule(), block);
        REQUIRE_FALSE(prepared);
        REQUIRE(space.GetCodeCache(Location{0x7bc0}) == nullptr);
    }
}

TEST_CASE("scalar ALU domains reject before dead instructions can be erased",
          "[ir][types][admission][scalar-alu]") {
    constexpr std::array operations{OpCode::Add, OpCode::Sub, OpCode::Adc, OpCode::Sbb,
                                    OpCode::Mul, OpCode::Div, OpCode::And, OpCode::Or,
                                    OpCode::Xor, OpCode::AndNot, OpCode::Not, OpCode::Neg,
                                    OpCode::MulSub, OpCode::SignedDiv64, OpCode::MulHigh,
                                    OpCode::Div128};
    const auto operand_based = [](OpCode op) {
        return op != OpCode::Neg && op != OpCode::MulSub && op != OpCode::SignedDiv64 &&
               op != OpCode::MulHigh && op != OpCode::Div128;
    };
    const auto append = [](compile::IrBuilder& factory, OpCode op, ValueType type, Value input,
                           u32 form = 0) {
        switch (op) {
            case OpCode::Neg: return factory.AppendInst(op, type, input);
            case OpCode::MulSub: return factory.AppendInst(op, type, input, input, input);
            case OpCode::SignedDiv64: return factory.AppendInst(op, type, input, input);
            case OpCode::MulHigh: return factory.AppendInst(op, type, input, input, Imm{u64{1}});
            case OpCode::Div128:
                return factory.AppendInst(op, type, input, input, input, Imm{u64{1}});
            default: {
                const auto operand = form == 1 ? Operand{input}
                        : form == 2 ? Operand{Imm{u64{1}}}
                        : form == 3 ? Operand{input, input}
                        : form == 4 ? Operand{DataClass{Imm{u64{1}}}, DataClass{input}}
                                    : Operand{input, Imm{u64{1}}};
                return factory.AppendInst(op, type, input, operand);
            }
        }
    };
    // Keep the existing signed/unsigned integer forms. The explicitly 64-bit
    // operations use 64-bit examples; this is not a new width policy.
    {
        Block block{0, Location{0x7bd0}};
        compile::IrBuilder factory{&block};
        const auto raw = factory.LoadUniform(Uniform{0, ValueType::U64});
        constexpr std::array types{ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64,
                                   ValueType::S8, ValueType::S16, ValueType::S32, ValueType::S64};
        for (const auto op : operations)
            for (const auto type : types) {
                if ((op == OpCode::SignedDiv64 || op == OpCode::MulHigh || op == OpCode::Div128) &&
                    type != ValueType::U64 && type != ValueType::S64) continue;
                for (u32 form = 0; form < (operand_based(op) ? 5u : 1u); ++form) {
                    CAPTURE(op, type, form);
                    REQUIRE(append(factory, op, type, Value{raw}.SetCastType(type), form)
                                    ->HasValidArguments());
                }
            }
    }
    for (const bool function_mode : {false, true})
        for (const auto op : operations)
            for (const auto bad_type : {ValueType::V64, ValueType::V128})
            for (const u32 form : {0u, 4u}) {
                if (form && !operand_based(op)) continue;
                for (int slot = -1; slot < Inst::max_args; ++slot) {
                    CAPTURE(function_mode, op, bad_type, slot, form);
                    HIRBuilder hir{1, false, FeatureSet{}};
                    auto* function = function_mode
                            ? hir.AppendFunction(Location{0x7be0}, Location{0x7be1}) : nullptr;
                    IntrusivePtr<Block> block;
                    if (!function) block = new Block{0, Location{0x7be0}};
                    compile::IrBuilder factory = function ? compile::IrBuilder{&hir}
                                                          : compile::IrBuilder{block.get()};
                    const auto raw = factory.LoadUniform(Uniform{0, ValueType::U64});
                    const auto* invalid = append(factory, op, ValueType::U64, raw, form);
                    auto* body = function
                            ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
                    factory.ReturnToHost();
                    if (slot >= 0 && !invalid->ArgAt(slot).IsValue() &&
                        !invalid->ArgAt(slot).IsImm()) continue;
                    {
                        compile::CompileWorkspace workspace;
                        compile::RewriteContext rewrite{body, workspace};
                        if (slot < 0)
                            rewrite.Change(body, invalid,
                                           [&](Inst& inst) { inst.SetReturn(bad_type); });
                        else if (invalid->ArgAt(slot).IsValue())
                            rewrite.SetArg(body, invalid, slot,
                                           Value{raw}.SetCastType(bad_type));
                        else rewrite.SetArg(body, invalid, slot, Imm{u64{1}, bad_type});
                    }
                    if (function) { function->ComputeRPO(); function->IdByRPO(); }
                    else body->ReIdInstr();
                    Config config{.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
                                  .backend_isa = kArm64, .uniform_buffer_size = 64};
                    config.global_opts = Optimizations::ConstantFolding |
                                         Optimizations::DeadCodeRemove;
                    config.stack_alignment = 16;
                    backend::AddressSpace space{config};
                    const auto node_count = body->GetInstList().size();
                    const auto invalid_address = reinterpret_cast<uintptr_t>(invalid);
                    // This result is deliberately unused. Check the real
                    // entry before inspecting the node: an incorrect entry
                    // can optimize it away, making its old pointer stale.
                    const auto prepared = function
                            ? backend::arm64::PrepareFunction(space.GetDefaultModule(), function)
                            : backend::arm64::PrepareBlock(space.GetDefaultModule(), block);
                    REQUIRE_FALSE(prepared);
                    REQUIRE(space.GetCodeCache(Location{0x7be0}) == nullptr);
                    REQUIRE(std::any_of(body->GetInstList().begin(), body->GetInstList().end(),
                                        [&](const Inst& inst) {
                                            return reinterpret_cast<uintptr_t>(&inst) == invalid_address;
                                        }));
                    REQUIRE(body->GetInstList().size() == node_count);
                    REQUIRE(invalid->GetOp() == op);
                    REQUIRE_FALSE(invalid->HasValidArguments());
                    REQUIRE_FALSE(backend::arm64::SupportsIR(body));
                    compile::SemanticFacts facts{body};
                    REQUIRE_FALSE(facts.PrepareSSAValidation());
                    alignas(backend::State) std::array<u8, sizeof(backend::State) + 64> storage{};
                    auto* state = new (storage.data()) backend::State{};
                    backend::interp::Interpreter interpreter{*state, body};
                    REQUIRE(interpreter.Run() == HaltReason::IllegalCode);
                }
            }
}

TEST_CASE("guest commit folds typed values at their original positions and retains flag outputs",
          "[ir][builder][commit]") {
    Block block{0, Location{0x6400}};
    compile::IrBuilder builder{&block};
    FeatureSet features{};
    features.int_imm_fold = true;
    builder.ConfigureGuestState(nullptr, features, backend::arm64::BuilderTarget());
    Value byte, extended, folded, flagged, unknown, distant;
    {
        compile::IrBuilder::GuestInstructionScope instruction{builder};
        byte = builder.LoadImm(ValueType::U8, Imm{0x80u});
        extended = builder.SignExtend(ValueType::S64, byte);
        REQUIRE(extended.Def()->GetOp() == OpCode::SignExtend);
        const auto input = builder.LoadUniform(Uniform{0, ValueType::U16});
        const auto immediate = builder.LoadImm(ValueType::U16, Imm{1u});
        folded = builder.Add(ValueType::U16, compile::FlagsIntent::None(), input, Operand{immediate});
        flagged = builder.Sub(ValueType::U16, compile::FlagsIntent::Declared(Flags::NZCV),
                              input, Operand{immediate});
        builder.SaveFlags(flagged, Flags::NZCV);
        unknown = builder.Add(ValueType::U16, input, Operand{immediate});
        for (u32 offset = 0; offset < 6; ++offset) {
            builder.StoreUniform(Uniform{8 + offset * 8, ValueType::U64}, extended);
        }
        distant = builder.Add(ValueType::U16, compile::FlagsIntent::None(), input, Operand{immediate});
        REQUIRE(folded.Def()->GetArg<Operand>(1).GetLeft().IsValue());
        // Future constants must not evict sources of earlier deferred queries.
        for (u32 i = 0; i < 32; ++i) builder.LoadImm(ValueType::U64, Imm{u64{i}});
    }
    REQUIRE(extended.Def()->GetOp() == OpCode::LoadImm);
    REQUIRE(extended.Def()->GetArg<Imm>(0).Get() == 0xffffffffffffff80);
    REQUIRE(folded.Def()->GetArg<Operand>(1).GetLeft().IsImm());
    REQUIRE(flagged.Def()->GetArg<Operand>(1).GetLeft().IsValue());
    REQUIRE(unknown.Def()->GetArg<Operand>(1).GetLeft().IsValue());
    REQUIRE(distant.Def()->GetArg<Operand>(1).GetLeft().IsValue());
    REQUIRE(byte.Type() == ValueType::U8);
}

TEST_CASE("fixed results are typed before Block and HIR argument registration",
          "[ir][builder][types]") {
    for (const auto invalid : {OpCode::COUNT, static_cast<OpCode>(0xff)}) {
        REQUIRE_THROWS_AS(GetIRMetaInfo(invalid), std::logic_error);
        REQUIRE_THROWS_AS(Inst{invalid}, std::logic_error);
    }
    for (unsigned mode = 0; mode != 6; ++mode) {
        for (bool fast : {false, true}) {
            CAPTURE(mode, fast);
            Block block{0, Location{0x6d00}};
            HIRBuilder hir{1, false, fast, FeatureSet{}};
            auto* function = mode && mode != 4
                                     ? hir.AppendFunction(Location{0x6d00}, Location{0x6e00})
                                     : nullptr;
            const auto check = [&](auto& factory) {
                const auto byte = factory.LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
                const auto wide = factory.LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
                const std::array<Value, 8> results{
                        factory.ZeroExtend32(byte), factory.ZeroExtend64(byte),
                        factory.ZeroExtend32To64(wide), factory.TestZero(wide),
                        factory.TestNotZero(wide), factory.TestBit(wide, Imm{63u}),
                        factory.TestFlags(Flags::Carry), factory.CondSet(Cond::CS)};
                const std::array expected{ValueType::U32, ValueType::U64, ValueType::U64,
                                          ValueType::U8, ValueType::U8, ValueType::U8,
                                          ValueType::U8, ValueType::U8};
                for (u32 i = 0; i < results.size(); ++i) {
                    REQUIRE(results[i].Type() == expected[i]);
                    REQUIRE(results[i].Def()->ReturnType() == expected[i]);
                    factory.StoreUniform(Uniform{16 + 8 * i, expected[i]}, results[i]);
                    if (function) {
                        const auto* recorded = function->GetHIRValue(results[i]);
                        REQUIRE(recorded);
                        REQUIRE(recorded->value.Type() == expected[i]);
                        REQUIRE(recorded->GetUses().size() == 1);
                    }
                }
                REQUIRE(byte.Def()->ReturnType() == ValueType::U8);
                REQUIRE(wide.Def()->ReturnType() == ValueType::U64);
            };
            if (mode == 0) check(block);
            else if (mode == 1) check(function->EditBlock(function->GetCurrentBlock()));
            else if (mode == 2) check(*function);
            else if (mode == 3) check(hir);
            else {
                auto factory = mode == 4 ? compile::IrBuilder{&block} : compile::IrBuilder{&hir};
                check(factory);
                const auto constant = factory.LoadImm(ValueType::U8, Imm{u8{0x80}});
                const auto widened = factory.ZeroExtend64(constant);
                REQUIRE(widened.Type() == ValueType::U64);
                REQUIRE(widened.Def()->ReturnType() == ValueType::U64);
                REQUIRE(widened.Def()->GetOp() == OpCode::LoadImm);
                REQUIRE(widened.Def()->GetArg<Imm>(0).Get() == 0x80);
                REQUIRE(constant.Def()->ReturnType() == ValueType::U8);
            }
            if (function) hir.ReturnToHost();
        }
    }
    Block source{0, Location{0x6e00}};
    const auto input = source.LoadImm(ValueType::U64, Imm{u64{0x12345678abcdef01}});
    for (bool fresh : {false, true}) {
        Inst extension{OpCode::ZeroExtend32};
        if (fresh) extension.SetArgsFresh(input);
        else extension.SetArgs(input);
        REQUIRE(extension.ReturnType() == ValueType::U32);
        extension.DestroyArgs();
    }
    Inst rewritten{OpCode::Void};
    rewritten.ZeroExtend32(input);
    REQUIRE(rewritten.ReturnType() == ValueType::U32);
    rewritten.DestroyArgs();
    // Low-level rewriting preserves its caller's explicit result contract.
    rewritten.SetReturn(ValueType::U64);
    rewritten.ZeroExtend64(input);
    REQUIRE(rewritten.ReturnType() == ValueType::U64);
    rewritten.DestroyArgs();
}

namespace {
template<class Factory> concept CompleteArithmeticArguments = requires(Factory& factory, Value value) {
    factory.Add(value, Operand{value});
};
template<class Factory> concept AcceptsValueAsOperand = requires(Factory& factory, Value value) {
    factory.Add(value, value);
};
template<class Factory> concept AcceptsOperandHeader = requires(Factory& factory, Value value) {
    factory.Add(value, OperandPlus);
};
template<class Factory> concept AcceptsParamsAsCallData = requires(Factory& factory, Params params) {
    factory.CallLambda(ValueType::VOID, Lambda{Imm{u64{1}}}, std::move(params));
};
template<class Factory, class List>
concept ConsumesCallParams = requires(Factory& factory, List&& params) {
    factory.CallDynamic(ValueType::VOID, Lambda{Imm{u64{1}}}, std::forward<List>(params));
};
template<class Factory, class List>
concept AppendsCallParams = requires(Factory& factory, List&& params) {
    factory.AppendInst(OpCode::CallDynamic, ValueType::VOID, Lambda{Imm{u64{1}}},
                       std::forward<List>(params));
};
template<class Instruction, class List>
concept SetsParams = requires(Instruction& instruction, List&& params) {
    instruction.SetArg(1, std::forward<List>(params));
};
template<class Owner, class Data>
concept PushesParam = requires(Owner& owner, const Data& data) { owner.Push(data); };
template<class Factory> concept AcceptsMissingCallParams = requires(Factory& factory) {
    factory.CallDynamic(ValueType::VOID, Lambda{Imm{u64{1}}});
};
template<class Factory> concept AcceptsOverflowingAppend = requires(Factory& factory, Operand operand) {
    factory.AppendInst(OpCode::Add, ValueType::U64, operand, operand);
};
template<class Instruction> concept AcceptsOverflowingSet = requires(Instruction& inst, Operand operand) {
    inst.SetArgs(operand, operand);
};
template<class Instruction> concept AcceptsPhysicalHeaderSet = requires(Instruction& inst) {
    inst.SetArgs(OperandPlus);
};
template<class Factory> concept AcceptsMismatchedOperation = requires(Factory& factory, Value value) {
    factory.AppendInst(meta::KnownOperation<OpCode::Add, Value, Operand>(),
                       ValueType::U64, value, value);
};
template<class Instruction> concept AcceptsMismatchedSet = requires(Instruction& inst, Value value) {
    inst.SetInst(meta::KnownOperation<OpCode::Add, Value, Operand>(),
                 ValueType::U64, value, value);
};
template<OpCode op, class... Args> concept HasKnownOperation = requires {
    meta::KnownOperation<op, Args...>();
};
template<class Factory> constexpr bool CheckConstructionArguments() {
    static_assert(CompleteArithmeticArguments<Factory>);
    static_assert(!AcceptsValueAsOperand<Factory>);
    static_assert(!AcceptsOperandHeader<Factory>);
    static_assert(!AcceptsParamsAsCallData<Factory>);
    static_assert(ConsumesCallParams<Factory, Params> && AppendsCallParams<Factory, Params>);
    static_assert(!ConsumesCallParams<Factory, Params&> && !AppendsCallParams<Factory, Params&>);
    static_assert(!ConsumesCallParams<Factory, const Params> &&
                  !AppendsCallParams<Factory, const Params>);
    static_assert(!ConsumesCallParams<Factory, Params::View> &&
                  !AppendsCallParams<Factory, Params::View>);
    static_assert(!AcceptsMissingCallParams<Factory>);
    static_assert(!AcceptsOverflowingAppend<Factory>);
    static_assert(!AcceptsMismatchedOperation<Factory>);
    return true;
}
static_assert(CheckConstructionArguments<Block>());
static_assert(CheckConstructionArguments<HIRBlock>());
static_assert(CheckConstructionArguments<HIRFunction>());
static_assert(CheckConstructionArguments<HIRBuilder>());
static_assert(CheckConstructionArguments<compile::IrBuilder>());
static_assert(CompleteArithmeticArguments<Inst> && !AcceptsValueAsOperand<Inst>);
static_assert(!AcceptsOperandHeader<Inst> && !AcceptsOverflowingSet<Inst>);
static_assert(!AcceptsPhysicalHeaderSet<Inst>);
static_assert(!AcceptsMismatchedSet<Inst>);
static_assert(SetsParams<Inst, Params> && !SetsParams<Inst, Params&>);
static_assert(!SetsParams<Inst, const Params> && !SetsParams<Inst, Params::View>);
static_assert(PushesParam<Params, Value> && PushesParam<Params, Imm>);
static_assert(!PushesParam<Params, Uniform> && !PushesParam<Params, Params::Param>);
using ImmediateOperation = meta::OperationFor<Imm>;
static_assert(sizeof(ImmediateOperation) == sizeof(OpCode));
static_assert(std::is_trivially_copyable_v<ImmediateOperation>);
static_assert(!std::is_default_constructible_v<ImmediateOperation>);
static_assert(!std::is_constructible_v<ImmediateOperation, OpCode>);
static_assert(!std::is_convertible_v<ImmediateOperation, OpCode>);
static_assert(std::is_same_v<meta::OperationFor<Value>, meta::OperationFor<U64>>);
static_assert(meta::KnownOperation<OpCode::LoadImm, Imm>().Code() == OpCode::LoadImm);
static_assert(meta::KnownOperation<OpCode::Nop>().Code() == OpCode::Nop);
static_assert(!HasKnownOperation<OpCode::Void> && !HasKnownOperation<OpCode::BASE_COUNT>);
static_assert(!HasKnownOperation<OpCode::LoadImm, Value>);
static_assert(!HasKnownOperation<OpCode::Add, Operand, Operand>);
static_assert(meta::ArgumentSlots<Operand, Value> == 4);
static_assert(meta::ArgumentSlots<Operand, Operand> == 6);
static_assert(meta::ValidArgumentPack<Value, Value, Value, Value, Value>(meta::kArgs_CompareAndSwap128));
static_assert(meta::ValidArgumentPack<Lambda, Imm, U8, Imm>(meta::kArgs_CallLambda));
}  // namespace

TEST_CASE("construction types precede HIR records and operand mutation",
          "[ir][builder][types]") {
    for (const bool fast : {false, true}) {
        for (unsigned mode = 0; mode < 6; ++mode) {
            CAPTURE(fast, mode);
            Block block{0, Location{0x6f00}};
            HIRBuilder hir{1, false, fast, FeatureSet{}};
            auto* function = mode != 0 && mode != 4
                    ? hir.AppendFunction(Location{0x6f00}, Location{0x6f01}) : nullptr;
            auto* current = function
                    ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : &block;
            const auto check = [&](auto& factory) {
                const auto byte = factory.LoadUniform(Uniform{0, ValueType::U8});
                const auto wide = factory.LoadImm(Imm{u64{0xfedcba9876543210}});
                const auto immediate = factory.LoadImm(Imm{u16{0x1234}});
                const Local local{0, ValueType::S32};
                factory.DefineLocal(local);
                const auto local_value = factory.LoadLocal(local);
                const auto sum = factory.Add(Value{wide}.SetCastType(ValueType::U16), Operand{byte});
                const std::array<Value, 5> results{byte, wide, immediate, local_value, sum};
                const std::array expected{ValueType::U8, ValueType::U64, ValueType::U16,
                                          ValueType::S32, ValueType::U16};
                for (u32 i = 0; i < results.size(); ++i) {
                    REQUIRE(results[i].Type() == expected[i]);
                    REQUIRE(results[i].Def()->ReturnType() == expected[i]);
                    if (function) {
                        const auto* record = function->GetHIRValue(results[i]);
                        REQUIRE(record);
                        REQUIRE(record->value.Type() == expected[i]);
                    }
                }
                // Missing widths fail before allocating/appending an instruction
                // or advancing HIR's dense instruction/value tables.
                const auto count = current->GetInstList().size();
                const auto uses = wide.Def()->GetUses(false);
                const auto instructions = function ? function->MaxInstrCount() : 0;
                const auto records = function ? function->GetHIRValues().size() : 0;
                REQUIRE_THROWS_AS(factory.AppendInst(OpCode::LoadMemory, Operand{Imm{u64{0}}}),
                                  std::logic_error);
                REQUIRE_THROWS_AS(factory.AppendInst(OpCode::CallLambda, Lambda{Imm{u64{1}}}, wide),
                                  std::logic_error);
                REQUIRE(current->GetInstList().size() == count);
                const auto rejected = [&](auto&& append) {
                    REQUIRE_THROWS_AS(append(), std::logic_error);
                    REQUIRE(current->GetInstList().size() == count);
                    REQUIRE(wide.Def()->GetUses(false) == uses);
                    if (function) {
                        REQUIRE(function->MaxInstrCount() == instructions);
                        REQUIRE(function->GetHIRValues().size() == records);
                    }
                };
                rejected([&] { factory.AppendInst(OpCode::LoadUniform, ValueType::U64, Imm{u64{7}}); });
                rejected([&] { factory.AppendInst(OpCode::LoadImm, ValueType::U64, wide); });
                rejected([&] { factory.AppendInst(OpCode::Add, ValueType::U64, wide, wide); });
                rejected([&] { factory.AppendInst(OpCode::CallDynamic, ValueType::VOID,
                                                  Lambda{Imm{u64{1}}}); });
                rejected([&] { factory.AppendInst(OpCode::CallLambda, ValueType::VOID,
                                                  Lambda{Imm{u64{1}}}, Params{}); });
                rejected([&] { factory.AppendInst(OpCode::Nop, ValueType::VOID, wide); });
                factory.template CallLambda<VOID>(Lambda{Imm{u64{1}}}, wide);
                const auto& call = current->GetInstList().back();
                REQUIRE(call.ReturnType() == ValueType::VOID);
                REQUIRE(call.GetArg<Value>(1).Def() == wide.Def());
                if (function) REQUIRE(function->GetHIRValue(Value{&call}) == nullptr);
                const auto after = factory.LoadImm(Imm{u32{9}});
                REQUIRE(after.Def()->ReturnType() == ValueType::U32);
                if (function) {
                    REQUIRE(after.Id() == call.Id() + 1);
                    REQUIRE(function->GetHIRValue(after));
                }
                // The enum entry checks a dynamic opcode once. Known and
                // checked opcodes then use the same body through every HIR
                // layer, with continuous IDs and matching result records.
                u32 last_id = after.Id();
                const auto append = [&](auto operation, u32 expected) {
                    const auto* result = factory.AppendInst(
                            operation, ValueType::U32, Imm{expected});
                    const Value value = [&] {
                        if constexpr (std::is_same_v<decltype(result), const HIRValue*>)
                            return result->value;
                        else return Value{result};
                    }();
                    REQUIRE(value.Type() == ValueType::U32);
                    REQUIRE(value.Def()->GetArg<Imm>(0).Get() == expected);
                    REQUIRE(value.Def()->HasValidArguments());
                    if (function) {
                        REQUIRE(value.Id() == last_id + 1);
                        REQUIRE(function->GetHIRValue(value)->value.Type() == ValueType::U32);
                    }
                    last_id = value.Id();
                };
                append(OpCode::LoadImm, 10);
                append(meta::KnownOperation<OpCode::LoadImm, Imm>(), 11);
                append(meta::CheckArgumentPack<Imm>(OpCode::LoadImm), 12);
            };
            if (mode == 0) check(block);
            else if (mode == 1) check(function->EditBlock(function->GetCurrentBlock()));
            else if (mode == 2) check(*function);
            else if (mode == 3) check(hir);
            else {
                auto factory = mode == 4 ? compile::IrBuilder{&block} : compile::IrBuilder{&hir};
                check(factory);
            }
            if (function) hir.ReturnToHost();
        }
    }
    Block source{0, Location{0x6f10}};
    const auto byte = source.LoadImm(Imm{u8{7}});
    for (const bool fresh : {false, true}) {
        const auto set = [&](Inst& inst, const auto& arg) {
            if (fresh) inst.SetArgFresh(0, arg);
            else inst.SetArg(0, arg);
            REQUIRE(inst.ReturnType() == ValueType::U64);
        };
        Inst value{OpCode::BitCast, ValueType::U64};
        Inst immediate{OpCode::LoadImm, ValueType::U64};
        Inst uniform{OpCode::LoadUniform, ValueType::U64};
        Inst local{OpCode::LoadLocal, ValueType::U64};
        set(value, byte);
        set(immediate, Imm{u8{3}});
        set(uniform, Uniform{0, ValueType::U8});
        set(local, Local{0, ValueType::U8});
        Inst no_result{OpCode::CallLambda, ValueType::VOID};
        if (fresh) no_result.SetArgsFresh(Lambda{Imm{u64{1}}}, byte);
        else no_result.SetArgs(Lambda{Imm{u64{1}}}, byte);
        REQUIRE(no_result.ReturnType() == ValueType::VOID);
        no_result.SetArg(1, Value{byte}.SetCastType(ValueType::U64));
        REQUIRE(no_result.ReturnType() == ValueType::VOID);
    }
    REQUIRE_THROWS_AS(Inst{OpCode::LoadMemory}, std::logic_error);
    Inst unchanged{OpCode::LoadImm, ValueType::U64};
    unchanged.SetArgs(Imm{u64{11}});
    const auto uses = byte.Def()->GetUses(false);
    REQUIRE_THROWS_AS(unchanged.SetInst(OpCode::Add, ValueType::U32, byte, byte), std::logic_error);
    REQUIRE(unchanged.GetOp() == OpCode::LoadImm);
    REQUIRE(unchanged.ReturnType() == ValueType::U64);
    REQUIRE(unchanged.GetArg<Imm>(0).Get() == 11);
    REQUIRE(byte.Def()->GetUses(false) == uses);
}

static u64 WriteConstructionMemory(u64 address, u64 value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
    return ~value;
}

TEST_CASE("construction types preserve data widths and ignored calls in interpreter and JIT",
          "[ir][builder][types][jit]") {
    constexpr u32 uniform_size = 64;
    constexpr u64 fallback = 0xfedcba9876543210;
    const std::array<std::pair<u8, u64>, 4> inputs{{
            {0, 0x100000000}, {0xff, 0xfedcba9876543210},
            {0x80, 0x800000000000ffff}, {1, UINT64_MAX}}};
    const auto initial = [](u8 byte, u64 wide) {
        std::array<u8, uniform_size> bytes;
        bytes.fill(0xa5);
        bytes[0] = byte;
        std::memcpy(bytes.data() + 8, &wide, sizeof(wide));
        return bytes;
    };
    const auto expected = [&](u8 byte, u64 wide) {
        auto bytes = initial(byte, wide);
        const u8 sum = byte + 7;
        const u16 view_sum = static_cast<u16>(wide) + 0x1234;
        const u64 selected = byte ? wide : fallback;
        const u64 loaded = wide & UINT16_MAX;
        std::memcpy(bytes.data() + 16, &sum, sizeof(sum));
        std::memcpy(bytes.data() + 24, &view_sum, sizeof(view_sum));
        std::memcpy(bytes.data() + 32, &selected, sizeof(selected));
        std::memcpy(bytes.data() + 40, &loaded, sizeof(loaded));
        return bytes;
    };
    for (const bool function_mode : {false, true}) {
        CAPTURE(function_mode);
        u64 memory{};
        Config config{.loc_start = 0x1000, .loc_end = 0x2000, .enable_jit = true,
                      .backend_isa = kArm64, .uniform_buffer_size = uniform_size};
        config.stack_alignment = 16;
        config.global_opts = Optimizations::None;
        backend::AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode
                ? hir.AppendFunction(Location{0x1200}, Location{0x1201}) : nullptr;
        IntrusivePtr<Block> block;
        if (!function_mode) block = new Block{0, Location{0x1200}};
        compile::IrBuilder factory = function ? compile::IrBuilder{&hir}
                                              : compile::IrBuilder{block.get()};
        const auto byte = factory.LoadUniform(Uniform{0, ValueType::U8});
        const auto wide = factory.LoadUniform(Uniform{8, ValueType::U64});
        const auto sum = factory.Add(byte, Operand{Imm{u32{7}}});
        const auto view_sum = factory.Add(Value{wide}.SetCastType(ValueType::U16),
                                          Operand{Imm{u64{0x1234}}});
        const auto selected = factory.Select(ValueType::U64, factory.TestNotZero(byte), wide,
                                               factory.LoadImm(Imm{fallback}));
        factory.StoreUniform(Uniform{16, ValueType::U8}, sum);
        factory.StoreUniform(Uniform{24, ValueType::U16}, view_sum);
        factory.StoreUniform(Uniform{32, ValueType::U64}, selected);
        const auto address = factory.LoadImm(Imm{reinterpret_cast<u64>(&memory)});
        factory.CallHost(ValueType::VOID, &WriteConstructionMemory, address, wide);
        const auto loaded = factory.LoadMemory(ValueType::U16, Operand{address});
        factory.StoreUniform(Uniform{40, ValueType::U64}, factory.ZeroExtend64(loaded));
        factory.AdvancePC(Imm{1u});
        Block* interpreted = function
                ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
        if (function) hir.ReturnToHost();
        else block->SetTerminal(terminal::ReturnToHost{});
        interpreted->ReIdInstr();
        for (const auto& [byte, wide] : inputs) {
            alignas(backend::State) std::array<u8, sizeof(backend::State) + uniform_size> storage{};
            auto* state = new (storage.data()) backend::State{};
            const auto bytes = initial(byte, wide), oracle = expected(byte, wide);
            std::memcpy(state->uniform_buffer_begin, bytes.data(), uniform_size);
            memory = ~wide;
            backend::interp::Interpreter interpreter{*state, interpreted};
            REQUIRE(interpreter.Run() == HaltReason::CallHost);
            REQUIRE(memory == wide);
            REQUIRE(std::memcmp(state->uniform_buffer_begin, oracle.data(), uniform_size) == 0);
        }
        if (block) REQUIRE(module->Push(block.get()));
        REQUIRE((function ? TranslateIR(module, function) : TranslateIR(module, block)) != nullptr);
        Runtime runtime{&space};
        for (const auto& [byte, wide] : inputs) {
            const auto bytes = initial(byte, wide), oracle = expected(byte, wide);
            std::memcpy(runtime.GetUniformBuffer().data(), bytes.data(), uniform_size);
            memory = ~wide;
            runtime.SetLocation(0x1200);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            REQUIRE(memory == wide);
            REQUIRE(std::memcmp(runtime.GetUniformBuffer().data(), oracle.data(), uniform_size) == 0);
        }
    }
}

TEST_CASE("fixed result defaults preserve truncation and predicates in interpreter and JIT",
          "[ir][builder][types][jit]") {
    constexpr u32 uniform_size = 96;
    const std::array<std::pair<u8, u64>, 4> inputs{{
            {0, 0}, {0xff, 0xfedcba9876543210}, {0x80, u64{1} << 32}, {0x7f, UINT64_MAX}}};
    const auto initial = [](u8 byte, u64 wide) {
        std::array<u8, uniform_size> bytes;
        bytes.fill(0xa5);
        bytes[0] = byte;
        std::memcpy(bytes.data() + 8, &wide, sizeof(wide));
        return bytes;
    };
    const auto expected = [&](u8 byte, u64 wide) {
        auto bytes = initial(byte, wide);
        const std::array<u64, 8> values{wide & UINT32_MAX, byte, wide & UINT32_MAX,
                                        wide & UINT32_MAX, wide == 0, wide != 0, 0, 1};
        const std::array<u32, 8> widths{4, 8, 8, 8, 1, 1, 1, 1};
        for (u32 i = 0; i < values.size(); ++i)
            std::memcpy(bytes.data() + 16 + 8 * i, &values[i], widths[i]);
        return bytes;
    };
    for (bool function_mode : {false, true}) {
        CAPTURE(function_mode);
        Config config{.loc_start = 0x1000, .loc_end = 0x2000, .enable_jit = true,
                      .backend_isa = kArm64, .uniform_buffer_size = uniform_size};
        config.stack_alignment = 16;
        config.global_opts = Optimizations::None;
        backend::AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode
                                 ? hir.AppendFunction(Location{0x1100}, Location{0x1101})
                                 : nullptr;
        IntrusivePtr<Block> block;
        if (!function_mode) block = new Block{0, Location{0x1100}};
        const auto build = [](auto& factory) {
            const auto byte = factory.LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
            const auto wide = factory.LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
            const auto narrow = factory.ZeroExtend32(wide);
            const std::array<Value, 8> values{
                    narrow, factory.ZeroExtend64(byte), factory.ZeroExtend32To64(wide),
                    Value{narrow}.SetCastType(ValueType::U64), factory.TestZero(wide),
                    factory.TestNotZero(wide), factory.TestFlags(Flags::Carry),
                    factory.TestNotFlags(Flags::Carry)};
            for (u32 i = 0; i < values.size(); ++i)
                factory.StoreUniform(Uniform{16 + 8 * i, values[i].Type()}, values[i]);
            factory.AdvancePC(Imm{1u});
        };
        Block* interpreted;
        if (function) {
            build(*function);
            interpreted = function->EditBlock(function->GetCurrentBlock()).GetBlock();
            hir.ReturnToHost();
        } else {
            build(*block);
            block->SetTerminal(terminal::ReturnToHost{});
            interpreted = block.get();
        }
        interpreted->ReIdInstr();
        for (const auto& [byte, wide] : inputs) {
            alignas(backend::State) std::array<u8, sizeof(backend::State) + uniform_size> storage{};
            auto* state = new (storage.data()) backend::State{};
            const auto bytes = initial(byte, wide), oracle = expected(byte, wide);
            std::memcpy(state->uniform_buffer_begin, bytes.data(), uniform_size);
            backend::interp::Interpreter interpreter{*state, interpreted};
            REQUIRE(interpreter.Run() == HaltReason::CallHost);
            REQUIRE(std::memcmp(state->uniform_buffer_begin, oracle.data(), uniform_size) == 0);
        }
        if (block) REQUIRE(module->Push(block.get()));
        REQUIRE((function ? TranslateIR(module, function) : TranslateIR(module, block)) != nullptr);
        Runtime runtime{&space};
        for (const auto& [byte, wide] : inputs) {
            const auto bytes = initial(byte, wide), oracle = expected(byte, wide);
            std::memcpy(runtime.GetUniformBuffer().data(), bytes.data(), uniform_size);
            runtime.SetLocation(0x1100);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            REQUIRE(std::memcmp(runtime.GetUniformBuffer().data(), oracle.data(), uniform_size) == 0);
        }
    }
}

TEST_CASE("builder immediate sources retain append windows through deferred width folds",
          "[ir][builder][commit]") {
    for (unsigned mode : {0u, 1u, 2u}) {
        for (bool deferred : {false, true}) {
            CAPTURE(mode, deferred);
            Block block{0, Location{0x6800}};
            HIRBuilder hir{1, false, mode == 2, FeatureSet{}};
            if (mode) hir.AppendFunction(Location{0x6800}, Location{0x6900});
            auto builder = mode ? compile::IrBuilder{&hir} : compile::IrBuilder{&block};
            FeatureSet features{};
            features.int_imm_fold = true;
            builder.ConfigureGuestState(nullptr, features, backend::arm64::BuilderTarget());
            const auto input = builder.LoadUniform(Uniform{0, ValueType::U64});
            std::optional<compile::IrBuilder::GuestInstructionScope> instruction;
            if (deferred) instruction.emplace(builder);
            const auto one = builder.LoadImm(ValueType::U64, Imm{u64{1}});
            for (u32 i = 0; i < 4; ++i) {
                // Raw appends are deliberately outside this builder's window.
                for (u32 j = 0; j < 3; ++j) {
                    if (mode)
                        hir.LoadImm(ValueType::U64, Imm{u64{100 + i * 3 + j}});
                    else
                        block.LoadImm(ValueType::U64, Imm{u64{100 + i * 3 + j}});
                }
                builder.StoreUniform(Uniform{8, ValueType::U64}, input);
            }
            const auto near =
                    builder.Add(ValueType::U64, compile::FlagsIntent::None(), input, Operand{one});
            const auto far =
                    builder.Add(ValueType::U64, compile::FlagsIntent::None(), input, Operand{one});
            const auto external = mode ? hir.LoadImm(ValueType::U64, Imm{u64{3}})
                                       : block.LoadImm(ValueType::U64, Imm{u64{3}});
            const auto untracked = builder.Add(
                    ValueType::U64, compile::FlagsIntent::None(), input, Operand{external});
            const auto byte = builder.LoadImm(ValueType::U8, Imm{0x7fu});
            const auto wide = builder.ZeroExtend64(ValueType::U64, byte);
            const auto from_width =
                    builder.Add(ValueType::U64, compile::FlagsIntent::None(), input, Operand{wide});
            for (u32 i = 0; i < 48; ++i) builder.LoadImm(ValueType::U64, Imm{u64{i}});
            if (deferred) {
                REQUIRE(near.Def()->GetArg<Operand>(1).GetLeft().IsValue());
                REQUIRE(wide.Def()->GetOp() == OpCode::ZeroExtend64);
            }
            instruction.reset();
            REQUIRE(near.Def()->GetArg<Operand>(1).GetLeft().IsImm());
            REQUIRE(near.Def()->GetArg<Operand>(1).GetLeft().imm.Get() == 1);
            REQUIRE(far.Def()->GetArg<Operand>(1).GetLeft().IsValue());
            REQUIRE(untracked.Def()->GetArg<Operand>(1).GetLeft().IsValue());
            REQUIRE(wide.Def()->GetOp() == OpCode::LoadImm);
            REQUIRE(from_width.Def()->GetArg<Operand>(1).GetLeft().IsImm());
            REQUIRE(from_width.Def()->GetArg<Operand>(1).GetLeft().imm.Get() == 0x7f);
            REQUIRE(byte.Type() == ValueType::U8);
            const auto expired =
                    builder.Add(ValueType::U64, compile::FlagsIntent::None(), input, Operand{one});
            REQUIRE(expired.Def()->GetArg<Operand>(1).GetLeft().IsValue());
            builder.ReturnToHost();
        }
    }
}

TEST_CASE("builder immediate history preserves configuration and HIR boundary semantics",
          "[ir][builder][commit]") {
    for (unsigned mode : {0u, 1u, 2u}) {
        CAPTURE(mode);
        Block block{0, Location{0x6a00}};
        FeatureSet features{};
        features.int_imm_fold = true;
        features.advpc_coalesce = true;
        HIRBuilder hir{1, false, mode == 2, features};
        if (mode) hir.AppendFunction(Location{0x6a00}, Location{0x6c00});
        auto builder = mode ? compile::IrBuilder{&hir} : compile::IrBuilder{&block};
        builder.ConfigureGuestState(nullptr, features, backend::arm64::BuilderTarget());
        const auto input = builder.LoadUniform(Uniform{0, ValueType::U64});
        Value zero;
        {
            compile::IrBuilder::GuestInstructionScope instruction{builder};
            zero = builder.LoadImm(ValueType::U64, Imm{u64{0}});
            for (u32 i = 0; i < 5; ++i) builder.StoreUniform(Uniform{8, ValueType::U64}, input);
        }
        // Disabled capabilities freeze the original append counter. The zero
        // source is still five positions behind the last tracked append.
        builder.ConfigureGuestState(nullptr, features, {});
        const auto neg =
                builder.Sub(ValueType::U64, compile::FlagsIntent::None(), zero, Operand{input});
        REQUIRE(neg.Def()->GetOp() == OpCode::Neg);
        const auto first =
                builder.ZeroExtend64(ValueType::U64, builder.LoadImm(ValueType::U8, Imm{7u}));
        const auto second =
                builder.ZeroExtend64(ValueType::U64, builder.LoadImm(ValueType::U8, Imm{9u}));
        REQUIRE(first.Def() != second.Def());
        REQUIRE(first.Def()->GetOp() == OpCode::LoadImm);
        REQUIRE(second.Def()->GetOp() == OpCode::LoadImm);
        builder.ConfigureGuestState(nullptr, features, backend::arm64::BuilderTarget());
        const auto first_use =
                builder.Add(ValueType::U64, compile::FlagsIntent::None(), input, Operand{first});
        const auto second_use =
                builder.Add(ValueType::U64, compile::FlagsIntent::None(), input, Operand{second});
        REQUIRE(first_use.Def()->GetArg<Operand>(1).GetLeft().IsImm());
        REQUIRE(first_use.Def()->GetArg<Operand>(1).GetLeft().imm.Get() == 7);
        REQUIRE(second_use.Def()->GetArg<Operand>(1).GetLeft().IsImm());
        REQUIRE(second_use.Def()->GetArg<Operand>(1).GetLeft().imm.Get() == 9);
        if (mode) {
            auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x6b00}});
            hir.SetCurBlock(next);
            const auto cross_block = builder.Add(
                    ValueType::U64, compile::FlagsIntent::None(), input, Operand{first});
            REQUIRE(cross_block.Def()->GetArg<Operand>(1).GetLeft().IsImm());
            REQUIRE(cross_block.Def()->GetArg<Operand>(1).GetLeft().imm.Get() == 7);
            const auto one = builder.LoadImm(ValueType::U64, Imm{u64{1}});
            auto* advance = builder.AppendInst(OpCode::AdvancePC, Imm{1u});
            REQUIRE(advance);
            for (u32 i = 0; i < 16; ++i)
                REQUIRE(builder.AppendInst(OpCode::AdvancePC, Imm{1u}) == nullptr);
            REQUIRE(advance->GetArg<Imm>(0).Get() == 17);
            for (u32 i = 0; i < 3; ++i) builder.StoreUniform(Uniform{8, ValueType::U64}, input);
            const auto after_metadata =
                    builder.Add(ValueType::U64, compile::FlagsIntent::None(), input, Operand{one});
            REQUIRE(after_metadata.Def()->GetArg<Operand>(1).GetLeft().IsImm());
        }
        builder.ReturnToHost();
    }
}

TEST_CASE("guest commit precedes HIR terminals and cancels deferred rewrites on exceptions",
          "[ir][builder][commit][lifetime]") {
    HIRBuilder hir{1, false, FeatureSet{}};
    auto* function = hir.AppendFunction(Location{0x6500}, Location{0x6600});
    compile::IrBuilder builder{&hir};
    Value input, extended;
    {
        compile::IrBuilder::GuestInstructionScope instruction{builder};
        input = builder.LoadImm(ValueType::U8, Imm{0x80u});
        extended = builder.SignExtend(ValueType::S64, input);
        builder.StoreUniform(Uniform{0, ValueType::S64}, extended);
        builder.ReturnToHost();
        REQUIRE(!hir.HasCurrentFunction());
        REQUIRE(extended.Def()->GetOp() == OpCode::LoadImm);
        REQUIRE(function->GetHIRValue(input)->GetUses().empty());
    }
    Block block{0, Location{0x6700}};
    compile::IrBuilder fallback{&block};
    Value unfinished;
    try {
        compile::IrBuilder::GuestInstructionScope instruction{fallback};
        const auto value = fallback.LoadImm(ValueType::U8, Imm{0x80u});
        unfinished = fallback.SignExtend(ValueType::S64, value);
        throw 1;
    } catch (int) {}
    REQUIRE(unfinished.Def()->GetOp() == OpCode::SignExtend);
    {
        compile::IrBuilder::GuestInstructionScope instruction{fallback};
        fallback.ReturnToHost();
    }
    REQUIRE(unfinished.Def()->GetOp() == OpCode::SignExtend);
}

TEST_CASE("typed aliases retain producer types and memory reads retain effects",
          "[ir][builder][effects]") {
    Block block{0, Location{0x5800}};
    compile::IrBuilder builder{&block};
    const auto source = builder.LoadImm(ValueType::U64, Imm{u64{0x1234567887654321}});
    const auto same = builder.BitCast(ValueType::U64, compile::FlagsIntent::None(), source);
    REQUIRE(same.Def() == source.Def());
    const auto narrow = builder.BitCast(ValueType::U32, compile::FlagsIntent::None(), source);
    REQUIRE(narrow.Type() == ValueType::U32);
    REQUIRE(source.Def()->ReturnType() == ValueType::U64);
    REQUIRE(narrow.Def() != source.Def());
    const auto first = builder.LoadMemory(ValueType::U64, Operand{source});
    const auto second = builder.LoadMemory(ValueType::U64, Operand{source});
    REQUIRE(first.Def() != second.Def());
    REQUIRE(first.Def()->HasSideEffects());
    REQUIRE(second.Def()->HasSideEffects());
}

TEST_CASE("integer immediate selection respects flags intent and target capability",
          "[ir][fold][flags]") {
    using compile::FlagsIntent;
    const auto target = backend::arm64::BuilderTarget();
    const Imm one{1u};
    REQUIRE_FALSE(compile::FoldIntegerImmediate(
            OpCode::Add, ValueType::U16, one, FlagsIntent::Unknown(), target));
    REQUIRE_FALSE(compile::FoldIntegerImmediate(
            OpCode::Sub, ValueType::U8, one, FlagsIntent::Declared(Flags::NZCV), target));
    REQUIRE(compile::FoldIntegerImmediate(
            OpCode::Add, ValueType::U16, one, FlagsIntent::None(), target));
    REQUIRE_FALSE(compile::FoldIntegerImmediate(
            OpCode::Add, ValueType::U64, one, FlagsIntent::None(), {}));
    const auto negative = compile::FoldIntegerImmediate(
            OpCode::Sub, ValueType::U64, Imm{s64{-1}}, FlagsIntent::Unknown(), target);
    REQUIRE(negative);
    REQUIRE(negative->GetSigned() == -1);
    Block width_source{0, Location{0x57f0}};
    const auto byte = width_source.LoadImm(ValueType::U8, Imm{0x80u});
    REQUIRE_FALSE(compile::FoldWidthValue(
            OpCode::SignExtend, ValueType::U64, byte, FlagsIntent::Unknown()));
    REQUIRE_FALSE(compile::FoldWidthValue(
            OpCode::SignExtend, ValueType::U64, byte, FlagsIntent::Declared(Flags::NZCV)));
}

TEST_CASE("allocation facts retain cross-block and implicit uses without full per-block tables",
          "[ir][allocation][facts]") {
    STATIC_REQUIRE((std::is_same_v<decltype(std::declval<const Inst&>().ArgAt(0)), const Arg&>));
    STATIC_REQUIRE((std::is_same_v<decltype(std::declval<const Inst&>().ArgAt(0)), const Arg&>));
    HIRBuilder builder{1, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{0x5200}, Location{0x5400});
    const auto source = function->LoadImm<U64>(Imm{u64{42}});
    const auto condition = function->GetHostGPR<BOOL>(Imm{u32{2}}, Imm{u32{0}});
    const auto nested_only = function->LoadImm<U64>(Imm{u64{91}});
    const auto param_capture = function->GetHostGPR<U64>(Imm{u32{3}}, Imm{u32{0}});
    function->SetHostGPR(source, Imm{u32{5}}, Imm{u32{0}});
    for (u32 i = 0; i < 32; ++i) function->LoadImm<U64>(Imm{u64{i}});
    auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x5300}});
    builder.SetCurBlock(next);
    const auto anchor = function->LoadImm<U64>(Imm{u64{17}});
    const auto target_capture = function->GetHostGPR<U64>(Imm{u32{7}}, Imm{u32{0}});
    Params params;
    params.Push(source);
    params.Push(nested_only);
    params.Push(nested_only);
    params.Push(Value{param_capture}.SetCastType(ValueType::U16));
    function->CallDynamic(ValueType::VOID, Lambda{Value{target_capture}.SetCastType(ValueType::U32)}, std::move(params));
    const auto* call = &next->GetBlock()->GetInstList().back();
    function->StoreUniform(Uniform{8, ValueType::U64}, source);
    function->EndBlock(terminal::If{
            condition, terminal::LinkBlock{Location{0x5300}}, terminal::ReturnToDispatch{}});
    auto* disconnected = function->AppendBlock(Location{0x53c0});
    function->SetCurBlock(disconnected);
    const auto dead_capture = function->GetHostGPR<U16>(Imm{u32{29}}, Imm{u32{0}});
    function->SetHostGPR(dead_capture, Imm{u32{30}}, Imm{u32{0}});
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    auto* block = next->GetBlock();
    const auto last = block->GetInstList().back().Id();
    compile::CompileUnit unit{function};
    std::array<const Inst*, 1> anchors{anchor.Def()};
    unit.SetLoopHoistMetadata(block, {anchor.Def(), anchors, 0, 1});
    std::vector<const Inst*> terminal_values;
    compile::VisitTerminalValues(block->GetTerminal(),
                                 [&](Value value) { terminal_values.push_back(value.Def()); });
    REQUIRE(terminal_values == std::vector<const Inst*>{condition.Def()});
    REQUIRE(std::find(terminal_values.begin(), terminal_values.end(), anchor.Def()) ==
            terminal_values.end());
    compile::SemanticFacts facts{function};
    REQUIRE_FALSE(facts.CanUseBlockIntervals());
    REQUIRE(facts.FindFunctionCFG() == nullptr);
    REQUIRE_FALSE(facts.HasSpillUses());
    REQUIRE(facts.FindValueLiveness() == nullptr);
    const auto& base = facts.PrepareValueLiveness();
    REQUIRE(std::none_of(function->GetHIRBlocksRPO().begin(),
                         function->GetHIRBlocksRPO().end(),
                         [&](const HIRBlock& member) { return &member == disconnected; }));
    REQUIRE(facts.GPRHomes() == ((u32{1} << 2) | (u32{1} << 3) | (u32{1} << 5) | (u32{1} << 7)));
    REQUIRE(facts.FindRegisterReads() == nullptr);
    CheckRegisterReadUses(facts, nullptr, function);
    REQUIRE(facts.FindUses(block) == nullptr);
    REQUIRE(facts.PrepareRegisterReads().Find(condition.Id())->last_use == last);
    REQUIRE(facts.PrepareRegisterReads().Find(condition.Id())->max_width == 1);
    REQUIRE(facts.PrepareRegisterReads().Find(param_capture.Id())->max_width == 2);
    REQUIRE(facts.PrepareRegisterReads().Find(target_capture.Id())->max_width == 4);
    REQUIRE_FALSE(facts.PrepareRegisterReads().Find(dead_capture.Id()));
    REQUIRE(base.UseEnd(source.Id()) == last);
    REQUIRE(base.UseEnd(condition.Id()) == last);
    REQUIRE(base.UseEnd(anchor.Id()) == last);
    REQUIRE(base.UseEnd(nested_only.Id()) == call->Id());
    REQUIRE_FALSE(base.UsesStayInBlock(block, source));
    REQUIRE(base.UsesStayInBlock(block, anchor));
    REQUIRE(&facts.PrepareValueLiveness() == &base);
    REQUIRE(facts.FindUses(block) == nullptr);
    REQUIRE_FALSE(facts.HasSpillUses());
    const auto& uses = facts.PrepareBlockUses(block);
    REQUIRE(uses[source.Id()] == last);
    REQUIRE(uses[condition.Id()] == last);
    REQUIRE(uses[anchor.Id()] == last);
    REQUIRE(uses[nested_only.Id()] == call->Id());
    REQUIRE(uses.StoredValueCount() <= block->GetInstList().size() + 4);
    (void)facts.PrepareBlockUses(function->GetHIRBlocksRPO().front().GetBlock());
    REQUIRE(&facts.PrepareBlockUses(block) == &uses);
    REQUIRE(facts.PreparedBlockCount() == 2);
    REQUIRE(facts.PrepareFlags().empty());
    REQUIRE_FALSE(facts.HasSpillUses());
    REQUIRE(facts.FindFunctionCFG() == nullptr);
    const auto& cfg_uses = facts.PrepareCFGUseEnds();
    const auto* cfg = facts.FindFunctionCFG();
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->Blocks().size() == 2);
    REQUIRE_FALSE(cfg->FindBlock(disconnected->GetBlock()));
    const auto next_id = cfg->FindBlock(block);
    REQUIRE(next_id);
    const auto successors = cfg->Successors(cfg->Blocks().front());
    REQUIRE(successors.size() == 1);
    REQUIRE(successors.front() == *next_id);
    // This fixture sets the tail terminal directly, without adding a HIR
    // edge. The shared projection must not silently repair that distinction.
    REQUIRE(cfg->Successors(cfg->Blocks()[*next_id]).empty());
    REQUIRE(cfg_uses[source.Id()] == last);
    REQUIRE(cfg_uses[condition.Id()] == last);
    REQUIRE(cfg_uses[anchor.Id()] == last);
    REQUIRE(cfg_uses[nested_only.Id()] == call->Id());
    REQUIRE(&facts.PrepareCFGUseEnds() == &cfg_uses);
    REQUIRE(&facts.PrepareFunctionCFG() == cfg);
    const auto& spill = facts.PrepareSpillUses();
    REQUIRE(std::find(spill[source.Id()].begin(), spill[source.Id()].end(), call->Id()) !=
            spill[source.Id()].end());
    REQUIRE(spill[condition.Id()].back() == last);
    REQUIRE(spill[anchor.Id()].back() == last);
    REQUIRE(spill[nested_only.Id()].back() == call->Id());
    REQUIRE(&facts.PrepareSpillUses() == &spill);
    facts.BeginInput(disconnected->GetBlock(), nullptr);
    REQUIRE(facts.FindFunctionCFG() == nullptr);
    REQUIRE(facts.PrepareCFGUseEnds().empty());
    REQUIRE(facts.FindFunctionCFG() == nullptr);
}

// Dense forward fixed point over raw HIR, independent of FunctionCFG and the
// production predecessor demand walk. Small analysis fixtures also cover
// conservative endpoints on IR that the mandatory SSA checker would reject.
static Vector<u32> ReferenceCFGUseEnds(const HIRFunction& function) {
    std::vector<const HIRBlock*> blocks;
    for (const auto& block : function.GetHIRBlocksRPO()) blocks.push_back(&block);
    const auto count = function.MaxInstrCount();
    using Bits = std::vector<u8>;
    std::vector<Bits> definitions(blocks.size(), Bits(count));
    std::vector<Bits> uses = definitions, incoming = definitions, outgoing = definitions;
    Vector<u32> ends(count);
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto* block = blocks[i]->GetBlock();
        const auto use = [&](Value value, u32 point) {
            value = ResolveBitCastSource(value);
            if (!value.Defined() || value.Type() == ValueType::VOID || value.Id() >= count) return;
            const auto id = value.Id();
            ends[id] = std::max(ends[id], point);
            if (!definitions[i][id]) uses[i][id] = 1;
        };
        for (const auto& instruction : block->GetInstList()) {
            if (!instruction.IsPseudoOperation())
                instruction.ForEachValue([&](Value value) { use(value, instruction.Id()); });
            if (instruction.HasValue() && !instruction.IsBitCastOperation() &&
                instruction.Id() < count)
                definitions[i][instruction.Id()] = 1;
        }
        const auto& list = block->GetInstList();
        compile::VisitImplicitUses(*block, list.empty() ? 0 : list.back().Id(), use);
    }
    bool changed;
    do {
        changed = false;
        for (size_t i = blocks.size(); i-- > 0;) {
            Bits next(count);
            const auto merge = [&](const HIRBlock* target) {
                const auto found = std::find(blocks.begin(), blocks.end(), target);
                if (found == blocks.end()) return;
                const auto& live = incoming[found - blocks.begin()];
                for (u32 id = 0; id < count; ++id) next[id] |= live[id];
            };
            for (const auto& edge : blocks[i]->GetOutgoingEdges()) merge(edge.GetTargetBlock());
            merge(blocks[i]->GetCallReturnBlock());
            Bits before = uses[i];
            for (u32 id = 0; id < count; ++id) before[id] |= next[id] && !definitions[i][id];
            changed |= before != incoming[i] || next != outgoing[i];
            incoming[i] = std::move(before);
            outgoing[i] = std::move(next);
        }
    } while (changed);
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& list = blocks[i]->GetInstList();
        const auto tail = list.empty() ? 0 : list.back().Id();
        for (u32 id = 0; id < count; ++id)
            if (outgoing[i][id]) ends[id] = std::max<u32>(ends[id], tail);
    }
    return ends;
}

TEST_CASE("sparse CFG liveness matches raw fixed points for loops aliases and logical returns",
          "[ir][allocation][facts][cfg]") {
    for (const unsigned shape : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u})
        for (const bool aliases : {false, true}) {
            CAPTURE(shape, aliases);
            HIRBuilder builder{1, true, FeatureSet{}};
            auto* function = builder.AppendFunction(Location{0x5000}, Location{0x5070});
            std::array<HIRBlock*, 6> blocks{};
            blocks[0] = &function->EditBlock(function->GetCurrentBlock());
            for (size_t i = 1; i < blocks.size(); ++i)
                blocks[i] = function->AppendBlock(Location{0x5000 + i * 0x10});
            std::vector<std::pair<unsigned, unsigned>> edges;
            switch (shape) {
                case 0:
                    edges = {{0, 1}, {1, 2}, {2, 3}, {3, 4}};
                    break;
                case 1:
                    edges = {{0, 1}, {0, 2}, {1, 3}, {2, 3}, {3, 4}};
                    break;
                case 2:
                    edges = {{0, 1}, {1, 2}, {2, 3}, {3, 1}, {3, 4}};
                    break;
                case 3:
                    edges = {{0, 1}, {0, 2}, {1, 3}, {2, 3}, {3, 1}, {3, 2}, {3, 4}};
                    break;
                case 4:
                    edges = {{0, 1}, {0, 1}, {1, 2}, {1, 2}, {2, 3}, {3, 1}, {3, 4}};
                    break;
                case 5:
                    edges = {{0, 2}, {1, 2}, {1, 3}, {3, 4}, {4, 1}};
                    break;
                case 6:
                    edges = {{0, 1}, {1, 2}};
                    break;
                case 7:
                    edges = {{0, 0}, {0, 1}, {1, 1}, {1, 2}, {2, 3}, {3, 4}};
                    break;
            }
            for (auto [from, to] : edges) function->AddEdge(blocks[from], blocks[to]);
            std::array<Value, 6> values, pseudo_only;
            for (size_t i = 0; i < blocks.size(); ++i) {
                builder.SetCurBlock(blocks[i]);
                values[i] = function->LoadUniform<U64>(Uniform{0, ValueType::U64});
                pseudo_only[i] = function->LoadUniform<U64>(Uniform{8, ValueType::U64});
            }
            for (size_t i = 0; i < blocks.size(); ++i) {
                builder.SetCurBlock(blocks[i]);
                Value input = values[(i + 2) % values.size()];
                if (aliases) input = function->BitCast<U64>(input);
                Params params;
                params.Push(input);
                params.Push(values[0]);
                params.Push(input);
                function->CallDynamic(ValueType::VOID, Lambda{values[(i + 1) % values.size()]}, std::move(params));
                function->SaveFlags(pseudo_only[i], Flags::All);
                if (shape == 7 && i == 1) {
                    function->StoreUniform(Uniform{16, ValueType::U64}, values[i]);
                    blocks[i]->GetBlock()->MoveBefore(&blocks[i]->GetInstList().back(),
                                                      values[i].Def());
                }
                if (shape == 5 && (i == 0 || i == 2)) builder.RegisterCallReturn(Location{0x5010});
                function->EndBlock(terminal::If{values[0].SetCastType(ValueType::U8),
                                                terminal::ReturnToHost{},
                                                terminal::ReturnToHost{}});
            }
            function->EndFunction();
            function->ComputeRPO();
            function->IdByRPO();
            compile::CompileUnit unit{function};
            std::array<const Inst*, 1> anchors{values[0].Def()};
            unit.SetLoopHoistMetadata(blocks[2]->GetBlock(), {values[0].Def(), anchors, 1, 0});
            const auto expected = ReferenceCFGUseEnds(*function);
            compile::CompileWorkspace workspace;
            compile::InstructionRowScope later{workspace, 1};
            later[0] = 73;
            compile::SemanticFacts facts{function};
            const auto before = SnapshotWorkspaceStorage();
            const auto& actual = facts.PrepareCFGUseEnds();
            const auto after = SnapshotWorkspaceStorage();
            REQUIRE(actual == expected);
            // Neither a fresh preparation nor a cached query needs the
            // selection rows, even while another user holds them.
            REQUIRE(&facts.PrepareCFGUseEnds() == &actual);
            REQUIRE(later.Find(0) == 73);
            if (before.enabled) {
                const auto row = static_cast<size_t>(WorkspaceBufferKind::InstructionRows);
                REQUIRE(after.buffers[row].allocation_calls ==
                        before.buffers[row].allocation_calls);
                REQUIRE(after.buffers[row].pattern_init_bytes ==
                        before.buffers[row].pattern_init_bytes);
                REQUIRE(after.buffers[row].retention_events ==
                        before.buffers[row].retention_events);
            }
        }
}

TEST_CASE("Function liveness leaves selection rows untouched during preparation",
          "[ir][allocation][facts][cfg]") {
    HIRBuilder builder{1, true, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{0x5080}, Location{0x50b0});
    for (unsigned i = 0; i < 3; ++i) {
        const auto value = function->LoadUniform<U64>(Uniform{0, ValueType::U64});
        function->StoreUniform(Uniform{8, ValueType::U64}, value);
        if (i != 2)
            builder.SetCurBlock(
                    builder.LinkBlock(terminal::LinkBlock{Location{0x5090 + i * 0x10}}));
        else
            function->EndBlock(terminal::ReturnToHost{});
    }
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    compile::CompileWorkspace workspace;
    compile::InstructionRowScope later{workspace, 1};
    later[0] = 91;
    compile::SemanticFacts facts{function};
    const auto& actual = facts.PrepareCFGUseEnds();
    REQUIRE(actual == ReferenceCFGUseEnds(*function));
    REQUIRE(facts.FindFunctionCFG() == nullptr);
    REQUIRE(&facts.PrepareCFGUseEnds() == &actual);
    REQUIRE(later.Find(0) == 91);
    Block empty{0, Location{0x50c0}};
    empty.SetTerminal(terminal::ReturnToHost{});
    facts.BeginInput(&empty, nullptr);
    REQUIRE(facts.PrepareCFGUseEnds().empty());
    REQUIRE(facts.FindFunctionCFG() == nullptr);
    REQUIRE(later.Find(0) == 91);
}

TEST_CASE("CFG liveness preserves full-width endpoints at the maximum instruction count",
          "[ir][allocation][facts][cfg]") {
    constexpr u32 count = std::numeric_limits<u16>::max();
    HIRBuilder builder{1, true, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{0x50d0}, Location{0x50f0});
    const auto carried = function->LoadUniform<U64>(Uniform{0, ValueType::U64});
    for (u32 i = 1; i < count - 3; ++i) function->Nop();
    builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x50e0}}));
    const auto local = function->LoadUniform<U64>(Uniform{8, ValueType::U64});
    function->StoreUniform(Uniform{16, ValueType::U64}, carried);
    function->StoreUniform(Uniform{24, ValueType::U64}, local);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    REQUIRE(function->MaxInstrCount() == count);
    REQUIRE(local.Id() == count - 3);
    compile::SemanticFacts facts{function};
    const auto& ends = facts.PrepareCFGUseEnds();
    REQUIRE(ends == ReferenceCFGUseEnds(*function));
    REQUIRE(ends[carried.Id()] == count - 2);
    REQUIRE(ends[local.Id()] == count - 1);
    REQUIRE(&facts.PrepareCFGUseEnds() == &ends);
}

TEST_CASE("call-return backedges extend liveness independently of ordinary successors",
          "[ir][ssa][allocation][facts]") {
    const char* raw = GetRawSvmConfigEnvForTest("SVM_VERIFY_PLAN");
    const auto saved = raw ? std::optional<std::string>{raw} : std::nullopt;
    const auto restore = ScopeExit([&] {
        if (saved)
            SetSvmConfigEnvForTest("SVM_VERIFY_PLAN", saved->c_str(), 1);
        else
            UnsetSvmConfigEnvForTest("SVM_VERIFY_PLAN");
    });
    SetSvmConfigEnvForTest("SVM_VERIFY_PLAN", "1", 1);
    Config config{.loc_start = 0,
                  .loc_end = 0x10000,
                  .enable_jit = true,
                  .backend_isa = kArm64,
                  .uniform_buffer_size = 64};
    config.region_edges = true;
    backend::AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    HIRBuilder builder{1, true, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{0x5d00}, Location{0x5d40});
    auto* entry = function->GetCurrentBlock();
    const auto carried = function->LoadUniform<U64>(Uniform{0, ValueType::U64});
    const auto call = [&] {
        const auto resume = function->LoadImm<U64>(Imm{u64{0x5d10}});
        const Lambda callee{Imm{u64{0x6d00}}};
        function->SetLocation(callee);
        function->CallReturn(resume, Imm{u64{0x5d10}}, callee);
        builder.RegisterCallReturn(Location{0x5d10});
        function->EndBlock(terminal::ReturnToDispatch{});
    };
    call();
    builder.SetCurBlock(Location{0x5d10});
    auto* returned = function->GetCurrentBlock();
    function->StoreUniform(Uniform{24, ValueType::U64}, carried);
    auto* caller = builder.LinkBlock(terminal::LinkBlock{Location{0x5d30}});
    builder.SetCurBlock(caller);
    const auto other = function->LoadUniform<U64>(Uniform{8, ValueType::U64});
    function->StoreUniform(Uniform{32, ValueType::U64}, other);
    call();
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    const auto* tail = &caller->GetBlock()->GetInstList().back();
    {
        compile::CompileUnit unit{function};
        unit.SealIR();
        compile::SemanticFacts facts{function};
        const auto& base = facts.PrepareValueLiveness();
        REQUIRE(base.FunctionEnd(carried.Id()) < tail->Id());
        REQUIRE(facts.PrepareSSAValidation());
        const auto& cfg = facts.PrepareFunctionCFG();
        const auto source_id = cfg.FindBlock(caller->GetBlock());
        const auto target_id = cfg.FindBlock(returned->GetBlock());
        const auto entry_id = cfg.FindBlock(entry->GetBlock());
        REQUIRE(source_id);
        REQUIRE(target_id);
        REQUIRE(entry_id);
        REQUIRE(*target_id < *source_id);
        REQUIRE(cfg.Successors(cfg.Blocks()[*source_id]).empty());
        REQUIRE(cfg.Successors(cfg.Blocks()[*entry_id]).empty());
        REQUIRE(cfg.Predecessors(cfg.Blocks()[*target_id]).empty());
        REQUIRE(cfg.Blocks()[*entry_id].return_block == *target_id);
        REQUIRE(cfg.Blocks()[*source_id].return_block == *target_id);
        const auto& ends = facts.PrepareCFGUseEnds();
        REQUIRE(ends[carried.Id()] == tail->Id());
        REQUIRE(&facts.PrepareCFGUseEnds() == &ends);
        REQUIRE(&facts.PrepareFunctionCFG() == &cfg);
    }
    // This checks the logical graph and the caller's physical live mask.
    // Executing a guest callee still needs its independent save/restore ABI.
    auto prepared = backend::arm64::PrepareFunction(module, function);
    REQUIRE(prepared);
    REQUIRE(prepared->ReadyForEmission(module));
    const auto allocation = prepared->Allocation();
    const auto residence = allocation.GPRLocationAt(
            carried, tail->Id(), compile::AllocationView::GPRView::WholeValue);
    REQUIRE(residence.kind == compile::AllocationView::GPR);
    REQUIRE(allocation.DirtyGPR(tail->Id()).Get(residence.slot));
    REQUIRE(space.GetCodeCache(Location{0x5d00}) == nullptr);
}

static HIRFunction* MakeSSAFunction(HIRBuilder& builder,
                                    bool definition_at_root,
                                    bool external_join,
                                    bool loop,
                                    bool terminal_use) {
    auto* function = builder.AppendFunction(Location{0x5b00}, Location{0x5b50});
    Value value;
    if (definition_at_root) value = function->LoadUniform<U64>(Uniform{8, ValueType::U64});
    const auto condition = function->LoadUniform<U8>(Uniform{0, ValueType::U8});
    auto [left, right] = builder.If(terminal::If{condition,
                                                 terminal::LinkBlock{Location{0x5b10}},
                                                 terminal::LinkBlock{Location{0x5b20}}});
    builder.SetCurBlock(left);
    if (!definition_at_root) value = function->LoadUniform<U64>(Uniform{8, ValueType::U64});
    auto* join = builder.LinkBlock(terminal::LinkBlock{Location{0x5b30}});
    builder.SetCurBlock(right);
    builder.LinkBlock(terminal::LinkBlock{Location{0x5b30}});
    builder.SetCurBlock(join);
    if (terminal_use) {
        function->EndBlock(terminal::If{value.SetCastType(ValueType::U8),
                                        terminal::ReturnToHost{},
                                        terminal::ReturnToHost{}});
    } else {
        function->StoreUniform(Uniform{16, ValueType::U64}, value);
        if (loop) {
            auto [again, exit] = builder.If(terminal::If{condition,
                                                         terminal::LinkBlock{Location{0x5b10}},
                                                         terminal::LinkBlock{Location{0x5b40}}});
            (void)again;
            builder.SetCurBlock(exit);
            function->EndBlock(terminal::ReturnToHost{});
        } else {
            function->EndBlock(terminal::ReturnToHost{});
        }
    }
    if (external_join) function->RegisterExternalEntryRoot(join);
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    return function;
}

TEST_CASE("public IR audit distinguishes definition identity from colliding ids",
          "[ir][ir-audit][ownership]") {
    using compile::audit::FailureKind;
    for (const bool foreign : {false, true}) {
        CAPTURE(foreign);
        // The user block is destroyed first, while its foreign definition is
        // still alive. Both definitions deliberately have the same legal id.
        Block outsider{0, Location{0x8100}};
        IntrusivePtr<Block> block{new Block{0, Location{0x8110}}};
        const auto other = outsider.LoadUniform<U64>(Uniform{0, ValueType::U64});
        const auto local = block->LoadUniform<U64>(Uniform{0, ValueType::U64});
        block->StoreUniform(Uniform{8, ValueType::U64}, foreign ? other : local);
        block->SetTerminal(terminal::ReturnToHost{});
        outsider.ReIdInstr();
        block->ReIdInstr();
        REQUIRE(other.Id() == local.Id());
        compile::CompileUnit unit{block};
        unit.SealIR();
        const std::array<const Block*, 1> blocks{block.get()};
        const compile::audit::IRReference reference{blocks, nullptr, block->MaxInstrId()};
        REQUIRE(reference.Valid() == !foreign);
        REQUIRE(reference.Failure() == (foreign ? FailureKind::Owner : FailureKind::None));
        if (!foreign) {
            REQUIRE(reference.Owner(local.Def()) == block.get());
            REQUIRE(reference.Owner(other.Def()) == nullptr);
            REQUIRE(reference.Dominates(block.get(), block.get()));
            const std::array<const Block*, 2> duplicate_blocks{block.get(), block.get()};
            const compile::audit::IRReference duplicate_reference{
                    duplicate_blocks, nullptr, block->MaxInstrId()};
            REQUIRE_FALSE(duplicate_reference.Valid());
            REQUIRE(duplicate_reference.Failure() == FailureKind::Blocks);
            REQUIRE_FALSE(duplicate_reference.Dominates(block.get(), block.get()));
        }
    }
    HIRBuilder duplicate_builder{1, false, FeatureSet{}};
    auto* duplicate = duplicate_builder.AppendFunction(Location{0x8120}, Location{0x8131});
    const auto first = duplicate->LoadImm<U64>(Imm{u64{11}});
    auto* second_block = duplicate_builder.LinkBlock(terminal::LinkBlock{Location{0x8130}});
    duplicate_builder.SetCurBlock(second_block);
    const auto second = duplicate->LoadImm<U64>(Imm{u64{29}});
    duplicate->StoreUniform(Uniform{0, ValueType::U64}, second);
    duplicate->EndBlock(terminal::ReturnToHost{});
    duplicate->EndFunction();
    duplicate->ComputeRPO();
    duplicate->IdByRPO();
    // A later local numbering pass restarts this member at zero, colliding
    // with the first member's already assigned Function-wide definition id.
    duplicate->EditBlock(second_block).GetBlock()->ReIdInstr();
    REQUIRE(first.Id() == second.Id());
    compile::CompileUnit unit{duplicate};
    unit.SealIR();
    std::vector<const Block*> blocks;
    for (const auto& member : duplicate->GetHIRBlocksRPO()) blocks.push_back(member.GetBlock());
    const compile::audit::IRReference reference{blocks, duplicate, duplicate->MaxInstrCount()};
    REQUIRE_FALSE(reference.Valid());
    REQUIRE(reference.Failure() == FailureKind::Numbering);
}

TEST_CASE("public IR audit checks paths to explicit terminal and hoist roots",
          "[ir][ir-audit][cfg]") {
    using compile::audit::FailureKind;
    for (const unsigned root : {0u, 1u, 2u})
        for (const bool before_branch : {false, true}) {
            CAPTURE(root, before_branch);
            IntrusivePtr<Block> block{new Block{0, Location{0x8200}}};
            const auto condition = block->LoadUniform<U8>(Uniform{0, ValueType::U8});
            Value source;
            if (before_branch) source = block->LoadUniform<U64>(Uniform{8, ValueType::U64});
            const auto skip = block->Goto(BOOL{condition});
            if (!before_branch) source = block->LoadUniform<U64>(Uniform{8, ValueType::U64});
            block->BindLabel(skip);
            if (root == 0) block->StoreUniform(Uniform{16, ValueType::U64}, source);
            if (root == 1)
                block->SetTerminal(terminal::If{source.SetCastType(ValueType::U8),
                                                terminal::ReturnToHost{},
                                                terminal::ReturnToHost{}});
            else
                block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            // Every definition has an earlier linear id than its observation;
            // only the definition before Goto exists on both actual paths.
            REQUIRE(source.Id() < block->GetInstList().back().Id());
            std::array<const Inst*, 1> anchors{source.Def()};
            compile::CompileUnit unit{block};
            if (root == 2) {
                unit.SetLoopHoistMetadata(block.get(), {source.Def(), anchors, 1, 0});
                REQUIRE(source.Def()->GetUses(false) == 0);
            }
            unit.SealIR();
            const std::array<const Block*, 1> blocks{block.get()};
            const compile::audit::IRReference reference{blocks, nullptr, block->MaxInstrId()};
            REQUIRE(reference.Valid() == before_branch);
            REQUIRE(reference.Failure() ==
                    (before_branch ? FailureKind::None : FailureKind::Dominance));
        }
}

TEST_CASE("public IR audit requires definitions on every Function entry path",
          "[ir][ir-audit][function-entry]") {
    using compile::audit::FailureKind;
    // A diamond with a root definition is valid. A definition on one arm,
    // or an independent external entry at the join, cannot supply every use.
    for (const unsigned shape : {0u, 1u, 2u}) {
        CAPTURE(shape);
        HIRBuilder builder{1, false, FeatureSet{}};
        auto* function = MakeSSAFunction(builder, shape != 1, shape == 2, false, true);
        std::vector<const Block*> blocks;
        const Block* root{};
        const Block* join{};
        for (const auto& member : function->GetHIRBlocksRPO()) {
            const auto* block = member.GetBlock();
            blocks.push_back(block);
            if (block->GetStartLocation() == Location{0x5b00}) root = block;
            if (block->GetStartLocation() == Location{0x5b30}) join = block;
        }
        REQUIRE(root);
        REQUIRE(join);
        compile::CompileUnit unit{function};
        unit.SealIR();
        const compile::audit::IRReference reference{blocks, function, function->MaxInstrCount()};
        REQUIRE(reference.Valid() == (shape == 0));
        REQUIRE(reference.Failure() ==
                (shape == 0 ? FailureKind::None : FailureKind::Dominance));
        if (shape == 0) {
            REQUIRE(reference.Owner(&root->GetInstList().front()) == root);
            REQUIRE(reference.Dominates(root, join));
            REQUIRE_FALSE(reference.Dominates(join, root));
        }
    }
}

TEST_CASE("public IR audit includes call return edges without inventing entry dominance",
          "[ir][ir-audit][call-return]") {
    using compile::audit::FailureKind;
    for (const bool external_resume : {false, true}) {
        CAPTURE(external_resume);
        HIRBuilder builder{1, false, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{0x8400}, Location{0x8420});
        auto* caller = function->GetCurrentBlock();
        const auto carried = function->LoadUniform<U64>(Uniform{0, ValueType::U64});
        const auto resume = function->LoadImm<U64>(Imm{u64{0x8410}});
        const Lambda callee{Imm{u64{0x9400}}};
        function->SetLocation(callee);
        function->CallReturn(resume, Imm{u64{0x8410}}, callee);
        builder.RegisterCallReturn(Location{0x8410});
        function->EndBlock(terminal::ReturnToDispatch{});
        builder.SetCurBlock(Location{0x8410});
        auto* returned = function->GetCurrentBlock();
        function->StoreUniform(Uniform{8, ValueType::U64}, carried);
        function->EndBlock(terminal::ReturnToHost{});
        if (external_resume) function->RegisterExternalEntryRoot(returned);
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        REQUIRE(caller->GetSuccessors().empty());
        REQUIRE(caller->GetCallReturnBlock() == returned);
        REQUIRE(returned->IsCallReturnBlock());
        std::vector<const Block*> blocks;
        for (const auto& member : function->GetHIRBlocksRPO()) blocks.push_back(member.GetBlock());
        compile::CompileUnit unit{function};
        unit.SealIR();
        const compile::audit::IRReference reference{blocks, function, function->MaxInstrCount()};
        REQUIRE(reference.Valid() == !external_resume);
        REQUIRE(reference.Failure() ==
                (external_resume ? FailureKind::Dominance : FailureKind::None));
        if (!external_resume) {
            REQUIRE(reference.Owner(carried.Def()) == caller->GetBlock());
            REQUIRE(reference.Dominates(caller->GetBlock(), returned->GetBlock()));
            REQUIRE_FALSE(reference.Dominates(returned->GetBlock(), caller->GetBlock()));
        }
    }
}

TEST_CASE("physical indexes require explicit preparation and follow placement journals",
          "[ir][allocation][contracts]") {
    compile::CompileWorkspace workspace;
    compile::SemanticFacts facts;
    compile::Allocation result{
            64, backend::GPRSMask{UINT32_MAX}, backend::FPRSMask{}, FeatureSet{}};
    compile::AllocationWriter writer{result, facts, workspace, UINT32_MAX};
    REQUIRE(writer.FindFixedGPRHomes() == nullptr);
    REQUIRE(writer.FindGPRPreservations() == nullptr);
    writer.PlanFixedGPRDefinition(2,
                                  {.home = 0,
                                   .transferred_home = 19,
                                   .end = 8,
                                   .transfer = 8,
                                   .last_use = 20,
                                   .preserved = true});
    writer.PlanFixedGPRDefinition(4, {.home = 0, .end = 30});
    writer.PlanFixedGPRDefinition(10, {.home = 22, .end = 25});
    const auto& homes = writer.PrepareFixedGPRHomes();
    const auto& preserves = writer.PrepareGPRPreservations();
    REQUIRE(homes.Locations().RefersToSameResult(writer));
    REQUIRE(preserves.Locations().RefersToSameResult(writer));
    const auto check = [&] {
        REQUIRE(&writer.PrepareFixedGPRHomes() == &homes);
        REQUIRE(&writer.PrepareGPRPreservations() == &preserves);
        for (const u16 home : {u16{0}, u16{19}, u16{22}})
            for (const u32 begin : {0u, 2u, 7u, 8u, 19u, 20u, 24u, 25u, 30u})
                for (const u32 end : {begin, begin + 1}) {
                    for (const u32 ignored : {UINT32_MAX, 2u, 4u}) {
                        bool conflict{};
                        for (u32 id = 0; id < writer.MapCount(); ++id) {
                            const auto* fixed = writer.FixedGPRDefinitionAt(id);
                            if (!fixed || id == ignored) continue;
                            conflict |= fixed->home == home && fixed->end > id && id <= end &&
                                        fixed->end > begin;
                            conflict |= fixed->transferred_home == home &&
                                        fixed->last_use > fixed->transfer &&
                                        fixed->transfer <= end && fixed->last_use > begin;
                        }
                        REQUIRE(homes.Conflicts(home, begin, end, ignored) == conflict);
                    }
                    bool conflict{};
                    for (u32 id = 0; id < writer.MapCount(); ++id) {
                        const auto* fixed = writer.FixedGPRDefinitionAt(id);
                        conflict |= fixed && fixed->preserved && fixed->home == home &&
                                    fixed->transfer > id && id <= end && fixed->transfer > begin;
                    }
                    REQUIRE(preserves.Conflicts(home, begin, end) == conflict);
                }
    };
    check();
    writer.PlanFixedGPRDefinition(4, {.home = 22, .end = 30});
    REQUIRE_FALSE(homes.Ready());
    REQUIRE(preserves.Ready());
    REQUIRE(writer.FindFixedGPRHomes() == nullptr);
    REQUIRE(writer.FindGPRPreservations() == &preserves);
    check();
    {
        compile::AllocationEdit edit{writer};
        writer.PlanFixedGPRDefinition(4, {.home = 19, .end = 30});
        check();
    }
    REQUIRE_FALSE(homes.Ready());
    REQUIRE_FALSE(preserves.Ready());
    REQUIRE(writer.FindFixedGPRHomes() == nullptr);
    REQUIRE(writer.FindGPRPreservations() == nullptr);
    REQUIRE(writer.FixedGPRDefinitionAt(4)->home == 22);
    check();
    writer.PlanFixedGPRDefinition(2,
                                  {.home = 0,
                                   .transferred_home = 19,
                                   .end = 9,
                                   .transfer = 9,
                                   .last_use = 20,
                                   .preserved = true});
    REQUIRE_FALSE(homes.Ready());
    REQUIRE_FALSE(preserves.Ready());
    check();
    Block next{0, Location{0x58f0}};
    next.SetTerminal(terminal::ReturnToHost{});
    writer.SetAllocationUnit(&next, nullptr);
    REQUIRE_FALSE(homes.Ready());
    REQUIRE_FALSE(preserves.Ready());
    check();
}

TEST_CASE("physical preservation windows match raw declarations across all homes",
          "[ir][allocation][contracts]") {
    compile::CompileWorkspace workspace;
    compile::SemanticFacts facts;
    compile::Allocation result{
            192, backend::GPRSMask{UINT32_MAX}, backend::FPRSMask{}, FeatureSet{}};
    compile::AllocationWriter writer{result, facts, workspace, UINT32_MAX};
    // Declaration order interleaves homes. Each home has overlapping and
    // touching prefixes, a separate window, an empty prefix and an ordinary
    // fixed declaration. The oracle below never uses the merged index.
    for (u16 home = 0; home < 32; ++home) {
        const u32 start = 31 - home;
        const auto declare = [&](u32 id, u32 transfer, bool preserved = true) {
            writer.PlanFixedGPRDefinition(id,
                                          {.home = home,
                                           .end = transfer,
                                           .transfer = transfer,
                                           .last_use = transfer,
                                           .width = 8,
                                           .preserved = preserved});
        };
        declare(start, start + 64);
        declare(start + 32, start + 48);
        declare(start + 64, start + 72);
        declare(start + 96, start + 104);
        declare(start + 128, start + 128);
        declare(start + 160, UINT32_MAX, false);
    }
    // A full-width end must survive sorting and merging without truncation.
    writer.PlanFixedGPRDefinition(191,
                                  {.home = 31,
                                   .end = UINT32_MAX,
                                   .transfer = UINT32_MAX,
                                   .last_use = UINT32_MAX,
                                   .width = 8,
                                   .preserved = true});
    const auto& windows = writer.PrepareGPRPreservations();
    REQUIRE(windows.Ready());
    for (u16 home = 0; home < 32; ++home) {
        const u32 start = 31 - home;
        for (const u32 begin : {0u,
                                start,
                                start + 31,
                                start + 32,
                                start + 39,
                                start + 40,
                                start + 63,
                                start + 64,
                                start + 71,
                                start + 72,
                                start + 95,
                                start + 96,
                                start + 103,
                                start + 104,
                                start + 128,
                                191u,
                                UINT32_MAX - 1,
                                UINT32_MAX})
            for (const u32 width : {0u, 1u, 32u}) {
                const u32 end = begin <= UINT32_MAX - width ? begin + width : UINT32_MAX;
                bool expected{};
                for (u32 id = 0; id < writer.MapCount(); ++id) {
                    const auto* fixed = writer.FixedGPRDefinitionAt(id);
                    expected |= fixed && fixed->preserved && fixed->home == home &&
                                fixed->transfer > id && id <= end && fixed->transfer > begin;
                }
                CAPTURE(home, begin, end);
                REQUIRE(windows.Conflicts(home, begin, end) == expected);
            }
    }
}

TEST_CASE("freezing unused physical results does not prepare conflict indexes",
          "[ir][allocation][contracts]") {
    compile::CompileWorkspace workspace;
    compile::SemanticFacts facts;
    compile::Allocation result{4, backend::GPRSMask{}, backend::FPRSMask{}, FeatureSet{}};
    compile::AllocationWriter writer{result, facts, workspace};
    writer.FreezeGPRLocations();
    REQUIRE(writer.GPRLocationsFrozen());
    REQUIRE(writer.FindFixedGPRHomes() == nullptr);
    REQUIRE(writer.FindGPRPreservations() == nullptr);
    // Explicit preparation can still inspect an immutable result. This is a
    // preparation API; emission has only AllocationView and frozen actions.
    REQUIRE_FALSE(writer.PrepareFixedGPRHomes().Conflicts(0, 0, 0));
    REQUIRE_FALSE(writer.PrepareGPRPreservations().Conflicts(0, 0, 0));
}

TEST_CASE("type views check canonical definitions and every nested use without a snapshot",
          "[ir][types][ssa]") {
    Inst definition{OpCode::LoadImm, ValueType::U64};
    definition.SetArgs(Imm{u64{7}});
    const Value source{&definition};
    REQUIRE_FALSE(Value{}.HasValidTypeView());
    REQUIRE_FALSE(Value{}.HasValidDataView());
    for (const auto type : {ValueType::U8, ValueType::S16, ValueType::U32, ValueType::U64})
        REQUIRE(Value{source}.SetCastType(type).HasValidTypeView());
    Inst call{OpCode::CallDynamic, ValueType::VOID};
    const auto parameters = [&](bool invalid_last) {
        Params params;
        for (u32 i = 0; i < 64; ++i)
            params.Push(Value{source}.SetCastType(invalid_last && i == 63
                    ? static_cast<ValueType>(255) : i % 2 ? ValueType::U8 : ValueType::U64));
        return params;
    };
    call.SetArgs(Lambda{source}, parameters(false));
    REQUIRE(call.HasValidResultType());
    REQUIRE(call.HasValidArguments());
    REQUIRE_FALSE(Value{&call}.SetCastType(ValueType::U64).HasValidTypeView());
    u32 uses{};
    REQUIRE(call.CheckArguments([&](Value value) {
        REQUIRE(value.Def() == &definition);
        REQUIRE(value.Type() == (uses == 0 || uses % 2 ? ValueType::U64 : ValueType::U8));
        ++uses;
    }));
    REQUIRE(uses == 65);
    for (const auto type : {ValueType::VOID, static_cast<ValueType>(255)}) {
        CAPTURE(type);
        definition.SetReturn(type);
        REQUIRE_FALSE(definition.HasValidResultType());
        // A legal-looking cast must not hide an invalid canonical definition.
        REQUIRE_FALSE(Value{source}.SetCastType(ValueType::U64).HasValidTypeView());
        uses = 0;
        REQUIRE_FALSE(call.CheckArguments([&](Value) { ++uses; }));
        REQUIRE(uses == 0);
    }
    definition.SetReturn(ValueType::U64);
    call.SetArg(1, parameters(true));
    uses = 0;
    REQUIRE_FALSE(call.CheckArguments([&](Value) { ++uses; }));
    REQUIRE(uses == 64);  // The bad 64th Params entry is visited and rejected.
    Inst store{OpCode::StoreUniform};
    store.SetArgs(Uniform{0, ValueType::S64}, source);
    REQUIRE(store.HasValidArguments());
    for (const auto type : {ValueType::U8, ValueType::VOID, static_cast<ValueType>(255)}) {
        store.SetArg(0, Uniform{0, type});
        REQUIRE_FALSE(store.HasValidArguments());
    }
    Inst local{OpCode::DefineLocal};
    local.SetArgs(Local{0, ValueType::U32});
    REQUIRE(local.HasValidArguments());
    local.SetArg(0, Local{0, static_cast<ValueType>(255)});
    REQUIRE_FALSE(local.HasValidArguments());
    store.SetArg(0, Uniform{0, ValueType::U64});
    store.SetReturn(ValueType::U64);
    REQUIRE(Value{&store}.HasValidTypeView());
    REQUIRE_FALSE(Value{&store}.HasValidDataView());
    REQUIRE_FALSE(store.HasValidResultType());
    store.SetReturn(ValueType::VOID);
    REQUIRE(store.HasValidResultType());
    for (const bool named : {false, true}) {
        CAPTURE(named);
        Inst rewritten{OpCode::LoadImm, ValueType::U64};
        rewritten.SetArgs(Imm{u64{3}});
        rewritten.Reset();
        if (named)
            rewritten.Nop();
        else
            rewritten.SetInst(OpCode::Nop);
        REQUIRE(rewritten.ReturnType() == ValueType::VOID);
        REQUIRE(rewritten.HasValidResultType());
        rewritten.SetReturn(ValueType::S64);
        rewritten.Reset();
        if (named)
            rewritten.StoreUniform(Uniform{0, ValueType::U64}, source);
        else
            rewritten.SetInst(OpCode::StoreUniform, Uniform{0, ValueType::U64}, source);
        REQUIRE(rewritten.ReturnType() == ValueType::VOID);
        REQUIRE(rewritten.HasValidResultType());
        REQUIRE(rewritten.HasValidArguments());
        rewritten.SetReturn(ValueType::S32);
        rewritten.Reset();
        if (named)
            rewritten.LoadImm(Imm{u64{7}});
        else
            rewritten.SetInst(OpCode::LoadImm, Imm{u64{7}});
        REQUIRE(rewritten.ReturnType() == ValueType::S32);
        REQUIRE(rewritten.HasValidResultType());
    }
    for (const bool whole_function : {false, true}) {
        CAPTURE(whole_function);
        HIRBuilder builder;
        auto* function = builder.AppendFunction(Location{0x5800});
        auto* block = function->EditBlock(function->GetCurrentBlock()).GetBlock();
        auto* nop = block->AppendInst(OpCode::Nop);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        nop->SetReturn(ValueType::U64);
        compile::SemanticFacts facts;
        facts.BeginInput(whole_function ? nullptr : block, whole_function ? function : nullptr);
        REQUIRE_FALSE(facts.PrepareSSAValidation());
        nop->SetReturn(ValueType::VOID);
        REQUIRE(facts.PrepareSSAValidation());
    }
    Inst branch{OpCode::Goto, ValueType::U8};
    branch.SetArgs(Value{source}.SetCastType(ValueType::U8));
    REQUIRE(Value{&branch}.HasValidTypeView());
    REQUIRE_FALSE(Value{&branch}.HasValidDataView());
    Inst label{OpCode::BindLabel};
    label.SetArgs(Value{&branch});
    REQUIRE(label.HasValidArguments());
    REQUIRE(label.HasValidResultType());
    label.SetReturn(ValueType::U64);
    REQUIRE(label.HasValidResultType());
    REQUIRE(Value{&label}.HasValidTypeView());
    REQUIRE_FALSE(Value{&label}.HasValidDataView());
    for (const bool named : {false, true}) {
        CAPTURE(named);
        Inst binding{OpCode::LoadImm, ValueType::U64};
        binding.SetArgs(Imm{u64{0}});
        binding.Reset();
        if (named)
            binding.BindLabel(Value{&branch});
        else
            binding.SetInst(OpCode::BindLabel, Value{&branch});
        REQUIRE(binding.ReturnType() == ValueType::U64);
        REQUIRE(binding.HasValidResultType());
        REQUIRE(binding.HasValidArguments());
        REQUIRE_FALSE(Value{&binding}.HasValidDataView());
    }
    Inst reader{OpCode::Add, ValueType::U64};
    reader.SetArgs(source, Operand{Value{&branch}});
    REQUIRE_FALSE(reader.HasValidArguments());
    for (const auto op : {OpCode::Goto, OpCode::NotGoto}) {
        Inst lowered_branch{op, ValueType::VOID};
        lowered_branch.SetArgs(Value{source}.SetCastType(ValueType::U8));
        REQUIRE(lowered_branch.HasValidResultType());
        REQUIRE(lowered_branch.HasValidArguments());
        REQUIRE_FALSE(Value{&lowered_branch}.SetCastType(ValueType::U8).HasValidDataView());
        Inst lowered_reader{OpCode::Add, ValueType::U64};
        lowered_reader.SetArgs(source, Operand{Value{&lowered_branch}.SetCastType(ValueType::U8)});
        REQUIRE_FALSE(lowered_reader.HasValidArguments());
        lowered_branch.SetReturn(static_cast<ValueType>(255));
        REQUIRE_FALSE(lowered_branch.HasValidResultType());
    }
    // Defaults and the explicit low-level proof contract are separate.
    Inst predicate{OpCode::TestZero, ValueType::U32};
    predicate.SetArgs(source);
    REQUIRE(predicate.HasValidResultType());
}

TEST_CASE("argument signatures check complete operands and unused physical slots",
          "[ir][types][signature]") {
    static_assert(meta::ValidArgCount(meta::kArgs_CallLambda, 1));
    static_assert(meta::ValidArgCount(meta::kArgs_CallLambda, 4));
    static_assert(!meta::ValidArgCount(meta::kArgs_CallLambda, 0));
    static_assert(!meta::ValidArgCount(meta::kArgs_CallLambda, 5));
    static_assert(meta::ValidArgCount(meta::kArgs_Not, 1));
    static_assert(meta::ValidArgCount(meta::kArgs_Not, 2));
    static_assert(!meta::ValidArgCount(meta::kArgs_Not, 0));
    static_assert(!meta::ValidArgCount(meta::kArgs_Add, 1));
    Inst definition{OpCode::LoadImm, ValueType::U64};
    definition.SetArgs(Imm{u64{7}});
    const Value source{&definition};
    Inst operation{OpCode::Not, ValueType::U64};
    operation.SetArgs(source);
    REQUIRE(operation.HasValidArguments());
    operation.SetArg(2, source);  // An absent Operand cannot retain a data slot.
    REQUIRE_FALSE(operation.HasValidArguments());
    operation.DestroyArg(2);
    operation.SetArg(1, Operand{source, source});
    REQUIRE(operation.HasValidArguments());
    std::vector<u8> slots;
    operation.ForEachValueUse([&](Value value, u8 slot) {
        REQUIRE(value.Def() == &definition);
        slots.push_back(slot);
    });
    REQUIRE((slots == std::vector<u8>{0, 2, 3}));
    operation.SetArg(2, Cond::EQ);
    REQUIRE_FALSE(operation.HasValidArguments());
    operation.SetArg(2, source);
    operation.SetArg(3, Flags::Carry);
    REQUIRE_FALSE(operation.HasValidArguments());
    operation.SetArg(3, Void{nullptr});
    REQUIRE(operation.HasValidArguments());
    operation.SetArg(2, Void{nullptr});
    REQUIRE_FALSE(operation.HasValidArguments());  // Operand left is required.
    operation.SetArg(2, source);
    operation.SetArg(1, Operand::Op{static_cast<OperandOp::Type>(255)});
    REQUIRE_FALSE(operation.HasValidArguments());
    operation.SetArg(1, Operand::Op{OperandOp::PlusExt, 64});
    REQUIRE_FALSE(operation.HasValidArguments());
    operation.SetArg(1, Operand::Op{OperandOp::PlusExt, 63});
    REQUIRE(operation.HasValidArguments());
    operation.SetArg(4, Imm{u32{1}});
    REQUIRE_FALSE(operation.HasValidArguments());
    operation.DestroyArg(4);
    REQUIRE(operation.HasValidArguments());
    Inst marker{OpCode::UniformBarrier};
    REQUIRE(marker.HasValidArguments());
    marker.SetArg(4, source);
    REQUIRE_FALSE(marker.HasValidArguments());
    Inst condition{OpCode::CondSet, ValueType::U8};
    condition.SetArgs(Cond::NV);
    REQUIRE(condition.HasValidArguments());
    condition.SetArg(0, static_cast<Cond>(255));
    REQUIRE_FALSE(condition.HasValidArguments());
    Inst flags{OpCode::ClearFlags};
    flags.SetArgs(Flags::All | Flags::Positive);
    REQUIRE(flags.HasValidArguments());
    flags.SetArg(0, static_cast<Flags>(u64{1} << 63));
    REQUIRE_FALSE(flags.HasValidArguments());
    REQUIRE_FALSE(Inst{}.HasValidArguments());
}

TEST_CASE("call signatures preserve immediates and reject holes and malformed nested data",
          "[ir][types][signature]") {
    Inst definition{OpCode::LoadImm, ValueType::U64};
    definition.SetArgs(Imm{u64{7}});
    const Value source{&definition};
    const Lambda direct{DataClass{Imm{u64{1}}}, HelperCallTraits{
            .uniform = UniformEffectId::None, .host_fp = HostFpEffect::FPCRTransparent}};
    REQUIRE(direct.IsImm());
    for (u8 count = 0; count <= 3; ++count) {
        CAPTURE(count);
        Inst call{OpCode::CallLambda, ValueType::VOID};
        call.SetArgs(direct);
        for (u8 slot = 1; slot <= count; ++slot) {
            if (slot == 2) call.SetArg(slot, source);
            else call.SetArg(slot, Imm{u64{slot}});
        }
        REQUIRE(call.HasValidArguments());
        call.SetArg(4, Imm{u64{9}});
        REQUIRE_FALSE(call.HasValidArguments());
        call.DestroyArg(4);
        if (count) {
            call.SetArg(count, Void{nullptr});
            REQUIRE(call.HasValidArguments());
            call.SetArg(count + 1, source);
            REQUIRE_FALSE(call.HasValidArguments());
        }
    }
    Inst call{OpCode::CallLambda, ValueType::VOID};
    call.SetArgs(direct);
    Params ignored;
    ignored.Push(source);
    call.SetArg(1, std::move(ignored));
    REQUIRE_FALSE(call.HasValidArguments());  // EmitCallLambda never consumes Params.
    for (const auto op : {OpCode::CallDynamic, OpCode::CallLocation}) {
        CAPTURE(op);
        Inst dynamic{op, ValueType::VOID};
        dynamic.SetArgs(direct);
        REQUIRE_FALSE(dynamic.HasValidArguments());
        Params params;
        params.Push(source);
        params.Push(Imm{u64{3}});
        params.Push(source);
        dynamic.SetArg(1, std::move(params));
        REQUIRE(dynamic.HasValidArguments());
        std::vector<u8> slots;
        dynamic.ForEachValueUse([&](Value value, u8 slot) {
            REQUIRE(value.Def() == &definition);
            slots.push_back(slot);
        });
        REQUIRE((slots == std::vector<u8>{Inst::params_arg_index, Inst::params_arg_index}));
        Params invalid;
        invalid.Push(Imm{u64{0}, static_cast<ValueType>(255)});
        dynamic.SetArg(1, std::move(invalid));
        REQUIRE_FALSE(dynamic.HasValidArguments());
        dynamic.SetArg(1, Params{});
        REQUIRE(dynamic.HasValidArguments());  // An empty list is still a required argument.
        dynamic.SetArg(0, Lambda{});
        REQUIRE_FALSE(dynamic.HasValidArguments());
        dynamic.SetArg(0, Lambda{Imm{u64{1}, static_cast<ValueType>(255)}});
        REQUIRE_FALSE(dynamic.HasValidArguments());
        dynamic.SetArg(0, direct);
        REQUIRE(dynamic.HasValidArguments());
    }
}

TEST_CASE("scalar contracts bound full immediates and preserve explicit result widths",
          "[ir][types][scalar][signature]") {
    static_assert(IsScalarIntegerType(ValueType::S64, 64));
    static_assert(IsScalarIntegerType(ValueType::U64, 64));
    static_assert(!IsScalarIntegerType(ValueType::V64, 64));
    static_assert(!IsScalarIntegerType(static_cast<ValueType>(255), 64));
    Block block{0, Location{0x7b80}};
    const auto source = block.LoadImm(Imm{u64{0xfedcba9876543210}});
    for (const auto op : {OpCode::BitExtract, OpCode::BitClear, OpCode::BitInsert}) {
        Inst inst{op, ValueType::U64};
        const auto set = [&](u64 lsb, u64 width) {
            if (op == OpCode::BitInsert) inst.SetArgs(source, source, Imm{lsb}, Imm{width});
            else inst.SetArgs(source, Imm{lsb}, Imm{width});
        };
        for (const auto [lsb, width] : std::array<std::pair<u64, u64>, 5>{
                     {{0, 0}, {63, 0}, {63, 1}, {0, 64}, {1, 63}}}) {
            CAPTURE(op, lsb, width);
            set(lsb, width);
            REQUIRE(inst.HasValidArguments());
        }
        for (const auto [lsb, width] : std::array<std::pair<u64, u64>, 6>{
                     {{64, 0}, {1, 64}, {0, 65}, {UINT64_MAX, 0}, {0, UINT64_MAX},
                      {UINT64_MAX, UINT64_MAX}}}) {
            CAPTURE(op, lsb, width);
            set(lsb, width);
            REQUIRE_FALSE(inst.HasValidArguments());
        }
        for (const auto type : {ValueType::V8, ValueType::V32, ValueType::V64, ValueType::V128}) {
            for (const u8 offset : {u8{0}, u8{1}}) {
                set(0, 0);
                inst.SetArg((op == OpCode::BitInsert ? 2 : 1) + offset, Imm{u64{0}, type});
                REQUIRE_FALSE(inst.HasValidArguments());
            }
        }
    }
    for (const auto op : {OpCode::ByteSwap, OpCode::Crc32c}) {
        Inst inst{op, ValueType::U64};
        for (const u64 width : {u64{0}, u64{8}, u64{16}, u64{32}, u64{64}, u64{65},
                                (u64{1} << 32) | 32, UINT64_MAX}) {
            if (op == OpCode::ByteSwap) inst.SetArgs(source, Imm{width});
            else inst.SetArgs(source, source, Imm{width});
            REQUIRE(inst.HasValidArguments() ==
                    ((width == 8 && op == OpCode::Crc32c) || width == 16 ||
                     width == 32 || width == 64));
        }
    }
    for (const auto op : {OpCode::ZeroExtend32, OpCode::ZeroExtend32To64, OpCode::ZeroExtend64,
                          OpCode::SignExtend, OpCode::PopCount, OpCode::CountLeadingZeros32,
                          OpCode::CountTrailingZeros32, OpCode::CountLeadingZeros64,
                          OpCode::CountTrailingZeros64}) {
        const bool word = op == OpCode::CountLeadingZeros32 || op == OpCode::CountTrailingZeros32;
        const bool wide = op == OpCode::CountLeadingZeros64 || op == OpCode::CountTrailingZeros64;
        Inst inst{op, ValueType::U16};  // Explicit widths need not equal the default token.
        for (const auto type : {ValueType::U8, ValueType::S16, ValueType::U32, ValueType::S32,
                                ValueType::U64, ValueType::S64, ValueType::V32, ValueType::V128}) {
            inst.SetArgs(Value{source}.SetCastType(type));
            const bool valid = word ? type == ValueType::U32 || type == ValueType::S32
                             : wide ? type == ValueType::U64 || type == ValueType::S64
                                    : IsScalarIntegerType(type);
            REQUIRE(inst.HasValidArguments() == valid);
        }
        inst.SetArgs(Value{source}.SetCastType(word ? ValueType::U32 : ValueType::U64));
        inst.SetReturn(ValueType::V64);
        REQUIRE_FALSE(inst.HasValidArguments());
    }
    for (const auto type : {ValueType::VOID, ValueType::U8, ValueType::U16, ValueType::U32,
                            ValueType::U64, ValueType::S8, ValueType::S16, ValueType::S32,
                            ValueType::S64, ValueType::V8, ValueType::V16, ValueType::V32,
                            ValueType::V64, ValueType::V128, ValueType::V256,
                            static_cast<ValueType>(255)}) {
        for (const u32 bits : {0u, 7u, 8u, 16u, 24u, 32u, 64u, 128u}) {
            const bool expected = IsScalarIntegerType(type) && GetValueSizeByte(type) * 8 == bits;
            REQUIRE(IsScalarIntegerType(type, bits) == expected);
        }
    }
    const auto target = backend::arm64::BuilderTarget();
    for (const auto type : {ValueType::VOID, ValueType::V8, ValueType::V64, ValueType::V256,
                            static_cast<ValueType>(255)}) {
        REQUIRE_FALSE(compile::FoldIntegerImmediate(OpCode::Add, type, Imm{u64{1}},
                                                    compile::FlagsIntent::None(), target));
        REQUIRE_FALSE(compile::FoldWidthValue(OpCode::ZeroExtend64, type, source,
                                              compile::FlagsIntent::None()));
    }
}

TEST_CASE("integer conversion folds observe raw input views and explicit result widths",
          "[ir][types][scalar][fold]") {
    static_assert(GetScalarIntegerBits(ValueType::S8) == 8);
    static_assert(GetScalarIntegerBits(ValueType::S64) == 64);
    static_assert(GetScalarIntegerBits(ValueType::VOID) == 0);
    static_assert(GetScalarIntegerBits(static_cast<ValueType>(255)) == 0);
    static_assert(IntegerConversionInputBits(OpCode::ZeroExtend32, 64) == 32);
    static_assert(IntegerConversionInputBits(OpCode::ZeroExtend32, 7) == 7);
    static_assert(IntegerConversionInputBits(OpCode::SignExtend, 64) == 64);
    static_assert(IntegerConversionInputBits(OpCode::BitCast, 8) == 0);
    static_assert(IntegerConversionInputBits(OpCode::ZeroExtend64, 0) == 0);
    static_assert(IntegerConversionInputBits(OpCode::ZeroExtend64, 65) == 0);
    for (const auto source_type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                   ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64})
    for (const u64 raw : {u64{0}, u64{1}, u64{0xdeadbeef00000080}, u64{0xabcdef0080008000},
                          u64{0x100000000}, u64{0x8000000080000000}, u64{0xdeadbeef12345678}, u64{UINT64_MAX}}) {
        Block block{0, Location{0x7ba0}};
        const auto value = block.LoadImm(Imm{raw});
        for (const bool alias : {false, true}) {
            const auto input = alias ? block.BitCast(source_type, value)
                                     : Value{value}.SetCastType(source_type);
            for (const auto op : {OpCode::ZeroExtend32, OpCode::ZeroExtend32To64,
                                  OpCode::ZeroExtend64, OpCode::SignExtend})
            for (const auto result_type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                           ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64}) {
                CAPTURE(source_type, op, result_type, raw, alias);
                const u32 source_bits = GetValueSizeByte(source_type) * 8;
                const u32 copied = op == OpCode::ZeroExtend32 || op == OpCode::ZeroExtend32To64
                        ? std::min(source_bits, 32u) : source_bits;
                u64 expected{};
                for (u32 bit = 0; bit < GetValueSizeByte(result_type) * 8; ++bit) {
                    const bool on = bit < copied ? (raw >> bit) & 1
                            : op == OpCode::SignExtend && ((raw >> (copied - 1)) & 1);
                    if (on) expected |= u64{1} << bit;
                }
                Inst candidate{op, result_type};
                candidate.SetArgs(input);
                REQUIRE(candidate.HasValidArguments());
                const auto folded = compile::FoldWidthValue(op, result_type, input,
                                                            compile::FlagsIntent::None());
                REQUIRE(folded);
                REQUIRE(folded->Get() == expected);
            }
        }
    }
}

TEST_CASE("Params adoption closes retained write aliases across sealing",
          "[ir][params][ownership][seal]") {
    IntrusivePtr<Block> block{new Block{0, Location{0x7b80}}};
    const auto source = block->LoadImm(Imm{u64{17}});
    Params params;
    params.Push(Value{source}.SetCastType(ValueType::U16));
    params.Push(Imm{u64{9}});
    const auto retained = params.begin();
    const auto call = block->CallDynamic(ValueType::VOID, Lambda{Imm{u64{1}}}, std::move(params));
    Inst* writer{};
    RewriteFixture{block.get(), false}.Get().Change(block.get(), call.Def(),
            [&](Inst& instruction) { writer = &instruction; });
    block->SetTerminal(terminal::ReturnToHost{});
    compile::CompileUnit unit{block};
    unit.SealIR();
    auto snapshot = *retained;
    snapshot.data = DataClass{Imm{u64{99}}};
    params.Push(Imm{u64{88}});
    params.Destroy();
    const auto installed = call.Def()->GetArg<Params>(1);
    REQUIRE(std::distance(installed.begin(), installed.end()) == 2);
    REQUIRE((*installed.begin()).data.value.Def() == source.Def());
    REQUIRE((*installed.begin()).data.value.Type() == ValueType::U16);
    REQUIRE(source.Def()->GetUses(false) == 1);
    Params replacement;
    replacement.Push(source);
    REQUIRE_THROWS_AS(writer->SetArg(1, std::move(replacement)), std::logic_error);
    REQUIRE(std::distance(replacement.begin(), replacement.end()) == 1);
    REQUIRE(source.Def()->GetUses(false) == 1);
    REQUIRE((*call.Def()->GetArg<Params>(1).begin()).data.value.Type() == ValueType::U16);
}

TEST_CASE("raw HIR append rejects an owned instruction before adopting it",
          "[ir][params][ownership][hir]") {
    for (const bool fast : {false, true}) {
        Block foreign{0, Location{0x7c00}};
        const auto source = foreign.LoadImm(Imm{u64{23}});
        Inst* existing{};
        RewriteFixture{&foreign, false}.Get().Change(&foreign, source.Def(),
                [&](Inst& instruction) { existing = &instruction; });
        HIRBuilder builder{1, false, fast};
        auto* function = builder.AppendFunction(Location{0x7c40});
        auto& body = function->EditBlock(function->GetCurrentBlock());
        REQUIRE_THROWS_AS(body.AppendInst(existing), std::logic_error);
        REQUIRE(existing->GetBlock() == &foreign);
        REQUIRE(foreign.GetInstList().size() == 1);
        REQUIRE(body.GetInstList().empty());
        REQUIRE(existing->GetArg<Imm>(0).Get() == 23);
    }
}

TEST_CASE("HIR updates use records for mixed call arguments and repeated Params values",
          "[ir][types][signature][hir]") {
    for (const bool fast : {false, true}) {
        CAPTURE(fast);
        HIRBuilder builder{1, true, fast};
        auto* function = builder.AppendFunction(Location{0x7b00}, Location{0x7b01});
        const auto source = function->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        auto* body = function->EditBlock(function->GetCurrentBlock()).GetBlock();
        function->CallLambda(ValueType::VOID, Lambda{source}, Imm{u64{5}}, source, Imm{u64{7}});
        const auto* call = &body->GetInstList().back();
        Params initial;
        initial.Push(Value{source}.SetCastType(ValueType::U8));
        initial.Push(Imm{u64{13}});
        initial.Push(Value{source}.SetCastType(ValueType::S32));
        function->CallDynamic(ValueType::VOID, Lambda{Imm{u64{1}}}, std::move(initial));
        REQUIRE(initial.begin() == initial.end());
        const auto* dynamic = &body->GetInstList().back();
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        const auto* record = function->GetHIRValues()[source.Id()];
        REQUIRE(source.Def()->GetUses(false) == 4);
        REQUIRE(std::count_if(record->GetUses().begin(), record->GetUses().end(),
                [&](const HIRUse& use) {
                    return use.inst == dynamic && use.arg_idx == HIRUse::USE_FUNC_CALL;
                }) == 2);
        std::vector<ValueType> initial_views;
        dynamic->ForEachValue([&](Value value) { initial_views.push_back(value.Type()); });
        REQUIRE((initial_views == std::vector<ValueType>{ValueType::U8, ValueType::S32}));
        compile::CompileWorkspace workspace;
        compile::RewriteContext rewrite{function, workspace};
        rewrite.SetArg(body, dynamic, 1, Params{});
        const auto uses = [&] {
            std::vector<u8> slots;
            for (const auto& use : record->GetUses()) {
                REQUIRE(use.inst == call);
                slots.push_back(use.arg_idx);
            }
            std::sort(slots.begin(), slots.end());
            return slots;
        };
        REQUIRE((uses() == std::vector<u8>{0, 2}));
        REQUIRE(source.Def()->GetUses(false) == 2);
        rewrite.SetArg(body, call, 2, Imm{u64{9}});
        REQUIRE((uses() == std::vector<u8>{0}));
        REQUIRE(source.Def()->GetUses(false) == 1);
        rewrite.SetArg(body, call, 0, Lambda{Imm{u64{1}}});
        REQUIRE(uses().empty());
        REQUIRE(source.Def()->GetUses(false) == 0);
        Params params;
        params.Push(source);
        params.Push(Imm{u64{11}});
        params.Push(Value{source}.SetCastType(ValueType::U16));
        const auto retained = params.begin();
        auto snapshot = *retained;
        call = dynamic;
        rewrite.SetArg(body, call, 1, std::move(params));
        REQUIRE(params.begin() == params.end());
        // Pre-install iterators and data snapshots expose copies, including
        // after ownership is transferred into an instruction.
        snapshot.data = DataClass{Imm{u64{99}}};
        auto second_snapshot = *retained;
        second_snapshot.data.value.SetCastType(ValueType::V128);
        REQUIRE((*call->GetArg<Params>(1).begin()).data.value == source);
        params.Push(Imm{u64{77}});
        params.Destroy();
        REQUIRE(std::distance(call->GetArg<Params>(1).begin(), call->GetArg<Params>(1).end()) == 3);
        std::vector<ValueType> views;
        call->ForEachValue([&](Value value) { views.push_back(value.Type()); });
        REQUIRE((views == std::vector<ValueType>{ValueType::U64, ValueType::U16}));
        REQUIRE((uses() == std::vector<u8>{HIRUse::USE_FUNC_CALL, HIRUse::USE_FUNC_CALL}));
        REQUIRE(source.Def()->GetUses(false) == 2);
        rewrite.SetArg(body, call, 1, Params{});
        REQUIRE(uses().empty());
        REQUIRE(source.Def()->GetUses(false) == 0);
    }
}

TEST_CASE("SSA and ARM64 admission reject invalid data views and implicit roots",
          "[ir][types][ssa][admission]") {
    for (const bool function_mode : {false, true})
        for (const unsigned scenario : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u,
                                       12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u,
                                       21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u}) {
            CAPTURE(function_mode, scenario);
            HIRBuilder hir{1, false, FeatureSet{}};
            auto* function = function_mode
                    ? hir.AppendFunction(Location{0x7a00}, Location{0x7a01}) : nullptr;
            IntrusivePtr<Block> block;
            if (!function) block = new Block{0, Location{0x7a00}};
            compile::IrBuilder factory = function ? compile::IrBuilder{&hir}
                                                  : compile::IrBuilder{block.get()};
            const auto source = factory.LoadUniform(Uniform{0, ValueType::U64});
            const auto sum = factory.Add(ValueType::U64, source, Operand{Imm{u64{1}}});
            const auto call = factory.CallLambda(ValueType::U64, Lambda{Imm{u64{1}}});
            Value scalar;
            if (scenario == 23) scalar = factory.ByteSwap(ValueType::U64, source, Imm{32u});
            else if (scenario == 24) scalar = factory.Crc32c(source, source, Imm{32u});
            else if (scenario == 25) scalar = factory.CountLeadingZeros64(source);
            else if (scenario == 26)
                scalar = factory.CountTrailingZeros32(Value{source}.SetCastType(ValueType::U32));
            else if (scenario == 27) scalar = factory.SignExtend(ValueType::U64, source);
            else if (scenario == 28)
                scalar = factory.BitInsert(ValueType::U64, source, source, Imm{0u}, Imm{8u});
            else if (scenario >= 21)
                scalar = factory.BitExtract(ValueType::U64, source, Imm{0u}, Imm{8u});
            factory.StoreUniform(Uniform{16, ValueType::U64}, sum);
            auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock()
                                  : block.get();
            auto* store = &body->GetInstList().back();
            if (function) {
                hir.ReturnToHost();
                function->ComputeRPO();
                function->IdByRPO();
            } else body->SetTerminal(terminal::ReturnToHost{});
            compile::CompileWorkspace workspace;
            auto rewrite = function ? compile::RewriteContext{function, workspace}
                                    : compile::RewriteContext{body, workspace};
            if (scenario == 1 || scenario == 2)
                rewrite.Change(body, source.Def(), [&](Inst& inst) {
                    inst.SetReturn(scenario == 1 ? ValueType::VOID : static_cast<ValueType>(255));
                });
            if (scenario == 3)
                rewrite.SetArg(body, sum.Def(), 1,
                               Operand{Value{source}.SetCastType(static_cast<ValueType>(255))});
            if (scenario == 4) {
                rewrite.SetArg(body, sum.Def(), 0, call);
                rewrite.Change(body, call.Def(), [](Inst& inst) { inst.SetReturn(ValueType::VOID); });
            }
            if (scenario == 5 || scenario == 6)
                rewrite.SetArg(body, store, 0, Uniform{16, scenario == 5
                        ? static_cast<ValueType>(255) : ValueType::U8});
            if (scenario == 7) {
                body->SetTerminal(terminal::If{BOOL{call},
                        terminal::ReturnToHost{}, terminal::ReturnToHost{}});
                rewrite.Change(body, call.Def(), [](Inst& inst) { inst.SetReturn(ValueType::VOID); });
            } else if (scenario == 8) {
                body->SetTerminal(terminal::Switch{
                        Value{source}.SetCastType(static_cast<ValueType>(255)), {}});
            }
            if (scenario == 12) rewrite.SetArg(body, sum.Def(), 2, Cond::EQ);
            if (scenario == 13) rewrite.SetArg(body, sum.Def(), 1, Void{nullptr});
            if (scenario == 14) rewrite.SetArg(body, sum.Def(), 4, source);
            if (scenario == 15) rewrite.SetArg(body, call.Def(), 0, Lambda{});
            if (scenario == 16) {
                Params params;
                params.Push(source);
                rewrite.SetArg(body, call.Def(), 1, std::move(params));
            }
            if (scenario == 17) rewrite.SetArg(body, source.Def(), 0, Imm{u64{1}});
            if (scenario == 18) rewrite.SetArg(body, call.Def(), 3, source);
            if (scenario == 19) rewrite.SetArg(body, call.Def(), 4, Imm{u64{1}});
            if (scenario == 20) rewrite.SetArg(body, sum.Def(), 3, Flags::Carry);
            if (scenario == 21) rewrite.SetArg(body, scalar.Def(), 1, Imm{u64{64}});
            if (scenario == 22) rewrite.SetArg(body, scalar.Def(), 2, Imm{UINT64_MAX});
            if (scenario == 23)
                rewrite.SetArg(body, scalar.Def(), 1, Imm{(u64{1} << 32) | 32});
            if (scenario == 24) rewrite.SetArg(body, scalar.Def(), 2, Imm{0u});
            if (scenario == 25)
                rewrite.SetArg(body, scalar.Def(), 0, Value{source}.SetCastType(ValueType::U32));
            if (scenario == 26) rewrite.SetArg(body, scalar.Def(), 0, source);
            if (scenario == 27 || scenario == 28)
                rewrite.SetArg(body, scalar.Def(), scenario == 28 ? 1 : 0,
                               Value{source}.SetCastType(ValueType::V64));
            if (scenario == 29)
                rewrite.Change(body, scalar.Def(), [](Inst& inst) { inst.SetReturn(ValueType::V64); });
            Config config{.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
                          .backend_isa = kArm64, .uniform_buffer_size = 64};
            config.global_opts = Optimizations::None;
            if (scenario >= 21)
                config.global_opts = Optimizations::DeadCodeRemove | Optimizations::ConstantFolding;
            config.stack_alignment = 16;
            backend::AddressSpace space{config};
            const auto module = space.GetDefaultModule();
            if (scenario == 10) {
                rewrite.Change(body, store, [](Inst& inst) { inst.SetReturn(ValueType::U64); });
                body->SetTerminal(terminal::If{Value{store}.SetCastType(ValueType::U8),
                                               terminal::ReturnToHost{}, terminal::ReturnToHost{}});
            }
            std::array<const Inst*, 1> anchors{scenario == 11 ? nullptr : call.Def()};
            {
                compile::CompileUnit unit = function ? compile::CompileUnit{function}
                                                      : compile::CompileUnit{block};
                if (scenario == 9 || scenario == 11) {
                    unit.SetLoopHoistMetadata(body, {call.Def(), anchors, 1, 0});
                    if (scenario == 9)
                        rewrite.Change(body, call.Def(), [](Inst& inst) { inst.SetReturn(ValueType::VOID); });
                    REQUIRE_FALSE(backend::arm64::SupportsIR(body));
                }
                body->ReIdInstr();
                if (function) { function->ComputeRPO(); function->IdByRPO(); }
                unit.SealIR();
                compile::SemanticFacts facts;
                facts.BeginInput(function ? nullptr : body, function);
                (void)facts.PrepareValueLiveness();
                REQUIRE(facts.FindSSAValidation() == std::optional<bool>{scenario == 0});
                REQUIRE(facts.PrepareSSAValidation() == (scenario == 0));
                REQUIRE(facts.FindFunctionCFG() == nullptr);
                REQUIRE(facts.PrepareSSAValidation() == (scenario == 0));
                if (scenario == 9 || scenario == 11) {
                    auto optimized = compile::OptimizedUnit::FromFinalIR(std::move(unit), FeatureSet{});
                    REQUIRE(optimized.Facts().FindSSAValidation() == std::optional<bool>{false});
                    REQUIRE_FALSE(optimized.PrepareFacts().PrepareSSAValidation());
                }
            }
            // Preflight must reject malformed types even when optimizations
            // could otherwise delete the producer. Implicit roots are checked
            // by the sealed proof before physical allocation.
            if (scenario != 9 && scenario != 11) {
                const auto prepared = function ? backend::arm64::PrepareFunction(module, function)
                                                : backend::arm64::PrepareBlock(module, block);
                REQUIRE(prepared.has_value() == (scenario == 0));
            }
            REQUIRE(space.GetCodeCache(Location{0x7a00}) == nullptr);
            if (scenario >= 21) {
                alignas(backend::State) std::array<u8, sizeof(backend::State) + 64> storage{};
                auto* state = new (storage.data()) backend::State{};
                backend::interp::Interpreter interpreter{*state, body};
                REQUIRE(interpreter.Run() == HaltReason::IllegalCode);
            }
        }
}

TEST_CASE("sealed base facts validate local SSA once without a Function graph",
          "[ir][ssa][admission]") {
    IntrusivePtr<Block> block{new Block{0, Location{0x5a00}}};
    const auto source = block->LoadUniform<U64>(Uniform{0, ValueType::U64});
    const auto value = block->Add<U64>(source, Operand{Imm{u64{1}}});
    Params params;
    params.Push(source);
    params.Push(source);
    block->CallDynamic(ValueType::VOID, Lambda{value}, std::move(params));
    block->SetTerminal(terminal::If{Value{value}.SetCastType(ValueType::U8),
                                    terminal::ReturnToHost{},
                                    terminal::ReturnToHost{}});
    block->ReIdInstr();
    compile::SemanticFacts facts;
    {
        compile::CompileUnit unit{block};
        std::array<const Inst*, 1> anchors{source.Def()};
        unit.SetLoopHoistMetadata(block.get(), {source.Def(), anchors, 1, 0});
        unit.SealIR();
        facts.BeginInput(block.get(), nullptr);
        REQUIRE_FALSE(facts.FindSSAValidation().has_value());
        (void)facts.PrepareValueLiveness();
        REQUIRE(facts.FindSSAValidation() == std::optional<bool>{true});
        REQUIRE(facts.PrepareSSAValidation());
        REQUIRE(facts.PrepareSSAValidation());
        REQUIRE_FALSE(facts.RequiresIncomingValues(block.get()));
        REQUIRE(facts.FindFunctionCFG() == nullptr);
    }
    Block empty{0, Location{0x5a10}};
    empty.SetTerminal(terminal::ReturnToHost{});
    facts.BeginInput(&empty, nullptr);
    REQUIRE_FALSE(facts.FindSSAValidation().has_value());
    REQUIRE(facts.PrepareSSAValidation());
    REQUIRE_FALSE(facts.RequiresIncomingValues(&empty));
    REQUIRE(facts.FindFunctionCFG() == nullptr);
}

TEST_CASE("Function SSA checks joins loops external roots and terminal values",
          "[ir][ssa][admission]") {
    for (const bool root : {false, true})
        for (const bool external : {false, true})
            for (const unsigned shape : {0u, 1u, 2u}) {
                CAPTURE(root, external, shape);
                HIRBuilder builder{1, true, FeatureSet{}};
                auto* function = MakeSSAFunction(builder, root, external, shape == 1, shape == 2);
                compile::CompileUnit unit{function};
                unit.SealIR();
                compile::SemanticFacts facts{function};
                const bool expected = root && !external;
                (void)facts.PrepareValueLiveness();
                REQUIRE(facts.FindSSAValidation() == std::optional<bool>{expected});
                REQUIRE(facts.PrepareSSAValidation() == expected);
                const auto* graph = facts.FindFunctionCFG();
                REQUIRE(graph);
                REQUIRE(facts.PrepareSSAValidation() == expected);
                REQUIRE(facts.FindFunctionCFG() == graph);
                if (expected) {
                    REQUIRE_FALSE(facts.RequiresIncomingValues(
                            function->GetHIRBlocksRPO().front().GetBlock()));
                    for (const auto& member : function->GetHIRBlocksRPO()) {
                        if (member.GetBlock()->GetStartLocation() == Location{0x5b30})
                            REQUIRE(facts.RequiresIncomingValues(member.GetBlock()));
                    }
                }
            }
}

TEST_CASE("SSA checks local joins implicit roots and values exported to HIR successors",
          "[ir][ssa][admission][cfg]") {
    for (const bool function_mode : {false, true})
        for (const bool inverted : {false, true})
            for (const unsigned shape : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}) {
                if (shape == 5 && !function_mode) continue;
                CAPTURE(function_mode, inverted, shape);
                HIRBuilder builder{1, true, FeatureSet{}};
                auto* function = builder.AppendFunction(Location{0x5700}, Location{0x5730});
                auto* first = &function->EditBlock(function->GetCurrentBlock());
                Value source;
                if (shape == 0) source = function->LoadUniform<U64>(Uniform{8, ValueType::U64});
                const auto condition = function->LoadUniform<U8>(Uniform{0, ValueType::U8});
                const auto skip = inverted ? function->NotGoto(BOOL{condition})
                                           : function->Goto(BOOL{condition});
                if (shape == 6) source = function->LoadUniform<U64>(Uniform{8, ValueType::U64});
                Value inner;
                if (shape == 6 || shape == 7) inner = function->NotGoto(BOOL{condition});
                if (shape != 0 && shape != 6)
                    source = function->LoadUniform<U64>(Uniform{8, ValueType::U64});
                if (inner.Defined()) function->BindLabel(inner);
                if (shape == 1 || shape == 6 || shape == 7)
                    function->StoreUniform(Uniform{16, ValueType::U64}, source);
                function->BindLabel(skip);
                if (shape == 0 || shape == 2)
                    function->StoreUniform(Uniform{16, ValueType::U64}, source);
                if (shape == 5) {
                    builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x5720}}));
                    function->StoreUniform(Uniform{16, ValueType::U64}, source);
                }
                if (shape == 3)
                    function->EndBlock(terminal::If{source.SetCastType(ValueType::U8),
                                                    terminal::ReturnToHost{},
                                                    terminal::ReturnToHost{}});
                else
                    function->EndBlock(terminal::ReturnToHost{});
                function->EndFunction();
                function->ComputeRPO();
                function->IdByRPO();
                if (!function_mode) first->GetBlock()->ReIdInstr();
                auto unit = function_mode ? compile::CompileUnit{function}
                                          : compile::CompileUnit{first};
                std::array<const Inst*, 1> anchors{source.Def()};
                if (shape == 4)
                    unit.SetLoopHoistMetadata(first->GetBlock(), {source.Def(), anchors, 1, 0});
                const bool expected = shape == 0 || shape == 1 || shape == 6;
                compile::SemanticFacts facts;
                facts.BeginInput(function_mode ? nullptr : first->GetBlock(),
                                 function_mode ? function : nullptr);
                REQUIRE(facts.PrepareSSAValidation() == expected);
                unit.SealIR();
                facts.BeginInput(function_mode ? nullptr : first->GetBlock(),
                                 function_mode ? function : nullptr);
                (void)facts.PrepareValueLiveness();
                REQUIRE(facts.FindSSAValidation() == std::optional<bool>{expected});
                REQUIRE(facts.PrepareSSAValidation() == expected);
                REQUIRE(facts.FindFunctionCFG() == nullptr);
            }
}

TEST_CASE("ARM64 rejects malformed local labels and skipped definitions without an audit",
          "[ir][ssa][admission][cfg]") {
    const char* raw = GetRawSvmConfigEnvForTest("SVM_VERIFY_PLAN");
    const auto saved = raw ? std::optional<std::string>{raw} : std::nullopt;
    const auto restore = ScopeExit([&] {
        if (saved)
            SetSvmConfigEnvForTest("SVM_VERIFY_PLAN", saved->c_str(), 1);
        else
            UnsetSvmConfigEnvForTest("SVM_VERIFY_PLAN");
    });
    SetSvmConfigEnvForTest("SVM_VERIFY_PLAN", "0", 1);
    Config config{.loc_start = 0,
                  .loc_end = 0x10000,
                  .enable_jit = true,
                  .backend_isa = kArm64,
                  .uniform_buffer_size = 64};
    backend::AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    for (const unsigned shape : {0u, 1u, 2u, 3u, 4u, 5u}) {
        CAPTURE(shape);
        IntrusivePtr<Block> block{new Block{0, Location{0x5800}}};
        const auto condition = block->LoadUniform<U8>(Uniform{0, ValueType::U8});
        const auto skip = block->Goto(BOOL{condition});
        if (shape == 1 || shape == 4) {
            block->BindLabel(skip);
            if (shape == 1)
                block->BindLabel(skip);
            else
                block->MoveBefore(&block->GetInstList().back(), skip.Def());
        } else if (shape == 2) {
            block->BindLabel(condition);
        } else if (shape == 3) {
            block->StoreUniform(Uniform{8, ValueType::U8}, condition);
            compile::CompileWorkspace workspace;
            compile::RewriteContext{block.get(), workspace}.SetArg(
                    block.get(), &block->GetInstList().back(), 1, skip);
            block->BindLabel(skip);
        } else if (shape == 5) {
            const auto guarded = block->LoadUniform<U64>(Uniform{8, ValueType::U64});
            block->BindLabel(skip);
            block->StoreUniform(Uniform{16, ValueType::U64}, guarded);
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        REQUIRE_FALSE(compile::SemanticFacts{block.get()}.PrepareSSAValidation());
        REQUIRE_FALSE(backend::arm64::PrepareBlock(module, block));
        REQUIRE(space.GetCodeCache(Location{0x5800}) == nullptr);
    }
    HIRBuilder builder{1, true, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{0x5900}, Location{0x5930});
    const auto condition = function->LoadUniform<U8>(Uniform{0, ValueType::U8});
    const auto skip = function->NotGoto(BOOL{condition});
    const auto guarded = function->LoadUniform<U64>(Uniform{8, ValueType::U64});
    function->BindLabel(skip);
    builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x5920}}));
    function->StoreUniform(Uniform{16, ValueType::U64}, guarded);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    REQUIRE_FALSE(backend::arm64::PrepareFunction(module, function));
    REQUIRE(space.GetCodeCache(Location{0x5900}) == nullptr);
}

TEST_CASE("native nested local branches preserve dominated values on both paths",
          "[ir][ssa][admission][native]") {
    Config config{.loc_start = 0,
                  .loc_end = 0x10000,
                  .enable_jit = true,
                  .backend_isa = kArm64,
                  .uniform_buffer_size = 64};
    config.region_edges = false;
    for (const bool inverted : {false, true}) {
        CAPTURE(inverted);
        backend::AddressSpace space{config};
        Runtime runtime{&space};
        const auto module = space.GetDefaultModule();
        HIRBuilder builder{1, true, backend::ResolveFeatureSet(module->GetModuleConfig())};
        auto* function = builder.AppendFunction(Location{0x5600}, Location{0x5630});
        const auto carried = function->LoadUniform<U64>(Uniform{0, ValueType::U64});
        const auto condition = function->LoadUniform<U8>(Uniform{16, ValueType::U8});
        const auto outer =
                inverted ? function->NotGoto(BOOL{condition}) : function->Goto(BOOL{condition});
        const auto guarded = function->LoadUniform<U64>(Uniform{8, ValueType::U64});
        function->StoreUniform(Uniform{24, ValueType::U64}, guarded);
        const auto inner =
                function->Goto(BOOL{function->LoadUniform<U8>(Uniform{17, ValueType::U8})});
        function->StoreUniform(Uniform{40, ValueType::U64},
                               function->LoadUniform<U64>(Uniform{32, ValueType::U64}));
        function->BindLabel(inner);
        function->StoreUniform(Uniform{48, ValueType::U64}, guarded);
        function->BindLabel(outer);
        builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x5620}}));
        function->StoreUniform(Uniform{56, ValueType::U64}, carried);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        REQUIRE(TranslateIR(module, function));
        for (const u8 outer_condition : {u8{0}, u8{1}})
            for (const u8 inner_condition : {u8{0}, u8{1}}) {
                CAPTURE(outer_condition, inner_condition);
                auto uniforms = runtime.GetUniformBuffer();
                std::fill(uniforms.begin(), uniforms.end(), 0);
                const auto put = [&](u32 offset, u64 value) {
                    std::memcpy(uniforms.data() + offset, &value, sizeof(value));
                };
                const auto get = [&](u32 offset) {
                    u64 value{};
                    std::memcpy(&value, uniforms.data() + offset, sizeof(value));
                    return value;
                };
                put(0, 37);
                put(8, 73);
                put(32, 99);
                uniforms[16] = outer_condition;
                uniforms[17] = inner_condition;
                runtime.SetLocation(0x5600);
                REQUIRE(runtime.Run() == HaltReason::CallHost);
                const bool skipped = inverted ? outer_condition == 0 : outer_condition != 0;
                REQUIRE(get(24) == (skipped ? 0 : 73));
                REQUIRE(get(40) == (skipped || inner_condition ? 0 : 99));
                REQUIRE(get(48) == (skipped ? 0 : 73));
                REQUIRE(get(56) == 37);
            }
    }
}

TEST_CASE("ARM64 SSA admission remains mandatory with the optional audit disabled",
          "[ir][ssa][admission]") {
    const char* raw = GetRawSvmConfigEnvForTest("SVM_VERIFY_PLAN");
    const auto saved = raw ? std::optional<std::string>{raw} : std::nullopt;
    const auto restore = ScopeExit([&] {
        if (saved)
            SetSvmConfigEnvForTest("SVM_VERIFY_PLAN", saved->c_str(), 1);
        else
            UnsetSvmConfigEnvForTest("SVM_VERIFY_PLAN");
    });
    SetSvmConfigEnvForTest("SVM_VERIFY_PLAN", "0", 1);
    Config config{.loc_start = 0,
                  .loc_end = 0x10000,
                  .enable_jit = true,
                  .backend_isa = kArm64,
                  .uniform_buffer_size = 64};
    config.region_edges = true;
    for (const unsigned scenario : {0u, 1u, 2u}) {
        CAPTURE(scenario);
        backend::AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        HIRBuilder builder{1, true, FeatureSet{}};
        auto* function = MakeSSAFunction(builder, scenario != 1, scenario == 2, false, false);
        auto prepared = backend::arm64::PrepareFunction(module, function);
        REQUIRE(prepared.has_value() == (scenario == 0));
        if (prepared) {
            REQUIRE(prepared->ReadyForEmission(module));
            const auto* certificate = prepared->Actions().Certificate();
            REQUIRE(certificate);
            REQUIRE(certificate->ssa_valid);
            auto missing_proof = *certificate;
            missing_proof.ssa_valid = false;
            REQUIRE_FALSE(missing_proof.MatchesUnit(
                    nullptr,
                    prepared->Function(),
                    prepared->Allocation(),
                    prepared->Actions(),
                    config,
                    backend::ResolveFeatureSet(module->GetModuleConfig())));
            missing_proof = *certificate;
            missing_proof.entry_state_valid = false;
            REQUIRE_FALSE(missing_proof.MatchesUnit(
                    nullptr,
                    prepared->Function(),
                    prepared->Allocation(),
                    prepared->Actions(),
                    config,
                    backend::ResolveFeatureSet(module->GetModuleConfig())));
            backend::arm64::FunctionCodeObjectEmitter emitter{module, std::span{&*prepared, 1}};
            REQUIRE(emitter.Emit());
            REQUIRE(emitter.CurrentBufferSize() != 0);
        }
        REQUIRE(space.GetCodeCache(Location{0x5b00}) == nullptr);
    }
    for (const bool foreign : {false, true}) {
        CAPTURE(foreign);
        backend::AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block{0, Location{0x5c00}}};
        Block outsider{0, Location{0x5c10}};
        const auto source = block->LoadUniform<U64>(Uniform{0, ValueType::U64});
        const auto value = block->Add<U64>(source, Operand{Imm{u64{1}}});
        const auto other = foreign ? outsider.LoadUniform<U64>(Uniform{8, ValueType::U64})
                                   : block->LoadUniform<U64>(Uniform{8, ValueType::U64});
        block->StoreUniform(Uniform{16, ValueType::U64}, value);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::CompileWorkspace workspace;
        {
            compile::RewriteContext edit{block.get(), workspace, nullptr, false};
            edit.SetArg(block.get(), value.Def(), 0, other);
        }
        REQUIRE_FALSE(backend::arm64::PrepareBlock(module, block));
        REQUIRE(space.GetCodeCache(Location{0x5c00}) == nullptr);
    }
}

static u64 InterruptSSAEntry(u64 address, u64 armed) {
    if (armed) reinterpret_cast<Runtime*>(static_cast<uintptr_t>(address))->SignalInterrupt();
    return 0;
}

TEST_CASE("Function value relays retain local state and external resumes decode canonical state",
          "[ir][ssa][function-entry][native]") {
#if defined(__aarch64__)
    for (const bool region : {false, true})
        for (const bool linked : {false, true}) {
            CAPTURE(region, linked);
            Config config{.loc_start = 0,
                          .loc_end = 0x10000,
                          .enable_jit = true,
                          .backend_isa = kArm64,
                          .uniform_buffer_size = 64};
            config.stack_alignment = 16;
            config.region_edges = region;
            config.global_opts = {};
            if (linked) config.global_opts = Optimizations::BlockLink;
            backend::AddressSpace space{config};
            Runtime runtime{&space};
            const auto module = space.GetDefaultModule();
            const auto put = [&](u32 offset, u64 value) {
                std::memcpy(runtime.GetUniformBuffer().data() + offset, &value, sizeof(value));
            };
            const auto get = [&](u32 offset) {
                u64 value{};
                std::memcpy(&value, runtime.GetUniformBuffer().data() + offset, sizeof(value));
                return value;
            };
            HIRBuilder builder{1, true, backend::ResolveFeatureSet(module->GetModuleConfig())};
            auto* function = builder.AppendFunction(Location{0x5f00}, Location{0x5f30});
            auto* root = function->GetCurrentBlock();
            const auto carried = function->LoadUniform<U64>(Uniform{0, ValueType::U64});
            function->StoreUniform(Uniform{16, ValueType::U64}, carried);
            function->StoreUniform(Uniform{0, ValueType::U64},
                                   function->LoadImm<U64>(Imm{u64{99}}));
            const auto address =
                    function->LoadImm<U64>(Imm{u64{reinterpret_cast<uintptr_t>(&runtime)}});
            const auto armed = function->LoadUniform<U64>(Uniform{56, ValueType::U64});
            function->CallLambda<U64>(
                    Lambda{Imm{u64{reinterpret_cast<uintptr_t>(&InterruptSSAEntry)}}},
                    address,
                    armed);
            function->AdvancePC(Imm{u64{1}});
            auto* relay = builder.LinkBlock(terminal::LinkBlock{Location{0x5f10}});
            builder.SetCurBlock(relay);
            function->StoreUniform(Uniform{32, ValueType::U64},
                                   function->LoadImm<U64>(Imm{u64{17}}));
            function->AdvancePC(Imm{u64{1}});
            auto* consumer = builder.LinkBlock(terminal::LinkBlock{Location{0x5f20}});
            builder.SetCurBlock(consumer);
            function->StoreUniform(Uniform{8, ValueType::U64}, carried);
            function->AdvancePC(Imm{u64{1}});
            const auto repeat = function->LoadUniform<U8>(Uniform{56, ValueType::U8});
            function->AddEdge(consumer, relay);
            function->EndBlock(terminal::If{
                    repeat, terminal::LinkBlock{Location{0x5f10}}, terminal::ReturnToHost{}});
            function->EndFunction();
            function->ComputeRPO();
            function->IdByRPO();
            {
                compile::CompileUnit unit{function};
                unit.SealIR();
                compile::SemanticFacts facts{function};
                REQUIRE_FALSE(facts.RequiresIncomingValues(root->GetBlock()));
                REQUIRE(facts.RequiresIncomingValues(relay->GetBlock()));
                REQUIRE(facts.RequiresIncomingValues(consumer->GetBlock()));
            }
            REQUIRE(TranslateIR(module, function));
            REQUIRE(space.GetCodeCache(Location{0x5f00}));
            REQUIRE_FALSE(space.GetCodeCache(Location{0x5f10}));
            REQUIRE_FALSE(space.GetCodeCache(Location{0x5f20}));
            put(0, 42);
            put(56, 0);
            runtime.SetLocation(0x5f00);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            REQUIRE(get(8) == 42);
            REQUIRE(get(0) == 99);

            // An unrelated external path has canonical guest homes and no
            // activation for the Function's old captured register value.
            put(8, 0);
            put(16, 77);
            runtime.SetLocation(0x5f20);
            REQUIRE(runtime.Run() == HaltReason::CodeMiss);
            REQUIRE(get(8) == 0);
            IntrusivePtr<Block> decoded_consumer{new Block{0, Location{0x5f20}}};
            decoded_consumer->StoreUniform(
                    Uniform{8, ValueType::U64},
                    decoded_consumer->LoadUniform<U64>(Uniform{16, ValueType::U64}));
            decoded_consumer->AdvancePC(Imm{u64{1}});
            decoded_consumer->SetTerminal(
                    terminal::If{decoded_consumer->LoadUniform<U8>(Uniform{56, ValueType::U8}),
                                 terminal::LinkBlock{Location{0x5f10}},
                                 terminal::ReturnToHost{}});
            REQUIRE(module->Push(decoded_consumer.get()));
            auto* consumer_code = TranslateIR(module, decoded_consumer);
            REQUIRE(consumer_code);
            space.PushCodeCache(0x5f20, consumer_code);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            REQUIRE(get(8) == 77);

            put(0, 131);
            put(8, 0);
            put(32, 0);
            put(56, 1);
            runtime.SetLocation(0x5f00);
            REQUIRE(runtime.Run() == HaltReason::Signal);
            REQUIRE(runtime.GetLocation() == 0x5f10);
            REQUIRE(get(16) == 131);
            // Forward internal edges may defer observation to the loop cut.
            REQUIRE((get(32) == 0 || get(32) == 17));
            put(56, 0);
            runtime.ClearInterrupt();
            REQUIRE(runtime.Run() == HaltReason::CodeMiss);
            IntrusivePtr<Block> decoded_relay{new Block{0, Location{0x5f10}}};
            decoded_relay->StoreUniform(Uniform{32, ValueType::U64},
                                        decoded_relay->LoadImm<U64>(Imm{u64{17}}));
            decoded_relay->AdvancePC(Imm{u64{1}});
            decoded_relay->SetTerminal(terminal::LinkBlock{Location{0x5f20}});
            REQUIRE(module->Push(decoded_relay.get()));
            auto* relay_code = TranslateIR(module, decoded_relay);
            REQUIRE(relay_code);
            space.PushCodeCache(0x5f10, relay_code);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            REQUIRE(get(32) == 17);
            REQUIRE(get(8) == 131);
            REQUIRE(get(0) == 99);
        }
#else
    SUCCEED("native Function entry execution requires AArch64");
#endif
}

TEST_CASE("guest calls return through canonical dispatch when continuation values are unavailable",
          "[ir][ssa][function-entry][native]") {
#if defined(__aarch64__)
    const char* raw = GetRawSvmConfigEnvForTest("SVM_FLAGS_REGS");
    const auto saved = raw ? std::optional<std::string>{raw} : std::nullopt;
    const auto restore = ScopeExit([&] {
        if (saved)
            SetSvmConfigEnvForTest("SVM_FLAGS_REGS", saved->c_str(), 1);
        else
            UnsetSvmConfigEnvForTest("SVM_FLAGS_REGS");
    });
    SetSvmConfigEnvForTest("SVM_FLAGS_REGS", "1", 1);
    for (const bool indirect : {false, true}) {
        CAPTURE(indirect);
        Config config{.loc_start = 0,
                      .loc_end = 0x10000,
                      .enable_jit = true,
                      .backend_isa = kArm64,
                      .uniform_buffer_size = 64};
        config.stack_alignment = 16;
        config.region_edges = true;
        config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
        backend::AddressSpace space{config};
        Runtime runtime{&space};
        const auto module = space.GetDefaultModule();
        const auto features = backend::ResolveFeatureSet(module->GetModuleConfig());
        HIRBuilder target_builder{1, true, features};
        auto* target = target_builder.AppendFunction(Location{0x6e00}, Location{0x6e04});
        target->StoreUniform(Uniform{24, ValueType::U64}, target->LoadImm<U64>(Imm{u64{777}}));
        target->AdvancePC(Imm{u64{1}});
        auto* tail = target_builder.LinkBlock(terminal::LinkBlock{Location{0x6e02}});
        target_builder.SetCurBlock(tail);
        target->SetLocation(Lambda{target->LoadUniform<U64>(Uniform{48, ValueType::U64})});
        target->EndBlock(terminal::PopRSBHint{});
        target->EndFunction();
        REQUIRE(TranslateIR(module, target));

        HIRBuilder builder{1, true, features};
        auto* source = builder.AppendFunction(Location{0x5e00}, Location{0x5e24});
        const auto carried = source->LoadUniform<U64>(Uniform{0, ValueType::U64});
        source->StoreUniform(Uniform{16, ValueType::U64}, carried);
        const auto resume = source->LoadImm<U64>(Imm{u64{0x5e20}});
        source->StoreUniform(Uniform{48, ValueType::U64}, resume);
        const auto callee = indirect ? Lambda{source->LoadUniform<U64>(Uniform{32, ValueType::U64})}
                                     : Lambda{Imm{u64{0x6e00}}};
        source->SetLocation(callee);
        source->CallReturn(resume, Imm{u64{0x5e20}}, callee);
        builder.RegisterCallReturn(Location{0x5e20});
        source->EndBlock(terminal::ReturnToDispatch{});
        builder.SetCurBlock(Location{0x5e20});
        source->StoreUniform(Uniform{40, ValueType::U64}, carried);
        source->AdvancePC(Imm{u64{1}});
        source->EndBlock(terminal::ReturnToHost{});
        source->EndFunction();
        REQUIRE(TranslateIR(module, source));
        REQUIRE_FALSE(space.GetCodeCache(Location{0x5e20}));
        const u64 input = 42, target_pc = 0x6e00;
        std::memcpy(runtime.GetUniformBuffer().data(), &input, 8);
        std::memcpy(runtime.GetUniformBuffer().data() + 32, &target_pc, 8);
        runtime.SetLocation(0x5e00);
        REQUIRE(runtime.Run() == HaltReason::CodeMiss);
        REQUIRE(runtime.GetLocation() == 0x5e20);
        REQUIRE(runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty);
        u64 witness{};
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 24, 8);
        REQUIRE(witness == 777);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 40, 8);
        REQUIRE(witness == 0);
        IntrusivePtr<Block> decoded_return{new Block{0, Location{0x5e20}}};
        decoded_return->StoreUniform(Uniform{40, ValueType::U64},
                                     decoded_return->LoadUniform<U64>(Uniform{16, ValueType::U64}));
        decoded_return->AdvancePC(Imm{u64{1}});
        decoded_return->SetTerminal(terminal::ReturnToHost{});
        REQUIRE(module->Push(decoded_return.get()));
        auto* return_code = TranslateIR(module, decoded_return);
        REQUIRE(return_code);
        space.PushCodeCache(0x5e20, return_code);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 40, 8);
        REQUIRE(witness == input);
        runtime.SetLocation(0x5e00);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 40, 8);
        REQUIRE(witness == input);
    }
#else
    SUCCEED("native guest-call entry execution requires AArch64");
#endif
}

TEST_CASE("shared semantic facts preserve local demand and stable consumer views",
          "[ir][allocation][facts]") {
    for (const bool consumers_first : {false, true}) {
        CAPTURE(consumers_first);
        IntrusivePtr<Block> block{new Block(0, Location{0x5500})};
        const auto source = block->LoadImm(ValueType::U64, Imm{u64{41}});
        const auto producer = block->Add(ValueType::U64, source, Operand{Imm{u64{7}}});
        const auto alias = block->BitCast(ValueType::U64, producer);
        const auto result = block->Add(ValueType::U64, alias, Operand{source});
        block->SaveFlags(producer, Flags::Carry);
        block->SaveFlags(result, Flags::Parity);
        block->StoreUniform(Uniform{0, ValueType::U64}, result);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::SemanticFacts facts{block.get()};
        const auto& readonly = facts;
        REQUIRE(readonly.FindUses(block.get()) == nullptr);
        REQUIRE(readonly.FindIndex(block.get()) == nullptr);
        auto& roster = facts.PrepareIndex(block.get());
        REQUIRE(readonly.FindIndex(block.get()) == &roster);
        REQUIRE_FALSE(roster.UsesReady());
        REQUIRE_FALSE(roster.ResolvedUsesReady());
        REQUIRE(roster.Contains(source.Def()));
        REQUIRE(roster.Instructions().size() == block->GetInstList().size());
        REQUIRE_THROWS_AS(roster.Uses(producer.Def()), std::logic_error);
        REQUIRE_THROWS_AS(roster.ResolvedUses(producer.Def()), std::logic_error);
        if (!consumers_first) {
            (void)facts.PrepareBlockUses(block.get());
            REQUIRE(readonly.FindUses(block.get()) == nullptr);
        }
        auto& graph = facts.PrepareUses(block.get());
        REQUIRE(&graph == &roster);
        REQUIRE(graph.UsesReady());
        REQUIRE(readonly.FindUses(block.get()) == &graph);
        REQUIRE_FALSE(graph.ResolvedUsesReady());
        const auto direct = graph.Uses(producer.Def());
        const auto& ends = facts.PrepareBlockUses(block.get());
        const compile::BlockUseEnds reference{block.get(), block->MaxInstrId()};
        for (const auto& inst : block->GetInstList())
            REQUIRE(ends[inst.Id()] == reference[inst.Id()]);
        graph.PrepareResolvedUses();
        REQUIRE(graph.ResolvedUsesReady());
        REQUIRE(graph.Uses(producer.Def()).data() == direct.data());
        REQUIRE(&facts.PrepareBlockUses(block.get()) == &ends);
        REQUIRE(&facts.PrepareUses(block.get()) == &graph);
        REQUIRE(graph.PseudoFlags().For(producer.Def()).set == Flags::Carry);
        REQUIRE(graph.PseudoFlags().SavesParity(result.Def()));
        REQUIRE(facts.PrepareFlags()[producer.Id()].pseudo_count == 1);
        REQUIRE_FALSE(facts.HasSpillUses());
    }

    HIRBuilder builder{1, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{0x5600}, Location{0x5800});
    const auto source = function->LoadImm<U64>(Imm{u64{3}});
    const auto producer = function->Add<U64>(source, Operand{Imm{u64{7}}});
    function->SaveFlags(producer, Flags::Carry);
    const auto* first = function->GetCurrentBlock()->GetBlock();
    for (u32 i = 0; i < 6; ++i) {
        auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x5610 + i * 16}});
        builder.SetCurBlock(next);
        function->AdvancePC(Imm{u64{1}});
    }
    function->SaveFlags(producer, Flags::Parity);
    const auto* last = function->GetCurrentBlock()->GetBlock();
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    compile::SemanticFacts facts{function};
    const auto& first_ends = facts.PrepareBlockUses(first);
    const auto* first_graph = &facts.PrepareUses(first);
    for (const auto& block : function->GetHIRBlocksRPO()) {
        (void)facts.PrepareBlockUses(block.GetBlock());
        (void)facts.PrepareUses(block.GetBlock());
    }
    REQUIRE(&facts.PrepareBlockUses(first) == &first_ends);
    REQUIRE(&facts.PrepareUses(first) == first_graph);
    REQUIRE(facts.PrepareFlags()[producer.Id()].requested == (Flags::Carry | Flags::Parity));
    REQUIRE(first_graph->PseudoFlags().For(producer.Def()).set == Flags::Carry);
    REQUIRE(facts.PrepareUses(last).PseudoFlags().For(producer.Def()).Null());
    REQUIRE_FALSE(facts.HasSpillUses());
    IntrusivePtr<Block> foreign{new Block(0, Location{0x5900})};
    REQUIRE_THROWS_AS(facts.PrepareUses(foreign.get()), std::logic_error);
}

TEST_CASE("shared flags retain block ownership across index moves and input resets",
          "[ir][allocation][facts][flags]") {
    for (const bool values_first : {false, true}) {
        CAPTURE(values_first);
        HIRBuilder builder{1, false, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{0x5680}, Location{0x5780});
        std::array<const Block*, 8> members{};
        std::array<Value, 8> producers{};
        for (u32 i = 0; i < members.size(); ++i) {
            members[i] = function->GetCurrentBlock()->GetBlock();
            const auto input = function->LoadUniform<U64>(Uniform{i * 8, ValueType::U64});
            producers[i] = function->Add<U64>(input, Operand{Imm{u64{1}}});
            function->SaveFlags(producers[i], Flags::Carry);
            function->SaveFlags(Value{producers[i]}.SetCastType(ValueType::U8), Flags::Parity);
            function->BranchOnlyFlags(producers[i], Flags::NZ);
            if (i + 1 < members.size())
                builder.SetCurBlock(builder.LinkBlock(
                        terminal::LinkBlock{Location{0x5690 + i * 16}}));
        }
        function->SaveFlags(producers.front(), Flags::Overflow);
        function->EndBlock(terminal::ReturnToHost{});
        auto* disconnected = function->AppendBlock(Location{0x5770});
        function->SetCurBlock(disconnected);
        const auto unused = function->LoadUniform<U64>(Uniform{0, ValueType::U64});
        function->SaveFlags(unused, Flags::All);
        function->SaveFlags(producers.front(), Flags::AuxiliaryCarry);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        compile::CompileUnit unit{function};
        unit.SealIR();
        compile::SemanticFacts facts{function};
        if (values_first) (void)facts.PrepareValueLiveness();
        const auto& flags = facts.PrepareFlags();
        const auto* unit_words = flags.data();
        const auto local_mask = Flags::Carry | Flags::Parity | Flags::NZ;
        REQUIRE(flags[producers.front().Id()].requested == (local_mask | Flags::Overflow));
        REQUIRE(flags[producers.front().Id()].pseudo_count == 4);
        const u16* first_words = nullptr;
        for (u32 i = 0; i < members.size(); ++i) {
            auto& index = facts.PrepareIndex(members[i]);
            const auto packed = index.PseudoFlags().View();
            REQUIRE(packed.For(producers[i].Def()).set == local_mask);
            REQUIRE(packed.For(producers[i].Def()).branch_only);
            REQUIRE(packed.SavesParity(producers[i].Def()));
            REQUIRE(flags[producers[i].Id()].has_save);
            REQUIRE(flags[producers[i].Id()].branch_only);
            if (i) {
                REQUIRE(packed.For(producers.front().Def()).Null());
                REQUIRE(flags[producers[i].Id()].pseudo_count == 3);
            } else {
                first_words = packed.words;
            }
            REQUIRE_FALSE(index.UsesReady());
        }
        REQUIRE(facts.PrepareIndex(disconnected->GetBlock()).PseudoFlags().Empty());
        REQUIRE(facts.FindSummary(disconnected->GetBlock())->InstructionCount() != 0);
        (void)facts.PrepareValueLiveness();
        REQUIRE(facts.PrepareFlags().data() == unit_words);
        REQUIRE(facts.PrepareIndex(members.front()).PseudoFlags().View().words == first_words);
        REQUIRE(facts.PrepareIndex(members.front()).PseudoFlags().For(producers.front().Def()).set ==
                local_mask);
        REQUIRE_FALSE(facts.PrepareIndex(members.front()).UsesReady());
        REQUIRE_FALSE(facts.HasSpillUses());
    }
    Block first{0, Location{0x5780}}, second{0, Location{0x5790}};
    const auto value = first.LoadUniform<U64>(Uniform{0, ValueType::U64});
    first.SaveFlags(value, Flags::Carry);
    first.ReIdInstr();
    second.LoadUniform<U64>(Uniform{0, ValueType::U64});
    second.ReIdInstr();
    compile::SemanticFacts facts{&first};
    REQUIRE(facts.PrepareFlags()[value.Id()].pseudo_count == 1);
    REQUIRE(facts.PrepareIndex(&first).PseudoFlags().For(value.Def()).set == Flags::Carry);
    facts.BeginInput(&second, nullptr);
    REQUIRE(facts.FindIndex(&first) == nullptr);
    REQUIRE(facts.PrepareFlags().empty());
    REQUIRE(facts.PrepareIndex(&second).PseudoFlags().Empty());
    facts.BeginInput(&first, nullptr);
    REQUIRE(facts.PrepareFlags()[value.Id()].pseudo_count == 1);
    REQUIRE(facts.PrepareIndex(&first).PseudoFlags().For(value.Def()).set == Flags::Carry);
}

TEST_CASE("narrow flags workspace keeps proofs local and releases failed borrows",
          "[ir][allocation][facts][narrow-flags-workspace]") {
    namespace arm64 = backend::arm64;
    const auto initial = SnapshotWorkspaceStorage();
    const auto row_kind = static_cast<size_t>(WorkspaceBufferKind::InstructionRows);
    {
        compile::CompileWorkspace workspace;
        HIRBuilder builder{1, false, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{0x57a0}, Location{0x57c0});
        const auto immediate = function->LoadImm<U32>(Imm{7u});
        builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x57b0}}));
        const auto narrow = function->LoadUniform<U8>(Uniform{0, ValueType::U8});
        const auto comparison = function->Sub<U8>(narrow, Operand{immediate});
        function->SaveFlags(comparison, Flags::Carry);
        auto* compare_block = function->GetCurrentBlock()->GetBlock();
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        compile::BlockAnalysisIndex compare_index;
        compare_index.Build(compare_block, nullptr, true);
        BitVector compare_coverage(function->MaxInstrCount());
        arm64::IntegerFlagsInputs compare_inputs{
                compare_coverage, compare_index, compare_index.PseudoFlags().View()};
        const auto compare_only =
                arm64::LowerNarrowIntegerFlags(compare_index, workspace, compare_inputs);
        REQUIRE(compare_only.Compare(comparison.Id()));
        REQUIRE(compare_only.Covered(immediate.Id()));
        REQUIRE_FALSE(compare_index.Contains(immediate.Def()));
        const auto after_compare = SnapshotWorkspaceStorage();
        if (initial.enabled) {
            REQUIRE(after_compare.buffers[row_kind].allocation_calls ==
                    initial.buffers[row_kind].allocation_calls);
            REQUIRE(after_compare.buffers[row_kind].pattern_init_bytes ==
                    initial.buffers[row_kind].pattern_init_bytes);
        }
        Block block{0, Location{0x57c0}};
        const auto base = block.LoadUniform<U32>(Uniform{0, ValueType::U32});
        const auto producer = block.Add<U32>(base, Operand{Imm{1u}});
        block.SaveFlags(producer, Flags::Carry);
        const auto zero = block.LoadImm<U32>(Imm{0u});
        const auto test = block.TestFlags<U32>(Flags::Carry).SetCastType(ValueType::U32);
        const auto add = block.Add<U32>(zero, Operand{test});
        const auto result = block.Add<U32>(base, Operand{add});
        block.StoreUniform(Uniform{8, ValueType::U8}, Value{result}.SetCastType(ValueType::U8));
        block.StoreUniform(Uniform{16, ValueType::U32}, zero);
        const auto* observer = &block.GetInstList().back();
        block.SetTerminal(terminal::ReturnToHost{});
        block.ReIdInstr();
        compile::BlockAnalysisIndex index;
        index.Build(&block, nullptr, true);
        BitVector coverage(block.MaxInstrId());
        arm64::IntegerFlagsInputs inputs{coverage, index, index.PseudoFlags().View()};
        const auto first = arm64::LowerNarrowIntegerFlags(index, workspace, inputs);
        REQUIRE(first.Carry(result.Id()));
        REQUIRE_FALSE(first.Covered(zero.Id()));
        const auto after_first = SnapshotWorkspaceStorage();
        coverage.set(observer->Id());
        const auto second = arm64::LowerNarrowIntegerFlags(index, workspace, inputs);
        REQUIRE(second.Carry(result.Id()));
        REQUIRE(second.Covered(zero.Id()));
        const auto after_second = SnapshotWorkspaceStorage();
        if (initial.enabled) {
            const auto bytes = block.GetInstList().size() * sizeof(u32);
            REQUIRE(after_first.buffers[row_kind].allocation_calls ==
                    initial.buffers[row_kind].allocation_calls + 1);
            REQUIRE(after_second.buffers[row_kind].allocation_calls ==
                    after_first.buffers[row_kind].allocation_calls);
            REQUIRE(after_first.buffers[row_kind].pattern_init_bytes ==
                    initial.buffers[row_kind].pattern_init_bytes + bytes);
            REQUIRE(after_second.buffers[row_kind].pattern_init_bytes ==
                    after_first.buffers[row_kind].pattern_init_bytes + bytes);
        }
        {
            compile::InstructionRowScope occupied{workspace, block.GetInstList().size()};
            occupied[0] = 17;
            REQUIRE(arm64::LowerNarrowIntegerFlags(compare_index, workspace, compare_inputs)
                            .Compare(comparison.Id()));
            REQUIRE_THROWS_AS(arm64::LowerNarrowIntegerFlags(index, workspace, inputs),
                              std::logic_error);
            REQUIRE(occupied.Find(0) == 17);
        }
        coverage.reset(observer->Id());
        const auto recovered = arm64::LowerNarrowIntegerFlags(index, workspace, inputs);
        REQUIRE(recovered.Carry(result.Id()));
        REQUIRE_FALSE(recovered.Covered(zero.Id()));
        REQUIRE(first.Carry(result.Id())->Source(*result.Def()).Def() == base.Def());
    }
    const auto released = SnapshotWorkspaceStorage();
    if (initial.enabled) {
        REQUIRE(released.buffers[row_kind].live_bytes == initial.buffers[row_kind].live_bytes);
        REQUIRE(released.buffers[row_kind].retained_bytes ==
                initial.buffers[row_kind].retained_bytes);
        REQUIRE(released.buffers[row_kind].deallocation_calls ==
                initial.buffers[row_kind].deallocation_calls + 1);
    }
}

TEST_CASE("fixed GPR transfer aliases borrow lazily and release rejected matches",
          "[ir][allocation][facts][transfer-workspace]") {
    namespace arm64 = backend::arm64;
    const auto initial = SnapshotWorkspaceStorage();
    const auto kind = static_cast<size_t>(WorkspaceBufferKind::LoweringCandidates);
    {
        compile::CompileWorkspace workspace;
        struct Input {
            IntrusivePtr<Block> block;
            const Inst* publication;
            const Inst* consumer;
            std::vector<const Inst*> aliases;
        };
        const auto make = [&](unsigned count, bool clobber) {
            Input input{IntrusivePtr<Block>{new Block(0, Location{0x57e0})}, nullptr, nullptr, {}};
            auto& block = *input.block;
            auto value = block.GetHostGPR<U64>(Imm{20u}, Imm{0u});
            block.SetHostGPR(value, Imm{23u}, Imm{0u});
            input.publication = &block.GetInstList().back();
            for (unsigned i = 0; i < count; ++i) {
                value = block.BitCast<U64>(value);
                input.aliases.push_back(value.Def());
            }
            if (clobber) {
                const auto replacement = block.LoadImm<U64>(Imm{u64{9}});
                block.SetHostGPR(replacement, Imm{23u}, Imm{0u});
            }
            const auto sum = block.Add<U64>(value, Operand{Imm{1u}});
            input.consumer = sum.Def();
            block.StoreUniform(Uniform{0, ValueType::U64}, sum);
            block.SetTerminal(terminal::ReturnToHost{});
            block.ReIdInstr();
            unsigned id = 50000;
            compile::RewriteContext rewrite{input.block.get(), workspace, nullptr, false};
            for (const auto& inst : block.GetInstList()) {
                rewrite.Change(input.block.get(), &inst, [&](Inst& target) { target.SetId(id); });
                ++id;
            }
            return input;
        };
        const auto direct = make(0, false);
        const auto good = make(12, false);
        const auto rejected = make(12, true);
        compile::BlockAnalysisIndex direct_index, good_index, rejected_index;
        direct_index.Build(direct.block.get());
        good_index.Build(good.block.get());
        rejected_index.Build(rejected.block.get());
        compile::SemanticFacts direct_facts{direct.block.get()}, good_facts{good.block.get()},
                               rejected_facts{rejected.block.get()};
        arm64::RegisterStatePreparation direct_state, good_state, rejected_state;
        direct_state.BeginBlock(direct.block.get(), FeatureSet{}, direct_facts);
        good_state.BeginBlock(good.block.get(), FeatureSet{}, good_facts);
        rejected_state.BeginBlock(rejected.block.get(), FeatureSet{}, rejected_facts);
        arm64::FixedGPRTransferUses aliases{workspace};
        {
            compile::LoweringCandidateScope occupied{workspace};
            occupied.Entries().push_back({7, 19});
            const auto before_direct = SnapshotWorkspaceStorage();
            const auto transfer = arm64::MatchFixedGPRTransfer(
                    direct.publication, direct_index, direct_state, aliases);
            REQUIRE(transfer);
            REQUIRE(transfer->aliases.empty());
            REQUIRE(transfer->last_use == direct.consumer->Id());
            REQUIRE_THROWS_AS(arm64::MatchFixedGPRTransfer(
                                      good.publication, good_index, good_state, aliases),
                              std::logic_error);
            REQUIRE(occupied.Entries().size() == 1);
            REQUIRE(occupied.Entries().front().position == 7);
            REQUIRE(occupied.Entries().front().roles == 19);
            if (initial.enabled) {
                const auto after_direct = SnapshotWorkspaceStorage();
                REQUIRE(after_direct.buffers[kind].allocation_calls ==
                        before_direct.buffers[kind].allocation_calls);
            }
        }
        auto transfer = arm64::MatchFixedGPRTransfer(
                good.publication, good_index, good_state, aliases);
        REQUIRE(transfer);
        REQUIRE(transfer->aliases.size() == good.aliases.size());
        REQUIRE(transfer->last_use == good.consumer->Id());
        for (size_t i = 0; i < good.aliases.size(); ++i) {
            const auto position = transfer->aliases[i].position;
            REQUIRE(position < good_index.Instructions().size());
            REQUIRE(good_index.Instructions()[position] == good.aliases[i]);
        }
        const auto after_first = SnapshotWorkspaceStorage();
        REQUIRE_FALSE(arm64::MatchFixedGPRTransfer(
                rejected.publication, rejected_index, rejected_state, aliases));
        transfer = arm64::MatchFixedGPRTransfer(
                good.publication, good_index, good_state, aliases);
        REQUIRE(transfer);
        REQUIRE(transfer->aliases.size() == good.aliases.size());
        if (initial.enabled) {
            const auto after_recovery = SnapshotWorkspaceStorage();
            REQUIRE(after_recovery.buffers[kind].allocation_calls ==
                    after_first.buffers[kind].allocation_calls);
        }
    }
    if (initial.enabled) {
        const auto released = SnapshotWorkspaceStorage();
        REQUIRE(released.buffers[kind].live_bytes == initial.buffers[kind].live_bytes);
        REQUIRE(released.buffers[kind].retained_bytes == initial.buffers[kind].retained_bytes);
        REQUIRE(released.buffers[kind].allocation_calls - initial.buffers[kind].allocation_calls ==
                released.buffers[kind].deallocation_calls - initial.buffers[kind].deallocation_calls);
    }
}

TEST_CASE("spilled GPR publication keeps foreign Function definitions on the ordinary path",
          "[ir][allocation][publication]") {
    namespace arm64 = backend::arm64;
    const FeatureSet features{};
    HIRBuilder builder{1, false, features};
    auto* function = builder.AppendFunction(Location{0x5820}, Location{0x5840});
    const auto value = function->LoadImm<U64>(Imm{u64{7}});
    builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x5830}}));
    function->SetHostGPR(value, Imm{23u}, Imm{0u});
    auto* member = function->GetCurrentBlock()->GetBlock();
    auto* publication = &member->GetInstList().back();
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    compile::CompileWorkspace workspace;
    compile::SemanticFacts facts{function};
    compile::Allocation physical{function->MaxInstrCount(),
                                  backend::GPRSMask{1u << 23},
                                  backend::FPRSMask{},
                                  features};
    arm64::UnitActions actions{physical.View()};
    arm64::AllocationPreparation allocation{physical, facts, workspace, actions};
    allocation.SetAllocationUnit(nullptr, function);
    allocation.MapMemSpill(value.Id(), SpillSlot{0});
    allocation.SetLiveEnd(value.Id(), publication->Id());
    arm64::RegisterStatePreparation semantics;
    semantics.BeginBlock(member, features, facts);
    arm64::ResidencePreparation placement{allocation, features, semantics};
    placement.Prepare(member);
    REQUIRE(value.Def()->GetBlock() != member);
    REQUIRE_FALSE(placement.UseFacts().Contains(value.Def()));
    REQUIRE(value.Def()->GetUses(false) == 1);
    REQUIRE(allocation.ValueType(value) == compile::AllocationView::MEM);
    placement.PrepareSpilledGPRPublications();
    placement.IndexPublications();
    REQUIRE_THROWS_AS(placement.PrepareSpilledGPRPublications(), std::logic_error);
    REQUIRE_FALSE(placement.PublicationFor(publication));
    REQUIRE_FALSE(placement.HasValueHome(value.Def()));
    REQUIRE_FALSE(allocation.IsHostWriteCoalesced(publication->Id()));
    REQUIRE(allocation.ValueType(value) == compile::AllocationView::MEM);
    const auto location = placement.UseLocation(value, publication);
    REQUIRE(location.kind == compile::AllocationView::MEM);
    REQUIRE(location.slot == 0);
}

TEST_CASE("publication candidate stages preserve eager and explicit spill requests",
          "[ir][allocation][publication]") {
    namespace arm64 = backend::arm64;
    const char* raw = GetRawSvmConfigEnvForTest("SVM_SKIP_PREP");
    const auto saved = raw ? std::optional<std::string>{raw} : std::nullopt;
    const auto restore_config = ScopeExit([&] {
        if (saved)
            SetSvmConfigEnvForTest("SVM_SKIP_PREP", saved->c_str(), 1);
        else
            UnsetSvmConfigEnvForTest("SVM_SKIP_PREP");
    });
    for (const bool skip_all : {false, true}) {
        CAPTURE(skip_all);
        REQUIRE(SetSvmConfigEnvForTest(
                        "SVM_SKIP_PREP",
                        skip_all ? "gprcopies,selectpub,transfers,pubviews,spillpub" : "",
                        1) == 0);
        Block block{0, Location{0x5850}};
        const auto value = block.LoadImm<U64>(Imm{u64{7}});
        block.SetHostGPR(value, Imm{23u}, Imm{0u});
        const auto* publication = &block.GetInstList().back();
        block.SetTerminal(terminal::ReturnToHost{});
        block.ReIdInstr();
        const FeatureSet features{};
        compile::CompileWorkspace workspace;
        compile::SemanticFacts facts{&block};
        compile::Allocation physical{
                block.MaxInstrId(), backend::GPRSMask{1u << 23}, backend::FPRSMask{}, features};
        arm64::UnitActions actions{physical.View()};
        arm64::AllocationPreparation allocation{physical, facts, workspace, actions};
        allocation.SetAllocationUnit(&block, nullptr);
        allocation.MapMemSpill(value.Id(), SpillSlot{0});
        allocation.SetLiveEnd(value.Id(), publication->Id());
        for (const auto& inst : block.GetInstList())
            allocation.SetActiveRegs(inst.Id(), allocation.GetGprs(), allocation.GetFprs());
        allocation.BeginTargetPlanning();
        const auto abort_planning = ScopeExit([&] { allocation.AbortTargetPlanning(); });
        arm64::RegisterStatePreparation semantics;
        semantics.BeginBlock(&block, features, facts);
        arm64::ResidencePreparation placement{allocation, features, semantics};
        placement.Prepare(&block);
        placement.PrepareSpilledGPRPublications();
        placement.IndexPublications();
        const auto* selected = placement.PublicationFor(publication);
        REQUIRE(selected);
        REQUIRE(selected->kind == arm64::ResidencePreparation::PublicationSelection::Kind::Spill);
        REQUIRE(selected->producer == value.Def());
        REQUIRE(selected->target == 23);
        const auto location = placement.UseLocation(value, publication);
        REQUIRE(location.kind == compile::AllocationView::GPR);
        REQUIRE(location.slot == 23);
        REQUIRE_THROWS_AS(placement.PrepareSpilledGPRPublications(), std::logic_error);
        auto pending = placement.FinishConstraints();
        REQUIRE(pending.writes.size() == 1);
        REQUIRE(pending.writes.front().instruction == value.Id());
        REQUIRE(pending.writes.front().home == 23);
    }
}

TEST_CASE("prepared allocation inputs retain their policy and drive physical ties",
          "[ir][allocation][inputs]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    IntrusivePtr<Block> block{new Block(0, Location{0x5880})};
    const auto address = block->LoadImm(ValueType::U64, Imm{u64{0x40}});
    const auto operand = block->GetOperand(ValueType::U64, Operand{address});
    const auto loaded = block->LoadMemory(ValueType::U64, Operand{operand});
    block->StoreUniform(Uniform{0, ValueType::U64}, loaded);
    block->GetHostGPR(ValueType::U64, Imm{u32{4}}, Imm{u32{0}});
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    compile::CompileUnit ir{block};
    ir.SealIR();
    const backend::GPRSMask gprs{~0x3ffffu};
    const backend::FPRSMask fprs{~0xffffu};
    auto features = FeatureSet{};
    features.mem_narrow_fuse = true;
    features.addr_ea_tie = false;
    compile::SemanticFacts facts{block.get()};
    compile::Allocation physical{block->MaxInstrId(), gprs, fprs, features};
    backend::arm64::UnitActions actions{physical.View()};
    {
        backend::arm64::AllocationPreparation writer{
                physical, facts, compilation_workspace, actions};
        auto input = backend::arm64::PrepareAllocationInputs(block.get(), writer, features);
        REQUIRE(input.fpr == nullptr);
        REQUIRE(input.gpr == nullptr);
        REQUIRE(facts.FindUses(block.get()) == nullptr);
        REQUIRE(input.DirectlyFeedsMemory(operand.Id()));
        REQUIRE_FALSE(input.DirectlyFeedsMemory(address.Id()));
        REQUIRE(input.TiesFor(operand.Id()).size() == 1);
        REQUIRE(input.TiesFor(operand.Id()).front().source == address.Id());
        REQUIRE_FALSE(input.TiesFor(operand.Id()).front().allow_fixed_owner);
        REQUIRE(input.pools[1].pool.Get(4));
        REQUIRE_FALSE(input.pools[0].pool.Get(4));
        auto foreign_pool = gprs;
        foreign_pool.Mark(5);
        REQUIRE_THROWS_AS(input.PolicyFor(foreign_pool), std::logic_error);
        IntrusivePtr<Block> foreign{new Block(0, Location{0x5890})};
        REQUIRE_THROWS_AS(input.Validate(foreign.get(), nullptr, gprs, fprs), std::logic_error);
        features.mem_narrow_fuse = false;
        const auto ties = input.TiesFor(operand.Id());
        auto moved = std::move(input);
        REQUIRE(moved.features.mem_narrow_fuse);
        REQUIRE(moved.TiesFor(operand.Id()).data() == ties.data());
        compile::AllocateBlock(block.get(), &writer, std::move(moved));
        REQUIRE(ties.front().source == address.Id());
        REQUIRE(writer.ValueType(address) == compile::Allocation::GPR);
        REQUIRE(writer.ValueGPR(address).id == writer.ValueGPR(operand).id);
        REQUIRE(writer.GetGprs().Get(4));
        writer.FreezeGPRLocations();
    }
    REQUIRE(physical.View().GPRLocationsFrozen());
    for (unsigned shape = 0; shape < 4; ++shape) {
        CAPTURE(shape);
        IntrusivePtr<Block> member{new Block(0, Location{0x5888})};
        const auto base = member->LoadImm(ValueType::U64, Imm{u64{0x80}});
        const auto view = member->GetOperand(ValueType::U64, Operand{base});
        auto memory_source = view;
        if (shape == 1) (void)member->Add(ValueType::U64, view, Operand{Imm{u64{1}}});
        // A zero-demand pseudo edge still precedes the memory consumer.
        if (shape == 2) member->SaveFlags(view, Flags::None);
        if (shape == 3) memory_source = member->BitCast(ValueType::U64, view);
        const auto memory_operand =
                shape == 0 ? Operand{view, view, OperandPlus} : Operand{memory_source};
        const auto value = member->LoadMemory(ValueType::U64, memory_operand);
        member->StoreUniform(Uniform{0, ValueType::U64}, value);
        member->SetTerminal(terminal::ReturnToHost{});
        member->ReIdInstr();
        compile::CompileUnit owner{member};
        owner.SealIR();
        auto options = FeatureSet{};
        options.mem_narrow_fuse = true;
        options.addr_ea_tie = false;
        compile::SemanticFacts member_facts{member.get()};
        compile::Allocation physical{member->MaxInstrId(), gprs, fprs, options};
        backend::arm64::UnitActions member_actions{physical.View()};
        backend::arm64::AllocationPreparation request{
                physical, member_facts, compilation_workspace, member_actions};
        const auto described =
                backend::arm64::PrepareAllocationInputs(member.get(), request, options);
        REQUIRE(member_facts.FindRegisterReads() == nullptr);
        REQUIRE(member_facts.FindUses(member.get()) == nullptr);
        // Independent complete reference: search each definition's first raw
        // consumer, preserving duplicates, pseudo edges and BitCast nodes.
        for (const auto& definition : member->GetInstList()) {
            bool expected = false;
            for (const auto& consumer : member->GetInstList()) {
                if (consumer.Id() <= definition.Id()) continue;
                bool uses = false;
                consumer.ForEachValue([&](Value v) { uses |= v.Def() == &definition; });
                if (!uses) continue;
                const auto op = consumer.GetOp();
                expected = op == OpCode::LoadMemory || op == OpCode::StoreMemory ||
                           op == OpCode::LoadMemoryTSO || op == OpCode::StoreMemoryTSO;
                break;
            }
            REQUIRE(described.DirectlyFeedsMemory(definition.Id()) == expected);
        }
        REQUIRE(described.DirectlyFeedsMemory(view.Id()) == (shape == 0));
        REQUIRE(described.TiesFor(view.Id()).empty());
    }
}

static std::unique_ptr<compile::GPRInputs> CopyGPRInputs(const compile::GPRInputs& source) {
    auto edited = std::make_unique<compile::GPRInputs>();
    edited->publications = source.publications;
    edited->reads = source.reads;
    edited->views = source.views;
    edited->w_publications = source.w_publications;
    edited->clobbers = source.clobbers;
    edited->consumer_rules = source.consumer_rules;
    edited->blocks = source.blocks;
    if (source.width) edited->width = std::make_unique<const compile::WidthInputs>(*source.width);
    if (source.writes)
        edited->writes = std::make_unique<const compile::GPRWriteInputs>(*source.writes);
    return edited;
}

TEST_CASE("prepared fixed publications retain captures and drive physical home reuse",
          "[ir][allocation][inputs]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    enum Shape {
        Ordinary,
        ClearedInput,
        OverwrittenCapture,
        FullExchange,
        SameValueOverwrite,
        SourceOperandOverwrite,
    };
    for (const auto shape : {Ordinary,
                             ClearedInput,
                             OverwrittenCapture,
                             FullExchange,
                             SameValueOverwrite,
                             SourceOperandOverwrite}) {
        CAPTURE(shape);
        IntrusivePtr<Block> block{new Block(0, Location{0x5892})};
        const auto source = block->GetHostGPR(ValueType::U32, Imm{u32{0}}, Imm{u32{0}});
        const auto extended = block->ZeroExtend32To64(ValueType::U64, source);
        block->SetHostGPR(extended, Imm{u32{22}}, Imm{u32{0}});
        if (shape == OverwrittenCapture) {
            const auto replacement = block->LoadImm(ValueType::U32, Imm{u32{7}});
            const auto wide = block->ZeroExtend32To64(ValueType::U64, replacement);
            block->SetHostGPR(wide, Imm{u32{22}}, Imm{u32{0}});
        } else if (shape == FullExchange) {
            // This later full-width exchange rejects earlier candidates too.
            const auto capture = block->GetHostGPR(ValueType::U64, Imm{u32{1}}, Imm{u32{0}});
            block->SetHostGPR(capture, Imm{u32{25}}, Imm{u32{0}});
        }
        const bool overwrite_boundary =
                shape == SameValueOverwrite || shape == SourceOperandOverwrite;
        if (overwrite_boundary)
            block->SetHostGPR(
                    shape == SameValueOverwrite ? extended : source, Imm{u32{22}}, Imm{u32{0}});
        else
            block->StoreUniform(Uniform{0, ValueType::U64}, extended);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::CompileUnit ir{block};
        ir.SealIR();
        backend::GPRSMask gprs;
        for (const u32 code :
             {0u, 1u, 2u, 3u, 4u, 5u, 19u, 20u, 21u, 22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u})
            gprs.Mark(code);
        const backend::FPRSMask fprs{~0xffffu};
        FeatureSet features;
        features.ra_coalesce = false;
        features.ra_width_chain = false;
        features.ra_width_chain_long = false;
        compile::SemanticFacts facts{block.get()};
        compile::Allocation physical{block->MaxInstrId(), gprs, fprs, features};
        backend::arm64::UnitActions actions{physical.View()};
        backend::arm64::AllocationPreparation writer{
                physical, facts, compilation_workspace, actions};
        auto input = backend::arm64::PrepareAllocationInputs(block.get(), writer, features);
        // Isolate this late home reuse from initial fixed precoloring and
        // from the separate guest-access/width coalescing families.
        input.fixed.clear();
        const bool enabled = input.PolicyFor(writer.GetGprs()).fixed_publications;
        const bool matched =
                enabled && (shape == Ordinary || shape == ClearedInput || overwrite_boundary);
        const auto publications =
                input.gpr ? std::span<const compile::FixedGPRPublication>{input.gpr->publications}
                          : std::span<const compile::FixedGPRPublication>{};
        const auto found =
                std::find_if(publications.begin(), publications.end(), [&](const auto& item) {
                    return item.value == extended.Id();
                });
        REQUIRE((found != publications.end()) == matched);
        if (matched) {
            // A repeated zext write reads the old value before overwriting it.
            // A source operand on that write prevents source-home reuse.
            REQUIRE(found->source == (shape == SourceOperandOverwrite ? UINT16_MAX : source.Id()));
            REQUIRE(found->home == 22);
            REQUIRE(found->block == block.get());
        }
        if (shape == FullExchange) REQUIRE(publications.empty());
        if (overwrite_boundary) continue;
        if (shape == ClearedInput && input.gpr) {
            auto edited = CopyGPRInputs(*input.gpr);
            edited->publications.clear();
            input.gpr = std::move(edited);
        }
        const auto* candidates = input.gpr.get();
        auto moved = std::move(input);
        REQUIRE(moved.gpr.get() == candidates);
        compile::AllocateBlock(block.get(), &writer, std::move(moved));
        const bool reused = enabled && shape == Ordinary;
        REQUIRE((writer.ValueGPR(extended).id == 22) == reused);
        REQUIRE((writer.ValueGPR(source).id == 22) == reused);
        writer.FreezeGPRLocations();
    }
}

TEST_CASE("prepared fixed validation preserves physical failure kinds and width views",
          "[ir][allocation][fixed-validation]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    IntrusivePtr<Block> block{new Block(0, Location{0x5896})};
    const auto value = block->LoadImm(ValueType::U32, Imm{u32{1}});
    const auto noise = block->LoadImm(ValueType::U32, Imm{u32{7}});
    const auto extended = block->ZeroExtend32To64(ValueType::U64, value);
    const auto narrowed = block->ZeroExtend32To64(ValueType::U64, U8{value});
    block->StoreUniform(Uniform{0, ValueType::U32}, value);
    const auto* use = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    compile::CompileUnit ir{block};
    ir.SealIR();
    FeatureSet features;
    features.ra_coalesce = false;
    features.ra_width_chain = false;
    features.ra_width_chain_long = false;
    features.ra_intwidth_tie = false;
    const backend::GPRSMask gprs{(1u << 20) | (1u << 31)};
    const backend::FPRSMask fprs{~0xffffu};
    compile::SemanticFacts facts{block.get()};
    compile::Allocation physical{block->MaxInstrId(), gprs, fprs, features};
    compile::AllocationWriter writer{physical, facts, compilation_workspace, 1u << 20};
    auto input = backend::arm64::PrepareAllocationInputs(block.get(), writer, features);
    REQUIRE(input.fixed_validation);
    const auto* data = input.fixed_validation.get();
    compile::AllocateBlock(block.get(), &writer, std::move(input));
    REQUIRE(writer.PreparedFixedValidation(features, data->contract_key) == data);
    auto different = features;
    different.helper_leaf_abi = !features.helper_leaf_abi;
    REQUIRE_FALSE(writer.PreparedFixedValidation(different, data->contract_key));
    REQUIRE_FALSE(writer.PreparedFixedValidation(features, data->contract_key ^ 1));
    writer.MapRegister(value.Id(), HostGPR{20});
    writer.MapRegister(noise.Id(), HostGPR{0});
    writer.MapRegister(extended.Id(), HostGPR{20});
    writer.MapRegister(narrowed.Id(), HostGPR{0});
    const compile::FixedGPRDefinition fixed{
            .home = 20, .end = use->Id(), .last_use = use->Id(), .width = 4};
    writer.PlanFixedGPRDefinition(value.Id(), fixed);
    u32 fail = 0;
    REQUIRE(compile::CheckFixedConstraints(block.get(), physical.View(), *data, {}, &fail));
    REQUIRE(fail == 0);
    writer.MapRegister(narrowed.Id(), HostGPR{20});
    REQUIRE_FALSE(compile::CheckFixedConstraints(block.get(), physical.View(), *data, {}, &fail));
    REQUIRE(fail == 2);
    writer.MapRegister(narrowed.Id(), HostGPR{0});
    const std::array<compile::PhysicalGPRWrite, 1> overwrite{{{noise.Id(), 20}}};
    fail = 0;
    REQUIRE_FALSE(
            compile::CheckFixedConstraints(block.get(), physical.View(), *data, overwrite, &fail));
    REQUIRE(fail == 2);
    writer.MapRegister(value.Id(), HostGPR{0});
    writer.PlanFixedGPRDefinition(value.Id(), fixed);
    fail = 0;
    REQUIRE_FALSE(compile::CheckFixedConstraints(block.get(), physical.View(), *data, {}, &fail));
    REQUIRE(fail == 1);
    REQUIRE_THROWS_AS(compile::VerifyFixedConstraints(block.get(), physical.View(), *data),
                      std::logic_error);
    writer.MapRegister(value.Id(), HostGPR{20});
    writer.PlanFixedGPRDefinition(value.Id(),
                                  {.home = 20,
                                   .end = noise.Id(),
                                   .transfer = noise.Id(),
                                   .last_use = use->Id(),
                                   .width = 4,
                                   .preserved = true});
    fail = 0;
    REQUIRE_FALSE(compile::CheckFixedConstraints(block.get(), physical.View(), *data, {}, &fail));
    REQUIRE(fail == 3);
    writer.PlanFixedGPRDefinition(value.Id(), fixed);
    REQUIRE_NOTHROW(compile::VerifyFixedConstraints(block.get(), physical.View(), *data));
    auto& index = facts.PrepareUses(block.get());
    backend::arm64::FixedValidationDescription reused{block.get(), writer, features, index};
    REQUIRE(&reused.Inputs() == data);
    backend::arm64::FixedValidationDescription alternate{block.get(), writer, different, index};
    REQUIRE(&alternate.Inputs() != data);
    REQUIRE_NOTHROW(
            compile::VerifyFixedConstraints(block.get(), physical.View(), alternate.Inputs()));
    writer.FreezeGPRLocations();
}

TEST_CASE("fixed validation separates homes and retains complete target consumption keys",
          "[ir][allocation][fixed-validation]") {
    for (bool sparse : {false, true}) {
        CAPTURE(sparse);
        IntrusivePtr<Block> block{new Block(0, Location{0x5898})};
        const auto value = block->LoadImm(ValueType::U64, Imm{u64{0x100000001}});
        const auto second = block->LoadImm(ValueType::U64, Imm{u64{0x200000002}});
        const auto transfer = block->LoadImm(ValueType::U64, Imm{u64{3}});
        const auto gap = block->LoadImm(ValueType::U64, Imm{u64{4}});
        block->StoreUniform(Uniform{0, ValueType::U64}, value);
        const auto* use = &block->GetInstList().back();
        block->StoreUniform(Uniform{8, ValueType::U64}, second);
        const auto* second_use = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::CompileWorkspace workspace;
        if (sparse) {
            compile::RewriteContext editing{block.get(), workspace, nullptr, false};
            for (const auto& inst : block->GetInstList())
                editing.Change(
                        block.get(), &inst, [](Inst& node) { node.SetId(100 + node.Id() * 11); });
        }
        compile::SemanticFacts facts{block.get()};
        FeatureSet features{};
        constexpr u32 homes = (1u << 20) | (1u << 21) | (1u << 22);
        const backend::GPRSMask pool{homes};
        const auto capacity = u32{block->GetInstList().back().Id()} + 1;
        compile::Allocation physical{capacity, pool, backend::FPRSMask{}, features};
        compile::AllocationWriter writer{physical, facts, workspace, homes};
        writer.MapRegister(value.Id(), HostGPR{20});
        writer.MapRegister(second.Id(), HostGPR{21});
        writer.MapRegister(transfer.Id(), HostGPR{0});
        writer.MapRegister(gap.Id(), HostGPR{0});
        writer.PlanFixedGPRDefinition(
                value.Id(), {.home = 20, .end = use->Id(), .last_use = use->Id(), .width = 8});
        writer.PlanFixedGPRDefinition(
                second.Id(),
                {.home = 21, .end = second_use->Id(), .last_use = second_use->Id(), .width = 8});
        auto& index = facts.PrepareIndex(block.get());
        backend::arm64::FixedValidationBuilder builder{features};
        builder.BeginBlock(block.get(), index);
        for (const auto* inst : index.Instructions()) builder.Describe(*inst);
        const auto input = builder.Finish();
        const auto check = [&](std::span<const compile::PhysicalGPRWrite> writes, bool expected) {
            u32 fail = 0;
            REQUIRE(compile::CheckFixedConstraints(
                            block.get(), physical.View(), *input, writes, &fail) == expected);
            REQUIRE(fail == (expected ? 0 : 2));
        };
        check({}, true);
        for (u16 home : {u16{19}, u16{20}, u16{21}, u16{30}}) {
            CAPTURE(home);
            const std::array<compile::PhysicalGPRWrite, 1> writes{{{gap.Id(), home}}};
            check(writes, home != 20 && home != 21);
        }
        const std::array<compile::PhysicalGPRWrite, 2> distant{
                {{u32{0x10000} + gap.Id(), 20}, {UINT32_MAX, 21}}};
        check(distant, true);
        const std::array<compile::PhysicalGPRWrite, 1> consumed{
                {{gap.Id(), 20, value.Id(), use->Id()}}};
        check(consumed, true);
        auto wrong = consumed;
        wrong[0].consumed_value += 0x10000;
        check(wrong, false);
        wrong[0].consumed_value = second.Id();
        check(wrong, false);
        const std::array<compile::PhysicalGPRWrite, 2> duplicate{
                {{gap.Id(), 19, value.Id(), use->Id()},
                 {transfer.Id(), 19, value.Id(), use->Id()}}};
        REQUIRE_THROWS_AS(
                compile::CheckFixedConstraints(block.get(), physical.View(), *input, duplicate),
                std::logic_error);
        writer.PlanFixedGPRDefinition(value.Id(),
                                      {.home = 20,
                                       .transferred_home = 22,
                                       .end = transfer.Id(),
                                       .transfer = transfer.Id(),
                                       .last_use = use->Id(),
                                       .width = 8});
        const std::array<compile::PhysicalGPRWrite, 1> suffix{{{gap.Id(), 22}}};
        check(suffix, false);
        const std::array<compile::PhysicalGPRWrite, 1> boundary{{{transfer.Id(), 22}}};
        check(boundary, true);
        REQUIRE_FALSE(index.UsesReady());
        REQUIRE_FALSE(index.ResolvedUsesReady());
    }
}

TEST_CASE("prepared GPR writes retain their semantic gates and drive home reuse",
          "[ir][allocation][inputs]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    enum Shape { Ordinary, Cleared, Observer };
    for (const auto shape : {Ordinary, Cleared, Observer}) {
        CAPTURE(shape);
        IntrusivePtr<Block> block{new Block(0, Location{0x5895})};
        const auto left = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        const auto right = block->LoadImm(ValueType::U64, Imm{u64{7}});
        const auto producer = block->Add(ValueType::U64, left, Operand{right});
        if (shape == Observer) block->UniformBarrier();
        block->SetHostGPR(producer, Imm{u32{22}}, Imm{u32{0}});
        const auto* store = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::CompileUnit ir{block};
        ir.SealIR();
        backend::GPRSMask gprs;
        for (const u32 code :
             {0u, 1u, 2u, 3u, 4u, 5u, 19u, 20u, 21u, 22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u})
            gprs.Mark(code);
        const backend::FPRSMask fprs{~0xffffu};
        FeatureSet features;
        features.ra_coalesce = true;
        features.ra_coalesce_live = false;
        features.ra_intwidth_tie = false;
        features.ra_width_chain = false;
        features.ra_width_chain_long = false;
        compile::SemanticFacts facts{block.get()};
        compile::Allocation physical{block->MaxInstrId(), gprs, fprs, features};
        backend::arm64::UnitActions actions{physical.View()};
        backend::arm64::AllocationPreparation writer{
                physical, facts, compilation_workspace, actions};
        auto input = backend::arm64::PrepareAllocationInputs(block.get(), writer, features);
        input.fixed.clear();
        const bool enabled = input.PolicyFor(writer.GetGprs()).gpr_coalescing;
        const auto* range = input.gpr ? input.gpr->BlockFor(block.get()) : nullptr;
        REQUIRE((range && range->write_count != 0) == (enabled && shape != Observer));
        if (enabled && shape != Observer) {
            REQUIRE(input.gpr->writes);
            REQUIRE(input.gpr->writes->writes.size() == 1);
            const auto& row = input.gpr->writes->writes.front();
            REQUIRE(row.produced == producer.Def());
            REQUIRE(row.live_end == store->Id());
            REQUIRE(row.modes & compile::GPRWriteInputs::Baseline);
            if (shape == Cleared) {
                auto edited = CopyGPRInputs(*input.gpr);
                edited->writes.reset();
                input.gpr = std::move(edited);
            }
        }
        const auto* payload = input.gpr.get();
        auto moved = std::move(input);
        REQUIRE(moved.gpr.get() == payload);
        compile::AllocateBlock(block.get(), &writer, std::move(moved));
        REQUIRE(writer.IsHostWriteCoalesced(store->Id()) == (enabled && shape == Ordinary));
        if (enabled && shape == Ordinary) REQUIRE(writer.ValueGPR(producer).id == 22);
        writer.FreezeGPRLocations();
    }
}

TEST_CASE("prepared width views drive physical bridge decisions", "[ir][allocation][inputs]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    for (const bool cleared : {false, true}) {
        CAPTURE(cleared);
        IntrusivePtr<Block> block{new Block(0, Location{0x5894})};
        const auto left = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
        const auto right = block->LoadImm(ValueType::U32, Imm{u32{7}});
        const auto producer = block->Add(ValueType::U32, left, Operand{right});
        const auto extract = block->BitExtract(ValueType::U32, producer, Imm{u32{0}}, Imm{u32{32}});
        const auto extend = block->ZeroExtend32To64(ValueType::U64, extract);
        block->StoreUniform(Uniform{8, ValueType::U64}, extend);
        block->StoreUniform(Uniform{16, ValueType::U32}, extract);
        block->StoreUniform(Uniform{20, ValueType::U32}, producer);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::CompileUnit ir{block};
        ir.SealIR();
        FeatureSet features;
        features.ra_width_chain = true;
        features.ra_width_chain_long = false;
        features.ra_coalesce = false;
        features.ra_intwidth_tie = false;
        const backend::GPRSMask gprs{~0x3ffu};
        const backend::FPRSMask fprs{~0xffffu};
        compile::SemanticFacts facts{block.get()};
        compile::Allocation physical{block->MaxInstrId(), gprs, fprs, features};
        backend::arm64::UnitActions actions{physical.View()};
        backend::arm64::AllocationPreparation writer{
                physical, facts, compilation_workspace, actions};
        auto input = backend::arm64::PrepareAllocationInputs(block.get(), writer, features);
        REQUIRE(input.gpr);
        const auto* range = input.gpr->BlockFor(block.get());
        REQUIRE(range);
        REQUIRE(input.gpr->Views(*range).size() == 2);
        for (const auto& view : input.gpr->Views(*range)) {
            REQUIRE(view.flags & compile::GPRInputs::WidthBridge);
            REQUIRE(view.flags & compile::GPRInputs::KnownWWrite);
        }
        REQUIRE_FALSE(input.gpr->width);
        if (cleared) {
            auto edited = CopyGPRInputs(*input.gpr);
            for (auto& view : edited->views) view.flags &= ~compile::GPRInputs::WidthBridge;
            input.gpr = std::move(edited);
        }
        const auto* payload = input.gpr.get();
        auto moved = std::move(input);
        REQUIRE(moved.gpr.get() == payload);
        compile::AllocateBlock(block.get(), &writer, std::move(moved));
        REQUIRE(writer.IsWidthChainCoalesced(extract.Id()) == !cleared);
        REQUIRE(writer.IsWidthChainCoalesced(extend.Id()) == !cleared);
        if (!cleared) {
            REQUIRE(writer.WidthChainAnchor(extract.Id()) == producer.Id());
            REQUIRE(writer.WidthChainAnchor(extend.Id()) == producer.Id());
        }
        writer.FreezeGPRLocations();
    }
}

TEST_CASE("prepared narrow reads and low32 copies drive physical reuse",
          "[ir][allocation][inputs]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    enum Shape { Read, ReadWithoutInput, ReadOverwritten, Copy, CopyWithoutInput, CopyWithoutRole };
    for (const auto shape :
         {Read, ReadWithoutInput, ReadOverwritten, Copy, CopyWithoutInput, CopyWithoutRole}) {
        CAPTURE(shape);
        const bool copy = shape == Copy || shape == CopyWithoutInput || shape == CopyWithoutRole;
        const bool clear =
                shape == ReadWithoutInput || shape == CopyWithoutInput || shape == CopyWithoutRole;
        IntrusivePtr<Block> block{new Block(0, Location{0x5893})};
        Value subject, parent;
        if (copy) {
            parent = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
            subject = block->BitExtract(ValueType::U32, parent, Imm{u32{0}}, Imm{u32{32}});
        } else {
            const auto seed = block->LoadImm(ValueType::U32, Imm{u32{7}});
            const auto published = block->ZeroExtend32To64(ValueType::U64, seed);
            block->SetHostGPR(published, Imm{u32{22}}, Imm{u32{0}});
            subject = block->GetHostGPR(ValueType::U32, Imm{u32{22}}, Imm{u32{0}});
            if (shape == ReadOverwritten) {
                const auto next = block->LoadImm(ValueType::U32, Imm{u32{9}});
                const auto wide = block->ZeroExtend32To64(ValueType::U64, next);
                block->SetHostGPR(wide, Imm{u32{22}}, Imm{u32{0}});
            }
        }
        const auto result = copy ? block->ZeroExtend32To64(ValueType::U64, subject) : subject;
        block->StoreUniform(Uniform{8, copy ? ValueType::U64 : ValueType::U32}, result);
        const auto* sink = &block->GetInstList().back();
        if (copy) block->StoreUniform(Uniform{16, ValueType::U64}, parent);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::CompileUnit ir{block};
        ir.SealIR();
        backend::GPRSMask gprs;
        for (const u32 code :
             {0u, 1u, 2u, 3u, 4u, 5u, 19u, 20u, 21u, 22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u})
            gprs.Mark(code);
        const backend::FPRSMask fprs{~0xffffu};
        FeatureSet features;
        features.ra_coalesce = true;
        features.ra_intwidth_tie = false;
        features.ra_width_chain = false;
        features.ra_width_chain_long = false;
        compile::SemanticFacts facts{block.get()};
        compile::Allocation physical{block->MaxInstrId(), gprs, fprs, features};
        backend::arm64::UnitActions actions{physical.View()};
        backend::arm64::AllocationPreparation writer{
                physical, facts, compilation_workspace, actions};
        auto input = backend::arm64::PrepareAllocationInputs(block.get(), writer, features);
        input.fixed.clear();
        const bool enabled = input.PolicyFor(writer.GetGprs()).gpr_coalescing;
        const auto* range = input.gpr ? input.gpr->BlockFor(block.get()) : nullptr;
        REQUIRE((range && (copy ? range->view_count : range->read_count)) ==
                (enabled && shape != ReadOverwritten));
        if (copy && enabled) {
            REQUIRE(input.gpr->Views(*range).size() == 1);
            REQUIRE(input.gpr->ConsumerFlags(sink->Id()) & compile::GPRInputs::AllowNoResult);
        }
        if (clear && input.gpr) {
            auto edited = CopyGPRInputs(*input.gpr);
            for (auto& block_range : edited->blocks) {
                if (copy) {
                    if (shape != CopyWithoutRole) block_range.view_count = 0;
                } else
                    block_range.read_count = 0;
            }
            if (shape == CopyWithoutRole)
                for (auto& view : edited->views) view.flags &= ~compile::GPRInputs::Low32;
            else if (copy)
                edited->views.clear();
            else
                edited->reads.clear();
            input.gpr = std::move(edited);
        }
        const auto* data = input.gpr.get();
        auto moved = std::move(input);
        REQUIRE(moved.gpr.get() == data);
        compile::AllocateBlock(block.get(), &writer, std::move(moved));
        const bool reused = copy ? writer.IsLow32CopyCoalesced(subject.Id())
                                 : writer.IsHostReadCoalesced(subject.Id());
        REQUIRE(reused == (enabled && !clear && shape != ReadOverwritten));
        if (shape == CopyWithoutRole) REQUIRE(facts.FindUses(block.get()) == nullptr);
        writer.FreezeGPRLocations();
    }
}

TEST_CASE("GPR publication inputs preserve the actual use width through BitCast",
          "[ir][allocation][inputs][width]") {
    for (const bool wide_source : {false, true})
        for (const auto type : {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64}) {
            CAPTURE(wide_source, type);
            compile::CompileWorkspace workspace;
            IntrusivePtr<Block> block{new Block{0, Location{0x5890}}};
            const auto source =
                    block->LoadImm(wide_source ? ValueType::U64 : ValueType::U32,
                                   Imm{wide_source ? u64{0xdeadbeef87654321} : u64{0x87654321}});
            const auto alias = block->BitCast(ValueType::U64, source);
            block->SetHostGPR(Value{alias}.SetCastType(type), HostRegIndex(22), Imm{u32{0}});
            const auto read = block->GetHostGPR(ValueType::U32, HostRegIndex(22), Imm{u32{0}});
            block->StoreUniform(Uniform{0, ValueType::U32}, read);
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            compile::CompileUnit ir{block};
            ir.SealIR();
            FeatureSet features{};
            features.ra_coalesce = true;
            features.ra_coalesce_live = false;
            features.ra_width_chain = false;
            features.ra_width_chain_long = false;
            backend::GPRSMask gprs;
            for (const u32 code : {0u,
                                   1u,
                                   2u,
                                   3u,
                                   4u,
                                   5u,
                                   19u,
                                   20u,
                                   21u,
                                   22u,
                                   23u,
                                   25u,
                                   26u,
                                   27u,
                                   28u,
                                   29u,
                                   30u,
                                   31u})
                gprs.Mark(code);
            const backend::FPRSMask fprs{~0xffffu};
            compile::SemanticFacts facts{block.get()};
            compile::Allocation physical{block->MaxInstrId(), gprs, fprs, features};
            backend::arm64::UnitActions actions{physical.View()};
            backend::arm64::AllocationPreparation writer{physical, facts, workspace, actions};
            const auto input =
                    backend::arm64::PrepareAllocationInputs(block.get(), writer, features);
            REQUIRE(input.PolicyFor(writer.GetGprs()).gpr_coalescing);
            const auto* range = input.gpr ? input.gpr->BlockFor(block.get()) : nullptr;
            const auto writes = range ? range->write_count : 0;
            const auto reads = range ? range->read_count : 0;
            const auto bytes = GetValueSizeByte(type);
            REQUIRE(reads == (bytes >= sizeof(u32) ? 1 : 0));
            // Partial updates preserve the old home; 32-bit updates clear its
            // high half. A wide producer cannot replace either operation.
            REQUIRE(writes ==
                    (bytes >= sizeof(u32) && (!wide_source || bytes == sizeof(u64)) ? 1 : 0));
        }
}

TEST_CASE("prepared scratch requirements drive reserve escalation after moving the request",
          "[ir][allocation][inputs]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    IntrusivePtr<Block> block{new Block(0, Location{0x5894})};
    std::array<Value, 5> values;
    for (u32 i = 0; i < values.size(); ++i)
        values[i] = block->LoadImm(ValueType::U64, Imm{u64{i + 1}});
    for (u32 i = 0; i < values.size(); ++i)
        block->StoreUniform(Uniform{i * 8, ValueType::U64}, values[i]);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    compile::CompileUnit ir{block};
    ir.SealIR();
    FeatureSet features;
    features.ra_spill_evict = false;
    const backend::GPRSMask gprs{~0xffu};
    const backend::FPRSMask fprs{~0xffffu};
    compile::SemanticFacts facts{block.get()};
    compile::Allocation physical{block->MaxInstrId(), gprs, fprs, features};
    backend::arm64::UnitActions actions{physical.View()};
    backend::arm64::AllocationPreparation writer{physical, facts, compilation_workspace, actions};
    auto input = backend::arm64::PrepareAllocationInputs(block.get(), writer, features);
    // A target can declare a wider emitter shape without changing common
    // allocation. The five simultaneous values cannot retain five scratch
    // registers at the ordinary reserve in this eight-register pool.
    for (auto& site : input.scratch) site.profile.need.gpr = 5;
    for (auto& pool : input.pools) pool.max_scratch.gpr = 5;
    const auto* sites = input.scratch.data();
    auto moved = std::move(input);
    REQUIRE(moved.ScratchFor(values.back().Id(), gprs).gpr == 5);
    REQUIRE(moved.scratch.data() == sites);
    REQUIRE_THROWS_AS(moved.ScratchFor(moved.instruction_count, gprs), std::logic_error);
    compile::AllocateBlock(block.get(), &writer, std::move(moved));
    REQUIRE(std::any_of(values.begin(), values.end(), [&](Value value) {
        return writer.ValueType(value) == compile::Allocation::MEM;
    }));
    REQUIRE(sites->profile.need.gpr == 5);
    for (const auto& inst : block->GetInstList())
        REQUIRE(compile::CheckInstructionScratch(writer, inst, {5, 0, 0, 3, false}));
    writer.FreezeGPRLocations();
}

TEST_CASE("prepared FPR ties preserve observer windows and the scalar insert policy",
          "[ir][allocation][inputs]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    for (const bool observer : {false, true}) {
        CAPTURE(observer);
        IntrusivePtr<Block> block{new Block(0, Location{0x5898})};
        const auto read = block->GetHostFPR(ValueType::V128, Imm{u32{17}}, Imm{u32{0}});
        const auto right = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
        const auto sum = block->VecFAddScalar64(ValueType::V128, read, right);
        if (observer) block->UniformBarrier();
        block->SetHostFPR(sum, Imm{u32{17}}, Imm{u32{0}});
        const auto* publication = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::CompileUnit ir{block};
        ir.SealIR();
        auto features = FeatureSet{};
        features.sse_scalar_tie = true;
        compile::SemanticFacts facts{block.get()};
        compile::Allocation physical{block->MaxInstrId(),
                                     backend::GPRSMask{~0x3ffffu},
                                     backend::FPRSMask{~0xffffu},
                                     features};
        backend::arm64::UnitActions actions{physical.View()};
        backend::arm64::AllocationPreparation writer{
                physical, facts, compilation_workspace, actions};
        auto input = backend::arm64::PrepareAllocationInputs(block.get(), writer, features);
        REQUIRE(input.fpr != nullptr);
        REQUIRE(input.fixed_reads.size() == 1);
        REQUIRE(input.fixed_reads.front().reg_class == compile::RegisterClass::FPR);
        REQUIRE(input.fixed_reads.front().home == 17);
        REQUIRE(input.fixed_reads.front().track_use_end);
        REQUIRE(input.fpr->TieSource(sum.Id(), false) == UINT16_MAX);
        REQUIRE(input.fpr->TieSource(sum.Id(), true) == read.Id());
        REQUIRE(input.fpr->ReplacesHome(sum.Id(), 17) == !observer);
        REQUIRE_FALSE(input.fpr->ReplacesHome(sum.Id(), 18));
        const auto* tables = input.fpr.get();
        features.sse_scalar_tie = false;
        compile::AllocateBlock(block.get(), &writer, std::move(input), true);
        REQUIRE(tables->TieSource(sum.Id(), true) == read.Id());
        REQUIRE(writer.ValueType(sum) == compile::Allocation::FPR);
        REQUIRE((writer.ValueFPR(sum).id == 17) == !observer);
        REQUIRE(writer.IsHostWriteCoalesced(publication->Id()) == !observer);
        writer.FreezeGPRLocations();
    }
}

TEST_CASE("fixed inputs survive physical retries while new requests discard old results",
          "[ir][allocation][inputs]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    IntrusivePtr<Block> first{new Block(0, Location{0x58a0})};
    const auto read = first->GetHostGPR(ValueType::U64, Imm{u32{20}}, Imm{u32{0}});
    const auto other = first->GetHostGPR(ValueType::U64, Imm{u32{21}}, Imm{u32{0}});
    first->StoreUniform(Uniform{0, ValueType::U64}, read);
    first->StoreUniform(Uniform{8, ValueType::U64}, other);
    first->SetTerminal(terminal::ReturnToHost{});
    first->ReIdInstr();
    IntrusivePtr<Block> second{new Block(0, Location{0x58b0})};
    const auto replacement = second->LoadImm(ValueType::U64, Imm{u64{17}});
    second->StoreUniform(Uniform{0, ValueType::U64}, replacement);
    second->SetTerminal(terminal::ReturnToHost{});
    second->ReIdInstr();
    const FeatureSet features{};
    compile::SemanticFacts facts;
    compile::Allocation physical{first->MaxInstrId(),
                                 backend::GPRSMask{~0x3ffffu},
                                 backend::FPRSMask{~0xffffu},
                                 features};
    backend::arm64::UnitActions actions{physical.View()};
    backend::arm64::AllocationPreparation writer{physical, facts, compilation_workspace, actions};
    auto input = backend::arm64::PrepareAllocationInputs(first.get(), writer, features);
    REQUIRE(input.declared_gprs == ((u32{1} << 20) | (u32{1} << 21)));
    const auto* capture_uses = &facts.PrepareRegisterReads();
    REQUIRE(input.fixed.size() == 2);
    REQUIRE_FALSE(writer.FixedGPRConstraintsReady());
    REQUIRE_FALSE(writer.HasFixedGPRDefinition(read.Id()));
    const auto* prepared = input.fixed.data();
    REQUIRE(prepared->value == read.Id());
    REQUIRE(prepared->definition.home == 20);
    compile::AllocateBlock(first.get(), &writer, std::move(input));
    REQUIRE(writer.FixedGPRConstraintsReady());
    REQUIRE(writer.ValueGPR(read).id == 20);
    REQUIRE(writer.ValueGPR(other).id == 21);
    // A physical policy decision must survive the retry; the immutable
    // original input is still owned by this request and must not be replayed.
    writer.PlanFixedGPRDefinition(read.Id(), {});
    compile::AllocationController::RetryWithoutFPRCoalescing(writer);
    REQUIRE(writer.FPRContractRetryCount() == 1);
    REQUIRE(writer.FixedGPRConstraintsReady());
    REQUIRE_FALSE(writer.HasFixedGPRDefinition(read.Id()));
    REQUIRE(writer.FixedGPRDefinitionAt(other.Id())->home == 21);
    REQUIRE(facts.FindRegisterReads() == capture_uses);
    REQUIRE(prepared->definition.home == 20);
    auto next = backend::arm64::PrepareAllocationInputs(second.get(), writer, features);
    REQUIRE(facts.FindRegisterReads() == nullptr);
    REQUIRE(next.declared_gprs == 0);
    REQUIRE(next.fixed.empty());
    REQUIRE_FALSE(writer.FixedGPRConstraintsReady());
    REQUIRE_FALSE(writer.HasFixedGPRDefinition(read.Id()));
    REQUIRE_FALSE(writer.HasFixedGPRDefinition(other.Id()));
    REQUIRE(writer.ValueType(replacement) == compile::Allocation::NONE);
    REQUIRE(writer.FPRContractRetryCount() == 0);
    REQUIRE_FALSE(writer.FPRCoalescingDisabled());
    compile::AllocateBlock(second.get(), &writer, std::move(next));
    REQUIRE(writer.ValueType(replacement) == compile::Allocation::GPR);
    REQUIRE(writer.ValueGPR(replacement).id != 20);
    REQUIRE(writer.ValueGPR(replacement).id != 21);
    writer.FreezeGPRLocations();
}

TEST_CASE("base liveness preserves capture writes and virtual alias semantics",
          "[ir][allocation][facts]") {
    for (const bool flags_first : {false, true}) {
        CAPTURE(flags_first);
        IntrusivePtr<Block> block{new Block(0, Location{0x5900})};
        const auto unused = block->LoadImm(ValueType::U64, Imm{u64{3}});
        const auto alias = block->BitCast(ValueType::U64, unused);
        const auto capture = block->GetHostGPR(ValueType::U64, Imm{u32{20}}, Imm{u32{0}});
        block->GetHostGPR(ValueType::U8, Imm{u32{3}}, Imm{u32{0}});
        block->GetHostGPR(ValueType::U64, Imm{u32{31}}, Imm{u32{0}});
        const auto narrow = block->GetHostGPR(ValueType::U32, Imm{u32{6}}, Imm{u32{0}});
        const auto narrow_alias =
                block->BitCast(ValueType::U32, Value{narrow}.SetCastType(ValueType::U16));
        block->StoreUniform(Uniform{48, ValueType::U64},
                            Value{narrow_alias}.SetCastType(ValueType::U64));
        const auto sum = block->Add(ValueType::U64, capture, Operand{Imm{u64{7}}});
        block->SaveFlags(sum, Flags::Carry);
        block->SetHostGPR(sum, Imm{u32{20}}, Imm{u32{0}});
        const auto* write = &block->GetInstList().back();
        const auto fpr = block->GetHostFPR(ValueType::V128, Imm{u32{17}}, Imm{u32{0}});
        block->SetHostFPR(fpr, Imm{u32{18}}, Imm{u32{0}});
        block->SetHostFPR(fpr, Imm{u32{17}}, Imm{u32{0}});
        const auto condition = block->TestNotZero(capture);
        block->PushRSB(Lambda{Imm{u64{0x5910}}});
        block->AdvancePC(Imm{u64{1}});
        block->SetTerminal(
                terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToDispatch{}});
        block->ReIdInstr();
        compile::SemanticFacts facts{block.get()};
        REQUIRE(facts.FindSummary(block.get()) == nullptr);
        if (flags_first) {
            REQUIRE(facts.PrepareFlags()[sum.Id()].requested == Flags::Carry);
            REQUIRE(facts.FindValueLiveness() == nullptr);
            REQUIRE(facts.GPRHomes() ==
                    ((u32{1} << 3) | (u32{1} << 6) | (u32{1} << 20) | (u32{1} << 31)));
        }
        const auto& base = facts.PrepareValueLiveness();
        REQUIRE(facts.FindValueLiveness() == &base);
        REQUIRE(facts.GPRHomes() ==
                ((u32{1} << 3) | (u32{1} << 6) | (u32{1} << 20) | (u32{1} << 31)));
        REQUIRE(facts.FindRegisterReads() == nullptr);
        CheckRegisterReadUses(facts, block.get());
        REQUIRE(facts.PrepareRegisterReads().Find(narrow.Id())->max_width == 8);
        REQUIRE_FALSE(facts.PrepareRegisterReads().Find(unused.Id()));
        REQUIRE(&facts.PrepareValueLiveness() == &base);
        REQUIRE(facts.FindUses(block.get()) == nullptr);
        REQUIRE_FALSE(facts.HasSpillUses());
        const auto* summary = facts.FindSummary(block.get());
        REQUIRE(summary != nullptr);
        REQUIRE(summary->InstructionCount() == block->GetInstList().size());
        REQUIRE(summary->HasFPRInstructions());
        REQUIRE(summary->HasConsumerDecisions());
        REQUIRE(summary->HasGPRInputCandidates());
        REQUIRE(summary->HasReturnAddressPush());
        REQUIRE_FALSE(summary->HasMemoryInstructions());
        REQUIRE(base.UseEnd(unused.Id()) == 0);
        REQUIRE(facts.PrepareBlockUses(block.get())[unused.Id()] == alias.Id());
        REQUIRE(base.UseEnd(capture.Id()) == condition.Id());
        REQUIRE(base.UseEnd(condition.Id()) == block->GetInstList().back().Id());
        REQUIRE_FALSE(base.GPRWrites().Crosses(20, capture.Id(), write->Id() - 1));
        REQUIRE(base.GPRWrites().Crosses(20, capture.Id(), write->Id()));
        REQUIRE_FALSE(base.GPRWrites().Crosses(20, write->Id(), condition.Id()));
        REQUIRE_FALSE(base.GPRWrites().Crosses(21, capture.Id(), condition.Id()));
        REQUIRE(base.FPRWrites().For(17).size() == 1);
        REQUIRE(base.FPRWrites().For(18).size() == 1);
        REQUIRE(base.FPRWrites().For(19).empty());
        auto& graph = facts.PrepareUses(block.get());
        REQUIRE(graph.Summary().InstructionCount() == summary->InstructionCount());
        REQUIRE(graph.PseudoFlags().For(sum.Def()).set == Flags::Carry);
        REQUIRE(facts.PrepareFlags()[sum.Id()].pseudo_count == 1);
    }
    // The semantic table follows instruction IDs without requiring monotonic
    // order or a dense array reaching the largest manually numbered capture.
    Block sparse{0, Location{0x59c0}};
    const auto read = sparse.GetHostGPR<U32>(Imm{u32{20}}, Imm{u32{0}});
    const auto unused = sparse.GetHostGPR<U32>(Imm{u32{21}}, Imm{u32{0}});
    const auto alias = sparse.BitCast(ValueType::U32, Value{read}.SetCastType(ValueType::U16));
    sparse.StoreUniform(Uniform{0, ValueType::U64}, Value{alias}.SetCastType(ValueType::U64));
    sparse.AdvancePC(Imm{u64{1}});
    const auto condition = Value{read}.SetCastType(ValueType::U8);
    const auto selector = Value{alias}.SetCastType(ValueType::U16);
    const Vector<terminal::Switch::Case> cases{
            {Imm{u32{0}},
             terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToDispatch{}}},
            {Imm{u32{1}}, terminal::ReturnToHost{}}};
    sparse.SetTerminal(terminal::CheckHalt{terminal::Condition{
            Cond::EQ,
            terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToDispatch{}},
            terminal::Switch{selector, cases}}});
    std::vector<Value> terminal_values;
    compile::VisitTerminalValues(sparse.GetTerminal(),
                                 [&](Value value) { terminal_values.push_back(value); });
    const std::array<Value, 3> expected_terminal{condition, selector, condition};
    REQUIRE(terminal_values.size() == expected_terminal.size());
    for (size_t i = 0; i < expected_terminal.size(); ++i) {
        REQUIRE(terminal_values[i].Def() == expected_terminal[i].Def());
        REQUIRE(terminal_values[i].Type() == expected_terminal[i].Type());
    }
    for (const bool spilled : {false, true}) {
        CAPTURE(spilled);
        compile::CompileWorkspace scratch_workspace;
        compile::SemanticFacts scratch_facts{&sparse};
        compile::Allocation physical{
                sparse.MaxInstrId(), backend::GPRSMask{}, backend::FPRSMask{}, FeatureSet{}};
        compile::AllocationWriter writer{physical, scratch_facts, scratch_workspace};
        writer.MapRegister(alias.Id(), HostGPR{19});
        if (spilled)
            writer.MapMemSpill(read.Id(), SpillSlot{2});
        else
            writer.MapRegister(read.Id(), HostGPR{20});
        // The selector resolves through its BitCast to the spilled root;
        // every nested occurrence contributes, including the repeated If.
        const auto reloads = compile::CountTerminalReloads(writer, sparse.GetTerminal());
        REQUIRE(reloads.gpr == (spilled ? 3 : 0));
        REQUIRE(reloads.fpr == 0);
    }
    compile::CompileWorkspace workspace;
    compile::RewriteContext rewrite{&sparse, workspace, nullptr, false};
    const std::array<u16, 5> ids{65000, UINT16_MAX, 0, 5, 900};
    size_t at = 0;
    for (const auto& inst : sparse.GetInstList())
        rewrite.Change(&sparse, &inst, [&](Inst& target) { target.SetId(ids[at++]); });
    REQUIRE(at == ids.size());
    compile::SemanticFacts facts{&sparse};
    REQUIRE(facts.FindValueLiveness() == nullptr);
    CheckRegisterReadUses(facts, &sparse);
    REQUIRE(facts.PrepareRegisterReads().Find(read.Id())->last_use == 900);
    REQUIRE(facts.PrepareRegisterReads().Find(read.Id())->max_width == 8);
    REQUIRE_FALSE(facts.PrepareRegisterReads().Find(unused.Id()));
    REQUIRE(facts.FindValueLiveness() == nullptr);
    REQUIRE(facts.FindUses(&sparse) == nullptr);
}

TEST_CASE("optimized ownership preserves facts until the last preparation borrower",
          "[ir][compile][ownership]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    const FeatureSet features{};
    IntrusivePtr<Block> unfinished{new Block(0, Location{0x5a00})};
    REQUIRE_THROWS_AS(
            compile::OptimizedUnit::FromFinalIR(compile::CompileUnit{unfinished}, features),
            std::logic_error);
    REQUIRE_FALSE(unfinished->IsIRReadOnly());

    IntrusivePtr<Block> owner{new Block(0, Location{0x5b00})};
    auto* block = owner.get();
    const auto value = block->LoadImm(ValueType::U64, Imm{u64{41}});
    block->StoreUniform(Uniform{0, ValueType::U64}, value);
    block->AdvancePC(Imm{u64{1}});
    block->SetTerminal(terminal::ReturnToHost{});
    compile::CompileOptions options;
    options.enabled = Optimizations::DeadCodeRemove;
    options.features.uniform_elim = false;
    auto optimized =
            compile::OptimizeBlock(compile::CompileUnit{owner}, compilation_workspace, options);
    owner = {};
    options.features.uniform_elim = true;
    REQUIRE(optimized.Unit().IsSealed());
    REQUIRE_FALSE(optimized.Features().uniform_elim);
    REQUIRE(optimized.Block()->GetEndLocation() == Location{0x5b01});
    auto& facts = optimized.PrepareFacts();
    REQUIRE(facts.FindValueLiveness() != nullptr);
    REQUIRE(facts.FindSummary(block) != nullptr);
    REQUIRE(facts.FindUses(block) == nullptr);
    const auto* base = &facts.PrepareValueLiveness();
    const auto* graph = &facts.PrepareUses(block);
    auto moved = std::move(optimized);
    REQUIRE(&moved.Facts() == &facts);
    REQUIRE(moved.Facts().FindUses(block) == graph);
    compile::Allocation physical{block->MaxInstrId(),
                                 backend::GPRSMask{~0x3ffffu},
                                 backend::FPRSMask{~0xffffu},
                                 moved.Features()};
    compile::Allocation another{
            block->MaxInstrId(), backend::GPRSMask{}, backend::FPRSMask{}, moved.Features()};
    {
        compile::AllocationWriter writer{physical, moved.PrepareFacts(), compilation_workspace};
        REQUIRE_THROWS_AS(
                (compile::AllocationWriter{another, moved.PrepareFacts(), compilation_workspace}),
                std::logic_error);
        REQUIRE_THROWS_AS(std::move(moved).TakeIR(), std::logic_error);
        writer.SetAllocationUnit(block, nullptr);
        compile::AllocateBlock(
                block,
                &writer,
                backend::arm64::DescribeAllocationInputs(
                        block, writer, moved.Features(), moved.PrepareFacts(), writer.Workspace()));
        REQUIRE(moved.Facts().FindValueLiveness() == base);
        REQUIRE(moved.Facts().FindUses(block) == graph);
        writer.FreezeGPRLocations();
    }
    auto ir = std::move(moved).TakeIR();
    REQUIRE(ir.IsSealed());
    REQUIRE(ir.GetBlock() == block);
    REQUIRE(physical.View().ValueType(value) == compile::Allocation::GPR);
    REQUIRE_THROWS_AS(block->AdvancePC(Imm{u64{1}}), std::logic_error);
}

TEST_CASE("compile workspaces isolate nesting and reset reused scratch storage",
          "[ir][compile][lifetime]") {
    const auto before_storage = SnapshotWorkspaceStorage();
    compile::CompileWorkspace* reusable;
    {
        compile::CompileWorkspaceScope outer;
        reusable = &outer.Get();
        auto cursor = outer.Get().PrepareSpillCursor(4);
        cursor[0] = 9;
        {
            compile::RewriteQueueScope deleted_scope{outer.Get()};
            auto& deleted = deleted_scope.Entries();
            REQUIRE_THROWS_AS((compile::RewriteQueueScope{outer.Get()}), std::logic_error);
            Block block{1, Location{0x5400}};
            deleted.push_back(block.LoadImm(Imm{u32{7}}).Def());
            const auto* pending = deleted.front();
            compile::LoweringCandidateScope candidates{outer.Get()};
            auto& entries = candidates.Entries();
            REQUIRE(entries.empty());
            entries.push_back({17, static_cast<u16>(compile::ConsumerRole::PairProducer)});
            REQUIRE_THROWS_AS((compile::LoweringCandidateScope{outer.Get()}), std::logic_error);
            const auto capacity = entries.capacity();
            {
                compile::CompileWorkspaceScope nested;
                REQUIRE(&nested.Get() != reusable);
                nested.Get().PrepareSpillCursor(12)[0] = 17;
                compile::RewriteQueueScope nested_queue{nested.Get()};
                REQUIRE(nested_queue.Entries().empty());
                compile::LoweringCandidateScope nested_candidates{nested.Get()};
                REQUIRE(nested_candidates.Entries().empty());
            }
            REQUIRE(entries.size() == 1);
            REQUIRE(entries.front().position == 17);
            REQUIRE(entries.capacity() == capacity);
            REQUIRE(cursor.size() == 4);
            REQUIRE(cursor[0] == 9);
            REQUIRE(deleted.size() == 1);
            REQUIRE(deleted.front() == pending);
        }
        {
            compile::RewriteQueueScope reset{outer.Get()};
            REQUIRE(reset.Entries().empty());
            compile::LoweringCandidateScope reset_candidates{outer.Get()};
            REQUIRE(reset_candidates.Entries().empty());
            REQUIRE(reset_candidates.Entries().capacity() >= 1);
        }
        REQUIRE_THROWS_AS(
                [&] {
                    compile::LoweringCandidateScope interrupted{outer.Get()};
                    interrupted.Entries().push_back(
                            {3, static_cast<u16>(compile::ConsumerRole::ReverseEvent)});
                    throw std::runtime_error("candidate selection interrupted");
                }(),
                std::runtime_error);
        {
            compile::LoweringCandidateScope recovered{outer.Get()};
            REQUIRE(recovered.Entries().empty());
        }
        REQUIRE_THROWS_AS(
                [&] {
                    compile::RewriteQueueScope failed{outer.Get()};
                    failed.Entries().push_back(nullptr);
                    throw std::runtime_error("rewrite interrupted");
                }(),
                std::runtime_error);
        {
            compile::RewriteQueueScope recovered{outer.Get()};
            REQUIRE(recovered.Entries().empty());
        }
    }
    compile::CompileWorkspaceScope next;
    REQUIRE(&next.Get() == reusable);
    REQUIRE(next.Get().SpillCursor().empty());
    compile::RewriteQueueScope next_queue{next.Get()};
    REQUIRE(next_queue.Entries().empty());
    compile::LoweringCandidateScope next_candidates{next.Get()};
    REQUIRE(next_candidates.Entries().empty());
    const auto& cursor = next.Get().PrepareSpillCursor(4);
    REQUIRE(std::all_of(cursor.begin(), cursor.end(), [](u32 value) { return value == 0; }));
    if (before_storage.enabled) {
        const auto after = SnapshotWorkspaceStorage();
        const auto spill = static_cast<size_t>(WorkspaceBufferKind::SpillCursor);
        REQUIRE(after.buffers[spill].zero_init_bytes ==
                before_storage.buffers[spill].zero_init_bytes + (4 + 12 + 4) * sizeof(u32));
    }

    // Caller-owned workspaces have no outer CompileWorkspaceScope::Reset.
    // Every borrower must release an oversized buffer on normal/unwind exit.
    compile::CompileWorkspace caller_owned;
    {
        compile::LoweringCandidateScope large{caller_owned};
        large.Entries().resize(65537);
    }
    {
        compile::LoweringCandidateScope small{caller_owned};
        REQUIRE(small.Entries().empty());
        REQUIRE(small.Entries().capacity() == 0);
    }
    REQUIRE_THROWS_AS(([&] {
                          compile::RewriteQueueScope large{caller_owned};
                          large.Entries().resize(65537);
                          throw std::runtime_error("large rewrite interrupted");
                      }()),
                      std::runtime_error);
    {
        compile::RewriteQueueScope small{caller_owned};
        REQUIRE(small.Entries().empty());
        REQUIRE(small.Entries().capacity() == 0);
    }
    const auto before_rows = SnapshotWorkspaceStorage();
    {
        compile::InstructionRowScope large{caller_owned, 65537};
        large[65536] = 19;
        REQUIRE(large.Find(65536) == 19);
    }
    {
        compile::InstructionRowScope small{caller_owned, 2};
        REQUIRE(small.Find(1) == UINT32_MAX);
        small[1] = 7;
        REQUIRE(small.Find(1) == 7);
    }
    if (before_rows.enabled) {
        const auto after = SnapshotWorkspaceStorage();
        const auto rows = static_cast<size_t>(WorkspaceBufferKind::InstructionRows);
        REQUIRE(after.buffers[rows].pattern_init_bytes ==
                before_rows.buffers[rows].pattern_init_bytes + (65537 + 2) * sizeof(u32));
    }

    // Move the owner only after the first borrower ends. The global storage
    // accounting must pair an allocation with its free on another thread.
    const auto before_handoff = SnapshotWorkspaceStorage();
    auto handoff = std::make_unique<compile::CompileWorkspace>();
    size_t retained = 0;
    {
        compile::LoweringCandidateScope seed{*handoff};
        seed.Entries().push_back({1, static_cast<u16>(compile::ConsumerRole::Entry)});
        retained = seed.Entries().capacity();
    }
    const auto cached_handoff = SnapshotWorkspaceStorage();
    if (before_handoff.enabled) {
        REQUIRE(cached_handoff.total.retained_bytes ==
                before_handoff.total.retained_bytes +
                        retained * sizeof(compile::LoweringCandidate));
        REQUIRE(cached_handoff.total.peak_retained_bytes >= cached_handoff.total.retained_bytes);
    }
    bool reset_observed = false, capacity_reused = false;
    std::thread receiver{
            [storage = std::move(handoff), retained, &reset_observed, &capacity_reused] {
                compile::LoweringCandidateScope reuse{*storage};
                reset_observed = reuse.Entries().empty();
                reuse.Entries().push_back(
                        {2, static_cast<u16>(compile::ConsumerRole::ReverseEvent)});
                capacity_reused = reuse.Entries().capacity() == retained;
            }};
    receiver.join();
    REQUIRE(reset_observed);
    REQUIRE(capacity_reused);
    const auto after_handoff = SnapshotWorkspaceStorage();
    if (before_handoff.enabled) {
        REQUIRE(after_handoff.total.allocation_calls == before_handoff.total.allocation_calls + 1);
        REQUIRE(after_handoff.total.deallocation_calls ==
                before_handoff.total.deallocation_calls + 1);
        REQUIRE(after_handoff.total.live_bytes == before_handoff.total.live_bytes);
        REQUIRE(after_handoff.total.retained_bytes == before_handoff.total.retained_bytes);
        REQUIRE(after_handoff.total.retention_events == before_handoff.total.retention_events + 2);

        // Independent owners remain idle and alive together. Observe the
        // process-wide retained sum before releasing them on their threads.
        constexpr size_t workers = 8;
        std::barrier stages{static_cast<std::ptrdiff_t>(workers + 1)};
        std::array<std::thread, workers> threads;
        std::array<size_t, workers> capacities{};
        for (size_t i = 0; i < workers; ++i)
            threads[i] = std::thread{[&stages, &capacities, i] {
                compile::CompileWorkspace storage;
                {
                    compile::LoweringCandidateScope seed{storage};
                    seed.Entries().resize(1024);
                    capacities[i] = seed.Entries().capacity();
                }
                stages.arrive_and_wait();
                stages.arrive_and_wait();
            }};
        stages.arrive_and_wait();
        const auto simultaneous = SnapshotWorkspaceStorage();
        stages.arrive_and_wait();
        for (auto& thread : threads) thread.join();
        const auto released = SnapshotWorkspaceStorage();
        const auto bytes = std::accumulate(capacities.begin(), capacities.end(), size_t{}) *
                           sizeof(compile::LoweringCandidate);
        REQUIRE(simultaneous.total.retained_bytes == after_handoff.total.retained_bytes + bytes);
        REQUIRE(simultaneous.total.live_bytes == after_handoff.total.live_bytes + bytes);
        REQUIRE(simultaneous.total.peak_retained_bytes >= simultaneous.total.retained_bytes);
        REQUIRE(released.total.retained_bytes == after_handoff.total.retained_bytes);
        REQUIRE(released.total.live_bytes == after_handoff.total.live_bytes);
        REQUIRE(released.total.allocation_calls == after_handoff.total.allocation_calls + workers);
        REQUIRE(released.total.deallocation_calls ==
                after_handoff.total.deallocation_calls + workers);
    }
}

TEST_CASE("value cleanup avoids cold roster allocation when its rule family is absent",
          "[ir][compile][storage]") {
    const FeatureSet features{};
    const compile::BuilderCapabilities target{};
    for (const bool function_mode : {false, true}) {
        HIRBuilder builder;
        auto* function = builder.AppendFunction(Location{0x5400});
        auto* body = function->EditBlock(function->GetCurrentBlock()).GetBlock();
        const auto value = function->LoadImm<U64>(Imm{u64{7}});
        function->StoreUniform(Uniform{0, ValueType::U64}, value);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        const auto rows = static_cast<size_t>(WorkspaceBufferKind::ValueRows);
        const auto lookup = static_cast<size_t>(WorkspaceBufferKind::ValueLookup);
        {
            compile::CompileWorkspace explicit_storage;
            auto rewrite = function_mode
                                   ? compile::RewriteContext{function, explicit_storage}
                                   : compile::RewriteContext{body, explicit_storage, function};
            const auto before = SnapshotWorkspaceStorage();
            compile::ValueWorklist explicit_queries{
                    rewrite, features, target, compile::ValueCleanup::All};
            REQUIRE_FALSE(explicit_queries.Pending());
            const auto after = SnapshotWorkspaceStorage();
            if (before.enabled) {
                REQUIRE(after.total.allocation_calls == before.total.allocation_calls + 2);
                REQUIRE(after.buffers[rows].allocation_calls ==
                        before.buffers[rows].allocation_calls + 1);
                REQUIRE(after.buffers[lookup].allocation_calls ==
                        before.buffers[lookup].allocation_calls + 1);
            }
        }
        {
            compile::CompileWorkspace cold_storage;
            auto rewrite = function_mode ? compile::RewriteContext{function, cold_storage}
                                         : compile::RewriteContext{body, cold_storage, function};
            const auto before = SnapshotWorkspaceStorage();
            std::optional<compile::ValueWorklist> values;
            compile::BeginValueCleanup(
                    values, rewrite, features, target, compile::ValueCleanup::All);
            REQUIRE_FALSE(values);
            REQUIRE(body->GetInstList().back().GetArg<Value>(1).Def() == value.Def());
            const auto after = SnapshotWorkspaceStorage();
            if (before.enabled) {
                REQUIRE(after.total.allocation_calls == before.total.allocation_calls);
                REQUIRE(after.total.allocated_bytes == before.total.allocated_bytes);
            }
        }
    }
}

TEST_CASE("Function DCE switches owner membership across mutable value epochs",
          "[ir][rewrite][lifetime]") {
    for (bool fast : {false, true}) {
        for (bool seed_before : {false, true}) {
            CAPTURE(fast, seed_before);
            HIRBuilder builder{1, false, fast, FeatureSet{}};
            auto* function = builder.AppendFunction(Location{0x5480});
            auto* body = function->EditBlock(function->GetCurrentBlock()).GetBlock();
            const auto byte = builder.LoadImm(ValueType::U8, Imm{0x80u});
            const auto wide = builder.SignExtend(ValueType::S64, byte);
            builder.StoreUniform(Uniform{0, ValueType::S64}, wide);
            const auto* store = &body->GetInstList().back();
            function->EndBlock(terminal::ReturnToHost{});
            builder.SetCurBlock(Location{0x54c0});
            auto* unreachable = function->EditBlock(function->GetCurrentBlock()).GetBlock();
            const auto dead = builder.LoadImm(ValueType::U8, Imm{7u});
            builder.SignExtend(ValueType::S64, dead);
            builder.ReturnToHost();
            function->ComputeRPO();
            function->IdByRPO();
            compile::CompileWorkspace workspace;
            compile::RewriteContext rewrite{function, workspace};
            if (seed_before) rewrite.CollectDead();
            FeatureSet features{};
            features.int_imm_fold = true;
            const compile::BuilderCapabilities target{};
            {
                compile::ValueWorklist work{rewrite, features, target, compile::ValueCleanup::All};
                work.Run();
                rewrite.CollectDead();
                work.Drain();
                rewrite.CollectDead();
                REQUIRE(unreachable->GetInstList().empty());
                REQUIRE(body->GetInstList().size() == 2);
                REQUIRE(body->GetInstList().front().GetOp() == OpCode::LoadImm);
                REQUIRE(store->GetArg<Value>(1).Def() == &body->GetInstList().front());
            }
            // The epoch's borrowed rows are gone. A later notified edit must
            // still let DCE find the newly dead input using its own membership.
            rewrite.Erase(body, store);
            rewrite.CollectDead();
            REQUIRE(body->GetInstList().empty());
            rewrite.CollectDead();
            REQUIRE(body->GetInstList().empty());
        }
    }
}

TEST_CASE("large mutable use graphs release capacity before caller workspace reuse",
          "[ir][compile][storage]") {
    compile::CompileWorkspace workspace;
    const FeatureSet features{};
    const compile::BuilderCapabilities target{};
    const auto uses = static_cast<size_t>(WorkspaceBufferKind::ValueUses);
    const auto links = static_cast<size_t>(WorkspaceBufferKind::ValueLinks);
    Block large{0, Location{0x5400}};
    const auto input = large.LoadImm<U64>(Imm{u64{7}});
    for (u32 call = 0; call < 1024; ++call) {
        Params params;
        for (u32 arg = 0; arg < 64; ++arg) params.Push(input);
        large.CallDynamic(ValueType::VOID, Lambda{input}, std::move(params));
    }
    const auto before = SnapshotWorkspaceStorage();
    {
        compile::RewriteContext rewrite{&large, workspace, nullptr, false};
        compile::ValueWorklist work{rewrite, features, target, compile::ValueCleanup::Widths};
        REQUIRE(work.SingleConsumer(input.Def()) == nullptr);
        REQUIRE(input.Def()->GetUses(false) == 1024 * 65);
        const auto active = SnapshotWorkspaceStorage();
        if (before.enabled) {
            REQUIRE(active.buffers[uses].allocation_calls > before.buffers[uses].allocation_calls);
            REQUIRE(active.buffers[uses].live_bytes > before.buffers[uses].live_bytes);
            REQUIRE(active.buffers[uses].allocated_bytes >
                    active.buffers[uses].live_bytes - before.buffers[uses].live_bytes);
            REQUIRE(active.buffers[links].pattern_init_bytes ==
                    before.buffers[links].pattern_init_bytes +
                            large.GetInstList().size() * 2 * sizeof(u32));
        }
    }
    const auto released = SnapshotWorkspaceStorage();
    if (before.enabled) {
        REQUIRE(released.buffers[uses].live_bytes == before.buffers[uses].live_bytes);
        REQUIRE(released.buffers[uses].retained_bytes == before.buffers[uses].retained_bytes);
    }
    Block small{0, Location{0x5500}};
    const auto source = small.LoadImm<U64>(Imm{u64{9}});
    small.StoreUniform(Uniform{0, ValueType::U64}, source);
    const auto* user = &small.GetInstList().back();
    {
        compile::RewriteContext rewrite{&small, workspace, nullptr, false};
        compile::ValueWorklist work{rewrite, features, target, compile::ValueCleanup::Widths};
        REQUIRE(work.SingleConsumer(source.Def()) == user);
    }
    const auto reusable = SnapshotWorkspaceStorage();
    {
        compile::RewriteContext rewrite{&small, workspace, nullptr, false};
        compile::ValueWorklist work{rewrite, features, target, compile::ValueCleanup::Widths};
        REQUIRE(work.SingleConsumer(source.Def()) == user);
    }
    const auto reused = SnapshotWorkspaceStorage();
    if (before.enabled) {
        REQUIRE(reused.total.allocation_calls == reusable.total.allocation_calls);
        REQUIRE(reused.total.live_bytes == reusable.total.live_bytes);
        REQUIRE(reused.total.retained_bytes == reusable.total.retained_bytes);
        REQUIRE(reused.buffers[links].pattern_init_bytes ==
                released.buffers[links].pattern_init_bytes + 2 * 2 * sizeof(u32) * 2);
    }
}

TEST_CASE("single-block interval qualification includes entry and definition order",
          "[ir][allocation][facts]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    HIRBuilder builder{1, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{0x5500}, Location{0x5600});
    auto* body = &function->EditBlock(function->GetCurrentBlock());
    const auto input = function->LoadImm<U64>(Imm{u64{1}});
    const auto result = function->Add<U64>(input, Operand{Imm{u64{2}}});
    const auto later = function->LoadImm<U64>(Imm{u64{3}});
    const auto condition = function->TestNotZero(result);
    function->EndBlock(terminal::If{
            condition, terminal::LinkBlock{Location{0x5500}}, terminal::ReturnToDispatch{}});
    function->AddEdge(body, body);
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    REQUIRE(compile::SemanticFacts{function}.CanUseBlockIntervals());

    // A malformed forward reference needs the general path's CFG treatment;
    // merely counting the executable blocks does not qualify this input.
    compile::RewriteContext{function, compilation_workspace, false}.SetArg(
            body->GetBlock(), result.Def(), 0, later);
    REQUIRE_FALSE(compile::SemanticFacts{function}.CanUseBlockIntervals());
    compile::RewriteContext{function, compilation_workspace, false}.SetArg(
            body->GetBlock(), result.Def(), 0, input);
    REQUIRE(compile::SemanticFacts{function}.CanUseBlockIntervals());

    function->EditBlock(function->GetEntryBlock()).LoadImm<U64>(Imm{u64{4}});
    function->IdByRPO();
    REQUIRE_FALSE(compile::SemanticFacts{function}.CanUseBlockIntervals());
}

TEST_CASE("IR use counts retain high fanout definitions", "[ir][regression]") {
    if constexpr (sizeof(void*) == 8)
        REQUIRE(((sizeof(Inst) + 15u) & ~size_t{15u}) == 96);
    for (u32 uses : {255u, 256u, 1024u}) {
        INFO("uses=" << uses);
        Block block{0, Location{0x1000}};
        auto value = block.LoadImm(Imm{u64{42}});
        for (u32 i = 0; i < uses; ++i)
            block.StoreUniform(Uniform{0, ValueType::U64}, value);
        block.SetTerminal(terminal::ReturnToHost{});
        REQUIRE(value.Def()->GetUses(false) == uses);
        RewriteFixture{&block}.Get().CollectDead();
        REQUIRE(value.Def()->GetUses(false) == uses);
        REQUIRE(block.GetInstList().size() == uses + 1);
    }
}

TEST_CASE("IR use counts include more than 255 pseudo edges", "[ir][regression]") {
    Block block{0, Location{0x1000}};
    auto left = block.LoadImm(Imm{u64{1}});
    auto result = block.Add(left, Operand{Imm{u64{1}}});
    for (u32 i = 0; i < 300; ++i) block.GetFlags(result.Type(), result, Flags::Carry);
    REQUIRE(result.Def()->GetUses(false) == 300);
    REQUIRE(result.Def()->GetUses() == 0);
    block.SetTerminal(terminal::ReturnToHost{});
    RewriteFixture{&block}.Get().CollectDead();
    REQUIRE(block.GetInstList().empty());

    Block flags_block{1, Location{0x1100}};
    const auto input = flags_block.LoadImm<U64>(Imm{u64{1}});
    const auto sum = flags_block.Add<U64>(input, Operand{Imm{u64{1}}});
    for (u32 i = 0; i < 300; ++i) flags_block.SaveFlags(sum, Flags::NZCV);
    flags_block.SetTerminal(terminal::ReturnToHost{});
    flags_block.ReIdInstr();
    compile::SemanticFacts facts{&flags_block};
    REQUIRE(facts.PrepareFlags()[sum.Id()].pseudo_count == 300);
}

TEST_CASE("ordered constant reuse matches the complete reference at window and control boundaries",
          "[ir][rewrite][constants]") {
    for (bool indexed : {false, true})
        for (unsigned shape : {0u, 1u, 2u})
            for (bool enabled : {false, true}) {
                CAPTURE(indexed, shape, enabled);
                FeatureSet features{};
                features.const_cse = enabled;
                features.int_imm_fold = true;
                HIRBuilder builder{2, false, features};
                auto* function = shape == 2 ? builder.AppendFunction(Location{0x5d00}) : nullptr;
                IntrusivePtr<Block> standalone;
                if (!function) standalone.reset(new Block{Location{0x5d00}});
                auto* current =
                        function ? function->EditBlock(function->GetCurrentBlock()).GetBlock()
                                 : standalone.get();
                std::vector<Block*> blocks{current};
                const auto append = [&]<typename... Args>(
                                            OpCode op, ValueType type, Args&&... args) {
                    return function ? function->AppendInst(op, type, std::forward<Args>(args)...)
                                    : current->AppendInst(op, type, std::forward<Args>(args)...);
                };
                const auto load = [&](ValueType type, u64 bits) {
                    return Value{append(OpCode::LoadImm, type, Imm{bits})};
                };
                const auto pad = [&] { append(OpCode::AdvancePC, ValueType::VOID, Imm{u64{1}}); };
                const auto store = [&](Value value) {
                    append(OpCode::StoreUniform, ValueType::VOID, Uniform{0, value.Type()}, value);
                };
                // Fill every slot, then reuse the oldest at the inclusive limit.
                for (u64 i = 0; i < 8; ++i) load(ValueType::U64, 0x100 + i);
                const auto oldest = load(ValueType::U64, 0x100);
                store(oldest);
                for (u32 distance : {7u, 8u, 9u}) {
                    load(ValueType::U64, 0x200 + distance);
                    for (u32 i = 1; i < distance; ++i) pad();
                    store(load(ValueType::U64, 0x200 + distance));
                }
                // Reusing a duplicate must not roll the original anchor forward.
                load(ValueType::U64, 0x300);
                for (unsigned i = 0; i < 6; ++i) pad();
                const auto before_expiry = load(ValueType::U64, 0x300);
                pad();
                const auto after_expiry = load(ValueType::U64, 0x300);
                Params params;
                params.Push(Value{before_expiry}.SetCastType(ValueType::U8));
                params.Push(Imm{u64{17}});
                params.Push(Value{after_expiry}.SetCastType(ValueType::U32));
                params.Push(Value{before_expiry}.SetCastType(ValueType::U64));
                append(OpCode::CallDynamic,
                       ValueType::U64,
                       Lambda{Value{before_expiry}.SetCastType(ValueType::U16)},
                       std::move(params));
                append(OpCode::SaveFlags, ValueType::VOID, before_expiry, Flags::None);
                for (auto type : {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64}) {
                    load(type, 0x55);
                    store(load(type, 0x55));
                }
                // Expire all entries, then reactivate with a fresh anchor.
                load(ValueType::U64, 0x400);
                for (unsigned i = 0; i < 9; ++i) pad();
                load(ValueType::U64, 0x400);
                store(load(ValueType::U64, 0x400));
                for (auto branch : {OpCode::Goto, OpCode::NotGoto}) {
                    load(ValueType::U64, 0x500);
                    store(Value{append(OpCode::VecLoadConst,
                                       ValueType::V128,
                                       Imm{u64{0xabcd}},
                                       Imm{u64{0xef01}})});
                    const auto token =
                            Value{append(branch, ValueType::U8, BOOL{load(ValueType::U8, 1)})};
                    store(load(ValueType::U64, 0x500));
                    store(Value{append(OpCode::VecLoadConst,
                                       ValueType::V128,
                                       Imm{u64{0xabcd}},
                                       Imm{u64{0xef01}})});
                    append(OpCode::BindLabel, ValueType::VOID, token);
                    store(load(ValueType::U64, 0x500));
                    store(Value{append(OpCode::VecLoadConst,
                                       ValueType::V128,
                                       Imm{u64{0xabcd}},
                                       Imm{u64{0xef01}})});
                }
                const auto vector = Value{append(
                        OpCode::VecLoadConst, ValueType::V128, Imm{u64{0x1234}}, Imm{u64{0x5678}})};
                const auto zero = Value{append(OpCode::VecSharedZero, ValueType::V128)};
                for (unsigned i = 0; i < 20; ++i) pad();
                store(Value{append(OpCode::VecLoadConst,
                                   ValueType::V128,
                                   Imm{u64{0x1234}},
                                   Imm{u64{0x5678}})});
                store(Value{append(OpCode::VecSharedZero, ValueType::V128)});
                store(vector);
                store(zero);
                // Many distinct keys must not change the bounded window policy.
                for (u64 i = 0; i < 256; ++i) load(ValueType::U64, 0x1000 + i);
                const auto exported = load(ValueType::U64, 0x10ff);
                store(exported);
                if (indexed) {
                    // A real eligible rule opens the production value epoch;
                    // the opaque input and immediate operand need no rewrite.
                    const auto input = Value{append(
                            OpCode::LoadUniform, ValueType::U64, Uniform{64, ValueType::U64})};
                    append(OpCode::Xor, ValueType::U64, input, Operand{Imm{u64{1}}});
                    load(ValueType::U64, 0x9000);
                    const auto duplicate = load(ValueType::U64, 0x9000);
                    append(OpCode::VecLoadConst, ValueType::V128, Imm{u64{9}}, Imm{u64{10}});
                    const auto duplicate_vector = Value{append(
                            OpCode::VecLoadConst, ValueType::V128, Imm{u64{9}}, Imm{u64{10}})};
                    append(OpCode::VecSharedZero, ValueType::V128);
                    const auto duplicate_zero =
                            Value{append(OpCode::VecSharedZero, ValueType::V128)};
                    append(OpCode::SaveFlags, ValueType::VOID, duplicate, Flags::None);
                    const auto token = Value{append(OpCode::Goto, ValueType::U8, BOOL{duplicate})};
                    append(OpCode::BindLabel, ValueType::VOID, token);
                    // Replacement users survive a boundary even though new
                    // canonical candidates may not cross it. Repeated raw
                    // inputs must be snapshotted before the use lists change.
                    Params many;
                    for (unsigned i = 0; i < 300; ++i)
                        many.Push(Value{duplicate}.SetCastType(i % 2 ? ValueType::U8
                                                                     : ValueType::U64));
                    many.Push(duplicate_vector);
                    many.Push(duplicate_zero);
                    append(OpCode::CallDynamic,
                           ValueType::U64,
                           Lambda{Value{duplicate}.SetCastType(ValueType::U16)},
                           std::move(many));
                }
                if (function) {
                    builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x5d80}}));
                    current = function->EditBlock(function->GetCurrentBlock()).GetBlock();
                    blocks.push_back(current);
                    store(exported);
                    store(load(ValueType::U64, 0x10ff));
                    function->EndBlock(terminal::ReturnToHost{});
                    function->EndFunction();
                    function->ComputeRPO();
                    function->IdByRPO();
                } else {
                    current->SetTerminal(terminal::ReturnToHost{});
                    current->ReIdInstr();
                    if (shape == 1) {
                        compile::CompileWorkspace editing_storage;
                        compile::RewriteContext editing{current, editing_storage, nullptr, false};
                        for (const auto& inst : current->GetInstList())
                            editing.Change(current, &inst, [](Inst& node) {
                                node.SetId(100 + node.Id() * 3);
                            });
                    }
                }
                using View = std::pair<const Inst*, ValueType>;
                std::map<const Inst*, std::vector<View>> expected;
                std::map<const Inst*, u32> expected_uses;
                for (const auto* block : blocks) {
                    // Independent old policy: retain all typed scalar keys and
                    // absolute visit positions, including stale entries.
                    std::map<std::pair<u64, ValueType>, std::pair<const Inst*, u32>> scalar;
                    std::map<std::pair<u64, u64>, const Inst*> vectors;
                    std::map<const Inst*, const Inst*> replacements;
                    const Inst* shared_zero{};
                    u32 position = 0;
                    for (const auto& inst : block->GetInstList()) {
                        ++position;
                        const auto op = inst.GetOp();
                        const bool boundary = op == OpCode::Goto || op == OpCode::NotGoto ||
                                              op == OpCode::BindLabel;
                        if (boundary) {
                            scalar.clear();
                            vectors.clear();
                            shared_zero = nullptr;
                        }
                        auto& views = expected[&inst];
                        inst.ForEachValue([&](Value value) {
                            const auto found = replacements.find(value.Def());
                            const auto* definition = !boundary && !inst.IsPseudoOperation() &&
                                                                     found != replacements.end()
                                                             ? found->second
                                                             : value.Def();
                            views.emplace_back(definition, value.Type());
                            if (definition) ++expected_uses[definition];
                        });
                        if (boundary) continue;
                        if (op == OpCode::LoadImm && enabled) {
                            auto [it, inserted] = scalar.try_emplace(
                                    std::pair{inst.GetArg<Imm>(0).Get(), inst.ReturnType()},
                                    &inst,
                                    position);
                            if (!inserted) {
                                if (position - it->second.second <= 8)
                                    replacements.emplace(&inst, it->second.first);
                                else
                                    it->second = {&inst, position};
                            }
                        } else if (op == OpCode::VecLoadConst) {
                            auto [it, inserted] = vectors.try_emplace(
                                    std::pair{inst.GetArg<Imm>(0).Get(), inst.GetArg<Imm>(1).Get()},
                                    &inst);
                            if (!inserted) replacements.emplace(&inst, it->second);
                        } else if (op == OpCode::VecSharedZero) {
                            if (shared_zero)
                                replacements.emplace(&inst, shared_zero);
                            else
                                shared_zero = &inst;
                        }
                    }
                }
                compile::CompileWorkspace workspace;
                auto rewrite = function ? compile::RewriteContext{function, workspace, false}
                                        : compile::RewriteContext{
                                                  standalone.get(), workspace, nullptr, false};
                std::optional<compile::ValueQueueScope> occupied;
                std::optional<compile::ValueWorklist> work;
                const compile::BuilderCapabilities target{};
                if (indexed) {
                    compile::BeginValueCleanup(
                            work, rewrite, features, target, compile::ValueCleanup::All);
                    REQUIRE(work);
                    REQUIRE_FALSE(work->Pending());
                } else {
                    occupied.emplace(workspace);
                    compile::ShareConstants(rewrite, features);
                }
                for (const auto* block : blocks)
                    for (const auto& inst : block->GetInstList()) {
                        std::vector<View> actual;
                        inst.ForEachValue([&](Value value) {
                            actual.emplace_back(value.Def(), value.Type());
                        });
                        REQUIRE(actual == expected.at(&inst));
                        if (!inst.HasValue()) continue;
                        REQUIRE(inst.GetUses(false) == expected_uses[&inst]);
                        if (function) {
                            const auto* value = function->GetHIRValue(Value{&inst});
                            REQUIRE(value);
                            REQUIRE(value->GetUses().size() == expected_uses[&inst]);
                        }
                    }
            }
}

TEST_CASE("constant feedback preserves ordered anchors and raw users",
          "[ir][rewrite][constants][worklist]") {
    for (unsigned shape : {0u, 1u, 2u})
        for (bool enabled : {false, true}) {
            CAPTURE(shape, enabled);
            FeatureSet features{};
            features.const_cse = enabled;
            features.int_imm_fold = true;
            HIRBuilder builder{2, false, features};
            auto* function = shape == 2 ? builder.AppendFunction(Location{0x5e00}) : nullptr;
            IntrusivePtr<Block> standalone, foreign;
            if (!function) standalone.reset(new Block{Location{0x5e00}});
            auto* block = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock()
                                   : standalone.get();
            const auto append = [&]<class... Args>(OpCode op, ValueType type, Args&&... args) {
                return function ? function->AppendInst(op, type, std::forward<Args>(args)...)
                                : block->AppendInst(op, type, std::forward<Args>(args)...);
            };
            const auto anchor = Value{append(OpCode::LoadImm, ValueType::U64, Imm{u64{0x55}})};
            Value initial_duplicate;
            for (unsigned i = 1; i < 7; ++i) {
                if (i == 3)
                    initial_duplicate =
                            Value{append(OpCode::LoadImm, ValueType::U64, Imm{u64{0x55}})};
                else
                    append(OpCode::AdvancePC, ValueType::VOID, Imm{u64{1}});
            }
            const auto early =
                    Value{append(OpCode::LoadUniform, ValueType::U64, Uniform{16, ValueType::U64})};
            for (unsigned i = 0; i < 6; ++i)
                append(OpCode::AdvancePC, ValueType::VOID, Imm{u64{1}});
            const auto late =
                    Value{append(OpCode::LoadUniform, ValueType::U64, Uniform{24, ValueType::U64})};
            const auto* early_store = append(OpCode::StoreUniform,
                                             ValueType::VOID,
                                             Uniform{0, ValueType::U8},
                                             Value{early}.SetCastType(ValueType::U8));
            const auto* late_store =
                    append(OpCode::StoreUniform, ValueType::VOID, Uniform{8, ValueType::U64}, late);
            const auto result = Value{append(OpCode::Xor, ValueType::U64, early, Operand{late})};
            append(OpCode::StoreUniform, ValueType::VOID, Uniform{32, ValueType::U64}, result);
            append(OpCode::SaveFlags, ValueType::VOID, early, Flags::None);
            const auto* pseudo = &block->GetInstList().back();
            const auto label = function ? function->Goto(BOOL{early}) : block->Goto(BOOL{early});
            const auto* branch = &block->GetInstList().back();
            if (function)
                function->BindLabel(label);
            else
                block->BindLabel(label);
            Params params;
            for (unsigned i = 0; i < 300; ++i)
                params.Push(Value{early}.SetCastType(i % 2 ? ValueType::U8 : ValueType::U64));
            const auto* call = append(OpCode::CallDynamic,
                                      ValueType::U64,
                                      Lambda{Value{early}.SetCastType(ValueType::U16)},
                                      std::move(params));
            const Inst* exported;
            if (function) {
                builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x5e40}}));
                function->StoreUniform(Uniform{40, ValueType::U64}, early);
                exported = &function->GetCurrentBlock()->GetBlock()->GetInstList().back();
                function->EndBlock(terminal::ReturnToHost{});
                function->EndFunction();
                function->ComputeRPO();
                function->IdByRPO();
            } else {
                block->SetTerminal(terminal::ReturnToHost{});
                block->ReIdInstr();
                foreign.reset(new Block{Location{0x5e40}});
                foreign->StoreUniform(Uniform{40, ValueType::U64}, early);
                exported = &foreign->GetInstList().back();
                foreign->SetTerminal(terminal::ReturnToHost{});
            }
            compile::CompileWorkspace workspace;
            auto rewrite = function ? compile::RewriteContext{function, workspace, false}
                                    : compile::RewriteContext{block, workspace, nullptr, false};
            if (shape == 1) {
                u16 id = 100;
                for (const auto& inst : block->GetInstList())
                    rewrite.Change(block, &inst, [&](Inst& writer) { writer.SetId(id += 17); });
            }
            const compile::BuilderCapabilities target{};
            std::optional<compile::ValueWorklist> work;
            compile::BeginValueCleanup(work, rewrite, features, target, compile::ValueCleanup::All);
            REQUIRE(work);
            REQUIRE(work->SharedConstant(initial_duplicate.Def()) == enabled);
            // The later edit is enqueued last. CSE must still settle the
            // earlier anchor before looking at the later eight-node window.
            for (const auto value : {early, late})
                rewrite.Change(block, value.Def(), [&](Inst& writer) {
                    writer.DestroyArgs();
                    writer.SetInst(OpCode::LoadImm, Imm{u64{0x55}});
                    writer.SetReturn(ValueType::U64);
                });
            work->Drain();
            REQUIRE_FALSE(work->Pending());
            const auto* expected = enabled ? anchor.Def() : early.Def();
            REQUIRE(early_store->GetArg<Value>(1).Def() == expected);
            REQUIRE(early_store->GetArg<Value>(1).Type() == ValueType::U8);
            REQUIRE(late_store->GetArg<Value>(1).Def() == late.Def());
            REQUIRE(pseudo->GetArg<Value>(0).Def() == early.Def());
            REQUIRE(branch->GetArg<Value>(0).Def() == early.Def());
            REQUIRE(exported->GetArg<Value>(1).Def() == early.Def());
            REQUIRE(call->GetArg<Lambda>(0).GetValue().Def() == expected);
            REQUIRE(call->GetArg<Lambda>(0).GetValue().Type() == ValueType::U16);
            unsigned at = 0;
            for (const auto& param : call->GetArg<Params>(1)) {
                REQUIRE(param.data.value.Def() == expected);
                REQUIRE(param.data.value.Type() == (at++ % 2 ? ValueType::U8 : ValueType::U64));
            }
            REQUIRE(at == 300);
            std::vector<const Block*> members;
            if (function)
                for (const auto* member : function->GetHIRBlocks())
                    members.push_back(member->GetBlock());
            else
                members = {block, foreign.get()};
            std::unordered_map<const Inst*, u32> expected_uses;
            for (const auto* member : members)
                for (const auto& inst : member->GetInstList())
                    inst.ForEachValue([&](Value value) { ++expected_uses[value.Def()]; });
            for (const auto* member : members)
                for (const auto& inst : member->GetInstList()) {
                    if (!inst.HasValue()) continue;
                    REQUIRE(inst.GetUses(false) == expected_uses[&inst]);
                    if (function)
                        REQUIRE(function->GetHIRValue(Value{&inst})->GetUses().size() ==
                                expected_uses[&inst]);
                }
        }
}

TEST_CASE("constant feedback folds single-use operands before sharing",
          "[ir][rewrite][constants][worklist]") {
    for (bool function_mode : {false, true})
        for (bool enabled : {false, true}) {
            CAPTURE(function_mode, enabled);
            FeatureSet features{};
            features.const_cse = enabled;
            features.int_imm_fold = true;
            HIRBuilder builder{2, false, features};
            auto* function = function_mode ? builder.AppendFunction(Location{0x5ec0}) : nullptr;
            IntrusivePtr<Block> standalone;
            if (!function) standalone.reset(new Block{Location{0x5ec0}});
            auto* block = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock()
                                   : standalone.get();
            const auto append = [&]<class... Args>(OpCode op, ValueType type, Args&&... args) {
                return function ? function->AppendInst(op, type, std::forward<Args>(args)...)
                                : block->AppendInst(op, type, std::forward<Args>(args)...);
            };
            const auto anchor = Value{append(OpCode::LoadImm, ValueType::U64, Imm{u64{0x55}})};
            append(OpCode::StoreUniform, ValueType::VOID, Uniform{0, ValueType::U64}, anchor);
            const auto constant =
                    Value{append(OpCode::LoadUniform, ValueType::U64, Uniform{16, ValueType::U64})};
            const auto input =
                    Value{append(OpCode::LoadUniform, ValueType::U64, Uniform{24, ValueType::U64})};
            const auto result =
                    Value{append(OpCode::Add, ValueType::U64, input, Operand{constant})};
            if (function) {
                builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x5ef0}}));
                function->StoreUniform(Uniform{8, ValueType::U64}, result);
                function->EndBlock(terminal::ReturnToHost{});
                function->EndFunction();
                function->ComputeRPO();
                function->IdByRPO();
            } else {
                block->StoreUniform(Uniform{8, ValueType::U64}, result);
                block->SetTerminal(terminal::ReturnToHost{});
                block->ReIdInstr();
            }
            compile::CompileWorkspace workspace;
            auto rewrite = function ? compile::RewriteContext{function, workspace, false}
                                    : compile::RewriteContext{block, workspace, nullptr, false};
            const auto target = backend::arm64::BuilderTarget();
            std::optional<compile::ValueWorklist> work;
            compile::BeginValueCleanup(work, rewrite, features, target, compile::ValueCleanup::All);
            REQUIRE(work);
            REQUIRE(result.Def()->GetArg<Operand>(1).GetLeft().value.Def() == constant.Def());
            rewrite.Change(block, constant.Def(), [&](Inst& writer) {
                writer.DestroyArgs();
                writer.SetInst(OpCode::LoadImm, Imm{u64{0x55}});
                writer.SetReturn(ValueType::U64);
            });
            work->Drain();
            REQUIRE_FALSE(work->Pending());
            const auto operand = result.Def()->GetArg<Operand>(1);
            REQUIRE(operand.IsImm());
            REQUIRE(operand.GetLeft().imm.Get() == 0x55);
            REQUIRE(constant.Def()->GetUses(false) == 0);
            REQUIRE(anchor.Def()->GetUses(false) == 1);
        }
}

TEST_CASE("constant feedback responds to DCE windows and local control boundaries",
          "[ir][rewrite][constants][worklist][dce]") {
    enum Boundary { None, Goto, NotGoto, Bound };
    for (const auto boundary : {None, Goto, NotGoto, Bound})
        for (unsigned gaps : {1u, 2u})
            for (bool enabled : {false, true}) {
                CAPTURE(boundary, gaps, enabled);
                FeatureSet features{};
                features.const_cse = enabled;
                features.int_imm_fold = true;
                Block block{0, Location{0x5f00}};
                const auto anchor = block.LoadImm<U64>(Imm{u64{0x333}});
                block.StoreUniform(Uniform{0, ValueType::U64}, anchor);
                std::optional<Value> label;
                if (boundary != None) {
                    const auto condition = block.LoadImm<BOOL>(Imm{u64{1}});
                    label = boundary == NotGoto ? block.NotGoto(condition) : block.Goto(condition);
                    if (boundary == Bound) block.BindLabel(*label);
                }
                for (unsigned i = 0; i < gaps; ++i)
                    block.LoadUniform<U64>(Uniform{16, ValueType::U64});
                while (block.GetInstList().size() < 8 + gaps) block.AdvancePC(Imm{u64{1}});
                const auto duplicate = block.LoadImm<U64>(Imm{u64{0x333}});
                block.StoreUniform(Uniform{8, ValueType::U64}, duplicate);
                const auto* store = &block.GetInstList().back();
                if (label && boundary != Bound) block.BindLabel(*label);
                const auto input = block.LoadUniform<U64>(Uniform{24, ValueType::U64});
                const auto result = block.Xor<U64>(input, Operand{duplicate});
                block.StoreUniform(Uniform{32, ValueType::U64}, result);
                block.SetTerminal(terminal::ReturnToHost{});
                block.ReIdInstr();
                compile::CompileWorkspace workspace;
                compile::RewriteContext rewrite{&block, workspace};
                const compile::BuilderCapabilities target{};
                std::optional<compile::ValueWorklist> work;
                compile::BeginValueCleanup(
                        work, rewrite, features, target, compile::ValueCleanup::All);
                REQUIRE(work);
                REQUIRE(store->GetArg<Value>(1).Def() == duplicate.Def());
                rewrite.CollectDead();
                work->Drain();
                const auto* expected = enabled && boundary == None ? anchor.Def() : duplicate.Def();
                REQUIRE(store->GetArg<Value>(1).Def() == expected);
                REQUIRE(result.Def()->GetArg<Operand>(1).GetLeft().value.Def() == expected);
                rewrite.CollectDead();
                work->Drain();
                REQUIRE_FALSE(work->Pending());
                std::unordered_map<const Inst*, u32> expected_uses;
                for (const auto& inst : block.GetInstList())
                    inst.ForEachValue([&](Value value) { ++expected_uses[value.Def()]; });
                for (const auto& inst : block.GetInstList())
                    if (inst.HasValue()) REQUIRE(inst.GetUses(false) == expected_uses[&inst]);
            }
}

TEST_CASE("IR constant CSE preserves each operand width view", "[ir][regression]") {
    for (auto view : {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64}) {
        INFO("view=" << unsigned(view));
        Block block{0, Location{0x1000}};
        auto canonical = block.LoadImm(Imm{u64{0x1122334455667788}});
        block.StoreUniform(Uniform{0, ValueType::U64}, canonical);
        auto duplicate = block.LoadImm(Imm{u64{0x1122334455667788}});
        block.StoreUniform(Uniform{8, view}, Value{duplicate}.SetCastType(view));
        block.SetTerminal(terminal::ReturnToHost{});
        compile::CompileWorkspace workspace;
        compile::RewriteContext rewrite{&block, workspace};
        compile::ValueQueueScope unused_value_storage{workspace};
        compile::FoldValues(rewrite, FeatureSet{});
        auto& store = block.GetInstList().back();
        REQUIRE(store.GetArg<Value>(1).Def() == canonical.Def());
        REQUIRE(store.GetArg<Value>(1).Type() == view);
        RewriteFixture{&block}.Get().CollectDead();
        REQUIRE(block.GetInstList().size() == 3);
    }
}

#if defined(__aarch64__)
namespace {
thread_local Runtime* region_poll_runtime{};

u64 InterruptRegionPoll() noexcept {
    region_poll_runtime->SignalInterrupt();
    return 0;
}
}  // namespace

TEST_CASE("non-self cycle polls resume at their edge target after an exit-latch fault",
          "[ir][regression][jit][cfg][fault]") {
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = 0x2000;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
    config.region_edges = true;
    config.global_opts = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove;
    backend::AddressSpace space{config};
    REQUIRE(space.ExitLatchEnabled());
    const auto module = space.GetDefaultModule();
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{0x1000}, Location{0x1041});
    const auto header_count = function->LoadUniform<U64>(Uniform{0, ValueType::U64});
    const auto next_header = function->Add<U64>(header_count, Operand{Imm{u64{1}}});
    function->StoreUniform(Uniform{0, ValueType::U64}, next_header);
    function->AdvancePC(Imm{u64{1}});
    auto* body = builder.LinkBlock(terminal::LinkBlock{Location{0x1040}});
    builder.SetCurBlock(body);
    const auto body_count = function->LoadUniform<U64>(Uniform{8, ValueType::U64});
    const auto next_body = function->Add<U64>(body_count, Operand{Imm{u64{1}}});
    function->StoreUniform(Uniform{8, ValueType::U64}, next_body);
    function->CallLambda<U64>(Lambda{Imm{reinterpret_cast<uintptr_t>(&InterruptRegionPoll)}});
    function->AdvancePC(Imm{u64{1}});
    builder.LinkBlock(terminal::LinkBlock{Location{0x1000}});
    function->EndFunction();
    REQUIRE(TranslateIR(module, function) != nullptr);
    Runtime runtime{&space};
    region_poll_runtime = &runtime;
    const auto reset = ScopeExit([] { region_poll_runtime = nullptr; });
    runtime.SetLocation(0x1000);
    std::array<u64, 2> counts{};
    std::memcpy(runtime.GetUniformBuffer().data(), counts.data(), sizeof(counts));
    for (u64 visits = 1; visits <= 2; ++visits) {
        REQUIRE(runtime.Run() == HaltReason::Signal);
        REQUIRE(runtime.GetLocation() == 0x1000);
        std::memcpy(counts.data(), runtime.GetUniformBuffer().data(), sizeof(counts));
        REQUIRE(counts[0] == visits);
        REQUIRE(counts[1] == visits);
        runtime.ClearInterrupt();
    }
}

TEST_CASE("width worklist preserves defined integer extensions after DCE",
          "[ir][regression][jit][width]") {
    for (const bool function_mode : {false, true})
        for (const bool optimize : {false, true})
            for (unsigned consumer = 0; consumer < 3; ++consumer) {
                CAPTURE(function_mode, optimize, consumer);
                Config config{};
                config.loc_start = 0x1000;
                config.loc_end = 0x2000;
                config.enable_jit = true;
                config.backend_isa = kArm64;
                config.uniform_buffer_size = 32;
                config.stack_alignment = 16;
                config.global_opts =
                        optimize ? Optimizations::ConstantFolding | Optimizations::DeadCodeRemove
                                 : Optimizations{};
                backend::AddressSpace space{config};
                HIRBuilder builder{1, false, FeatureSet{}};
                auto* function =
                        function_mode ? builder.AppendFunction(Location{0x1000}, Location{0x1040})
                                      : nullptr;
                IntrusivePtr<Block> block{function_mode ? nullptr : new Block{0, Location{0x1000}}};
                const auto append = [&](OpCode op, ValueType type, const auto&... args) {
                    return Value{function ? function->AppendInst(op, type, args...)
                                          : block->AppendInst(op, type, args...)};
                };
                const auto wide =
                        append(OpCode::LoadUniform, ValueType::U64, Uniform{0, ValueType::U64});
                const auto extracted = append(OpCode::BitExtract,
                                              ValueType::U32,
                                              Value{wide}.SetCastType(ValueType::U32),
                                              Imm{u32{0}},
                                              Imm{u32{32}});
                // The unused second use disappears in DCE. Reconsidering the
                // extract must retain the explicit zero/sign extension below.
                append(OpCode::Add, ValueType::U32, extracted, Operand{Imm{u32{1}}});
                const auto value =
                        consumer == 0 ? append(OpCode::ZeroExtend32To64, ValueType::U64, extracted)
                        : consumer == 1 ? append(OpCode::SignExtend,
                                                 ValueType::U64,
                                                 Value{extracted}.SetCastType(ValueType::S32))
                                        : append(OpCode::ZeroExtend64, ValueType::U64, extracted);
                append(OpCode::StoreUniform, ValueType::VOID, Uniform{8, ValueType::U64}, value);
                auto module = space.GetDefaultModule();
                if (function) {
                    function->EndBlock(terminal::ReturnToHost{});
                    function->EndFunction();
                    function->ComputeRPO();
                    function->IdByRPO();
                } else {
                    block->SetTerminal(terminal::ReturnToHost{});
                    REQUIRE(module->Push(block.get()));
                }
                auto* entry = function ? TranslateIR(module, function) : TranslateIR(module, block);
                REQUIRE(entry != nullptr);
                space.PushCodeCache(0x1000, entry);
                Runtime runtime{&space};
                auto state = runtime.GetUniformBuffer();
                const u64 input = 0xdeadbeef87654321;
                std::memcpy(state.data(), &input, sizeof(input));
                runtime.SetLocation(0x1000);
                REQUIRE(runtime.Run() == HaltReason::CallHost);
                u64 result;
                std::memcpy(&result, state.data() + 8, sizeof(result));
                REQUIRE(result == (consumer == 1 ? u64{0xffffffff87654321} : u64{0x87654321}));
            }
}

TEST_CASE("pending flag clears are observed before and after guest commit",
          "[ir][regression][jit][flags][pending]") {
    for (const bool function_mode : {false, true})
        for (const bool optimize : {false, true})
            for (const bool with_producer : {false, true})
                for (const bool commit_before_read : {false, true}) {
                    CAPTURE(function_mode, optimize, with_producer, commit_before_read);
                    Config config{};
                    config.loc_start = 0x1000;
                    config.loc_end = 0x2000;
                    config.enable_jit = true;
                    config.backend_isa = kArm64;
                    config.uniform_buffer_size = 64;
                    config.stack_alignment = 16;
                    config.global_opts = optimize ? Optimizations::ConstantFolding |
                                                            Optimizations::DeadCodeRemove |
                                                            Optimizations::FlagElimination
                                                  : Optimizations{};
                    backend::AddressSpace space{config};
                    HIRBuilder builder{1, false, FeatureSet{}};
                    auto* function = function_mode ? builder.AppendFunction(Location{0x1000},
                                                                            Location{0x1040})
                                                   : nullptr;
                    IntrusivePtr<Block> block{function_mode ? nullptr
                                                            : new Block{0, Location{0x1000}}};
                    const auto append = [&](OpCode op, ValueType type, const auto&... args) {
                        return Value{function ? function->AppendInst(op, type, args...)
                                              : block->AppendInst(op, type, args...)};
                    };
                    const auto input =
                            append(OpCode::LoadUniform, ValueType::U64, Uniform{0, ValueType::U64});
                    const auto clear = [&](Flags mask) {
                        Value producer = input;
                        if (with_producer) {
                            producer = append(
                                    OpCode::Add, ValueType::U64, input, Operand{Imm{u64{1}}});
                            append(OpCode::SaveFlags, ValueType::VOID, producer, Flags::All);
                        }
                        append(OpCode::ClearFlags, ValueType::VOID, mask);
                        if (commit_before_read)
                            append(OpCode::AdvancePC, ValueType::VOID, Imm{u32{1}});
                        return producer;
                    };
                    const auto store = [&](u32 offset, Value value) {
                        append(OpCode::StoreUniform,
                               ValueType::VOID,
                               Uniform{offset, value.Type()},
                               value);
                    };
                    clear(Flags::Carry);
                    store(16, append(OpCode::TestFlags, ValueType::U8, Flags::Carry));
                    clear(Flags::Negate);
                    store(17, append(OpCode::TestNotFlags, ValueType::U8, Flags::Negate));
                    clear(Flags::Zero);
                    store(18, append(OpCode::CondSet, ValueType::U8, Cond::EQ));
                    const auto yes = append(OpCode::LoadImm, ValueType::U8, Imm{u32{0x11}});
                    const auto no = append(OpCode::LoadImm, ValueType::U8, Imm{u32{0x22}});
                    clear(Flags::Overflow);
                    store(19, append(OpCode::CondSelect, ValueType::U8, Cond::VS, yes, no));
                    clear(Flags::Zero);
                    const auto condition = append(OpCode::CondSet, ValueType::U8, Cond::EQ);
                    store(22, append(OpCode::Select, ValueType::U8, condition, yes, no));
                    clear(Flags::Parity);
                    store(20, append(OpCode::TestFlags, ValueType::U8, Flags::Parity));
                    clear(Flags::AuxiliaryCarry);
                    store(21, append(OpCode::TestNotFlags, ValueType::U8, Flags::AuxiliaryCarry));
                    const auto whole = clear(Flags::All);
                    // The legacy mask does not restrict GetFlags' actual read.
                    store(24, append(OpCode::GetFlags, ValueType::U64, whole, Flags::Carry));
                    clear(Flags::Carry);
                    store(32, append(OpCode::Adc, ValueType::U64, input, Operand{Imm{u64{0}}}));
                    clear(Flags::Carry);
                    store(40, append(OpCode::Sbb, ValueType::U64, input, Operand{Imm{u64{0}}}));
                    clear(Flags::Carry);
                    append(OpCode::InvertCarry, ValueType::VOID);
                    store(48, append(OpCode::TestFlags, ValueType::U8, Flags::Carry));
                    clear(Flags::Carry);
                    const auto carry = append(OpCode::TestFlags, ValueType::U32, Flags::Carry);
                    store(52,
                          append(OpCode::Add,
                                 ValueType::U32,
                                 Value{input}.SetCastType(ValueType::U32),
                                 Operand{carry}));
                    // Also observe final state at host return, without a
                    // reader or AdvancePC to force the final clear to commit.
                    if (with_producer) {
                        const auto last =
                                append(OpCode::Add, ValueType::U64, input, Operand{Imm{u64{1}}});
                        append(OpCode::SaveFlags, ValueType::VOID, last, Flags::All);
                    }
                    append(OpCode::ClearFlags, ValueType::VOID, Flags::NZCV);
                    auto module = space.GetDefaultModule();
                    if (function) {
                        function->EndBlock(terminal::ReturnToHost{});
                        function->EndFunction();
                        function->ComputeRPO();
                        function->IdByRPO();
                    } else {
                        block->SetTerminal(terminal::ReturnToHost{});
                        REQUIRE(module->Push(block.get()));
                    }
                    auto* entry =
                            function ? TranslateIR(module, function) : TranslateIR(module, block);
                    REQUIRE(entry != nullptr);
                    space.PushCodeCache(0x1000, entry);
                    Runtime runtime{&space};
                    auto state = runtime.GetUniformBuffer();
                    for (const u64 bits : {u64{0xffffffffffffffff}, u64{0x7fffffffffffffff}}) {
                        std::fill(state.begin(), state.end(), 0xa5);
                        std::memcpy(state.data(), &bits, sizeof(bits));
                        // Set N/Z/C/V and AF; parity byte zero means PF=1.
                        runtime.GetState()->host_cpu_flags = 0xf4000000u;
                        runtime.SetLocation(0x1000);
                        REQUIRE(runtime.Run() == HaltReason::CallHost);
                        REQUIRE(state[16] == 0);
                        REQUIRE(state[17] == 1);
                        REQUIRE(state[18] == 0);
                        REQUIRE(state[19] == 0x22);
                        REQUIRE(state[20] == 0);
                        REQUIRE(state[21] == 1);
                        REQUIRE(state[22] == 0x22);
                        const auto word = [&](u32 offset) {
                            u64 value;
                            std::memcpy(&value, state.data() + offset, sizeof(value));
                            return value;
                        };
                        REQUIRE(word(24) == 1);
                        REQUIRE(word(32) == bits);
                        REQUIRE(word(40) == bits - 1);
                        REQUIRE(state[48] == 1);
                        u32 narrow_sum;
                        std::memcpy(&narrow_sum, state.data() + 52, sizeof(narrow_sum));
                        REQUIRE(narrow_sum == static_cast<u32>(bits));
                        REQUIRE((runtime.GetState()->host_cpu_flags & 0xf0000000u) == 0);
                    }
                }
}

TEST_CASE("live logical flags and whole GPR publication share their zeroing result",
          "[ir][regression][jit][width][flags]") {
    for (const bool function_mode : {false, true})
        for (const bool observe_flags : {false, true})
            for (const auto projection_type : {ValueType::U32, ValueType::U64})
                for (const auto publication_type : {ValueType::U16, ValueType::U64}) {
                    CAPTURE(function_mode, observe_flags, projection_type, publication_type);
                    // A lone reserved home does not admit the production
                    // coalescing policy. Supply the complete level-2 ABI.
                    std::array<UniformMapDesc, 12> homes{UniformMapDesc{0, 8, 22, false},
                                                         UniformMapDesc{24, 8, 23, false},
                                                         UniformMapDesc{32, 8, 29, false},
                                                         UniformMapDesc{40, 8, 19, false},
                                                         UniformMapDesc{48, 8, 20, false},
                                                         UniformMapDesc{56, 8, 21, false},
                                                         UniformMapDesc{64, 8, 0, false},
                                                         UniformMapDesc{72, 8, 1, false},
                                                         UniformMapDesc{80, 8, 2, false},
                                                         UniformMapDesc{88, 8, 3, false},
                                                         UniformMapDesc{96, 8, 4, false},
                                                         UniformMapDesc{104, 8, 5, false}};
                    Config config{};
                    config.loc_start = 0x1000;
                    config.loc_end = 0x2000;
                    config.enable_jit = true;
                    config.backend_isa = kArm64;
                    config.uniform_buffer_size = 112;
                    config.stack_alignment = 16;
                    config.buffers_static_alloc = homes;
                    config.global_opts = Optimizations::ConstantFolding |
                                         Optimizations::DeadCodeRemove |
                                         Optimizations::FlagElimination;
                    backend::AddressSpace space{config};
                    const bool coalescing_pool =
                            backend::X86PinExtLevel2Enabled(space.GetTrampolines().GetGPRRegs());
                    REQUIRE(coalescing_pool == (GetSvmConfig().x86_pin_ext >= 2));
                    HIRBuilder builder{1, false, FeatureSet{}};
                    auto* function = function_mode ? builder.AppendFunction(Location{0x1000},
                                                                            Location{0x1040})
                                                   : nullptr;
                    IntrusivePtr<Block> block{function_mode ? nullptr
                                                            : new Block{0, Location{0x1000}}};
                    const auto append = [&](OpCode op, ValueType type, const auto&... args) {
                        return Value{function ? function->AppendInst(op, type, args...)
                                              : block->AppendInst(op, type, args...)};
                    };
                    const auto input =
                            append(OpCode::LoadUniform, ValueType::U32, Uniform{8, ValueType::U32});
                    const auto extracted = append(
                            OpCode::BitExtract, ValueType::U32, input, Imm{u32{0}}, Imm{u32{32}});
                    // Normalizing the extract exposes a self-XOR. Its flags
                    // must survive even when its data result becomes known.
                    const auto zero =
                            append(OpCode::Xor, ValueType::U32, extracted, Operand{input});
                    if (observe_flags) {
                        append(OpCode::ClearFlags,
                               ValueType::VOID,
                               Flags::Carry | Flags::Overflow | Flags::AuxiliaryCarry);
                        append(OpCode::SaveFlags,
                               ValueType::VOID,
                               zero,
                               Flags::Zero | Flags::Negate | Flags::Parity);
                    }
                    const auto projected = append(OpCode::ZeroExtend32To64, projection_type, zero);
                    append(OpCode::SetHostGPR,
                           ValueType::VOID,
                           Value{projected}.SetCastType(publication_type),
                           HostRegIndex(22),
                           Imm{u32{0}});
                    append(OpCode::AdvancePC, ValueType::VOID, Imm{u32{1}});
                    if (observe_flags) {
                        u32 offset = 16;
                        for (const auto flag : {Flags::Parity,
                                                Flags::Zero,
                                                Flags::Negate,
                                                Flags::Carry,
                                                Flags::Overflow}) {
                            const auto bit = append(OpCode::TestFlags, ValueType::U8, flag);
                            append(OpCode::StoreUniform,
                                   ValueType::VOID,
                                   Uniform{offset++, ValueType::U8},
                                   bit);
                        }
                    }
                    auto module = space.GetDefaultModule();
                    if (function) {
                        function->EndBlock(terminal::ReturnToHost{});
                        function->EndFunction();
                        function->ComputeRPO();
                        function->IdByRPO();
                    } else {
                        block->SetTerminal(terminal::ReturnToHost{});
                        REQUIRE(module->Push(block.get()));
                    }
                    auto* entry =
                            function ? TranslateIR(module, function) : TranslateIR(module, block);
                    REQUIRE(entry != nullptr);
                    space.PushCodeCache(0x1000, entry);
                    const auto features = ResolveFeatureSet(module->GetModuleConfig());
                    if (observe_flags && publication_type == ValueType::U64 &&
                        features.ra_coalesce && coalescing_pool && GetSvmConfig().flags_regs) {
                        backend::FaultEntry allocation;
                        REQUIRE(module->LookupCodeAllocation(static_cast<u8*>(entry), allocation));
                        unsigned shared_zeroes{};
                        for (const auto* at = allocation.host_start;
                             at + sizeof(u32) <= allocation.host_end;
                             at += sizeof(u32)) {
                            u32 instruction;
                            std::memcpy(&instruction, at, sizeof(instruction));
                            // ANDS W22, WZR, WZR writes both the flags result
                            // and the published home, instead of a second zero.
                            shared_zeroes += instruction == 0x6a1f03f6u;
                        }
                        REQUIRE(shared_zeroes == 1);
                    }
                    Runtime runtime{&space};
                    auto state = runtime.GetUniformBuffer();
                    for (const u64 input_bits :
                         {u64{0xdeadbeef87654321}, u64{0x0123456789abcdef}}) {
                        std::fill(state.begin(), state.end(), 0xa5);
                        const u64 initial_home = 0xfeedbeefcafebabe;
                        std::memcpy(state.data(), &initial_home, sizeof(initial_home));
                        std::memcpy(state.data() + 8, &input_bits, sizeof(input_bits));
                        runtime.GetState()->host_cpu_flags = 0xa0000000u;
                        runtime.SetLocation(0x1000);
                        REQUIRE(runtime.Run() == HaltReason::CallHost);
                        u64 result;
                        std::memcpy(&result, state.data(), sizeof(result));
                        REQUIRE(result == (publication_type == ValueType::U64
                                                   ? u64{0}
                                                   : u64{0xfeedbeefcafe0000}));
                        if (observe_flags) {
                            REQUIRE(state[16] == 1);
                            REQUIRE(state[17] == 1);
                            REQUIRE(state[18] == 0);
                            REQUIRE(state[19] == 0);
                            REQUIRE(state[20] == 0);
                        }
                    }
                }
}

TEST_CASE("IR CSE narrow stores retain surrounding bytes in ARM64 JIT", "[ir][regression][jit]") {
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = 0x2000;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove;
    backend::AddressSpace space{config};
    Runtime runtime{&space};
    auto state = runtime.GetUniformBuffer();
    std::fill(state.begin(), state.end(), 0xa5);
    IntrusivePtr<Block> block{new Block(0, Location{0x1000})};
    auto first = block->LoadImm(Imm{u64{0x1122334455667788}});
    block->StoreUniform(Uniform{0, ValueType::U64}, first);
    auto second = block->LoadImm(Imm{u64{0x1122334455667788}});
    block->StoreUniform(Uniform{8, ValueType::U8}, Value{second}.SetCastType(ValueType::U8));
    block->SetTerminal(terminal::ReturnToHost{});
    auto module = space.GetModule(0x1000);
    REQUIRE(module->Push(block.get()));
    REQUIRE(TranslateIR(module, block) != nullptr);
    runtime.SetLocation(0x1000);
    REQUIRE(runtime.Run() == HaltReason::CallHost);
    REQUIRE(state[8] == 0x88);
    for (size_t i = 9; i < state.size(); ++i) REQUIRE(state[i] == 0xa5);
}
#endif

TEST_CASE("IR effects separate liveness from observable behavior", "[ir][effects]") {
    Block block{0, Location{0x1000}};
    auto address = block.LoadImm(Imm{u64{0x1000}});
    auto sum = block.Add(address, Operand{Imm{u64{1}}});
    block.StoreUniform(Uniform{0, ValueType::U64}, sum);
    REQUIRE(address.Def()->GetUses(false) != 0);
    REQUIRE_FALSE(address.Def()->HasSideEffects());
    auto load = block.LoadMemory(ValueType::U64, Operand{address});
    REQUIRE(load.Def()->HasSideEffects());
    block.SetTerminal(terminal::ReturnToHost{});
    RewriteFixture{&block}.Get().CollectDead();
    REQUIRE(load.Def()->GetUses(false) == 0);
    REQUIRE(load.Def()->GetOp() == OpCode::LoadMemory);
    for (u32 op = 1; op < static_cast<u32>(OpCode::BASE_COUNT); ++op)
        REQUIRE(GetIRMetaInfo(static_cast<OpCode>(op)).effects != IREffect::Unknown);
}

TEST_CASE("helper state traits are shared without weakening opaque call effects",
          "[ir][effects][helper]") {
    for (const auto access : {HelperGuestStateEffect::MayReadWrite,
                              HelperGuestStateEffect::ReadOnly,
                              HelperGuestStateEffect::WriteOnly,
                              HelperGuestStateEffect::None})
        for (const bool fault : {false, true})
            for (const bool reentry : {false, true})
                for (const bool direct : {false, true}) {
                    CAPTURE(access, fault, reentry, direct);
                    Block block{0, Location{0x1000}};
                    const auto target = block.GetHostGPR(ValueType::U64, Imm{20u}, Imm{0u});
                    const auto helper = direct
                            ? Lambda{DataClass{Imm{u64{1}}},
                                     HelperCallTraits{
                                             .uniform = UniformEffectId::None,
                                             .guest_state = access,
                                             .fault = fault ? HelperFaultEffect::MayFault
                                                            : HelperFaultEffect::NoDirectFault,
                                             .reentry = reentry ? HelperReentryEffect::MayReenter
                                                                : HelperReentryEffect::NoReentry,
                                             .host_flags = HostFlagsEffect::PreservesNZCV}}
                            : Lambda{target};
                    const bool reads = !direct || access == HelperGuestStateEffect::MayReadWrite ||
                                       access == HelperGuestStateEffect::ReadOnly;
                    const bool writes = !direct || access == HelperGuestStateEffect::MayReadWrite ||
                                        access == HelperGuestStateEffect::WriteOnly;
                    const bool observes = reads || writes || fault || reentry || !direct;
                    const auto state = HelperStateEffects::Resolve(helper);
                    REQUIRE(state.direct == direct);
                    REQUIRE(state.ReadsGuestState() == reads);
                    REQUIRE(state.WritesGuestState() == writes);
                    REQUIRE(state.MayFault() == (!direct || fault));
                    REQUIRE(state.MayReenter() == (!direct || reentry));
                    REQUIRE(state.RequiresGuestStatePublication() == observes);
                    REQUIRE(state.uniform ==
                            (direct ? UniformEffectId::None : UniformEffectId::Unknown));
                    const auto contract = backend::arm64::HelperCallContract::Resolve(helper, {});
                    REQUIRE(contract.IsDirect() == direct);
                    REQUIRE(contract.ReadsGuestState() == reads);
                    REQUIRE(contract.WritesGuestState() == writes);
                    REQUIRE(contract.MayFault() == (!direct || fault));
                    REQUIRE(contract.MayReenter() == (!direct || reentry));
                    REQUIRE(contract.RequiresGuestStatePublication() == observes);
                    REQUIRE(contract.RetainsPendingNZCV() == (direct && !observes));
                    const auto call = block.CallLambda(ValueType::VOID, helper);
                    const auto effects = call.Def()->GetEffects();
                    REQUIRE(True(effects & IREffect::ReadGuestState) ==
                            (!direct || reentry || reads));
                    REQUIRE(True(effects & IREffect::WriteGuestState) ==
                            (!direct || reentry || writes));
                    REQUIRE(True(effects & IREffect::MayFault) == (!direct || reentry || fault));
                    REQUIRE(True(effects & IREffect::ReadMemory));
                    REQUIRE(True(effects & IREffect::WriteMemory));
                    REQUIRE(True(effects & IREffect::Call));
                }
}

TEST_CASE("helper write policies distinguish declared ranges from nonreentrant state",
          "[ir][effects][state]") {
    static const UniformEffectSet missing{nullptr, 1};
    static constexpr UniformEffectRange overflow_ranges[]{{0, 8}, {32, 1}};
    static const UniformEffectSet overflow{overflow_ranges, 2};
    const auto missing_id = RegisterUniformEffectSet(&missing);
    const auto overflow_id = RegisterUniformEffectSet(&overflow);
    struct Case {
        HelperCallTraits traits;
        bool declared, invariant;
    };
    const Case cases[]{
            {{.uniform = UniformEffectId::None}, true, false},
            {{.uniform = UniformEffectId::None,
              .reentry = HelperReentryEffect::NoReentry}, true, true},
            {{.guest_state = HelperGuestStateEffect::ReadOnly,
              .reentry = HelperReentryEffect::NoReentry}, false, true},
            {{.guest_state = HelperGuestStateEffect::None,
              .fault = HelperFaultEffect::NoDirectFault,
              .reentry = HelperReentryEffect::NoReentry}, false, true},
            {{.uniform = missing_id,
              .reentry = HelperReentryEffect::NoReentry}, false, false},
            {{.uniform = overflow_id,
              .reentry = HelperReentryEffect::NoReentry}, false, false},
            {{.uniform = missing_id,
              .guest_state = HelperGuestStateEffect::ReadOnly,
              .reentry = HelperReentryEffect::NoReentry}, false, true},
    };
    for (const auto op : {OpCode::CallLambda, OpCode::CallDynamic, OpCode::CallLocation})
        for (const bool direct : {false, true})
            for (const auto& item : cases) {
                CAPTURE(op, direct, item.declared, item.invariant);
                Block block{0, Location{0x1010}};
                const auto target = block.GetHostGPR(ValueType::U64, Imm{20u}, Imm{0u});
                const auto helper = direct ? Lambda{DataClass{Imm{u64{1}}}, item.traits}
                                           : Lambda{target};
                Params params;
                const auto call = op == OpCode::CallLambda ? block.CallLambda(ValueType::VOID, helper)
                                  : op == OpCode::CallDynamic ? block.CallDynamic(ValueType::VOID, helper, std::move(params))
                                                             : block.CallLocation(ValueType::VOID, helper, std::move(params));
                const auto* declared = compile::HelperUniformWrites(
                        *call.Def(), 32, compile::UniformWritePolicy::Declared);
                const auto* invariant = compile::HelperUniformWrites(
                        *call.Def(), 32, compile::UniformWritePolicy::NoReentry);
                REQUIRE((declared != nullptr) == (direct && item.declared));
                REQUIRE((invariant != nullptr) == (direct && item.invariant));
                if (declared) REQUIRE(declared->count == 0);
                if (invariant) REQUIRE(invariant->count == 0);
                if (op != OpCode::CallLambda) {
                    const auto effects = call.Def()->GetEffects();
                    REQUIRE(True(effects & IREffect::ReadGuestState));
                    REQUIRE(True(effects & IREffect::WriteGuestState));
                    REQUIRE(True(effects & IREffect::MayFault));
                }
            }
}

TEST_CASE("helper effects invalidate decode and residual uniform forwarding",
          "[ir][builder][state]") {
    const char* raw = GetRawSvmConfigEnvForTest("SVM_BUILDER_UNIFORM");
    const auto saved = raw ? std::optional<std::string>{raw} : std::nullopt;
    const auto restore = ScopeExit([&] {
        if (saved)
            SetSvmConfigEnvForTest("SVM_BUILDER_UNIFORM", saved->c_str(), 1);
        else
            UnsetSvmConfigEnvForTest("SVM_BUILDER_UNIFORM");
    });
    REQUIRE(SetSvmConfigEnvForTest("SVM_BUILDER_UNIFORM", "1", 1) == 0);
    static constexpr UniformEffectRange touched_ranges[]{{0, 8}};
    static const UniformEffectSet touched{touched_ranges, 1};
    const auto touched_id = RegisterUniformEffectSet(&touched);
    const compile::GuestStateLayout layout{.uniform_size = 64};
    for (const bool decode : {false, true})
        for (const bool range : {false, true})
            for (const auto type : {ValueType::U64, ValueType::V128})
                for (const auto call : {OpCode::CallLambda, OpCode::CallLocation, OpCode::CallDynamic})
                    for (const auto effect : {UniformEffectId::Unknown, UniformEffectId::None, touched_id}) {
                        CAPTURE(decode, range, type, call, effect);
                        FeatureSet features{};
                        features.ir_uniform_range = range;
                        features.uniform_dse = false;
                        features.xmm_fault_sink = false;
                        Block block{0, Location{0x1020}};
                        compile::IrBuilder builder{&block};
                        if (decode) builder.ConfigureGuestState(&layout, features, {});
                        const auto low_value = builder.LoadImm(type, Imm{u64{11}});
                        const auto high_value = builder.LoadImm(type, Imm{u64{22}});
                        builder.StoreUniform(Uniform{0, type}, low_value);
                        builder.StoreUniform(Uniform{32, type}, high_value);
                        Params params;
                        const Lambda helper{DataClass{Imm{u64{1}}}, effect};
                        if (call == OpCode::CallLambda) builder.CallLambda(ValueType::VOID, helper);
                        else if (call == OpCode::CallLocation) builder.CallLocation(ValueType::VOID, helper, std::move(params));
                        else builder.CallDynamic(ValueType::VOID, helper, std::move(params));
                        const auto low = builder.LoadUniform(Uniform{0, type});
                        const auto high = builder.LoadUniform(Uniform{32, type});
                        builder.StoreUniform(Uniform{16, type}, low);
                        builder.StoreUniform(Uniform{48, type}, high);
                        block.SetTerminal(terminal::ReturnToHost{});
                        if (!decode) compile::CleanupState(RewriteFixture{&block, false}.Get(), layout, features);
                        REQUIRE((low.Def()->GetOp() != OpCode::LoadUniform) ==
                                (range && effect == UniformEffectId::None));
                        REQUIRE((high.Def()->GetOp() != OpCode::LoadUniform) ==
                                (range && effect != UniformEffectId::Unknown));
                    }

    // A conditional upper-lane store preserves the low lane on both paths.
    // Compare both IR entry points with a byte oracle, including narrow-FPR
    // opt-outs. The snapshots must survive writes in an unrelated output slot.
    constexpr u32 uniform_size = 64;
    for (const bool decode : {false, true})
        for (const bool path : {false, true})
            for (const bool xmm : {false, true})
                for (const bool narrow : {false, true})
                    for (const bool taken : {false, true}) {
                        CAPTURE(decode, path, xmm, narrow, taken);
                        FeatureSet features{};
                        features.uniform_dse = false;
                        features.xmm_fault_sink = false;
                        features.uniform_path_fwd = path;
                        features.xmm_uniform_fwd = xmm;
                        features.xmm_narrow_fwd = narrow;
                        features.xmm_ssa_fwd2 = true;
                        compile::GuestStateLayout vector_layout{.uniform_size = uniform_size};
                        vector_layout.xmm_uniform_ranges.push_back({0, 16});
                        Block block{0, Location{0x1040}};
                        compile::IrBuilder builder{&block};
                        if (decode) builder.ConfigureGuestState(&vector_layout, features, {});
                        constexpr u64 low_bits = 0x0706050403020100;
                        constexpr u64 high_bits = 0x0f0e0d0c0b0a0908;
                        constexpr u64 patch_bits = 0x1716151413121110;
                        const auto vector = builder.VecLoadConst(ValueType::V128,
                                                                 Imm{low_bits}, Imm{high_bits});
                        builder.StoreUniform(Uniform{0, ValueType::V128}, vector);
                        const auto condition = builder.LoadImm<BOOL>(Imm{u32{taken}});
                        const auto edge = builder.NotGoto(condition);
                        const auto patch = builder.LoadImm(ValueType::U64, Imm{patch_bits});
                        builder.StoreUniform(Uniform{8, ValueType::U64}, patch);
                        builder.BindLabel(edge);
                        const auto low = builder.LoadUniform(Uniform{0, ValueType::V64});
                        builder.StoreUniform(Uniform{32, ValueType::V64}, low);
                        const auto whole = builder.LoadUniform(Uniform{0, ValueType::V128});
                        builder.StoreUniform(Uniform{16, ValueType::V128}, whole);
                        builder.ReturnToHost();
                        block.ReIdInstr();
                        const auto check = [&] {
                            for (const u8 fill : {u8{0}, u8{0xa5}, u8{0xff}}) {
                                alignas(backend::State) std::array<u8,
                                        sizeof(backend::State) + uniform_size> storage{};
                                auto* state = new (storage.data()) backend::State{};
                                std::memset(state->uniform_buffer_begin, fill, uniform_size);
                                std::array<u8, uniform_size> expected;
                                expected.fill(fill);
                                const std::array<u64, 2> bits{low_bits,
                                                             taken ? patch_bits : high_bits};
                                std::memcpy(expected.data(), bits.data(), 16);
                                std::memcpy(expected.data() + 16, bits.data(), 16);
                                std::memcpy(expected.data() + 32, bits.data(), 8);
                                backend::interp::Interpreter interpreter{*state, &block};
                                REQUIRE(interpreter.Run() == HaltReason::CallHost);
                                REQUIRE(std::memcmp(state->uniform_buffer_begin,
                                                    expected.data(), uniform_size) == 0);
                            }
                        };
                        if (!decode) {
                            check();
                            compile::CleanupState(RewriteFixture{&block, false}.Get(),
                                                  vector_layout, features);
                        }
                        REQUIRE((low.Def()->GetOp() == OpCode::BitCast) == (path && xmm && narrow));
                        REQUIRE(whole.Def()->GetOp() == OpCode::LoadUniform);
                        check();
                    }

    // A register number has no optimization contract until the layout's
    // target declares one. A different target can declare a different home;
    // range forwarding remains independent of load-to-load fact seeding.
    for (const bool decode : {false, true})
        for (const bool range : {false, true})
            for (const u32 policy : {0u, 1u, 2u})
                for (const u16 home : {u16{5}, u16{20}, u16{22}, u16{29}})
                    for (const auto type : {ValueType::U32, ValueType::U64}) {
                        CAPTURE(decode, range, policy, home, type);
                        compile::GuestStateLayout mapped{.uniform_size = 32};
                        mapped.uni_gprs.Mark(home);
                        const compile::GuestRegisterBinding binding{
                                Uniform{8, ValueType::U64},
                                HostReg{.gpr = {home}, .is_fpr = false}};
                        mapped.uniform_regs_map.Map(8, 16, binding);
                        if (policy == 1) backend::arm64::ConfigureGuestStatePolicy(mapped);
                        else if (policy == 2) mapped.optimization.forwarded_gprs.Mark(20);
                        const bool seeded = policy == 1 ? home != 20 : policy == 2 && home == 20;
                        FeatureSet features{};
                        features.ir_uniform_range = range;
                        features.uniform_dse = false;
                        features.xmm_fault_sink = false;
                        Block block{0, Location{0x1060}};
                        compile::IrBuilder builder{&block};
                        if (decode) builder.ConfigureGuestState(
                                &mapped, features, backend::arm64::BuilderTarget());
                        const Uniform view{8, type};
                        const auto first = builder.LoadUniform(view);
                        const auto second = builder.LoadUniform(view);
                        builder.StoreUniform(Uniform{0, type}, first);
                        builder.StoreUniform(Uniform{24, type}, second);
                        const auto replacement = builder.LoadImm(type, Imm{u64{0x12345678}});
                        builder.StoreUniform(view, replacement);
                        const auto third = builder.LoadUniform(view);
                        builder.StoreUniform(Uniform{16, type}, third);
                        builder.ReturnToHost();
                        if (!decode) compile::CleanupState(
                                RewriteFixture{&block, false}.Get(), mapped, features);
                        const auto forwarded = type == ValueType::U64
                                ? OpCode::BitCast : OpCode::BitExtract;
                        REQUIRE(first.Def()->GetOp() == OpCode::GetHostGPR);
                        REQUIRE(second.Def()->GetOp() == (seeded ? forwarded : OpCode::GetHostGPR));
                        REQUIRE(third.Def()->GetOp() ==
                                (seeded || range ? forwarded : OpCode::GetHostGPR));
                        if (seeded) REQUIRE(second.Def()->GetArg<Value>(0).Def() == first.Def());
                        if (seeded || range)
                            REQUIRE(third.Def()->GetArg<Value>(0).Def() == replacement.Def());
                    }
}

TEST_CASE("IR helper effects retain conservative memory contracts", "[ir][effects]") {
    Block block{0, Location{0x1000}};
    auto helper = block.CallLambda(ValueType::VOID, Lambda{DataClass{Imm{u64{1}}},
        HelperCallTraits{.guest_state = HelperGuestStateEffect::None,
                         .fault = HelperFaultEffect::NoDirectFault,
                         .reentry = HelperReentryEffect::NoReentry}});
    auto effects = helper.Def()->GetEffects();
    CHECK((effects & IREffect::ReadGuestState) == IREffect::None);
    CHECK((effects & IREffect::WriteGuestState) == IREffect::None);
    CHECK((effects & IREffect::MayFault) == IREffect::None);
    CHECK((effects & IREffect::WriteMemory) != IREffect::None);
    CHECK(helper.Def()->HasSideEffects());
}

TEST_CASE("flag liveness follows helper guest-state observation contracts",
          "[ir][effects][flags]") {
    for (const bool function_mode : {false, true})
        for (unsigned scenario = 0; scenario < 6; ++scenario) {
            CAPTURE(function_mode, scenario);
            FeatureSet features{};
            features.flag_carry_elim = true;
            features.flag_full_elim = true;
            compile::CompileWorkspace workspace;
            HIRBuilder builder{1, false, features};
            auto* function = builder.AppendFunction(Location{0x1000}, Location{0x1040});
            auto* block = function->EditBlock(function->GetCurrentBlock()).GetBlock();
            const auto input = function->LoadUniform<U64>(Uniform{0, ValueType::U64});
            const auto result = function->Add<U64>(input, Operand{Imm{u64{1}}});
            function->SaveFlags(result, Flags::All);
            HelperCallTraits traits{.guest_state = HelperGuestStateEffect::None,
                                    .fault = HelperFaultEffect::NoDirectFault,
                                    .reentry = HelperReentryEffect::NoReentry};
            if (scenario == 1) traits.fault = HelperFaultEffect::MayFault;
            if (scenario == 2) traits.reentry = HelperReentryEffect::MayReenter;
            if (scenario == 3) traits.guest_state = HelperGuestStateEffect::ReadOnly;
            if (scenario == 5) traits = {};
            const auto target =
                    scenario == 4 ? Lambda{function->LoadUniform<U64>(Uniform{8, ValueType::U64})}
                                  : Lambda{DataClass{Imm{u64{1}}}, traits};
            // This is an IR liveness test. The arbitrary helper address is
            // never executed; its memory effects must keep the call alive.
            function->CallLambda(ValueType::VOID, target);
            function->ClearFlags(Flags::All);
            function->EndBlock(terminal::ReturnToHost{});
            function->EndFunction();
            function->ComputeRPO();
            function->IdByRPO();
            auto rewrite = function_mode ? compile::RewriteContext{function, workspace}
                                         : compile::RewriteContext{block, workspace, function};
            compile::EliminateFlags(rewrite, features);
            rewrite.CollectDead();
            const auto count = [&](OpCode opcode) {
                return std::count_if(block->GetInstList().begin(),
                                     block->GetInstList().end(),
                                     [&](const Inst& inst) { return inst.GetOp() == opcode; });
            };
            REQUIRE(count(OpCode::SaveFlags) == (scenario == 0 ? 0 : 1));
            REQUIRE(count(OpCode::CallLambda) == 1);
        }
}

TEST_CASE("mutable value uses retain multiplicity and follow notified edits",
          "[ir][rewrite][analysis]") {
    compile::CompileWorkspace workspace;
    const FeatureSet features{};
    const compile::BuilderCapabilities target{};
    Block block{0, Location{0x1000}};
    const auto first = block.LoadImm(Imm{u64{1}});
    const auto twice = block.Add(first, Operand{first});
    const auto single = block.LoadImm(Imm{u64{2}});
    const auto user = block.Add(single, Operand{Imm{u64{1}}});
    Params params;
    params.Push(Value{first}.SetCastType(ValueType::U16));
    params.Push(Value{first}.SetCastType(ValueType::U16));
    block.CallDynamic(ValueType::VOID, Lambda{Value{first}.SetCastType(ValueType::U32)}, std::move(params));
    const auto* call = &block.GetInstList().back();
    compile::RewriteContext rewrite{&block, workspace, nullptr, false};
    {
        compile::ValueWorklist work{rewrite, features, target, compile::ValueCleanup::Widths};
        REQUIRE_THROWS_AS(compile::ValueQueueScope{workspace}, std::logic_error);
        REQUIRE(work.SingleConsumer(first.Def()) == nullptr);
        REQUIRE(work.SingleConsumer(single.Def()) == user.Def());
        rewrite.SetArg(&block, user.Def(), 0, single.WithDefinition(twice.Def()));
        REQUIRE(work.SingleConsumer(single.Def()) == nullptr);
        REQUIRE(work.SingleConsumer(twice.Def()) == user.Def());
        work.Drain();
        REQUIRE_FALSE(work.Pending());
        rewrite.SetArg(&block, call, 0, Lambda{Value{single}.SetCastType(ValueType::U32)});
        REQUIRE_FALSE(work.Pending());
        REQUIRE(work.RequestedFlags(call) == Flags::None);
        REQUIRE(work.SingleConsumer(single.Def()) == call);
        Params repeated;
        repeated.Push(Value{single}.SetCastType(ValueType::U16));
        repeated.Push(Value{single}.SetCastType(ValueType::U16));
        rewrite.SetArg(&block, call, 1, std::move(repeated));
        REQUIRE(work.SingleConsumer(single.Def()) == nullptr);
        REQUIRE(single.Def()->GetUses(false) == 3);
        Params immediate;
        immediate.Push(Imm{u64{7}});
        rewrite.SetArg(&block, call, 1, std::move(immediate));
        REQUIRE(work.SingleConsumer(single.Def()) == call);
        rewrite.Erase(&block, call);
        REQUIRE(work.SingleConsumer(call) == nullptr);
        REQUIRE(work.SingleConsumer(single.Def()) == nullptr);
        work.Drain();
        REQUIRE_FALSE(work.Pending());
    }
    REQUIRE_THROWS_AS(([&] {
                          compile::ValueWorklist interrupted{
                                  rewrite, features, target, compile::ValueCleanup::Widths};
                          throw std::runtime_error("leave value cleanup");
                      }()),
                      std::runtime_error);
    compile::ValueWorklist reused{rewrite, features, target, compile::ValueCleanup::Widths};
    REQUIRE(reused.SingleConsumer(twice.Def()) == user.Def());
}

TEST_CASE("state sink preserves observation order while relinking stores",
          "[ir][rewrite][state]") {
    for (unsigned shape : {0u, 1u, 2u})
        for (bool after_producer : {false, true}) {
            CAPTURE(shape, after_producer);
            HIRBuilder builder{1, false, FeatureSet{}};
            auto* function = shape == 2 ? builder.AppendFunction(Location{0x7200}) : nullptr;
            IntrusivePtr<Block> standalone;
            if (!function) standalone.reset(new Block{Location{0x7200}});
            auto* block = function
                    ? function->EditBlock(function->GetCurrentBlock()).GetBlock()
                    : standalone.get();
            const auto append = [&]<class... Args>(
                                        OpCode op, ValueType type, const Args&... args) {
                return function ? function->AppendInst(op, type, args...)
                                : block->AppendInst(op, type, args...);
            };
            // Finish a flush at another slot before repeated observations.
            // One covered store dies; the surviving writes must retain
            // their literal values and original non-store order.
            const auto prefix_old = Value{append(OpCode::VecLoadConst, ValueType::V128,
                                                 Imm{u64{0x10}}, Imm{u64{0x20}})};
            append(OpCode::StoreUniform, ValueType::VOID,
                   Uniform{16, ValueType::V128}, prefix_old);
            const auto prefix_latest = Value{append(OpCode::VecLoadConst, ValueType::V128,
                                                    Imm{u64{0x30}}, Imm{u64{0x40}})};
            const auto* prefix_store = append(OpCode::StoreUniform, ValueType::VOID,
                                              Uniform{16, ValueType::V128}, prefix_latest);
            const auto* prefix_boundary = append(OpCode::UniformBarrier, ValueType::VOID);
            struct Group {
                Value first, latest;
                const Inst* first_store{};
                const Inst* latest_store{};
                const Inst* boundary{};
                const Inst* introduced{};
            };
            std::array<Group, 12> groups;
            u64 bits{};
            for (auto& group : groups) {
                group.first = Value{append(OpCode::VecLoadConst, ValueType::V128,
                                           Imm{++bits}, Imm{u64{0x11}})};
                group.first_store = append(OpCode::StoreUniform, ValueType::VOID,
                                           Uniform{0, ValueType::V128}, group.first);
                for (unsigned padding = 0; padding < 3; ++padding)
                    append(OpCode::Nop, ValueType::VOID);
                group.latest = Value{append(OpCode::VecLoadConst, ValueType::V128,
                                            Imm{++bits}, Imm{u64{0x22}})};
                group.latest_store = append(OpCode::StoreUniform, ValueType::VOID,
                                            Uniform{0, ValueType::V128}, group.latest);
                group.boundary = append(OpCode::UniformBarrier, ValueType::VOID);
            }
            compile::GuestStateLayout layout{.uniform_size = 48};
            layout.xmm_uniform_ranges.push_back({0, 32});
            // Allocate the new observation nodes last, then put them on
            // either side of each producer. Pointer order and numeric IDs
            // cannot stand in for the source order of these instructions.
            for (auto& group : groups) {
                group.introduced = append(OpCode::UniformBarrier, ValueType::VOID);
                block->RemoveInst(group.introduced);
                block->InsertBefore(group.introduced,
                                    after_producer ? group.latest_store : group.latest.Def());
            }
            if (function) {
                function->EndBlock(terminal::ReturnToHost{});
                function->EndFunction();
                function->ComputeRPO();
                function->IdByRPO();
            } else {
                block->SetTerminal(terminal::ReturnToHost{});
                block->ReIdInstr();
                if (shape == 1) {
                    compile::CompileWorkspace editing_storage;
                    compile::RewriteContext editing{block, editing_storage, nullptr, false};
                    u16 id = 100;
                    for (const auto& inst : block->GetInstList()) {
                        editing.Change(block, &inst, [&](Inst& writer) { writer.SetId(id); });
                        id += 3;
                    }
                }
            }
            std::vector<const Inst*> expected_nonstores;
            for (const auto& inst : block->GetInstList())
                if (inst.GetOp() != OpCode::StoreUniform) expected_nonstores.push_back(&inst);
            compile::CompileWorkspace workspace;
            auto rewrite = function ? compile::RewriteContext{function, workspace, false}
                                    : compile::RewriteContext{block, workspace, nullptr, false};
            compile::SinkStateStores(rewrite, layout);
            std::vector<const Inst*> actual_nonstores;
            size_t stores{};
            const auto& instructions = block->GetInstList();
            for (const auto& inst : instructions) {
                if (inst.GetOp() == OpCode::StoreUniform) ++stores;
                else actual_nonstores.push_back(&inst);
            }
            REQUIRE(actual_nonstores == expected_nonstores);
            REQUIRE(stores == 2 * groups.size() + 1);
            const auto prefix = instructions.iterator_to(*prefix_boundary);
            REQUIRE(prefix != instructions.begin());
            REQUIRE(&*std::prev(prefix) == prefix_store);
            REQUIRE(prefix_old.Def()->GetUses(false) == 0);
            for (const auto& group : groups) {
                const auto introduced = instructions.iterator_to(*group.introduced);
                const auto boundary = instructions.iterator_to(*group.boundary);
                REQUIRE(introduced != instructions.begin());
                REQUIRE(boundary != instructions.begin());
                REQUIRE(&*std::prev(introduced) == group.first_store);
                REQUIRE(&*std::prev(boundary) == group.latest_store);
                REQUIRE(group.first_store->GetArg<Value>(1) == group.first);
                REQUIRE(group.latest_store->GetArg<Value>(1) == group.latest);
                REQUIRE(group.first_store->GetBlock() == block);
                REQUIRE(group.latest_store->GetBlock() == block);
            }
        }
}

TEST_CASE("partial state flushes preserve independent byte observations and terminal stores",
          "[ir][rewrite][state][jit]") {
    constexpr u32 uniform_size = 256;
    enum class Kind { Write, Read, Barrier };
    struct Step { Kind kind; u32 offset; ValueType type; u64 bits; };
    std::vector<Step> steps;
    const auto write = [&](u32 offset, ValueType type, u64 bits) {
        steps.push_back({Kind::Write, offset, type, bits});
    };
    const auto read = [&](u32 offset) {
        steps.push_back({Kind::Read, offset, ValueType::U64, 0});
    };
    steps.push_back({Kind::Barrier, 0, ValueType::VOID, 0});
    // Exceed the first pending capacity, then repeatedly compact disjoint
    // ranges. A flush must clear coverage without forgetting unselected rows.
    for (u32 round = 0; round < 3; ++round) {
        for (u32 i = 0; i < 24; ++i)
            write((i % 4) * 8, ValueType::U32, 0x11000000 + round * 256 + i);
        read(56);  // no selected pending store
        for (u32 offset : {0u, 8u, 16u, 24u}) read(offset);
    }
    write(0, ValueType::U32, 0x03020100);
    read(0);  // coverage left by an earlier flush cannot erase this store
    write(32, ValueType::U32, 0x33323130);
    write(36, ValueType::U32, 0x37363534);
    write(48, ValueType::U32, 0xdeadbeef);
    read(32);  // two distinct narrow producers cannot forward one U64 value
    write(48, ValueType::U8, 0x49);
    write(49, ValueType::U8, 0x4a);
    write(50, ValueType::U16, 0x4c4b);
    read(48);  // overlapping writes jointly cover the older U32 store
    write(0, ValueType::U64, 0x8877665544332211);
    read(0);  // exact forwarding leaves the store pending until a boundary
    steps.push_back({Kind::Barrier, 0, ValueType::VOID, 0});
    steps.push_back({Kind::Barrier, 0, ValueType::VOID, 0});
    write(60, ValueType::U32, 0x63626160);  // terminal-only publication
    const auto oracle = [&](u8 fill) {
        std::array<u8, uniform_size> bytes;
        bytes.fill(fill);
        u32 output = 96;
        for (const auto& step : steps) {
            if (step.kind == Kind::Write)
                std::memcpy(bytes.data() + step.offset, &step.bits, GetValueSizeByte(step.type));
            else if (step.kind == Kind::Read) {
                std::memcpy(bytes.data() + output, bytes.data() + step.offset, sizeof(u64));
                output += sizeof(u64);
            }
        }
        return bytes;
    };
    for (bool function_mode : {false, true}) {
        CAPTURE(function_mode);
        Config config{.loc_start = 0x7000, .loc_end = 0x8000, .enable_jit = true,
                      .backend_isa = kArm64, .uniform_buffer_size = uniform_size};
        config.stack_alignment = 16;
        config.global_opts = Optimizations::DeadCodeRemove;
        backend::AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode
                ? hir.AppendFunction(Location{0x7400}, Location{0x7401}) : nullptr;
        IntrusivePtr<Block> standalone;
        if (!function) standalone.reset(new Block{Location{0x7400}});
        auto factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{standalone.get()};
        u32 output = 96;
        for (const auto& step : steps) {
            if (step.kind == Kind::Write)
                factory.StoreUniform(Uniform{step.offset, step.type},
                                     factory.LoadImm(step.type, Imm{step.bits}));
            else if (step.kind == Kind::Read) {
                const auto value = factory.LoadUniform(ValueType::U64,
                                                       Uniform{step.offset, ValueType::U64});
                factory.StoreUniform(Uniform{output, ValueType::U64}, value);
                output += sizeof(u64);
            } else factory.UniformBarrier();
        }
        factory.AdvancePC(Imm{1u});
        auto* interpreted = function
                ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : standalone.get();
        if (function) {
            hir.ReturnToHost();
            function->ComputeRPO();
            function->IdByRPO();
        } else {
            interpreted->SetTerminal(terminal::ReturnToHost{});
            interpreted->ReIdInstr();
        }
        const auto check_interpreter = [&] {
            for (u8 fill : {u8{0}, u8{0xa5}, u8{0xff}}) {
                alignas(backend::State) std::array<u8, sizeof(backend::State) + uniform_size> storage{};
                auto* state = new (storage.data()) backend::State{};
                std::memset(state->uniform_buffer_begin, fill, uniform_size);
                backend::interp::Interpreter interpreter{*state, interpreted};
                const auto expected = oracle(fill);
                REQUIRE(interpreter.Run() == HaltReason::CallHost);
                REQUIRE(std::memcmp(state->uniform_buffer_begin, expected.data(), uniform_size) == 0);
            }
        };
        check_interpreter();
        compile::GuestStateLayout layout{.uniform_size = uniform_size};
        layout.xmm_uniform_ranges.push_back({0, 64});
        {
            compile::CompileWorkspace workspace;
            auto rewrite = function ? compile::RewriteContext{function, workspace}
                                    : compile::RewriteContext{interpreted, workspace};
            compile::SinkStateStores(rewrite, layout);
            rewrite.CollectDead();
        }
        check_interpreter();
        if (standalone) REQUIRE(module->Push(standalone.get()));
        REQUIRE((function ? TranslateIR(module, function) : TranslateIR(module, standalone)) != nullptr);
        Runtime runtime{&space};
        for (u8 fill : {u8{0}, u8{0xa5}, u8{0xff}}) {
            std::memset(runtime.GetUniformBuffer().data(), fill, uniform_size);
            const auto expected = oracle(fill);
            runtime.SetLocation(0x7400);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            REQUIRE(std::memcmp(runtime.GetUniformBuffer().data(), expected.data(), uniform_size) == 0);
        }
    }
}

TEST_CASE("state sink preserves overlap dependencies across partial observations",
          "[ir][rewrite][state][jit]") {
    constexpr u32 uniform_size = 128;
    struct Step { bool write; u32 offset; ValueType type; u64 low{}, high{}; };
    const std::array<std::vector<Step>, 7> programs{{
            {{true, 8, ValueType::U32, 0xdeadbeef},
             {true, 0, ValueType::V128, 0x0706050403020100, 0x0f0e0d0c0b0a0908},
             {false, 0, ValueType::U32}, {false, 8, ValueType::U64}},
            // The first write only overlaps the middle one. Publishing the
            // last write must also preserve this transitive source order.
            {{true, 0, ValueType::U64, 0x1716151413121110},
             {true, 4, ValueType::U64, 0x2726252423222120},
             {true, 8, ValueType::V128, 0x3736353433323130, 0x4746454443424140},
             {false, 20, ValueType::U32}, {false, 0, ValueType::U64},
             {false, 8, ValueType::U64}, {false, 4, ValueType::U64}},
            // A later disjoint-from-read write may remain pending; it must
            // still override the earlier vector when it is finally published.
            {{true, 0, ValueType::V128, 0x5756555453525150, 0x6766656463626160},
             {true, 12, ValueType::U32, 0xaabbccdd},
             {false, 0, ValueType::U32}, {false, 8, ValueType::U64}},
            // A direct wide store crosses the end of the sink-eligible range.
            {{true, 0, ValueType::V128, 0x0706050403020100, 0x0f0e0d0c0b0a0908},
             {true, 8, ValueType::V128, 0x1716151413121110, 0x1f1e1d1c1b1a1918},
             {false, 0, ValueType::U64}, {false, 8, ValueType::U64},
             {false, 16, ValueType::U64}},
            // A direct read needs both pending and untouched bytes across an edge.
            {{true, 0, ValueType::V128, 0x2726252423222120, 0x2f2e2d2c2b2a2928},
             {false, 12, ValueType::U64}},
            // Crossing the beginning must publish the older overlapping write first.
            {{true, 16, ValueType::V128, 0x3736353433323130, 0x3f3e3d3c3b3a3938},
             {true, 12, ValueType::U64, 0x4746454443424140},
             {false, 12, ValueType::U64}, {false, 16, ValueType::U64},
             {false, 24, ValueType::U64}},
            // A write spanning a gap is ineligible, but overlaps both requested ranges.
            {{true, 0, ValueType::U64, 0x5756555453525150},
             {true, 16, ValueType::U64, 0x6766656463626160},
             {true, 4, ValueType::V128, 0x7776757473727170, 0x7f7e7d7c7b7a7978},
             {false, 0, ValueType::U64}, {false, 8, ValueType::U64},
             {false, 16, ValueType::U64}},
    }};
    for (size_t program = 0; program < programs.size(); ++program)
        for (bool function_mode : {false, true}) {
            CAPTURE(program, function_mode);
            const auto& steps = programs[program];
            const auto oracle = [&](u8 fill) {
                std::array<u8, uniform_size> bytes;
                bytes.fill(fill);
                u32 output = 64;
                for (const auto& step : steps) {
                    const auto size = GetValueSizeByte(step.type);
                    if (step.write) {
                        const std::array<u64, 2> bits{step.low, step.high};
                        std::memcpy(bytes.data() + step.offset, bits.data(), size);
                    } else {
                        std::memcpy(bytes.data() + output, bytes.data() + step.offset, size);
                        output += sizeof(u64);
                    }
                }
                return bytes;
            };
            Config config{.loc_start = 0x7600, .loc_end = 0x7700, .enable_jit = true,
                          .backend_isa = kArm64, .uniform_buffer_size = uniform_size};
            config.stack_alignment = 16;
            config.global_opts = Optimizations::DeadCodeRemove;
            backend::AddressSpace space{config};
            const auto module = space.GetDefaultModule();
            HIRBuilder hir{1, false, FeatureSet{}};
            auto* function = function_mode
                    ? hir.AppendFunction(Location{0x7600}, Location{0x7601}) : nullptr;
            IntrusivePtr<Block> standalone;
            if (!function) standalone.reset(new Block{Location{0x7600}});
            auto factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{standalone.get()};
            u32 output = 64;
            for (const auto& step : steps) {
                if (step.write) {
                    const auto value = step.type == ValueType::V128
                            ? factory.VecLoadConst(ValueType::V128, Imm{step.low}, Imm{step.high})
                            : factory.LoadImm(step.type, Imm{step.low});
                    factory.StoreUniform(Uniform{step.offset, step.type}, value);
                } else {
                    const auto value = factory.LoadUniform(step.type, Uniform{step.offset, step.type});
                    factory.StoreUniform(Uniform{output, step.type}, value);
                    output += sizeof(u64);
                }
            }
            factory.AdvancePC(Imm{1u});
            auto* block = function
                    ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : standalone.get();
            if (function) {
                hir.ReturnToHost();
                function->ComputeRPO();
                function->IdByRPO();
            } else {
                block->SetTerminal(terminal::ReturnToHost{});
                block->ReIdInstr();
            }
            const auto check_interpreter = [&] {
                for (u8 fill : {u8{0}, u8{0xa5}, u8{0xff}}) {
                    alignas(backend::State) std::array<u8, sizeof(backend::State) + uniform_size> storage{};
                    auto* state = new (storage.data()) backend::State{};
                    std::memset(state->uniform_buffer_begin, fill, uniform_size);
                    backend::interp::Interpreter interpreter{*state, block};
                    const auto expected = oracle(fill);
                    REQUIRE(interpreter.Run() == HaltReason::CallHost);
                    REQUIRE(std::memcmp(state->uniform_buffer_begin, expected.data(), uniform_size) == 0);
                }
            };
            check_interpreter();
            compile::GuestStateLayout layout{.uniform_size = uniform_size};
            if (program == 3 || program == 4) layout.xmm_uniform_ranges = {{0, 16}};
            else if (program == 5) layout.xmm_uniform_ranges = {{16, 32}};
            else if (program == 6) layout.xmm_uniform_ranges = {{0, 8}, {16, 24}};
            else layout.xmm_uniform_ranges = {{0, 32}};
            {
                compile::CompileWorkspace workspace;
                auto rewrite = function ? compile::RewriteContext{function, workspace}
                                        : compile::RewriteContext{block, workspace};
                compile::SinkStateStores(rewrite, layout);
                rewrite.CollectDead();
            }
            check_interpreter();
            if (standalone) REQUIRE(module->Push(standalone.get()));
            REQUIRE((function ? TranslateIR(module, function) : TranslateIR(module, standalone)) != nullptr);
            Runtime runtime{&space};
            for (u8 fill : {u8{0}, u8{0xa5}, u8{0xff}}) {
                std::memset(runtime.GetUniformBuffer().data(), fill, uniform_size);
                const auto expected = oracle(fill);
                runtime.SetLocation(0x7600);
                REQUIRE(runtime.Run() == HaltReason::CallHost);
                REQUIRE(std::memcmp(runtime.GetUniformBuffer().data(), expected.data(), uniform_size) == 0);
            }
        }

    // A pending narrow write must preserve the zero-extension performed by
    // the architectural load before a helper consumes its full X register.
    // The source comes from runtime state, with dirty bits above its cast view.
    const auto identity = +[](u64 value) -> u64 { return value; };
    for (const bool function_mode : {false, true})
        for (const auto type : {ValueType::U8, ValueType::U16, ValueType::U32}) {
            CAPTURE(function_mode, type);
            Config config{.loc_start = 0x7000, .loc_end = 0x8000, .enable_jit = true,
                          .backend_isa = kArm64, .uniform_buffer_size = uniform_size};
            config.stack_alignment = 16;
            config.global_opts = Optimizations::DeadCodeRemove;
            backend::AddressSpace space{config};
            const auto module = space.GetDefaultModule();
            HIRBuilder hir{1, false, FeatureSet{}};
            auto* function = function_mode ? hir.AppendFunction(Location{0x7800}) : nullptr;
            IntrusivePtr<Block> standalone;
            if (!function) standalone.reset(new Block{Location{0x7800}});
            auto factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{standalone.get()};
            const auto wide = factory.LoadUniform(ValueType::U64, Uniform{32, ValueType::U64});
            factory.StoreUniform(Uniform{0, type}, Value{wide.Def()}.SetCastType(type));
            const auto narrow = factory.LoadUniform(type, Uniform{0, type});
            const auto result = factory.CallLambda(
                    ValueType::U64, Lambda{Imm{reinterpret_cast<VAddr>(identity)}}, narrow);
            factory.StoreUniform(Uniform{48, ValueType::U64}, result);
            factory.AdvancePC(Imm{1u});
            auto* block = function
                    ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : standalone.get();
            if (function) {
                hir.ReturnToHost();
                function->ComputeRPO();
                function->IdByRPO();
            } else {
                block->SetTerminal(terminal::ReturnToHost{});
                block->ReIdInstr();
            }
            compile::GuestStateLayout layout{.uniform_size = uniform_size};
            layout.xmm_uniform_ranges.push_back({0, 16});
            {
                compile::CompileWorkspace workspace;
                auto rewrite = function ? compile::RewriteContext{function, workspace}
                                        : compile::RewriteContext{block, workspace};
                compile::SinkStateStores(rewrite, layout);
                rewrite.CollectDead();
            }
            if (standalone) REQUIRE(module->Push(standalone.get()));
            REQUIRE((function ? TranslateIR(module, function) : TranslateIR(module, standalone)) != nullptr);
            Runtime runtime{&space};
            for (const u64 input : {u64{0xa5a5a5a5feebcafe}, u64{0xfedcba9876543210}}) {
                auto buffer = runtime.GetUniformBuffer();
                std::memset(buffer.data(), 0, uniform_size);
                std::memcpy(buffer.data() + 32, &input, sizeof(input));
                runtime.SetLocation(0x7800);
                REQUIRE(runtime.Run() == HaltReason::CallHost);
                u64 actual{};
                std::memcpy(&actual, buffer.data() + 48, sizeof(actual));
                const u64 expected = input & ((u64{1} << (GetValueSizeByte(type) * 8u)) - 1);
                CAPTURE(input, actual, expected, narrow.Def()->GetOp());
                CHECK(actual == expected);
            }
        }

    // Once both obsolete stores are gone, DCE must keep each effect and the
    // allocator/emitter must handle its result with no artificial consumer.
    const auto page_size = sysconf(_SC_PAGESIZE);
    REQUIRE(page_size > 0);
    const auto update = +[](u64* value) -> u64 { return ++*value; };
    for (const bool function_mode : {false, true})
        for (const bool sink : {false, true})
            for (unsigned kind = 0; kind < 4; ++kind) {
                CAPTURE(function_mode, sink, kind);
                auto* data = static_cast<u64*>(mmap(nullptr, static_cast<size_t>(page_size),
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
                REQUIRE(data != MAP_FAILED);
                const auto release = ScopeExit([&] {
                    munmap(data, static_cast<size_t>(page_size));
                });
                data[0] = 5;
                data[1] = 17;
                Config config{.loc_start = 0x7000, .loc_end = 0x8000, .enable_jit = true,
                              .backend_isa = kArm64, .uniform_buffer_size = uniform_size};
                config.stack_alignment = 16;
                config.global_opts = Optimizations::DeadCodeRemove;
                backend::AddressSpace space{config};
                const auto module = space.GetDefaultModule();
                HIRBuilder hir{1, false, FeatureSet{}};
                auto* function = function_mode ? hir.AppendFunction(Location{0x7900}) : nullptr;
                IntrusivePtr<Block> standalone;
                if (!function) standalone.reset(new Block{Location{0x7900}});
                auto factory = function ? compile::IrBuilder{&hir}
                                        : compile::IrBuilder{standalone.get()};
                const Imm pointer{reinterpret_cast<VAddr>(data)};
                const auto type = kind == 0 ? ValueType::V128 : ValueType::U64;
                Value root;
                if (kind == 0)
                    root = factory.LoadMemory(type, Operand{pointer});
                else if (kind == 1)
                    root = factory.LoadMemoryTSO(type, Operand{pointer});
                else if (kind == 2)
                    root = factory.AtomicFetchAdd(type,
                            factory.LoadImm(ValueType::U64, pointer),
                            factory.LoadImm(ValueType::U64, Imm{u64{3}}));
                else
                    root = factory.CallLambda(type,
                            Lambda{Imm{reinterpret_cast<VAddr>(update)}}, pointer);
                const auto pure = kind == 0
                        ? factory.VecXor(type, root,
                                factory.VecLoadConst(ValueType::V128, Imm{u64{0x11}}, Imm{u64{0x22}}))
                        : factory.Add(type, root, Operand{Imm{u64{7}}});
                factory.StoreUniform(Uniform{0, type}, pure);
                factory.StoreUniform(Uniform{0, type}, pure);
                const auto replacement = kind == 0
                        ? factory.VecLoadConst(ValueType::V128, Imm{u64{0x55}}, Imm{u64{0xaa}})
                        : factory.LoadImm(type, Imm{u64{0x55}});
                factory.StoreUniform(Uniform{0, type}, replacement);
                factory.AdvancePC(Imm{1u});
                auto* block = function
                        ? function->EditBlock(function->GetCurrentBlock()).GetBlock()
                        : standalone.get();
                if (function) {
                    hir.ReturnToHost();
                    function->ComputeRPO();
                    function->IdByRPO();
                } else {
                    block->SetTerminal(terminal::ReturnToHost{});
                    block->ReIdInstr();
                }
                compile::GuestStateLayout layout{.uniform_size = uniform_size};
                layout.xmm_uniform_ranges.push_back({0, 16});
                FeatureSet features{};
                features.uniform_dse = !sink;
                features.xmm_fault_sink = sink;
                features.xmm_uniform_fwd = true;
                {
                    compile::CompileWorkspace workspace;
                    auto rewrite = function ? compile::RewriteContext{function, workspace}
                                            : compile::RewriteContext{block, workspace};
                    if (sink) compile::SinkStateStores(rewrite, layout);
                    else compile::CleanupState(rewrite, layout, features);
                    rewrite.CollectDead();
                }
                REQUIRE(std::any_of(block->GetInstList().begin(), block->GetInstList().end(),
                                    [&](const Inst& inst) { return &inst == root.Def(); }));
                REQUIRE(root.Def()->GetUses(false) == 0);
                REQUIRE(std::count_if(block->GetInstList().begin(), block->GetInstList().end(),
                        [](const Inst& inst) { return inst.GetOp() == OpCode::StoreUniform; }) == 1);
                if (standalone) REQUIRE(module->Push(standalone.get()));
                REQUIRE((function ? TranslateIR(module, function) : TranslateIR(module, standalone)) != nullptr);
                Runtime runtime{&space};
                auto buffer = runtime.GetUniformBuffer();
                std::memset(buffer.data(), 0xa5, uniform_size);
                runtime.SetLocation(0x7900);
                REQUIRE(runtime.Run() == HaltReason::CallHost);
                CHECK(data[0] == (kind == 2 ? 8u : kind == 3 ? 6u : 5u));
                u64 output{};
                std::memcpy(&output, buffer.data(), sizeof(output));
                CHECK(output == 0x55);
                if (kind == 0) {
                    std::memcpy(&output, buffer.data() + 8, sizeof(output));
                    CHECK(output == 0xaa);
                }
                if (kind != 3) {
                    REQUIRE(mprotect(data, static_cast<size_t>(page_size), PROT_NONE) == 0);
                    std::memset(buffer.data(), 0xa5, uniform_size);
                    runtime.SetLocation(0x7900);
                    REQUIRE(runtime.Run() == HaltReason::PageFatal);
                    std::array<u8, 16> untouched;
                    untouched.fill(0xa5);
                    CHECK(std::memcmp(buffer.data(), untouched.data(), GetValueSizeByte(type)) == 0);
                }
            }

    // An SSA observer does not publish this value into guest state. A faulting
    // ordinary store reads it, but the earlier XMM publication must still be
    // current before that store faults, even if a later write overwrites it.
    for (const bool function_mode : {false, true})
    for (const bool derived : {false, true})
    for (const bool vector : {false, true}) {
        CAPTURE(function_mode, derived, vector);
        auto* destination = mmap(nullptr, static_cast<size_t>(page_size), PROT_NONE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(destination != MAP_FAILED);
        const auto release = ScopeExit([&] {
            munmap(destination, static_cast<size_t>(page_size));
        });
        std::array<u64, 2> input{0x1122334455667788, 0x99aabbccddeeff00};
        Config config{.loc_start = 0x7000, .loc_end = 0x8000, .enable_jit = true,
                      .backend_isa = kArm64, .uniform_buffer_size = uniform_size};
        config.stack_alignment = 16;
        config.global_opts = Optimizations::DeadCodeRemove;
        backend::AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode ? hir.AppendFunction(Location{0x7b00}) : nullptr;
        IntrusivePtr<Block> standalone;
        if (!function) standalone.reset(new Block{Location{0x7b00}});
        auto factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{standalone.get()};
        const auto type = vector ? ValueType::V128 : ValueType::U64;
        const auto loaded = factory.LoadMemory(type,
                Operand{Imm{reinterpret_cast<VAddr>(input.data())}});
        constexpr u64 low_mask = 0x0101010101010101, high_mask = 0x1010101010101010;
        const auto value = !derived ? loaded : vector
                ? factory.VecXor(type, loaded,
                        factory.VecLoadConst(type, Imm{low_mask}, Imm{high_mask}))
                : factory.Add(type, loaded, Operand{Imm{u64{7}}});
        auto expected = input;
        if (derived && vector) { expected[0] ^= low_mask; expected[1] ^= high_mask; }
        if (derived && !vector) expected[0] += 7;
        factory.StoreUniform(Uniform{0, type}, value);
        factory.StoreMemory(Operand{Imm{reinterpret_cast<VAddr>(destination)}}, value);
        factory.StoreUniform(Uniform{0, type}, vector
                ? factory.VecLoadConst(type, Imm{u64{0}}, Imm{u64{0}})
                : factory.LoadImm(type, Imm{u64{0}}));
        factory.AdvancePC(Imm{1u});
        auto* block = function
                ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : standalone.get();
        if (function) {
            hir.ReturnToHost();
            function->ComputeRPO();
            function->IdByRPO();
        } else {
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
        }
        compile::GuestStateLayout layout{.uniform_size = uniform_size};
        if (vector) layout.xmm_uniform_ranges.push_back({0, 16});
        FeatureSet features{};
        features.uniform_dse = true;
        features.xmm_fault_sink = true;
        features.xmm_uniform_fwd = true;
        {
            compile::CompileWorkspace workspace;
            auto rewrite = function ? compile::RewriteContext{function, workspace}
                                    : compile::RewriteContext{block, workspace};
            compile::CleanupState(rewrite, layout, features);
            compile::SinkStateStores(rewrite, layout);
            rewrite.CollectDead();
        }
        if (standalone) REQUIRE(module->Push(standalone.get()));
        REQUIRE((function ? TranslateIR(module, function) : TranslateIR(module, standalone)) != nullptr);
        Runtime runtime{&space};
        auto buffer = runtime.GetUniformBuffer();
        std::memset(buffer.data(), 0xa5, uniform_size);
        runtime.SetLocation(0x7b00);
        REQUIRE(runtime.Run() == HaltReason::PageFatal);
        CHECK(std::memcmp(buffer.data(), expected.data(), GetValueSizeByte(type)) == 0);
    }
}

TEST_CASE("state publications preserve low views across repeated observations",
          "[ir][rewrite][state][jit]") {
    for (bool function_mode : {false, true}) {
        CAPTURE(function_mode);
        constexpr u32 uniform_size = 64;
        const std::array<u64, 2> bits{0x0706050403020100, 0x0f0e0d0c0b0a0908};
        Config config{.loc_start = 0x7a00, .loc_end = 0x7b00, .enable_jit = true,
                      .backend_isa = kArm64, .uniform_buffer_size = uniform_size};
        config.stack_alignment = 16;
        config.global_opts = Optimizations::DeadCodeRemove;
        backend::AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        HIRBuilder builder{1, false, FeatureSet{}};
        auto* function = function_mode
                ? builder.AppendFunction(Location{0x7a00}, Location{0x7a01}) : nullptr;
        IntrusivePtr<Block> standalone;
        if (!function) standalone.reset(new Block{Location{0x7a00}});
        auto* block = function
                ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : standalone.get();
        const auto append = [&]<class... Args>(OpCode op, ValueType type, const Args&... args) {
            return function ? function->AppendInst(op, type, args...)
                            : block->AppendInst(op, type, args...);
        };
        std::array<std::array<u8, 16>, 4> observed{};
        std::array<std::array<u8, 16>, 4> expected_observations;
        for (auto& value : expected_observations)
            std::memcpy(value.data(), bits.data(), sizeof(bits));
        std::memset(expected_observations[1].data(), 0xff, 8);
        std::memset(expected_observations[2].data() + 4, 0xff, 4);
        const auto state = Value{append(OpCode::GetUniformAddress, ValueType::U64, Imm{0u})};
        const auto full = Value{append(OpCode::VecLoadConst, ValueType::V128,
                                      Imm{bits[0]}, Imm{bits[1]})};
        append(OpCode::StoreUniform, ValueType::VOID, Uniform{0, ValueType::V128}, full);
        const auto* boundary = append(OpCode::UniformBarrier, ValueType::VOID);
        compile::GuestStateLayout layout{.uniform_size = uniform_size};
        layout.xmm_uniform_ranges = {{0, 16}};
        const auto insert = [&]<class... Args>(OpCode op, ValueType type, const Args&... args) {
            const auto* inst = append(op, type, args...);
            block->RemoveInst(inst);
            block->InsertBefore(inst, boundary);
            return inst;
        };
        const auto observe = [&](size_t index) {
            const auto copy = +[](u64 input, u64 output) {
                std::memcpy(reinterpret_cast<void*>(output),
                            reinterpret_cast<const void*>(input), 16);
            };
            insert(OpCode::CallLambda, ValueType::VOID,
                   Lambda{Imm{reinterpret_cast<u64>(copy)}}, state,
                   Imm{reinterpret_cast<u64>(observed[index].data())});
        };
        insert(OpCode::UniformBarrier, ValueType::VOID); // publish the original full value
        const auto changed = Value{insert(OpCode::VecLoadConst, ValueType::V64,
                                         Imm{u64{0xffffffffffffffff}}, Imm{u64{0}})};
        observe(0); // A computed future value is not yet a guest-state write.
        insert(OpCode::StoreUniform, ValueType::VOID, Uniform{0, ValueType::V64}, changed);
        insert(OpCode::UniformBarrier, ValueType::VOID); // replace only low bytes
        observe(1);
        insert(OpCode::StoreUniform, ValueType::VOID, Uniform{0, ValueType::V32},
               Value{full}.SetCastType(ValueType::V32));
        insert(OpCode::UniformBarrier, ValueType::VOID); // still only partially restored
        observe(2);
        insert(OpCode::StoreUniform, ValueType::VOID, Uniform{0, ValueType::V64},
               Value{full}.SetCastType(ValueType::V64));
        insert(OpCode::UniformBarrier, ValueType::VOID); // restore without a new V128 store
        observe(3);
        append(OpCode::AdvancePC, ValueType::VOID, Imm{1u});
        if (function) {
            builder.ReturnToHost();
            function->ComputeRPO();
            function->IdByRPO();
        } else {
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
        }
        {
            compile::CompileWorkspace workspace;
            auto rewrite = function ? compile::RewriteContext{function, workspace}
                                    : compile::RewriteContext{block, workspace};
            const FeatureSet features{};
            compile::CleanupState(rewrite, layout, features);
            compile::SinkStateStores(rewrite, layout);
            rewrite.CollectDead();
        }
        if (standalone) REQUIRE(module->Push(standalone.get()));
        REQUIRE((function ? TranslateIR(module, function) : TranslateIR(module, standalone)) != nullptr);
        Runtime runtime{&space};
        for (u8 fill : {u8{0}, u8{0xa5}, u8{0xff}}) {
            std::array<u8, uniform_size> expected;
            expected.fill(fill);
            std::memcpy(expected.data(), bits.data(), sizeof(bits));
            std::memset(runtime.GetUniformBuffer().data(), fill, uniform_size);
            for (auto& value : observed) value.fill(0x99);
            runtime.SetLocation(0x7a00);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            for (size_t index = 0; index < observed.size(); ++index)
                REQUIRE(observed[index] == expected_observations[index]);
            REQUIRE(std::memcmp(runtime.GetUniformBuffer().data(), expected.data(), uniform_size) == 0);
        }
    }
}

TEST_CASE("IR type validation rejects inconsistent uniform store widths", "[ir][types]") {
    auto constant = std::make_unique<Inst>(OpCode::LoadImm, ValueType::U64);
    constant->SetArgs(Imm{u64{7}});
    auto store = std::make_unique<Inst>(OpCode::StoreUniform);
    store->SetArgs(Uniform{0, ValueType::U8}, Value{constant.get()});
    CHECK_FALSE(store->HasValidArguments());
    store->SetArg(1, Value{constant.get()}.SetCastType(ValueType::U8));
    CHECK(store->HasValidArguments());
}

TEST_CASE("IR DCE removes a long unused definition chain", "[ir][regression]") {
    Block block{0, Location{0x1000}};
    auto value = block.LoadImm(Imm{u64{1}});
    for (u32 i = 0; i < 8192; ++i)
        value = block.Add(value, Operand{Imm{u64{1}}});
    block.SetTerminal(terminal::ReturnToHost{});
    RewriteFixture{&block}.Get().CollectDead();
    REQUIRE(block.GetInstList().empty());
}

TEST_CASE("IR DCE reaches predecessor definitions across blocks", "[ir][analysis]") {
    HIRBuilder builder;
    auto* function = builder.AppendFunction(Location{0x1000});
    auto* predecessor = function->EditBlock(function->GetCurrentBlock()).GetBlock();
    auto root = builder.LoadImm(Imm{u64{1}});
    auto middle = builder.Add(root, Operand{Imm{u64{1}}});
    auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x2000}});
    function->SetCurBlock(next);
    builder.Add(middle, Operand{Imm{u64{1}}});
    builder.ReturnToHost();
    RewriteFixture{function}.Get().CollectDead();
    REQUIRE(predecessor->GetInstList().empty());
    REQUIRE(next->GetBlock()->GetInstList().empty());
}

#if defined(__aarch64__)
TEST_CASE("IR ARM64 compilation rejects Phi instead of emitting an undefined result", "[ir][cfg]") {
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = 0x2000;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
    backend::AddressSpace space{config};
    for (const bool hidden_by_width : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x1000})};
        const auto input = block->LoadImm(Imm{u64{1}});
        Params incoming;
        incoming.Push(input);
        const auto phi = block->AddPhi(ValueType::U64, std::move(incoming));
        const auto value =
                hidden_by_width
                        ? block->BitExtract(ValueType::U32,
                                            block->And(ValueType::U64, phi, Operand{Imm{u64{0}}}),
                                            Imm{u32{32}},
                                            Imm{u32{32}})
                        : phi;
        block->StoreUniform(Uniform{0, hidden_by_width ? ValueType::U32 : ValueType::U64}, value);
        block->SetTerminal(terminal::ReturnToHost{});
        REQUIRE(TranslateIR(space.GetModule(0x1000), block) == nullptr);
        REQUIRE(space.GetCodeCache(Location{0x1000}) == nullptr);
    }
}
#endif

TEST_CASE("terminal contracts reject nested malformed shapes before effects or cleanup",
          "[ir][terminal][admission][ssa]") {
    for (unsigned unit = 0; unit < 3; ++unit)
        for (unsigned shape = 0; shape < 7; ++shape) {
            CAPTURE(unit, shape);
            Config config{.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
                          .backend_isa = kArm64, .uniform_buffer_size = 64};
            config.stack_alignment = 16;
            config.global_opts = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove |
                                 Optimizations::FlagElimination;
            backend::AddressSpace space{config};
            const auto module = space.GetDefaultModule();
            HIRBuilder builder{1, false, FeatureSet{}};
            auto* function = unit ? builder.AppendFunction(Location{0x6a00}, Location{0x6a40})
                                  : nullptr;
            IntrusivePtr<Block> block;
            if (!function) block = new Block{0, Location{0x6a00}};
            if (unit == 2) {
                auto* root = function->GetCurrentBlock();
                auto* tail = function->AppendBlock(Location{0x6a10});
                function->EndBlock(terminal::LinkBlock{Location{0x6a10}});
                function->AddEdge(root, tail);
                function->SetCurBlock(tail);
            }
            compile::IrBuilder factory = function ? compile::IrBuilder{&builder}
                                                   : compile::IrBuilder{block.get()};
            const auto source = factory.LoadUniform(Uniform{0, ValueType::U64});
            const auto marker = factory.LoadImm(ValueType::U64, Imm{u64{0x11112222}});
            factory.StoreUniform(Uniform{8, ValueType::U64}, marker);
            const auto* dead = factory.AppendInst(OpCode::Add, ValueType::U64, source,
                                                  Operand{Imm{u64{1}}});
            Value domain = source;
            if (shape == 2 || shape == 3)
                domain = factory.LoadUniform(Uniform{16, ValueType::V128});
            else if (shape == 4)
                // FP scalar payloads use V32 storage, distinct from V128.
                domain = factory.LoadUniform(Uniform{16, ValueType::V32});
            Terminal terminal = terminal::ReturnToHost{};
            switch (shape) {
                case 0:
                    terminal = terminal::Condition{static_cast<Cond>(16),
                            terminal::ReturnToHost{}, terminal::ReturnToDispatch{}};
                    break;
                case 1: {
                    const auto no = factory.LoadImm(ValueType::U8, Imm{u8{0}});
                    const auto zero = factory.LoadImm(ValueType::U64, Imm{u64{0}});
                    // The selected runtime path reaches case 0 and returns.
                    // The invalid later case remains part of the IR contract.
                    terminal = terminal::CheckHalt{terminal::If{
                            BOOL{no}, terminal::ReturnToHost{},
                            terminal::Condition{Cond::AL,
                                    terminal::Switch{zero,
                                            {{Imm{u8{0}}, terminal::ReturnToHost{}},
                                             {Imm{u16{1}}, terminal::Condition{
                                                     static_cast<Cond>(255),
                                                     terminal::ReturnToHost{},
                                                     terminal::ReturnToDispatch{}}}}},
                                    terminal::ReturnToDispatch{}}}};
                    break;
                }
                case 2: {
                    BOOL condition{domain};
                    (void)condition.SetCastType(ValueType::V128);
                    terminal = terminal::If{condition, terminal::ReturnToHost{},
                                             terminal::ReturnToDispatch{}};
                    break;
                }
                case 3:
                    terminal = terminal::Switch{domain, {}};
                    break;
                case 4:
                    terminal = terminal::Switch{domain,
                            {{Imm{u8{0}}, terminal::ReturnToHost{}}}};
                    break;
                case 5:
                case 6:
                    terminal = terminal::Switch{source,
                            {{Imm{u8{0}}, terminal::ReturnToHost{}},
                             {Imm{u64{1}, shape == 5 ? ValueType::V64 : ValueType::V32},
                                     terminal::ReturnToDispatch{}}}};
                    break;
            }
            auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock()
                                  : block.get();
            if (function) {
                function->EndBlock(std::move(terminal));
                function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
            } else {
                body->SetTerminal(std::move(terminal)); body->ReIdInstr();
            }
            REQUIRE(std::all_of(body->GetInstList().begin(), body->GetInstList().end(),
                    [](const Inst& inst) {
                        return inst.HasValidResultType() && inst.HasValidArguments();
                    }));
            REQUIRE_FALSE(backend::arm64::SupportsIR(body));
            if (function) REQUIRE_FALSE(backend::arm64::SupportsIR(function));
            const auto prepared = function ? backend::arm64::PrepareFunction(module, function)
                                           : backend::arm64::PrepareBlock(module, block);
            REQUIRE_FALSE(prepared);
            REQUIRE(space.GetCodeCache(Location{0x6a00}) == nullptr);
            REQUIRE(space.GetCodeCache(body->GetStartLocation()) == nullptr);
            const auto retained = std::find_if(body->GetInstList().begin(),
                    body->GetInstList().end(), [&](const Inst& inst) { return &inst == dead; });
            REQUIRE(retained != body->GetInstList().end());
            REQUIRE(retained->GetOp() == OpCode::Add);
            REQUIRE(retained->GetUses(false) == 0);
            {
                auto owner = function ? compile::CompileUnit{function} : compile::CompileUnit{block};
                owner.SealIR();
                compile::SemanticFacts facts;
                facts.BeginInput(function ? nullptr : body, function);
                REQUIRE_FALSE(facts.PrepareSSAValidation());
            }
            alignas(backend::State) std::array<u8, sizeof(backend::State) + 64> storage{};
            auto* state = new (storage.data()) backend::State{};
            std::array<u8, 64> unchanged;
            unchanged.fill(0xa5);
            std::memcpy(state->uniform_buffer_begin, unchanged.data(), unchanged.size());
            backend::interp::Interpreter interpreter{*state, body};
            REQUIRE(interpreter.Run() == HaltReason::IllegalCode);
            REQUIRE(std::memcmp(state->uniform_buffer_begin, unchanged.data(), unchanged.size()) == 0);
        }

    // An explicit but disconnected member is omitted from executable RPO.
    // Both original-unit admission and a direct sealed proof still validate
    // its terminal shape without requiring unreachable SSA dominance.
    Config config{.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
                  .backend_isa = kArm64, .uniform_buffer_size = 64};
    config.stack_alignment = 16;
    backend::AddressSpace space{config};
    HIRBuilder builder{1, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{0x6c00}, Location{0x6c20});
    function->Nop();
    function->EndBlock(terminal::ReturnToHost{});
    auto* disconnected = function->AppendBlock(Location{0x6c10});
    function->SetCurBlock(disconnected);
    function->Nop();
    function->EndBlock(terminal::CheckHalt{terminal::Condition{static_cast<Cond>(255),
            terminal::ReturnToHost{}, terminal::ReturnToDispatch{}}});
    function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
    REQUIRE(std::none_of(function->GetHIRBlocksRPO().begin(), function->GetHIRBlocksRPO().end(),
            [&](const HIRBlock& member) { return &member == disconnected; }));
    REQUIRE_FALSE(backend::arm64::SupportsIR(function));
    REQUIRE_FALSE(backend::arm64::PrepareFunction(space.GetDefaultModule(), function));
    REQUIRE(space.GetCodeCache(Location{0x6c00}) == nullptr);
    REQUIRE(space.GetCodeCache(Location{0x6c10}) == nullptr);
    compile::SemanticFacts facts{function};
    REQUIRE_FALSE(facts.PrepareSSAValidation());
    (void)facts.PrepareValueLiveness();
    compile::CompileUnit owner{function};
    owner.SealIR();
    // Reuse of pre-seal liveness takes the independent checker; a fresh
    // facts object takes the fused collection path. Both reject this member.
    REQUIRE_FALSE(facts.PrepareSSAValidation());
    compile::SemanticFacts sealed_facts{function};
    REQUIRE_FALSE(sealed_facts.PrepareSSAValidation());
}

TEST_CASE("terminal contracts retain legal scalar views and ordered repeated uses",
          "[ir][terminal][types][ssa]") {
    for (unsigned program = 0; program < 5; ++program) {
        CAPTURE(program);
        Config config{.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
                      .backend_isa = kArm64, .uniform_buffer_size = 64};
        config.stack_alignment = 16;
        config.global_opts = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove |
                             Optimizations::FlagElimination;
        backend::AddressSpace space{config};
        IntrusivePtr<Block> block{new Block{0, Location{0x6e00}}};
        const auto marker = block->LoadImm(ValueType::U64, Imm{u64{0xbeef}});
        block->StoreUniform(Uniform{8, ValueType::U64}, marker);
        if (program == 0) {
            block->SetLocation(Lambda{Imm{u64{0x6e01}}});
            block->SetTerminal(terminal::Invalid{});
        } else {
            const auto source = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
            if (program == 1) block->SetTerminal(terminal::Switch{source, {}});
            else if (program == 2)
                block->SetTerminal(terminal::Switch{source,
                        {{Imm{u16{7}}, terminal::ReturnToHost{}},
                         {Imm{s32{7}}, terminal::ReturnToDispatch{}},
                         {Imm{s8{-1}}, terminal::ReturnToHost{}},
                         {Imm{s64{-1}}, terminal::ReturnToHost{}}}});
            else {
                BOOL condition{source};
                if (program == 4) (void)condition.SetCastType(ValueType::S64);
                REQUIRE(source.Def()->ReturnType() == ValueType::U64);
                block->SetTerminal(terminal::If{condition, terminal::ReturnToHost{},
                                                terminal::ReturnToDispatch{}});
            }
        }
        block->ReIdInstr();
        REQUIRE(backend::arm64::SupportsIR(block.get()));
        {
            compile::CompileUnit owner{block};
            owner.SealIR();
            compile::SemanticFacts facts{block.get()};
            REQUIRE(facts.PrepareSSAValidation());
        }
        const std::array<u64, 4> inputs = program == 2
                ? std::array<u64, 4>{7, 255, UINT64_MAX, 99}
                : program == 3 ? std::array<u64, 4>{0, 0x100, 0x100000001, 0}
                : program == 4 ? std::array<u64, 4>{0, u64{1} << 32, u64{1} << 63, 0}
                               : std::array<u64, 4>{0, 0, 0, 0};
        const unsigned samples = program == 2 ? 4 : program >= 3 ? 3 : 1;
        for (unsigned sample = 0; sample < samples; ++sample) {
            CAPTURE(sample, inputs[sample]);
            alignas(backend::State) std::array<u8, sizeof(backend::State) + 64> storage{};
            auto* state = new (storage.data()) backend::State{};
            std::memcpy(state->uniform_buffer_begin, &inputs[sample], sizeof(u64));
            const bool host = program == 2 ? inputs[sample] != 99
                    : program == 3 ? (inputs[sample] & 0xff) != 0
                    : program == 4 ? inputs[sample] != 0 : false;
            backend::interp::Interpreter interpreter{*state, block.get()};
            REQUIRE(interpreter.Run() == (host ? HaltReason::CallHost : HaltReason::None));
            u64 observed{};
            std::memcpy(&observed, state->uniform_buffer_begin + 8, sizeof(observed));
            REQUIRE(observed == 0xbeef);
            if (program == 0) REQUIRE(state->current_loc == Location{0x6e01});
        }
        REQUIRE(backend::arm64::PrepareBlock(space.GetDefaultModule(), block));
    }

    IntrusivePtr<Block> block{new Block{0, Location{0x6f00}}};
    const auto source = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    BOOL narrow{Value{source}.SetCastType(ValueType::U8)};
    BOOL wide{source};
    (void)wide.SetCastType(ValueType::S64);
    block->SetTerminal(terminal::CheckHalt{terminal::Condition{Cond::EQ,
            terminal::If{narrow,
                    terminal::Switch{source,
                            {{Imm{u8{0}}, terminal::If{wide, terminal::ReturnToHost{},
                                                               terminal::ReturnToDispatch{}}}}},
                    terminal::ReturnToHost{}},
            terminal::Switch{source, {}}}});
    block->ReIdInstr();
    std::vector<Value> occurrences;
    compile::VisitTerminalValues(block->GetTerminal(),
            [&](Value value) { occurrences.push_back(value); });
    const std::array<ValueType, 4> views{
            ValueType::U8, ValueType::U64, ValueType::S64, ValueType::U64};
    REQUIRE(occurrences.size() == views.size());
    for (size_t index = 0; index < views.size(); ++index) {
        REQUIRE(occurrences[index].Def() == source.Def());
        REQUIRE(occurrences[index].Type() == views[index]);
    }
    REQUIRE(source.Def()->GetUses(false) == views.size());
    REQUIRE(backend::arm64::SupportsIR(block.get()));
    {
        compile::CompileUnit owner{block};
        owner.SealIR();
        compile::SemanticFacts facts{block.get()};
        REQUIRE(facts.PrepareSSAValidation());
    }
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 64> storage{};
    auto* state = new (storage.data()) backend::State{};
    const u64 input = 0x100000001;
    std::memcpy(state->uniform_buffer_begin, &input, sizeof(input));
    backend::interp::Interpreter interpreter{*state, block.get()};
    REQUIRE(interpreter.Run() == HaltReason::None); // Z=0 chooses the empty Switch.
}

#if defined(__aarch64__)
TEST_CASE("ARM64 terminal conditions execute all NZCV states in Blocks and regions",
          "[ir][terminal][jit][conditions]") {
    for (const bool region : {false, true}) {
        CAPTURE(region);
        Config config{.loc_start = 0x7000, .loc_end = 0xa000, .enable_jit = true,
                      .backend_isa = kArm64, .uniform_buffer_size = 64};
        config.stack_alignment = 16;
        config.region_edges = region;
        config.global_opts = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove |
                             Optimizations::FlagElimination;
        backend::AddressSpace space{config};
        const auto module = space.MapModule(0x7000, 0xa000,
                {.read_only = true, .optimizations = config.global_opts});
        std::array<IntrusivePtr<Block>, 2> flat_leaves;
        if (!region) {
            for (unsigned arm = 0; arm < flat_leaves.size(); ++arm) {
                const u64 pc = arm == 0 ? 0x9000 : 0x9010;
                auto& leaf = flat_leaves[arm];
                leaf = new Block{0, Location{pc}};
                leaf->SetEndLocation(Location{pc + 1});
                const auto value = leaf->LoadImm(ValueType::U64, Imm{u64{arm == 0 ? 11u : 22u}});
                leaf->StoreUniform(Uniform{8, ValueType::U64}, value);
                leaf->SetTerminal(terminal::ReturnToHost{});
                REQUIRE(module->Push(leaf.get()));
                auto* entry = TranslateIR(module, leaf);
                REQUIRE(entry != nullptr);
                space.PushCodeCache(pc, entry);
                REQUIRE(space.GetCodeCache(Location{pc}) == entry);
            }
        }
        for (u32 code = 0; code < 16; ++code) {
            CAPTURE(code);
            for (u32 prefix = 0; prefix < (code >= 14 ? 4u : 1u); ++prefix) {
                CAPTURE(prefix);
                const u64 guest = 0x8000 + code * 0x40 + prefix * 0x10;
                const Location then_pc{region ? guest + 1 : 0x9000};
                const Location else_pc{region ? guest + 2 : 0x9010};
                HIRBuilder builder{1, false, FeatureSet{}};
                auto* function = region ? builder.AppendFunction(Location{guest}, Location{guest + 3})
                                        : nullptr;
                IntrusivePtr<Block> block;
                if (!function) block = new Block{0, Location{guest}};
                auto* root = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock()
                                      : block.get();
                root->SetEndLocation(Location{guest + 1});
                const auto emit_prefix = [&](auto& factory) {
                    if (code < 14) factory.Nop();
                    else if (prefix == 0) factory.ClearFlags(Flags::NZCV);
                    else if (prefix == 3) factory.ClearFlags(Flags::Carry);
                    else {
                        const auto input = factory.LoadUniform(ValueType::U64,
                                                              Uniform{0, ValueType::U64});
                        const auto sum = factory.Add(ValueType::U64, input, Operand{Imm{u64{1}}});
                        factory.SaveFlags(sum, Flags::NZCV);
                        if (prefix == 2) factory.ClearFlags(Flags::Carry);
                    }
                };
                if (function) emit_prefix(*function);
                else emit_prefix(*block);
                const terminal::Condition terminal{static_cast<Cond>(code),
                        terminal::LinkBlock{then_pc}, terminal::LinkBlock{else_pc}};
                if (function) {
                    auto* entry_block = function->GetCurrentBlock();
                    auto* then_block = function->AppendBlock(then_pc);
                    auto* else_block = function->AppendBlock(else_pc);
                    function->EndBlock(terminal);
                    function->AddEdge(entry_block, then_block);
                    function->AddEdge(entry_block, else_block);
                    const auto leaf = [&](const HIRBlock* target, u64 result) {
                        function->SetCurBlock(target);
                        const auto value = function->LoadImm(ValueType::U64, Imm{result});
                        function->StoreUniform(Uniform{8, ValueType::U64}, value);
                        function->EndBlock(terminal::ReturnToHost{});
                    };
                    leaf(then_block, 11); leaf(else_block, 22);
                    function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
                    REQUIRE(function->GetHIRBlocksRPO().size() == 3);
                } else {
                    block->SetTerminal(terminal);
                    REQUIRE(module->Push(block.get()));
                }
                auto* entry = function ? TranslateIR(module, function) : TranslateIR(module, block);
                REQUIRE(entry != nullptr);
                space.PushCodeCache(guest, entry);
                Runtime runtime{&space};
                auto uniform = runtime.GetUniformBuffer();
                constexpr std::array<u64, 4> add_inputs{
                        0, 0x7fffffffffffffffull, UINT64_MAX, 0x8000000000000000ull};
                constexpr std::array<u64, 4> add_flags{
                        0, 0x90000000u, 0x60000000u, 0x80000000u};
                for (u32 nzcv = 0; nzcv < 16; ++nzcv) {
                    CAPTURE(nzcv);
                    const bool n = (nzcv & 8) != 0;
                    const bool z = (nzcv & 4) != 0;
                    const bool c = (nzcv & 2) != 0;
                    const bool v = (nzcv & 1) != 0;
                    const std::array<bool, 16> truth{
                            z, !z, c, !c, n, !n, v, !v, c && !z, !c || z,
                            n == v, n != v, !z && (n == v), z || (n != v), true, true};
                    std::fill(uniform.begin(), uniform.end(), 0xa5);
                    const u64 incoming_flags = (u64{nzcv} << 28) | (u64{1} << 26);
                    u64 expected_flags = incoming_flags & 0xf0000000u;
                    if (code >= 14) {
                        if (prefix == 0) expected_flags = 0;
                        else if (prefix == 1 || prefix == 2) {
                            const auto sample = nzcv % add_inputs.size();
                            std::memcpy(uniform.data(), &add_inputs[sample], sizeof(u64));
                            expected_flags = add_flags[sample];
                        }
                        if (prefix == 2 || prefix == 3) expected_flags &= ~u64{0x20000000};
                    }
                    runtime.GetState()->host_cpu_flags = incoming_flags;
                    runtime.SetLocation(guest);
                    REQUIRE(runtime.Run() == HaltReason::CallHost);
                    u64 selected{};
                    std::memcpy(&selected, uniform.data() + 8, sizeof(selected));
                    REQUIRE(selected == (truth[code] ? 11 : 22));
                    REQUIRE((runtime.GetState()->host_cpu_flags & 0xf0000000u) == expected_flags);
                    REQUIRE((runtime.GetState()->host_cpu_flags & (u64{1} << 26)) ==
                            (incoming_flags & (u64{1} << 26)));
                }
            }
        }
    }
}

TEST_CASE("direct flag writers publish full and partial updates without a guest commit",
          "[ir][regression][jit][flags][observation]") {
    const char* raw_flags = GetRawSvmConfigEnvForTest("SVM_FLAGS_REGS");
    const auto saved_flags = raw_flags ? std::optional<std::string>{raw_flags} : std::nullopt;
    const auto restore = ScopeExit([&] {
        if (saved_flags) SetSvmConfigEnvForTest("SVM_FLAGS_REGS", saved_flags->c_str(), 1);
        else UnsetSvmConfigEnvForTest("SVM_FLAGS_REGS");
    });
    struct ExpectedFlags {
        bool n{}, z{}, c{}, v{}, p{}, a{};
    };
    struct Seed {
        u64 word;
        ExpectedFlags flags;
    };
    constexpr std::array<Seed, 4> seeds{{
            {0x00000001u, {false, false, false, false, false, false}},
            {0xf4000000u, {true, true, true, true, true, true}},
            {0x54000001u, {false, true, false, true, false, true}},
            {0xa0000000u, {true, false, true, false, true, false}},
    }};
    // Independent ADD64 + 1 answers. In particular MAX signed overflows,
    // UINT64_MAX wraps with carry, and AF/PF cover both values independently
    // of the incoming seeds.
    constexpr std::array<u64, 4> inputs{
            0, 0x7fffffffffffffffull, UINT64_MAX, 0x8000000000000000ull};
    constexpr std::array<ExpectedFlags, 4> sums{{
            {false, false, false, false, false, false},
            {true, false, false, true, true, true},
            {false, true, true, false, true, true},
            {true, false, false, false, false, false},
    }};
    // The first two writers insert a computed bit. The rest save complete,
    // single-bit and compound subsets from a new arithmetic producer.
    const std::array<Flags, 12> masks{
            Flags::None, Flags::None, Flags::All, Flags::NZCV,
            Flags::Carry, Flags::Overflow, Flags::NZ, Flags::Negate, Flags::Zero,
            Flags::Parity, Flags::AuxiliaryCarry, Flags::Parity | Flags::AuxiliaryCarry};
    const std::array<Cond, 12> terminal_conditions{
            Cond::CS, Cond::VS, Cond::EQ, Cond::EQ, Cond::CS, Cond::VS,
            Cond::EQ, Cond::MI, Cond::EQ, Cond::MI, Cond::MI, Cond::MI};
    // Kind0 is a standalone Block, kind1 a one-block Function with external
    // leaves, and kind2 a three-block Function with both leaves inside it.
    for (const bool flags_regs : {false, true})
        for (u32 unit_kind = 0; unit_kind < 3; ++unit_kind)
            for (const bool optimize : {false, true}) {
                const bool function_mode = unit_kind != 0;
                const bool internal_region = unit_kind == 2;
                CAPTURE(flags_regs, unit_kind, optimize);
                REQUIRE(SetSvmConfigEnvForTest("SVM_FLAGS_REGS", flags_regs ? "1" : "0", 1) == 0);
                REQUIRE(GetSvmConfig().flags_regs == flags_regs);
                Config config{.loc_start = 0xb000, .loc_end = 0xd000, .enable_jit = true,
                              .backend_isa = kArm64, .uniform_buffer_size = 64};
                config.stack_alignment = 16;
                config.region_edges = function_mode;
                config.global_opts = optimize ? Optimizations::ConstantFolding |
                                                       Optimizations::DeadCodeRemove |
                                                       Optimizations::FlagElimination
                                             : Optimizations{};
                backend::AddressSpace space{config};
                const auto module = space.MapModule(0xb000, 0xd000,
                        {.read_only = true, .optimizations = config.global_opts});
                std::array<IntrusivePtr<Block>, 2> leaves;
                if (!internal_region) {
                    for (u32 arm = 0; arm < leaves.size(); ++arm) {
                        const u64 pc = 0xc000 + arm * 0x10;
                        auto& leaf = leaves[arm];
                        leaf = new Block{0, Location{pc}};
                        leaf->SetEndLocation(Location{pc + 1});
                        const auto value = leaf->LoadImm(ValueType::U64, Imm{u64{arm ? 22u : 11u}});
                        leaf->StoreUniform(Uniform{40, ValueType::U64}, value);
                        leaf->SetTerminal(terminal::ReturnToHost{});
                        REQUIRE(module->Push(leaf.get()));
                        auto* entry = TranslateIR(module, leaf);
                        REQUIRE(entry != nullptr);
                        space.PushCodeCache(pc, entry);
                    }
                }
                for (u32 writer = 0; writer < masks.size(); ++writer)
                    for (u32 prefix = 0; prefix < (writer < 2 ? 3u : 2u); ++prefix) {
                        CAPTURE(writer, prefix);
                        const u64 guest = 0xb000 + (writer * 3 + prefix) * 0x10;
                        HIRBuilder builder{1, false, FeatureSet{}};
                        auto* function = function_mode
                                ? builder.AppendFunction(Location{guest},
                                                         Location{guest + (internal_region ? 10u : 1u)})
                                : nullptr;
                        IntrusivePtr<Block> block;
                        if (!function) block = new Block{0, Location{guest}};
                        auto* body = function
                                ? function->EditBlock(function->GetCurrentBlock()).GetBlock()
                                : block.get();
                        body->SetEndLocation(Location{guest + 1});
                        const auto append = [&](OpCode op, ValueType type, const auto&... args) {
                            return Value{function ? function->AppendInst(op, type, args...)
                                                  : block->AppendInst(op, type, args...)};
                        };
                        const auto input = append(OpCode::LoadUniform, ValueType::U64,
                                                  Uniform{0, ValueType::U64});
                        const auto bit = append(OpCode::LoadUniform, ValueType::U8,
                                                Uniform{8, ValueType::U8});
                        // Prefix0 preserves the incoming flags, prefix1 has a
                        // queued full clear, and prefix2 has dirty arithmetic
                        // flags before a SetCarry/SetOverflow overwrites one bit.
                        const auto write = [&] {
                            if (prefix == 1) append(OpCode::ClearFlags, ValueType::VOID, Flags::All);
                            else if (prefix == 2) {
                                const auto pending = append(OpCode::Add, ValueType::U64,
                                                            input, Operand{Imm{u64{1}}});
                                append(OpCode::SaveFlags, ValueType::VOID, pending, Flags::All);
                            }
                            if (writer < 2) {
                                append(writer == 0 ? OpCode::SetCarry : OpCode::SetOverflow,
                                       ValueType::VOID, bit);
                                return input;
                            }
                            const auto sum = append(OpCode::Add, ValueType::U64,
                                                    input, Operand{Imm{u64{1}}});
                            append(OpCode::SaveFlags, ValueType::VOID, sum, masks[writer]);
                            return sum;
                        };
                        const auto store = [&](u32 offset, Value value) {
                            append(OpCode::StoreUniform, ValueType::VOID,
                                   Uniform{offset, value.Type()}, value);
                        };
                        // Each reader sees a fresh write as its first flag
                        // observation; another reader cannot flush it first.
                        const std::array<Flags, 4> reads{
                                Flags::Carry, Flags::Overflow, Flags::Parity, Flags::AuxiliaryCarry};
                        for (u32 index = 0; index < reads.size(); ++index) {
                            write();
                            store(16 + index, append(OpCode::TestFlags, ValueType::U8, reads[index]));
                        }
                        const std::array<Cond, 4> conditions{Cond::CS, Cond::VS, Cond::EQ, Cond::MI};
                        for (u32 index = 0; index < conditions.size(); ++index) {
                            write();
                            store(20 + index, append(OpCode::CondSet, ValueType::U8, conditions[index]));
                        }
                        const auto snapshot_source = write();
                        store(24, append(OpCode::GetFlags, ValueType::U64, snapshot_source, Flags::All));
                        const auto yes = append(OpCode::LoadImm, ValueType::U8, Imm{u8{0x11}});
                        const auto no = append(OpCode::LoadImm, ValueType::U8, Imm{u8{0x22}});
                        for (u32 index = 0; index < conditions.size(); ++index) {
                            write();
                            store(32 + index,
                                  append(OpCode::CondSelect, ValueType::U8, conditions[index], yes, no));
                        }
                        write();
                        const Location then_pc{internal_region ? guest + 4 : 0xc000};
                        const Location else_pc{internal_region ? guest + 8 : 0xc010};
                        const terminal::Condition terminal{terminal_conditions[writer],
                                terminal::LinkBlock{then_pc}, terminal::LinkBlock{else_pc}};
                        REQUIRE(std::none_of(body->GetInstList().begin(), body->GetInstList().end(),
                                [](const Inst& inst) { return inst.GetOp() == OpCode::AdvancePC; }));
                        if (function) {
                            if (internal_region) {
                                auto* entry_block = function->GetCurrentBlock();
                                auto* then_block = function->AppendBlock(then_pc);
                                auto* else_block = function->AppendBlock(else_pc);
                                function->EndBlock(terminal);
                                function->AddEdge(entry_block, then_block);
                                function->AddEdge(entry_block, else_block);
                                const auto leaf = [&](const HIRBlock* target, u64 result) {
                                    function->SetCurBlock(target);
                                    auto* member = function->EditBlock(target).GetBlock();
                                    member->SetEndLocation(Location{member->GetStartLocation().Value() + 1});
                                    const auto value = function->LoadImm(ValueType::U64, Imm{result});
                                    function->StoreUniform(Uniform{40, ValueType::U64}, value);
                                    function->EndBlock(terminal::ReturnToHost{});
                                };
                                leaf(then_block, 11);
                                leaf(else_block, 22);
                            } else {
                                function->EndBlock(terminal);
                            }
                            function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
                            REQUIRE(function->GetHIRBlocksRPO().size() == (internal_region ? 3u : 1u));
                            if (internal_region) {
                                for (const auto& member : function->GetHIRBlocksRPO()) {
                                    REQUIRE(std::none_of(member.GetInstList().begin(), member.GetInstList().end(),
                                            [](const Inst& inst) { return inst.GetOp() == OpCode::AdvancePC; }));
                                }
                            }
                        } else {
                            block->SetTerminal(terminal);
                            REQUIRE(module->Push(block.get()));
                        }
                        auto* entry = function ? TranslateIR(module, function) : TranslateIR(module, block);
                        REQUIRE(entry != nullptr);
                        space.PushCodeCache(guest, entry);
                        Runtime runtime{&space};
                        auto uniform = runtime.GetUniformBuffer();
                        for (const auto& seed : seeds)
                            for (u32 sample = 0; sample < inputs.size(); ++sample)
                                for (u32 set_bit = 0; set_bit < (writer < 2 ? 2u : 1u); ++set_bit) {
                                    CAPTURE(seed.word, sample, set_bit);
                                    const auto base = prefix == 1 ? ExpectedFlags{}
                                                      : prefix == 2 ? sums[sample] : seed.flags;
                                    const auto sum = sums[sample];
                                    const std::array<ExpectedFlags, 12> answers{{
                                            {base.n, base.z, set_bit != 0, base.v, base.p, base.a},
                                            {base.n, base.z, base.c, set_bit != 0, base.p, base.a},
                                            sum,
                                            {sum.n, sum.z, sum.c, sum.v, base.p, base.a},
                                            {base.n, base.z, sum.c, base.v, base.p, base.a},
                                            {base.n, base.z, base.c, sum.v, base.p, base.a},
                                            {sum.n, sum.z, base.c, base.v, base.p, base.a},
                                            {sum.n, base.z, base.c, base.v, base.p, base.a},
                                            {base.n, sum.z, base.c, base.v, base.p, base.a},
                                            {base.n, base.z, base.c, base.v, sum.p, base.a},
                                            {base.n, base.z, base.c, base.v, base.p, sum.a},
                                            {base.n, base.z, base.c, base.v, sum.p, sum.a},
                                    }};
                                    const auto expected = answers[writer];
                                    const u64 packed = (u64{expected.n} << 31) | (u64{expected.z} << 30) |
                                                       (u64{expected.c} << 29) | (u64{expected.v} << 28) |
                                                       (u64{expected.a} << 26);
                                    std::fill(uniform.begin(), uniform.end(), 0xa5);
                                    std::memcpy(uniform.data(), &inputs[sample], sizeof(u64));
                                    uniform[8] = static_cast<u8>(set_bit);
                                    runtime.GetState()->host_cpu_flags = seed.word;
                                    runtime.SetLocation(guest);
                                    REQUIRE(runtime.Run() == HaltReason::CallHost);
                                    REQUIRE(uniform[16] == expected.c);
                                    REQUIRE(uniform[17] == expected.v);
                                    REQUIRE(uniform[18] == expected.p);
                                    REQUIRE(uniform[19] == expected.a);
                                    const std::array<bool, 4> taken{expected.c, expected.v, expected.z, expected.n};
                                    for (u32 index = 0; index < taken.size(); ++index) {
                                        REQUIRE(uniform[20 + index] == taken[index]);
                                        REQUIRE(uniform[32 + index] == (taken[index] ? 0x11 : 0x22));
                                    }
                                    u64 snapshot{}, selected{};
                                    std::memcpy(&snapshot, uniform.data() + 24, sizeof(snapshot));
                                    std::memcpy(&selected, uniform.data() + 40, sizeof(selected));
                                    REQUIRE((snapshot & 0xf4000000u) == packed);
                                    REQUIRE((std::popcount(static_cast<u8>(snapshot)) % 2 == 0) == expected.p);
                                    const std::array<bool, 12> terminal_taken{
                                            expected.c, expected.v, expected.z, expected.z,
                                            expected.c, expected.v, expected.z, expected.n,
                                            expected.z, expected.n, expected.n, expected.n};
                                    REQUIRE(selected == (terminal_taken[writer] ? 11 : 22));
                                    const auto final_flags = runtime.GetState()->host_cpu_flags;
                                    REQUIRE((final_flags & 0xf4000000u) == packed);
                                    REQUIRE((std::popcount(static_cast<u8>(final_flags)) % 2 == 0) == expected.p);
                                }
                    }
            }
}
#endif

TEST_CASE("value byte widths retain checked invalid-type failures", "[ir][types][size]") {
    const std::array widths{
            std::pair{ValueType::U8, 1u}, std::pair{ValueType::U16, 2u},
            std::pair{ValueType::U32, 4u}, std::pair{ValueType::U64, 8u},
            std::pair{ValueType::S8, 1u}, std::pair{ValueType::S16, 2u},
            std::pair{ValueType::S32, 4u}, std::pair{ValueType::S64, 8u},
            std::pair{ValueType::V8, 1u}, std::pair{ValueType::V16, 2u},
            std::pair{ValueType::V32, 4u}, std::pair{ValueType::V64, 8u},
            std::pair{ValueType::V128, 16u}, std::pair{ValueType::V256, 32u}};
    for (const auto& [type, bytes] : widths) {
        CAPTURE(type);
        REQUIRE(GetValueSizeByte(type) == bytes);
    }
    const std::array invalid{
            ValueType::VOID,
            static_cast<ValueType>(static_cast<unsigned>(ValueType::V256) + 1u),
            static_cast<ValueType>(255)};
    for (const auto type : invalid) {
        CAPTURE(type);
        try {
            (void)GetValueSizeByte(type);
            FAIL("invalid byte-width type was accepted");
        } catch (const std::logic_error& error) {
            REQUIRE(std::string{error.what()} == "Check Failed! ");
        }
    }
}

TEST_CASE("plain assertions preserve condition evaluation and exact failure text",
          "[ir][logging][assert]") {
    unsigned evaluations = 0;
    unsigned skipped = 0;
    ASSERT(++evaluations == 1 || ++skipped != 0);
    REQUIRE(evaluations == 1);
    REQUIRE(skipped == 0);

    try {
        ASSERT(++evaluations == 0);
        FAIL("false plain assertion was accepted");
    } catch (const std::logic_error& error) {
        REQUIRE(evaluations == 2);
        REQUIRE(std::string{error.what()} == "Check failed: \"++evaluations == 0\"");
    }
}


namespace {
enum class FlagObservationKind { Load, Store, TSOLoad, TSOStore, Atomic, Alignment, NoFaultHelper };

const Inst* AppendFlagObservation(compile::IrBuilder& factory, const Block* block,
                                  FlagObservationKind kind, Value source) {
    const auto address = factory.LoadUniform(Uniform{8, ValueType::U64});
    switch (kind) {
        case FlagObservationKind::Load:
            (void)factory.LoadMemory(ValueType::U64, Operand{address});
            break;
        case FlagObservationKind::Store:
            factory.StoreMemory(Operand{address}, source);
            break;
        case FlagObservationKind::TSOLoad:
            (void)factory.LoadMemoryTSO(ValueType::U64, Operand{address});
            break;
        case FlagObservationKind::TSOStore:
            factory.StoreMemoryTSO(Operand{address}, source);
            break;
        case FlagObservationKind::Atomic:
            (void)factory.AtomicRMW(ValueType::U64,
                    Imm{static_cast<u8>(AtomicRMWOp::Add)}, address, source,
                    factory.LoadImm(ValueType::U8, Imm{u8{0}}));
            break;
        case FlagObservationKind::Alignment:
            factory.CheckMemoryAlignment(address, Imm{15u});
            break;
        case FlagObservationKind::NoFaultHelper:
            factory.CallLambda(ValueType::VOID, Lambda{DataClass{Imm{u64{1}}},
                    HelperCallTraits{.uniform = UniformEffectId::None,
                                     .guest_state = HelperGuestStateEffect::None,
                                     .fault = HelperFaultEffect::NoDirectFault,
                                     .reentry = HelperReentryEffect::NoReentry}});
            break;
    }
    return &block->GetInstList().back();
}
}  // namespace

TEST_CASE("flag cleanup retains fault observations before a complete overwrite",
          "[ir][flags][fault]") {
    // Structural IR only: neither the guest address nor the helper is run.
    // A fault observes the flags before its operation succeeds. A full write
    // before that point, unlike one after it, kills the incoming definition.
    for (const bool function_mode : {false, true})
    for (const bool overwrite_first : {false, true})
    for (const auto kind : {FlagObservationKind::Load, FlagObservationKind::Store,
                           FlagObservationKind::TSOLoad, FlagObservationKind::TSOStore,
                           FlagObservationKind::Atomic, FlagObservationKind::Alignment,
                           FlagObservationKind::NoFaultHelper}) {
        CAPTURE(function_mode, overwrite_first, kind);
        FeatureSet features{};
        features.flags_branch_only = false;
        features.flag_carry_elim = true;
        features.flag_full_elim = true;
        HIRBuilder builder{1, false, features};
        auto* function = builder.AppendFunction(Location{0x8a00}, Location{0x8b00});
        auto* root = function->GetCurrentBlock();
        compile::IrBuilder factory{&builder};
        const auto input = factory.LoadUniform(Uniform{0, ValueType::U64});
        const auto sum = factory.Add(ValueType::U64, input, Operand{Imm{1u}});
        factory.SaveFlags(sum, Flags::All);
        const auto* publication = &root->GetInstList().back();
        auto* body = root;
        if (function_mode) {
            body = function->AppendBlock(Location{0x8a10});
            function->EndBlock(terminal::LinkBlock{Location{0x8a10}});
            function->AddEdge(root, body);
            builder.SetCurBlock(body);
        }
        if (overwrite_first) factory.ClearFlags(Flags::All);
        const auto* observation = AppendFlagObservation(factory, body->GetBlock(), kind, input);
        factory.ClearFlags(Flags::All);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        const auto expected = !overwrite_first && kind != FlagObservationKind::NoFaultHelper
                ? Flags::All : Flags::None;
        if (function_mode) {
            compile::FlagFlow flow{function};
            flow.ComputeLiveIn();
            REQUIRE(flow.LiveIn(body) == std::optional<Flags>{expected});
            compile::EliminateFlags(RewriteFixture{function}.Get(), features);
        } else {
            compile::EliminateFlags(RewriteFixture{function->EditBlock(root).GetBlock(), true, function}.Get(), features);
        }
        const auto& list = root->GetInstList();
        const auto found = std::find_if(list.begin(), list.end(),
                [&](const Inst& inst) { return &inst == publication; });
        REQUIRE((found != list.end()) == (expected != Flags::None));
        if (found != list.end()) REQUIRE(found->GetArg<Flags>(1) == Flags::All);
        const auto& body_list = body->GetInstList();
        REQUIRE(std::any_of(body_list.begin(), body_list.end(),
                [&](const Inst& inst) { return &inst == observation; }));
    }
}

TEST_CASE("fault-aware local flag demand preserves skipped and partial overwrites",
          "[ir][flags][fault][labels]") {
    for (const bool function_mode : {false, true})
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        CAPTURE(function_mode, scenario);
        FeatureSet features{};
        features.flags_branch_only = false;
        features.flag_carry_elim = true;
        features.flag_full_elim = true;
        HIRBuilder builder{1, false, features};
        auto* function = builder.AppendFunction(Location{0x8b00}, Location{0x8c00});
        auto* root = function->GetCurrentBlock();
        compile::IrBuilder factory{&builder};
        const auto input = factory.LoadUniform(Uniform{0, ValueType::U64});
        const auto sum = factory.Add(ValueType::U64, input, Operand{Imm{1u}});
        factory.SaveFlags(sum, Flags::All);
        const auto* publication = &root->GetInstList().back();
        auto* body = root;
        if (function_mode) {
            body = function->AppendBlock(Location{0x8b10});
            function->EndBlock(terminal::LinkBlock{Location{0x8b10}});
            function->AddEdge(root, body);
            builder.SetCurBlock(body);
        }
        const auto condition = factory.LoadUniform(Uniform{16, ValueType::U8});
        if (scenario == 1) factory.ClearFlags(Flags::All);
        if (scenario == 2) factory.ClearFlags(Flags::Carry);
        const auto jump = Value{factory.AppendInst(OpCode::Goto, ValueType::U8, condition)};
        factory.ClearFlags(scenario == 3 ? Flags::Carry : Flags::All);
        factory.BindLabel(jump);
        const auto* fault = AppendFlagObservation(
                factory, body->GetBlock(), FlagObservationKind::TSOLoad, input);
        factory.ClearFlags(Flags::All);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        // The branch can skip a full or partial write. Only writes before
        // both paths may remove bits from the fault's incoming demand.
        const Flags expected[] = {Flags::All, Flags::None,
                                  Flags::All & ~Flags::Carry, Flags::All};
        if (function_mode) {
            compile::FlagFlow flow{function};
            flow.ComputeLiveIn();
            REQUIRE(flow.LiveIn(body) == std::optional<Flags>{expected[scenario]});
            compile::EliminateFlags(RewriteFixture{function}.Get(), features);
        } else {
            compile::EliminateFlags(RewriteFixture{function->EditBlock(root).GetBlock(), true, function}.Get(), features);
        }
        const auto& list = root->GetInstList();
        const auto found = std::find_if(list.begin(), list.end(),
                [&](const Inst& inst) { return &inst == publication; });
        REQUIRE((found != list.end()) == (expected[scenario] != Flags::None));
        if (found != list.end()) {
            // Function cleanup intentionally keeps its existing None/All
            // publication policy; the demand query above stays bit-precise.
            REQUIRE(found->GetArg<Flags>(1) ==
                    (function_mode ? Flags::All : expected[scenario]));
        }
        const auto& body_list = body->GetInstList();
        REQUIRE(std::any_of(body_list.begin(), body_list.end(),
                [&](const Inst& inst) { return &inst == fault; }));
    }
}
