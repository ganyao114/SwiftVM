#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <stdexcept>
#include "runtime/backend/arm64/compile/call_preparation.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/frontend/x86/sse42str_helper.h"
#include "runtime/ir/hir_builder.h"
#include "tests/support/allocation_fixture.h"

#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/arm64/compile/effects.h"
#include "runtime/common/helper_abi.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend::arm64;
using namespace swift::runtime::ir;

TEST_CASE("helper call contract resolves register and state effects", "[helper-contract]") {
    FeatureSet features{};
    features.helper_leaf_abi = true;

    const auto conservative = HelperCallContract::Resolve(Lambda{Imm{1}}, features);
    REQUIRE(conservative.IsDirect());
    REQUIRE(conservative.ClobbersGPR(7));
    REQUIRE_FALSE(conservative.ClobbersGPR(20));
    REQUIRE(conservative.ClobbersFPR(20));
    REQUIRE(conservative.UniformEffects() == UniformEffectId::Unknown);
    REQUIRE(conservative.ReadsGuestState());
    REQUIRE(conservative.WritesGuestState());
    REQUIRE(conservative.MayFault());
    REQUIRE(conservative.MayReenter());
    REQUIRE_FALSE(conservative.PreservesHostNZCV());
    REQUIRE(conservative.RequiresGuestStatePublication());
    REQUIRE_FALSE(conservative.RetainsPendingNZCV());

    const auto resident = HelperCallContract::Resolve(
            Lambda{DataClass{Imm{1}},
                   HelperCallTraits{
                           .uniform = UniformEffectId::None,
                           .host_fp = HostFpEffect::FPCRTransparent,
                           .host_registers = HostRegisterEffect::PreservesPinnedState,
                           .guest_state = HelperGuestStateEffect::None,
                           .fault = HelperFaultEffect::NoDirectFault,
                           .reentry = HelperReentryEffect::NoReentry,
                           .host_flags = HostFlagsEffect::PreservesNZCV,
                   }},
            features);
    REQUIRE(resident.FPCRTransparent());
    REQUIRE(resident.PreservesPinnedState());
    REQUIRE(resident.UniformEffects() == UniformEffectId::None);
    REQUIRE(resident.ClobbersGPR(2));
    REQUIRE_FALSE(resident.ClobbersGPR(7));
    REQUIRE(resident.ClobbersGPR(11));
    REQUIRE_FALSE(resident.ClobbersGPR(20));
    REQUIRE(resident.ClobbersFPR(7));
    REQUIRE(resident.ClobbersFPR(8));
    REQUIRE_FALSE(resident.ClobbersFPR(20));
    REQUIRE_FALSE(resident.ArgumentRequiresSlot(7));
    REQUIRE(resident.ArgumentRequiresSlot(2));
    REQUIRE(resident.RequiresGPRCapture(11, true));
    REQUIRE_FALSE(resident.ReadsGuestState());
    REQUIRE_FALSE(resident.WritesGuestState());
    REQUIRE_FALSE(resident.MayFault());
    REQUIRE_FALSE(resident.MayReenter());
    REQUIRE(resident.PreservesHostNZCV());
    REQUIRE_FALSE(resident.RequiresGuestStatePublication());
    REQUIRE(resident.RetainsPendingNZCV());

    const auto leaf = HelperCallContract::Resolve(
            Lambda{DataClass{Imm{1}}, HelperCallTraits{.abi = HelperABI::PreserveAllLeaf}},
            features);
#if SVM_HAS_HELPER_PRESERVE_ALL
    REQUIRE(leaf.PreserveAllLeaf());
    REQUIRE_FALSE(leaf.ClobbersGPR(9));
    REQUIRE_FALSE(leaf.ClobbersFPR(8));
#else
    REQUIRE_FALSE(leaf.PreserveAllLeaf());
#endif
}

TEST_CASE("guest state observation uses exact helper effects", "[helper-contract]") {
    Block block{0, Location{0x4510}};
    auto* conservative = block.AppendInst(OpCode::CallLambda, ValueType::VOID, Lambda{Imm{swift::u64{1}}});
    auto* exact = block.AppendInst(
            OpCode::CallLambda, ValueType::VOID,
            Lambda{DataClass{Imm{swift::u64{1}}},
                   HelperCallTraits{
                           .guest_state = HelperGuestStateEffect::None,
                           .fault = HelperFaultEffect::NoDirectFault,
                           .reentry = HelperReentryEffect::NoReentry,
                   }});
    const FeatureSet features{};
    REQUIRE(TargetMayFaultOrObserve(*conservative, features));
    REQUIRE_FALSE(TargetMayFaultOrObserve(*exact, features));
}

