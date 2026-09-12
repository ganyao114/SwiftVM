#include "support/register_alloc_test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/scalar_fpr_liveness.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

constexpr swift::u32 kResidentTarget = 17;

FPRSMask ResidentFPRs() {
    FPRSMask result{0};
    result.Mark(kResidentTarget);
    return result;
}

struct ScalarPublication {
    IntrusivePtr<Block> block;
    Value result;
    Inst* publish;
};

ScalarPublication MakeScalarPublication(bool scalar32, bool fixed_left,
                                        bool fault_observer = false,
                                        bool keep_left_live = false) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8730})};
    auto left = fixed_left
            ? block->GetHostFPR(HostRegIndex(kResidentTarget), Imm{0u})
                      .SetType(ValueType::V128)
            : block->LoadUniform(Uniform{0, ValueType::V128})
                      .SetType(ValueType::V128);
    auto right = block->LoadUniform(Uniform{16, ValueType::V128})
                         .SetType(ValueType::V128);
    auto result = scalar32
            ? block->VecFAddScalar32(left, right).SetType(ValueType::V128)
            : block->VecFAddScalar64(left, right).SetType(ValueType::V128);
    if (keep_left_live) {
        block->StoreUniform(Uniform{32, ValueType::V128}, left);
    }
    if (fault_observer) {
        auto address = block->LoadUniform(Uniform{48, ValueType::U64})
                               .SetType(ValueType::U64);
        (void)block->LoadMemory(Operand{address}).SetType(ValueType::U64);
    }
    auto* publish = block->AppendInst(
            OpCode::SetHostFPR, result, HostRegIndex(kResidentTarget), Imm{0u});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), result, publish};
}

std::unique_ptr<RegAlloc> Allocate(ScalarPublication& item, bool enabled = true) {
    const GPRSMask gprs{~((1u << 8) - 1u)};
    auto alloc = std::make_unique<RegAlloc>(
            item.block->MaxInstrId(), gprs, ResidentFPRs(), FeatureSet{});
    RegisterAllocTestSupport::RunForXmmResidentTest(
            item.block.get(), alloc.get(), enabled);
    return alloc;
}

swift::u32 EmitSize(Block* block, RegAlloc& alloc) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.MapModule(
            LocationDescriptor{0x8750}, LocationDescriptor{0x8790},
            ModuleConfig{});
    backend::arm64::JitContext context{module, alloc};
    backend::arm64::JitTranslator translator{context};
    translator.Translate(block);
    context.Finish();
    return context.CurrentBufferSize();
}

bool ScalarProductUpperDead(bool propagate_upper) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8790})};
    auto left = block->LoadUniform(Uniform{0, ValueType::V128})
                        .SetType(ValueType::V128);
    auto right = block->LoadUniform(Uniform{16, ValueType::V128})
                         .SetType(ValueType::V128);
    auto other = block->LoadUniform(Uniform{32, ValueType::V128})
                         .SetType(ValueType::V128);
    auto product = block->VecFMulScalar64(left, right).SetType(ValueType::V128);
    auto result = propagate_upper
            ? block->VecFAddScalar64(product, other).SetType(ValueType::V128)
            : block->VecFAddScalar64(other, product).SetType(ValueType::V128);
    block->StoreUniform(Uniform{48, ValueType::V128}, result);
    block->ReIdInstr();

    backend::arm64::ScalarFPRLiveness liveness;
    liveness.Analyze(block.get());
    return liveness.UpperDead(product.Def());
}

}  // namespace

TEST_CASE("legacy scalar FPR results publish directly to a resident home") {
    for (bool scalar32 : {false, true}) {
        CAPTURE(scalar32);
        auto item = MakeScalarPublication(scalar32, false);
        auto alloc = Allocate(item);
        REQUIRE(alloc->ValueFPR(item.result).id == kResidentTarget);
        REQUIRE(alloc->IsHostWriteCoalesced(item.publish->Id()));
    }
}

TEST_CASE("scalar right-only chains discard unobserved upper lanes") {
    REQUIRE(ScalarProductUpperDead(false));
    REQUIRE_FALSE(ScalarProductUpperDead(true));
}

TEST_CASE("legacy scalar FPR publication preserves a fixed left source") {
    for (bool scalar32 : {false, true}) {
        CAPTURE(scalar32);
        auto item = MakeScalarPublication(scalar32, true, false, true);
        auto alloc = Allocate(item);
        REQUIRE(alloc->ValueFPR(item.result).id != kResidentTarget);
        REQUIRE_FALSE(alloc->IsHostWriteCoalesced(item.publish->Id()));
    }
}

TEST_CASE("legacy scalar FPR results reuse a dead fixed left home") {
    for (bool scalar32 : {false, true}) {
        CAPTURE(scalar32);
        auto item = MakeScalarPublication(scalar32, true);
        auto baseline = Allocate(item, false);
        const auto baseline_size = EmitSize(item.block.get(), *baseline);
        auto alloc = Allocate(item);
        REQUIRE(alloc->ValueFPR(item.result).id == kResidentTarget);
        REQUIRE(alloc->IsHostWriteCoalesced(item.publish->Id()));
        const swift::u32 saved_instructions = scalar32 ? 2 : 1;
        REQUIRE(EmitSize(item.block.get(), *alloc) +
                saved_instructions * vixl::aarch64::kInstructionSize ==
                baseline_size);
    }
}

TEST_CASE("legacy scalar FPR publication stops before a fault observer") {
    auto item = MakeScalarPublication(false, false, true);
    auto alloc = Allocate(item);
    REQUIRE(alloc->ValueFPR(item.result).id != kResidentTarget);
    REQUIRE_FALSE(alloc->IsHostWriteCoalesced(item.publish->Id()));
}

