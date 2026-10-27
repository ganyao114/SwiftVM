#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <type_traits>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/compiler.h"

#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

static_assert(std::is_constructible_v<arm64::JitContext,
                                      const std::shared_ptr<Module>&,
                                      compile::AllocationView,
                                      const arm64::UnitActions&>);
static_assert(std::is_same_v<decltype(std::declval<arm64::JitContext&>().GetAllocation()),
                             compile::AllocationView>);
static_assert(std::is_same_v<decltype(std::declval<const arm64::JitContext&>().GetAllocation()),
                             compile::AllocationView>);

void CheckCoalescingContract(OpCode opcode, bool live) {
    Config config{.loc_start = 0, .loc_end = 1ull << 48,
                  .enable_jit = true, .has_local_operation = false,
                  .backend_isa = kArm64};
    AddressSpace space{config};
    ModuleConfig module_config{};
    module_config.feature_overrides.Set(FeatureId::ra_coalesce, true);
    module_config.feature_overrides.Set(FeatureId::ra_coalesce_live, live);
    auto module = space.MapModule(0x9000, 0x9100, module_config);
    auto features = ResolveFeatureSet(module_config);
    IntrusivePtr<Block> block{new Block(0, Location{0x9000})};
    Value result;
    switch (opcode) {
        case OpCode::GetHostFPR:
            result = block->GetHostFPR(ValueType::U64, HostRegIndex(24), Imm{0u});
            break;
        case OpCode::SignedDiv64: {
            auto left = block->LoadImm(ValueType::U64, Imm{swift::u64{42}});
            auto right = block->LoadImm(ValueType::U64, Imm{swift::u64{3}});
            result = block->SignedDiv64(left, right);
            break;
        }
        case OpCode::VecMovMask: {
            auto input = block->GetHostFPR(ValueType::V128, HostRegIndex(24), Imm{0u});
            result = block->VecMovMask(ValueType::U32, input, Imm{8u});
            result = block->ZeroExtend32To64(ValueType::U64, result);
            break;
        }
        default: FAIL("unexpected test opcode");
    }
    block->SetHostGPR(result, HostRegIndex(19), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    GPRSMask gprs{swift::u32{0}};
    for (swift::u32 code : {0u, 1u, 2u, 3u, 4u, 5u, 19u, 20u, 21u,
                            22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u}) {
        gprs.Mark(code);
    }
    AllocationFixture alloc{block->MaxInstrId(), gprs, FPRSMask{~((1u << 8) - 1u)}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(block.get(), alloc, features),
            false);
    REQUIRE(alloc.IsHostWriteCoalesced(publication->Id()));
    REQUIRE(alloc.ValueGPR(result).id == 19);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    REQUIRE_NOTHROW(translator.Translate(block.get()));
    REQUIRE_NOTHROW(context.Finish());
    REQUIRE(context.CurrentBufferSize() > 0);
}
}

TEST_CASE("coalesced producers satisfy emitter contracts", "[coalescing-contract]") {
    for (const auto opcode : {OpCode::GetHostFPR, OpCode::SignedDiv64, OpCode::VecMovMask}) {
        for (bool live : {false, true}) {
            DYNAMIC_SECTION("opcode=" << static_cast<unsigned>(opcode) << " live=" << live) {
                CheckCoalescingContract(opcode, live);
            }
        }
    }
}
