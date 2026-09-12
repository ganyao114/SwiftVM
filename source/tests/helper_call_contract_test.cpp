#include <catch2/catch_test_macros.hpp>

#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/arm64/jit/guest_state_map.h"
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
    auto* conservative = block.AppendInst(OpCode::CallLambda, Lambda{Imm{swift::u64{1}}});
    auto* exact = block.AppendInst(
            OpCode::CallLambda,
            Lambda{DataClass{Imm{swift::u64{1}}},
                   HelperCallTraits{
                           .guest_state = HelperGuestStateEffect::None,
                           .fault = HelperFaultEffect::NoDirectFault,
                           .reentry = HelperReentryEffect::NoReentry,
                   }});
    GuestStateMap state;
    state.Analyze(&block, FeatureSet{});
    REQUIRE(state.MayFaultOrObserve(*conservative));
    REQUIRE_FALSE(state.MayFaultOrObserve(*exact));
}

TEST_CASE("SSE4.2 string contracts expose exact lowering effects",
          "[helper-contract][sse42]") {
    Block block{0, Location{0x4520}};
    auto left = block.LoadUniform(Uniform{0, ValueType::V128});
    auto right = block.LoadUniform(Uniform{16, ValueType::V128});
    auto* native = block.Sse42Str(left, right, Imm{swift::u64{0x02}}).Def();
    auto* inline_ = block.Sse42Str(left, right, Imm{swift::u64{0x00}}).Def();
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

    GuestStateMap state;
    state.Analyze(&block, features);
    REQUIRE_FALSE(state.MayFaultOrObserve(*native));
    REQUIRE_FALSE(state.MayFaultOrObserve(*inline_));
}

}  // namespace