TEST_CASE("resident FPR publication keeps a live result in its fixed home") {
    IntrusivePtr<Block> block{new Block(0, Location{0x8750})};
    auto left = block->LoadUniform(Uniform{0, ValueType::V128});
    auto right = block->LoadUniform(Uniform{16, ValueType::V128});
    auto result = block->VecXor(left, right).SetType(ValueType::V128);
    auto* publish = block->AppendInst(
            OpCode::SetHostFPR, result, HostRegIndex(kResidentTarget), Imm{0u});
    block->StoreUniform(Uniform{32, ValueType::V128}, result);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    ScalarPublication item{std::move(block), result, publish};
    auto baseline = Allocate(item, false);
    const auto baseline_size = EmitSize(item.block.get(), *baseline);
    auto alloc = Allocate(item);
    REQUIRE(alloc->ValueFPR(item.result).id == kResidentTarget);
    REQUIRE(alloc->IsHostWriteCoalesced(item.publish->Id()));
    REQUIRE(EmitSize(item.block.get(), *alloc) +
            vixl::aarch64::kInstructionSize == baseline_size);
}

TEST_CASE("resident FPR publication preserves a live result across a later fixed write") {
    IntrusivePtr<Block> block{new Block(0, Location{0x8760})};
    auto left = block->LoadUniform(Uniform{0, ValueType::V128});
    auto right = block->LoadUniform(Uniform{16, ValueType::V128});
    auto result = block->VecXor(left, right).SetType(ValueType::V128);
    auto replacement = block->LoadUniform(Uniform{32, ValueType::V128});
    auto* publish = block->AppendInst(
            OpCode::SetHostFPR, result, HostRegIndex(kResidentTarget), Imm{0u});
    block->SetHostFPR(replacement, HostRegIndex(kResidentTarget), Imm{0u});
    block->StoreUniform(Uniform{48, ValueType::V128}, result);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    ScalarPublication item{std::move(block), result, publish};
    auto alloc = Allocate(item);
    REQUIRE(alloc->ValueFPR(item.result).id != kResidentTarget);
    REQUIRE_FALSE(alloc->IsHostWriteCoalesced(item.publish->Id()));
}

TEST_CASE("one live FPR result occupies only one resident home") {
    constexpr swift::u32 other_target = 18;
    IntrusivePtr<Block> block{new Block(0, Location{0x8770})};
    auto left = block->LoadUniform(Uniform{0, ValueType::V128});
    auto right = block->LoadUniform(Uniform{16, ValueType::V128});
    auto result = block->VecXor(left, right).SetType(ValueType::V128);
    auto* first = block->AppendInst(
            OpCode::SetHostFPR, result, HostRegIndex(kResidentTarget), Imm{0u});
    auto* second = block->AppendInst(
            OpCode::SetHostFPR, result, HostRegIndex(other_target), Imm{0u});
    block->StoreUniform(Uniform{32, ValueType::V128}, result);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    auto resident_fprs = ResidentFPRs();
    resident_fprs.Mark(other_target);
    const GPRSMask gprs{~((1u << 8) - 1u)};
    RegAlloc alloc{
            block->MaxInstrId(), gprs, resident_fprs, FeatureSet{}};
    RegisterAllocTestSupport::RunForXmmResidentTest(block.get(), &alloc, true);

    REQUIRE(alloc.ValueFPR(result).id == kResidentTarget);
    REQUIRE(alloc.IsHostWriteCoalesced(first->Id()));
    REQUIRE_FALSE(alloc.IsHostWriteCoalesced(second->Id()));
    REQUIRE(EmitSize(block.get(), alloc) != 0);
}

TEST_CASE("scalar square root publishes through its resident merge home") {
    for (bool keep_merge_live : {false, true}) {
        CAPTURE(keep_merge_live);
        IntrusivePtr<Block> block{new Block(0, Location{0x8780})};
        auto source = block->LoadUniform(Uniform{0, ValueType::V128});
        auto merge = block->GetHostFPR(
                                  HostRegIndex(kResidentTarget), Imm{0u})
                             .SetType(ValueType::V128);
        auto result = block->VecFUnary(
                                   source, merge, Imm{64u}, Imm{0u}, Imm{1u})
                              .SetType(ValueType::V128);
        if (keep_merge_live) {
            block->StoreUniform(Uniform{16, ValueType::V128}, merge);
        }
        auto* publish = block->AppendInst(
                OpCode::SetHostFPR, result,
                HostRegIndex(kResidentTarget), Imm{0u});
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        const GPRSMask gprs{~((1u << 8) - 1u)};
        RegAlloc baseline{
                block->MaxInstrId(), gprs, ResidentFPRs(), FeatureSet{}};
        RegisterAllocTestSupport::RunForXmmResidentTest(
                block.get(), &baseline, false);
        const auto baseline_size = EmitSize(block.get(), baseline);
        RegAlloc alloc{
                block->MaxInstrId(), gprs, ResidentFPRs(), FeatureSet{}};
        RegisterAllocTestSupport::RunForXmmResidentTest(
                block.get(), &alloc, true);

        if (keep_merge_live) {
            REQUIRE(alloc.ValueFPR(result).id != kResidentTarget);
            REQUIRE_FALSE(alloc.IsHostWriteCoalesced(publish->Id()));
        } else {
            REQUIRE(alloc.ValueFPR(result).id == kResidentTarget);
            REQUIRE(alloc.IsHostReadCoalesced(merge.Id()));
            REQUIRE(alloc.IsHostWriteCoalesced(publish->Id()));
            REQUIRE(EmitSize(block.get(), alloc) +
                    2 * vixl::aarch64::kInstructionSize == baseline_size);
        }
    }
}