TEST_CASE("SSE4.2 string contracts expose exact lowering effects",
          "[helper-contract][sse42]") {
    Block block{0, Location{0x4520}};
    auto left = block.LoadUniform(Uniform{0, ValueType::V128});
    auto right = block.LoadUniform(Uniform{16, ValueType::V128});
    auto* native = block.Sse42Str(ValueType::U64, left, right, Imm{swift::u64{0x02}}).Def();
    auto* inline_ = block.Sse42Str(ValueType::U64, left, right, Imm{swift::u64{0x00}}).Def();
    FeatureSet features{};

    const auto native_contract = HelperCallContract::Resolve(*native, features);
    REQUIRE(native_contract.has_value());
    REQUIRE(native_contract->IsDirect());
    REQUIRE(native_contract->FPCRTransparent());
    REQUIRE(native_contract->PreservesPinnedState());
    REQUIRE_FALSE(native_contract->RequiresGuestStatePublication());
    REQUIRE_FALSE(native_contract->RetainsPendingNZCV());
    REQUIRE_FALSE(native_contract->ClobbersGPR(0));
    REQUIRE_FALSE(native_contract->ClobbersGPR(7));
    REQUIRE(native_contract->ClobbersGPR(11));
    REQUIRE(native_contract->ClobbersGPR(16));
    REQUIRE(native_contract->ClobbersGPR(17));
    REQUIRE(native_contract->ClobbersFPR(0));
    REQUIRE(native_contract->ClobbersFPR(1));
    REQUIRE_FALSE(native_contract->ClobbersFPR(2));
    REQUIRE(native_contract->ClobbersFPR(3));
    REQUIRE(native_contract->ClobbersFPR(7));

    const auto inline_contract = HelperCallContract::Resolve(*inline_, features);
    REQUIRE(inline_contract.has_value());
    REQUIRE_FALSE(inline_contract->RequiresGuestStatePublication());
    REQUIRE_FALSE(inline_contract->ClobbersGPR(11));
    REQUIRE_FALSE(inline_contract->ClobbersFPR(0));


    REQUIRE_FALSE(TargetMayFaultOrObserve(*native, features));
    REQUIRE_FALSE(TargetMayFaultOrObserve(*inline_, features));
}

TEST_CASE("shared host calls select complete targets and discard stale preparation",
          "[helper-contract][ownership]") {
    const FeatureSet features;
    HIRBuilder builder{1, false, features};
    auto* function = builder.AppendFunction(Location{0x4580}, Location{0x45f1});
    constexpr swift::u64 first = 0x100000042, second = 0x200000042;
    function->CallLambda(ValueType::VOID, Lambda{Imm{first}});
    function->CallLambda(ValueType::VOID, Lambda{Imm{second}});
    function->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{0}}});
    function->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{0}}});
    function->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{0x76543210}}});
    const auto dynamic = function->LoadImm(ValueType::U64, Imm{swift::u64{0x76543211}});
    function->CallLambda(ValueType::VOID, Lambda{dynamic});
    function->CallLambda(ValueType::VOID, Lambda{dynamic});
    const auto left = function->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
    const auto right = function->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
    function->Sse42Str(ValueType::U64, left, right, Imm{swift::u64{0x02}});
    function->Sse42Str(ValueType::U64, left, right, Imm{swift::u64{0x00}});
    builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x45c0}}));
    for (unsigned i = 0; i < 300; ++i) function->CallLambda(ValueType::VOID, Lambda{Imm{first}});
    function->CallLambda(ValueType::VOID, Lambda{Imm{second}});
    function->Sse42Str(ValueType::U64, left, right, Imm{swift::u64{0x02}});
    builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x45d0}}));
    function->AdvancePC(Imm{swift::u64{1}});
    function->EndBlock(terminal::ReturnToHost{});
    // An unconnected member must not turn a single reachable call into a
    // shared target, nor introduce an unreachable-only target.
    function->SetCurBlock(function->AppendBlock(Location{0x45e0}));
    function->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{0x76543210}}});
    function->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{0x76543212}}});
    function->CallLambda(ValueType::VOID, Lambda{Imm{swift::u64{0x76543212}}});
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    compile::SemanticFacts facts{function};
    std::vector<swift::u64> expected{first, second};
    if (const auto sse = swift::x86::Sse42StrVectorHelperAddress(0x02)) expected.push_back(sse);
    std::sort(expected.begin(), expected.end());
    const auto targets = PrepareSharedHostCalls(*function, facts);
    REQUIRE(targets == expected);
    REQUIRE_FALSE(facts.HasSpillUses());
    unsigned call_blocks = 0, ordinary_blocks = 0;
    for (const auto& member : function->GetHIRBlocksRPO()) {
        if (facts.PrepareSummary(member.GetBlock()).HasHostCallCandidates())
            ++call_blocks;
        else
            ++ordinary_blocks;
        REQUIRE_FALSE(facts.FindIndex(member.GetBlock()));
    }
    REQUIRE(call_blocks == 2);
    REQUIRE(ordinary_blocks == 1);
    swift::runtime::test::AllocationFixture allocation{
            function->MaxInstrCount(), backend::GPRSMask{}, backend::FPRSMask{}, features};
    REQUIRE(allocation.Actions().SharedHostCallTargets().empty());
    allocation.BeginTargetPlanning();
    allocation.StoreSharedHostCalls(targets);
    allocation.EndTargetPlanning();
    const auto saved = allocation.Actions().SharedHostCallTargets();
    REQUIRE(std::equal(saved.begin(), saved.end(), expected.begin(), expected.end()));
    REQUIRE_THROWS_AS(allocation.StoreSharedHostCalls(targets), std::logic_error);
    allocation.ResetAllocations();
    REQUIRE(allocation.Actions().SharedHostCallTargets().empty());
    allocation.BeginTargetPlanning();
    REQUIRE(allocation.Actions().SharedHostCallTargets().empty());
    allocation.StoreSharedHostCalls(targets);
    allocation.AbortTargetPlanning();
    REQUIRE(allocation.Actions().SharedHostCallTargets().empty());
}

}  // namespace
