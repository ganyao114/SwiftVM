#include <catch2/catch_test_macros.hpp>

#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

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
            result = block->GetHostFPR(HostRegIndex(24), Imm{0u}).SetType(ValueType::U64);
            break;
        case OpCode::SignedDiv64: {
            auto left = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
            auto right = block->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
            result = block->SignedDiv64(left, right);
            break;
        }
        case OpCode::VecMovMask: {
            auto input = block->GetHostFPR(HostRegIndex(24), Imm{0u}).SetType(ValueType::V128);
            result = block->VecMovMask(input, Imm{8u}).SetType(ValueType::U32);
            result = block->ZeroExtend32To64(result).SetType(ValueType::U64);
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
    RegAlloc alloc{block->MaxInstrId(), gprs, FPRSMask{~((1u << 8) - 1u)}, features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    REQUIRE(alloc.IsHostWriteCoalesced(publication->Id()));
    REQUIRE(alloc.ValueGPR(result).id == 19);
    arm64::JitContext context{module, alloc};
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
