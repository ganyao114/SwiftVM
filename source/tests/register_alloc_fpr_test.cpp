#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/compiler.h"
#include "support/register_alloc_test_support.h"

#include <array>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/fpr_preparation.h"
#include "runtime/backend/arm64/compile/lower_consumers.h"
#include "runtime/backend/arm64/compile/vector_preparation.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/arm64/pshufd_direct.h"
#include "runtime/backend/runtime.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/ir/hir_builder.h"

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
    const Inst* publish;
};

ScalarPublication MakeScalarPublication(bool scalar32, bool fixed_left,
                                        bool fault_observer = false,
                                        bool keep_left_live = false) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8730})};
    auto left = fixed_left
                        ? block->GetHostFPR(ValueType::V128, HostRegIndex(kResidentTarget), Imm{0u})

                        : block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
    auto right = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
    auto result = scalar32 ? block->VecFAddScalar32(ValueType::V128, left, right)
                           : block->VecFAddScalar64(ValueType::V128, left, right);
    if (keep_left_live) {
        block->StoreUniform(Uniform{32, ValueType::V128}, left);
    }
    if (fault_observer) {
        auto address = block->LoadUniform(ValueType::U64, Uniform{48, ValueType::U64});
        (void)block->LoadMemory(ValueType::U64, Operand{address});
    }
    auto* publish = block->AppendInst(
            OpCode::SetHostFPR, result, HostRegIndex(kResidentTarget), Imm{0u});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), result, publish};
}

std::unique_ptr<AllocationFixture> Allocate(ScalarPublication& item, bool enabled = true) {
    const GPRSMask gprs{~((1u << 8) - 1u)};
    auto alloc = std::make_unique<AllocationFixture>(
            item.block->MaxInstrId(), gprs, ResidentFPRs(), FeatureSet{});
    RegisterAllocTestSupport::RunForXmmResidentTest(
            item.block.get(), alloc.get(), enabled);
    return alloc;
}

swift::u32 EmitSize(Block* block, AllocationFixture& alloc) {
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
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    backend::arm64::JitContext context{module, alloc, alloc.Actions()};
    backend::arm64::JitTranslator translator{context};
    translator.Translate(block);
    context.Finish();
    return context.CurrentBufferSize();
}

void CheckScalarChain(bool scalar32, bool propagate_upper) {
    Config config{.loc_start = 0,
                  .loc_end = 0x10000,
                  .enable_jit = true,
                  .has_local_operation = false,
                  .backend_isa = kArm64};
    config.uniform_buffer_size = 64;
    config.stack_alignment = 16;
    AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x8790})};
    block->SetEndLocation(Location{0x8791});
    auto left = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
    auto right = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
    auto other = block->LoadUniform(ValueType::V128, Uniform{32, ValueType::V128});
    auto product = scalar32 ? block->VecFMulScalar32(ValueType::V128, left, right)
                            : block->VecFMulScalar64(ValueType::V128, left, right);
    const auto merge = propagate_upper ? product : other;
    const auto operand = propagate_upper ? other : product;
    auto result = scalar32 ? block->VecFAddScalar32(ValueType::V128, merge, operand)
                           : block->VecFAddScalar64(ValueType::V128, merge, operand);
    block->StoreUniform(Uniform{48, ValueType::V128}, result);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    const auto module = space.GetDefaultModule();
    REQUIRE(module->Push(block.get()));
    auto* entry = TranslateIR(module, block);
    REQUIRE(entry != nullptr);
    space.PushCodeCache(block->GetStartLocation(), entry);
    REQUIRE(space.GetCodeCache(block->GetStartLocation()) == entry);
#if defined(__aarch64__)
    // 3 * 2 + 4 = 10 in both precisions. Distinct inactive lanes identify
    // the left merge source independently of the selected allocation/action.
    const std::array<swift::u64, 2> left_bits{
            scalar32 ? 0xdeadbeef40400000ull : 0x4008000000000000ull, 0x1122334455667788ull};
    const std::array<swift::u64, 2> right_bits{
            scalar32 ? 0xc001d00d40000000ull : 0x4000000000000000ull, 0x8877665544332211ull};
    const std::array<swift::u64, 2> other_bits{
            scalar32 ? 0xfeedface40800000ull : 0x4010000000000000ull, 0x99aabbccddeeff00ull};
    Runtime runtime{&space};
    const auto uniform = runtime.GetUniformBuffer();
    std::memcpy(uniform.data(), left_bits.data(), sizeof(left_bits));
    std::memcpy(uniform.data() + 16, right_bits.data(), sizeof(right_bits));
    std::memcpy(uniform.data() + 32, other_bits.data(), sizeof(other_bits));
    runtime.SetLocation(block->GetStartLocation().Value());
    REQUIRE(runtime.Run() == HaltReason::CallHost);
    std::array<swift::u64, 2> actual{};
    std::memcpy(actual.data(), uniform.data() + 48, sizeof(actual));
    const std::array<swift::u64, 2> expected{
            scalar32 ? (propagate_upper ? 0xdeadbeef41200000ull : 0xfeedface41200000ull)
                     : 0x4024000000000000ull,
            propagate_upper ? left_bits[1] : other_bits[1]};
    CHECK(actual == expected);
#endif
}

}  // namespace

TEST_CASE("FPR contract preparation separates direct resolved and empty proof inputs",
          "[fpr-plan]") {
    for (unsigned scenario = 0; scenario < 19; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0x87b0})};
        const auto unrelated = block->LoadUniform(ValueType::U64, Uniform{80, ValueType::U64});
        (void)block->BitCast(ValueType::U32, unrelated);
        Value source, other, result, mask, owner, key, zero, aes_first;
        const Inst* publication{};
        FeatureSet features{};
        features.sse_scalar_tie = true;
        features.sse_shufps_imm = true;
        features.ra_aes_chain_tie = scenario >= 17;
        if (scenario == 0)
            block->StoreUniform(Uniform{88, ValueType::U64}, unrelated);
        else if (scenario <= 3) {
            source = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
            other = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
            if (scenario == 1)
                result = block->VecAnd(ValueType::V128, source, other);
            else if (scenario == 2)
                result = block->VecFAddScalar64(ValueType::V128, source, other);
            else
                result = block->VecShuffle32TwoSrc(ValueType::V128, source, source, Imm{0x4eu});
            block->StoreUniform(Uniform{32, ValueType::V128}, result);
        } else if (scenario <= 6) {
            source = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
            const auto [low, high] = backend::arm64::PshufdIndexMask(0x4e);
            mask = block->VecLoadConst(ValueType::V128, Imm{low}, Imm{high});
            result = block->VecShuffle32Indexed(ValueType::V128, source, mask);
            block->StoreUniform(Uniform{16, ValueType::V128}, result);
            if (scenario == 5) (void)block->VecTableLookup8(ValueType::V128, source, mask);
        } else if (scenario <= 9) {
            source = block->GetHostFPR(
                    scenario == 8 ? ValueType::U64 : ValueType::V128, HostRegIndex(17), Imm{0u});
            block->StoreUniform(Uniform{0, source.Type()}, source);
        } else if (scenario <= 13) {
            source = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
            if (scenario == 11 || scenario == 12)
                owner = block->LoadUniform(
                        scenario == 12 ? ValueType::U64 : ValueType::V128,
                        Uniform{16, scenario == 12 ? ValueType::U64 : ValueType::V128});
            block->SetHostFPR(source, HostRegIndex(17), Imm{0u});
            publication = &block->GetInstList().back();
            if (owner.Defined()) block->StoreUniform(Uniform{32, owner.Type()}, owner);
        } else {
            source = block->GetHostFPR(ValueType::V128, HostRegIndex(17), Imm{0u});
            other = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
            if (scenario <= 15) {
                result = block->VecFAddScalar64(ValueType::V128, source, other);
                if (scenario == 15) block->StoreUniform(Uniform{32, ValueType::V128}, source);
                block->StoreUniform(Uniform{48, ValueType::V128}, result);
            } else {
                key = other;
                zero = block->LoadUniform(ValueType::V128, Uniform{32, ValueType::V128});
                aes_first = block->VecAesEncFast(ValueType::V128, source, key, zero);
                if (scenario == 18) block->UniformBarrier();
                result = block->VecAesEncLastFast(ValueType::V128, aes_first, key, zero);
                block->SetHostFPR(result, HostRegIndex(17), Imm{0u});
                publication = &block->GetInstList().back();
            }
        }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, features};
        if (source.Defined())
            allocation.MapRegister(
                    source.Id(),
                    HostFPR{static_cast<swift::u16>(scenario == 9 || scenario == 13 ? 18 : 17)});
        if (other.Defined()) allocation.MapRegister(other.Id(), HostFPR{18});
        if (result.Defined())
            allocation.MapRegister(result.Id(),
                                   HostFPR{static_cast<swift::u16>(scenario <= 2 ? 19 : 17)});
        if (owner.Defined()) allocation.MapRegister(owner.Id(), HostFPR{17});
        if (zero.Defined()) allocation.MapRegister(zero.Id(), HostFPR{19});
        if (scenario >= 4 && scenario <= 6) {
            if (scenario != 6) allocation.MarkPshufdDirect(mask.Id());
            allocation.MarkPshufdDirect(result.Id());
        }
        if ((scenario >= 7 && scenario <= 9) || scenario >= 14)
            allocation.MarkHostReadCoalesced(source.Id());
        if (publication) allocation.MarkHostWriteCoalesced(publication->Id());
        if (aes_first.Defined()) {
            allocation.MapRegister(aes_first.Id(), HostFPR{17});
            allocation.MarkAesChainTied(aes_first.Id(), 17);
            allocation.MarkAesChainTied(result.Id(), 17);
        }
        auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
        REQUIRE_FALSE(index.UsesReady());
        REQUIRE_FALSE(index.ResolvedUsesReady());
        const auto prepare = [&] {
            return backend::arm64::PrepareFPRContract(
                    block.get(), allocation, features, true, true, index, allocation.Workspace());
        };
        if (scenario <= 3) {
            compile::LoweringCandidateScope held_candidates{allocation.Workspace()};
            held_candidates.Entries().push_back({9, 0xffff});
            compile::InstructionRowScope held_rows{allocation.Workspace(), 1};
            held_rows[0] = 23;
            const auto empty = prepare();
            REQUIRE(empty.valid);
            REQUIRE(empty.instructions.empty());
            REQUIRE(held_candidates.Entries().size() == 1);
            REQUIRE(held_candidates.Entries().front().roles == 0xffff);
            REQUIRE(held_rows.Find(0) == 23);
        }
        backend::arm64::FPRContract contract;
        if (scenario >= 4 && scenario <= 6) {
            // Direct shuffle certification never needs the recursive cache.
            compile::InstructionRowScope held{allocation.Workspace(), 1};
            held[0] = 37;
            contract = prepare();
            REQUIRE(held.Find(0) == 37);
        } else
            contract = prepare();
        auto& facts = allocation.PrepareFacts(block.get());
        const auto* shared_registers = facts.FindGuestStateRegisterAccesses(block.get());
        const auto shared_observations = facts.FindGuestStateObservations(block.get());
        const bool state_windows = scenario >= 10 && scenario != 14 && scenario != 15;
        REQUIRE((shared_registers != nullptr) == state_windows);
        REQUIRE(shared_observations.has_value() == state_windows);
        REQUIRE(index.UsesReady() == (scenario == 4 || scenario == 5 || scenario >= 7));
        REQUIRE(index.ResolvedUsesReady() == (scenario >= 7));
        const bool valid = scenario <= 4 || scenario == 7 || scenario == 10 || scenario == 14 ||
                           scenario == 17;
        REQUIRE(contract.valid == valid);
        if (scenario == 4) {
            REQUIRE(contract.Control(mask.Id()) == std::optional<swift::u8>{0x4e});
            REQUIRE(contract.Control(result.Id()) == std::optional<swift::u8>{0x4e});
        }
        if (scenario == 5) REQUIRE(contract.rejected == mask.Def());
        if (scenario == 6 || scenario == 15) REQUIRE(contract.rejected == result.Def());
        if (scenario == 8 || scenario == 9) REQUIRE(contract.rejected == source.Def());
        if (scenario == 11 || scenario == 12 || scenario == 13 || scenario == 18)
            REQUIRE(contract.rejected == publication);
        if (scenario == 16) REQUIRE(contract.rejected == aes_first.Def());
        if (scenario == 14)
            REQUIRE(contract.Has(result.Id(), backend::arm64::FPRAction::ScalarTie));
        if (scenario == 17) {
            REQUIRE(contract.Has(aes_first.Id(), backend::arm64::FPRAction::AesTie));
            REQUIRE(contract.Has(result.Id(), backend::arm64::FPRAction::AesTie));
            REQUIRE(contract.Has(publication->Id(), backend::arm64::FPRAction::Write));
        }
        const auto repeated = prepare();
        REQUIRE(repeated.valid == contract.valid);
        REQUIRE(repeated.rejected == contract.rejected);
        REQUIRE(repeated.instructions == contract.instructions);
        REQUIRE(facts.FindGuestStateRegisterAccesses(block.get()) == shared_registers);
        if (shared_observations)
            REQUIRE(facts.FindGuestStateObservations(block.get())->data() == shared_observations->data());
    }
}

TEST_CASE("FPR proof cache keeps recursive components independent of ID gaps and later consumers",
          "[fpr-plan]") {
    for (bool cycle : {false, true}) {
        std::vector<std::array<swift::u32, 3>> dense_records;
        for (bool sparse : {false, true}) {
            CAPTURE(cycle, sparse);
            IntrusivePtr<Block> block{new Block(0, Location{0x87c0})};
            const auto left =
                    cycle ? block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128})
                          : block->GetHostFPR(ValueType::V128, HostRegIndex(17), Imm{0u});
            const auto right = block->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
            const Inst* initial_write{};
            if (cycle) {
                block->SetHostFPR(left, HostRegIndex(17), Imm{0u});
                initial_write = &block->GetInstList().back();
            }
            const auto first = block->VecFAddScalar64(ValueType::V128, left, right);
            block->SetHostFPR(first, HostRegIndex(17), Imm{0u});
            const auto* first_write = &block->GetInstList().back();
            const auto second = block->VecFMulScalar64(ValueType::V128, first, right);
            block->SetHostFPR(second, HostRegIndex(17), Imm{0u});
            const auto* second_write = &block->GetInstList().back();
            block->StoreUniform(Uniform{32, ValueType::V128}, second);
            const auto condition = block->CondSet(ValueType::U8, Cond::EQ);
            const auto branch = block->Goto(condition);
            block->BindLabel(branch);
            const auto resident = block->GetHostFPR(ValueType::U64, HostRegIndex(18), Imm{0u});
            block->StoreMemory(Operand{Imm{swift::u64{0x1000}}}, resident);
            const auto* store = &block->GetInstList().back();
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            if (sparse) {
                const auto step = (UINT16_MAX - 1) / (block->GetInstList().size() - 1);
                swift::u32 position{};
                compile::CompileWorkspace numbering_workspace;
                compile::RewriteContext numbering{block.get(), numbering_workspace, nullptr, false};
                for (const auto& inst : block->GetInstList())
                    numbering.Change(block.get(), &inst, [&](Inst& target) {
                        target.SetId(static_cast<swift::u16>(position++ * step));
                    });
            }
            FeatureSet features{};
            features.sse_scalar_tie = true;
            AllocationFixture allocation{sparse ? swift::u32{UINT16_MAX} + 1 : block->MaxInstrId(),
                                         GPRSMask{},
                                         FPRSMask{},
                                         features};
            for (const auto value : {left, first, second})
                allocation.MapRegister(value.Id(), HostFPR{17});
            allocation.MapRegister(right.Id(), HostFPR{18});
            if (!cycle) allocation.MarkHostReadCoalesced(left.Id());
            if (cycle) allocation.MarkHostWriteCoalesced(initial_write->Id());
            allocation.MarkHostWriteCoalesced(first_write->Id());
            allocation.MarkHostWriteCoalesced(second_write->Id());
            compile::BlockAnalysisIndex index;
            index.Build(block.get());
            REQUIRE(index.DenseInstructionIDs() == !sparse);
            const auto prepare = [&] {
                return backend::arm64::PrepareFPRContract(block.get(),
                                                          allocation,
                                                          features,
                                                          true,
                                                          true,
                                                          index,
                                                          allocation.Workspace());
            };
            const auto contract = prepare();
            auto& facts = allocation.PrepareFacts(block.get());
            const auto* shared_registers = facts.FindGuestStateRegisterAccesses(block.get());
            const auto shared_observations = facts.FindGuestStateObservations(block.get());
            REQUIRE(contract.valid == !cycle);
            if (cycle)
                REQUIRE(contract.rejected == initial_write);
            else
                REQUIRE(contract.instructions.size() == 5);
            std::vector<std::array<swift::u32, 3>> records;
            for (const auto row : contract.instructions) {
                const auto position = index.InstructionPosition(index.FindInstruction(row.id));
                REQUIRE(position);
                records.push_back({static_cast<swift::u32>(*position),
                                   static_cast<swift::u32>(row.actions),
                                   row.control});
            }
            if (!sparse)
                dense_records = records;
            else
                REQUIRE(records == dense_records);
            auto terminal_node = block->GetTerminal();
            const auto consumers = backend::arm64::LowerConsumers(
                    index,
                    allocation.Workspace(),
                    [](const Inst&) { return true; },
                    [](const Inst&) { return false; },
                    &terminal_node);
            REQUIRE(consumers.valid);
            REQUIRE(consumers.Has(condition.Id(), backend::arm64::ConsumerFlag::LocalCondition));
            const auto vector = backend::arm64::PrepareVector(
                    block.get(), allocation, index, allocation.Workspace());
            REQUIRE(vector.resident.Find(store->Id()));
            const auto repeated = prepare();
            REQUIRE(repeated.valid == contract.valid);
            REQUIRE(repeated.rejected == contract.rejected);
            REQUIRE(repeated.instructions == contract.instructions);
            REQUIRE(facts.FindGuestStateRegisterAccesses(block.get()) == shared_registers);
            if (shared_observations)
                REQUIRE(facts.FindGuestStateObservations(block.get())->data() == shared_observations->data());
        }
    }
}

TEST_CASE("legacy scalar FPR results publish directly to a resident home") {
    for (bool scalar32 : {false, true}) {
        CAPTURE(scalar32);
        auto item = MakeScalarPublication(scalar32, false);
        auto alloc = Allocate(item);
        REQUIRE(alloc->ValueFPR(item.result).id == kResidentTarget);
        REQUIRE(alloc->IsHostWriteCoalesced(item.publish->Id()));
    }
}

TEST_CASE("scalar chains preserve inactive lanes from the left merge source") {
    for (bool scalar32 : {false, true})
        for (bool propagate_upper : {false, true}) {
            CAPTURE(scalar32, propagate_upper);
            CheckScalarChain(scalar32, propagate_upper);
        }
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

TEST_CASE("FPR plan rejects a live old value before emission", "[fpr-plan]") {
    auto item = MakeScalarPublication(false, true, false, true);
    auto alloc = Allocate(item);
    // Simulate an invalid allocator candidate, rather than relying on the
    // allocator itself to catch the ownership violation.
    alloc->MapRegister(item.result.Id(), HostFPR{kResidentTarget});
    alloc->MarkHostWriteCoalesced(item.publish->Id());
    const auto plan = backend::arm64::PrepareFPRContract(
            item.block.get(),
            *alloc,
            FeatureSet{},
            false,
            false,
            alloc->PrepareFacts(item.block.get()).PrepareUses(item.block.get()),
            alloc->Workspace());
    REQUIRE_FALSE(plan.valid);
    REQUIRE(plan.rejected == item.publish);
}

TEST_CASE("rejected FPR candidates reallocate before freezing", "[fpr-plan]") {
    for (unsigned mode : {0u, 1u, 2u}) {
        CAPTURE(mode);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        AddressSpace space{config};
        FeatureSet features{};
        HIRBuilder builder{1, true, features};
        IntrusivePtr<Block> standalone;
        HIRFunction* function = nullptr;
        Block* block;
        Value result;
        const Inst* publication;
        const auto populate = [&](auto* ir) {
            const auto left =
                    ir->GetHostFPR(ValueType::V128, HostRegIndex(kResidentTarget), Imm{0u});
            const auto right = ir->LoadUniform(ValueType::V128, Uniform{16, ValueType::V128});
            result = ir->VecFAddScalar64(ValueType::V128, left, right);
            ir->StoreUniform(Uniform{32, ValueType::V128}, left);
            ir->SetHostFPR(result, HostRegIndex(kResidentTarget), Imm{0u});
        };
        if (!mode) {
            standalone.reset(new Block(0, Location{0x8730}));
            block = standalone.get();
            populate(block);
            publication = &block->GetInstList().back();
            block->SetTerminal(terminal::ReturnToDispatch{});
            block->ReIdInstr();
        } else {
            function = builder.AppendFunction(Location{0x8730}, Location{0x8740});
            populate(function);
            function->EndBlock(terminal::ReturnToDispatch{});
            function->EndFunction();
            function->ComputeRPO();
            function->IdByRPO();
            block = function->EditBlock(&function->GetHIRBlocksRPO().front()).GetBlock();
            for (auto& inst : block->GetInstList())
                if (inst.GetOp() == OpCode::SetHostFPR) publication = &inst;
        }
        const GPRSMask gprs{~((1u << 8) - 1u)};
        AllocationFixture alloc{mode ? function->MaxInstrCount() : block->MaxInstrId(),
                                gprs,
                                ResidentFPRs(),
                                features};
        std::thread allocate([&] {
            if (!mode)
                compile::AllocateBlock(block,
                                       &alloc,
                                       ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                                               block, alloc, features),
                                       false);
            else
                compile::AllocateFunction(function,
                                          &alloc,
                                          ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                                                  function, alloc, features),
                                          {.scalar_insert = false, .block_intervals = mode == 1});
        });
        allocate.join();
        const auto* uses = &alloc.PrepareFacts().PrepareUses(block);
        std::vector<std::pair<const Inst*, swift::u32>> numbered;
        for (const auto& inst : block->GetInstList()) numbered.emplace_back(&inst, inst.Id());
        alloc.MapRegister(result.Id(), HostFPR{kResidentTarget});
        alloc.MarkHostWriteCoalesced(publication->Id());
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(space.GetDefaultModule(), alloc);
        REQUIRE(alloc.FPRCoalescingDisabled());
        REQUIRE(alloc.FPRContractRetryCount() == 1);
        REQUIRE_FALSE(alloc.IsHostWriteCoalesced(publication->Id()));
        REQUIRE(alloc.ValueFPR(result).id != kResidentTarget);
        REQUIRE(alloc.GPRLocationsFrozen());
        REQUIRE(&alloc.PrepareFacts().PrepareUses(block) == uses);
        for (const auto& [inst, id] : numbered) REQUIRE(inst->Id() == id);
        REQUIRE_FALSE(alloc.Actions().FPRVerified(publication->Id(), arm64::FPRAction::Write));
        arm64::JitContext context{space.GetDefaultModule(), alloc, alloc.Actions()};
        arm64::JitTranslator translator{context};
        const bool translated = function ? translator.Translate(function) : translator.Translate(block);
        REQUIRE(translated);
        context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
    }
}

TEST_CASE("FPR retry policy survives discarding a physical attempt", "[fpr-plan]") {
    AllocationFixture alloc{1, GPRSMask{}, FPRSMask{}, FeatureSet{}};
    alloc.BeginTargetPlanning();
    alloc.DisableFPRCoalescing();
    REQUIRE(alloc.FPRCoalescingDisabled());
    alloc.AbortTargetPlanning();
    REQUIRE(alloc.FPRCoalescingDisabled());
}

TEST_CASE("mixed shuffle masks cancel direct lowering before emission", "[fpr-plan][shuffle-plan]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x87a0})};
    auto source = block->LoadUniform(ValueType::V128, Uniform{0, ValueType::V128});
    auto mask = block->VecLoadConst(ValueType::V128,
                                    Imm{swift::u64{0x0706050403020100}},
                                    Imm{swift::u64{0x0f0e0d0c0b0a0908}});
    auto result = block->VecShuffle32Indexed(ValueType::V128, source, mask);
    block->StoreUniform(Uniform{32, ValueType::V128}, result);
    block->StoreUniform(Uniform{64, ValueType::V128}, mask);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    AllocationFixture alloc{block->MaxInstrId(),
                            space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(),
                            FeatureSet{}};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), alloc, FeatureSet{}),
            false);
    alloc.MarkPshufdDirect(mask.Id()); alloc.MarkPshufdDirect(result.Id());
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(space.GetDefaultModule(), alloc);
    backend::arm64::JitContext context{space.GetDefaultModule(), alloc, alloc.Actions()};
    REQUIRE(context.CurrentBufferSize() == 0); REQUIRE(alloc.FPRCoalescingDisabled());
    REQUIRE_FALSE(alloc.IsPshufdDirect(mask.Id())); REQUIRE_FALSE(alloc.IsPshufdDirect(result.Id()));
    backend::arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);
}

TEST_CASE("resident FPR publication keeps a live result in its fixed home") {
    for (const auto opcode : {OpCode::VecXor, OpCode::VecAndNot, OpCode::LoadMemoryTSO}) {
        CAPTURE(opcode);
        IntrusivePtr<Block> block{new Block(0, Location{0x8750})};
        auto left = block->LoadUniform(Uniform{0, ValueType::V128});
        auto right = block->LoadUniform(Uniform{16, ValueType::V128});
        const auto result = [&] {
            if (opcode == OpCode::LoadMemoryTSO) {
                auto address = block->LoadUniform(Uniform{64, ValueType::U64});
                return block->LoadMemoryTSO(ValueType::V128, Operand{address});
            }
            return opcode == OpCode::VecAndNot
                    ? block->VecAndNot(ValueType::V128, left, right)
                    : block->VecXor(ValueType::V128, left, right);
        }();
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
}

TEST_CASE("resident FPR publication preserves a live result across a later fixed write") {
    for (const auto opcode : {OpCode::VecXor, OpCode::VecAndNot, OpCode::LoadMemoryTSO}) {
        CAPTURE(opcode);
        IntrusivePtr<Block> block{new Block(0, Location{0x8760})};
        auto left = block->LoadUniform(Uniform{0, ValueType::V128});
        auto right = block->LoadUniform(Uniform{16, ValueType::V128});
        const auto result = [&] {
            if (opcode == OpCode::LoadMemoryTSO) {
                auto address = block->LoadUniform(Uniform{64, ValueType::U64});
                return block->LoadMemoryTSO(ValueType::V128, Operand{address});
            }
            return opcode == OpCode::VecAndNot
                    ? block->VecAndNot(ValueType::V128, left, right)
                    : block->VecXor(ValueType::V128, left, right);
        }();
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
}

TEST_CASE("vector publication admission retains old-home and fault barriers", "[fpr-plan]") {
    for (const auto opcode : {OpCode::VecAndNot, OpCode::LoadMemoryTSO}) {
        for (unsigned scenario = 0; scenario < (opcode == OpCode::VecAndNot ? 3u : 2u); ++scenario) {
            CAPTURE(opcode, scenario);
            IntrusivePtr<Block> block{new Block(0, Location{0x8768})};
            auto left = block->LoadUniform(Uniform{0, ValueType::V128});
            auto right = block->LoadUniform(Uniform{16, ValueType::V128});
            auto address = block->LoadUniform(Uniform{64, ValueType::U64});
            auto result = opcode == OpCode::LoadMemoryTSO
                    ? block->LoadMemoryTSO(ValueType::V128, Operand{address})
                    : block->VecAndNot(ValueType::V128, left, right);
            Value observed;
            if (scenario == 0)
                observed = block->GetHostFPR(
                        ValueType::V128, HostRegIndex(kResidentTarget), Imm{0u});
            else if (scenario == 1)
                (void)block->LoadMemoryTSO(ValueType::U64, Operand{address});
            auto* publish = block->AppendInst(
                    OpCode::SetHostFPR, result, HostRegIndex(kResidentTarget), Imm{0u});
            block->StoreUniform(Uniform{32, ValueType::V128},
                                scenario == 0 ? observed : scenario == 1 ? result : right);
            block->SetTerminal(terminal::ReturnToDispatch{});
            block->ReIdInstr();

            ScalarPublication item{std::move(block), result, publish};
            auto alloc = Allocate(item);
            if (scenario < 2) {
                REQUIRE_FALSE(alloc->IsHostWriteCoalesced(publish->Id()));
                REQUIRE(alloc->ValueFPR(result).id != kResidentTarget);
            } else {
                // BIC permits dst==right, but not while that old right-hand
                // value remains live after the publication.
                alloc->MapRegister(right.Id(), HostFPR{kResidentTarget});
            }

            // Force precisely the forbidden candidate too: final physical
            // certification must reject it independently of allocation.
            alloc->MapRegister(result.Id(), HostFPR{kResidentTarget});
            alloc->MarkHostWriteCoalesced(publish->Id());
            const auto proof = backend::arm64::PrepareFPRContract(
                    item.block.get(), *alloc, FeatureSet{}, false, false,
                    alloc->PrepareFacts(item.block.get()).PrepareIndex(item.block.get()),
                    alloc->Workspace());
            REQUIRE_FALSE(proof.valid);
            REQUIRE(proof.rejected == publish);
        }
    }
}

TEST_CASE("one live FPR result occupies only one resident home") {
    constexpr swift::u32 other_target = 18;
    IntrusivePtr<Block> block{new Block(0, Location{0x8770})};
    auto left = block->LoadUniform(Uniform{0, ValueType::V128});
    auto right = block->LoadUniform(Uniform{16, ValueType::V128});
    auto result = block->VecXor(ValueType::V128, left, right);
    auto* first =
            block->AppendInst(OpCode::SetHostFPR, result, HostRegIndex(kResidentTarget), Imm{0u});
    auto* second = block->AppendInst(
            OpCode::SetHostFPR, result, HostRegIndex(other_target), Imm{0u});
    block->StoreUniform(Uniform{32, ValueType::V128}, result);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    auto resident_fprs = ResidentFPRs();
    resident_fprs.Mark(other_target);
    const GPRSMask gprs{~((1u << 8) - 1u)};
    AllocationFixture alloc{block->MaxInstrId(), gprs, resident_fprs, FeatureSet{}};
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
        auto merge = block->GetHostFPR(ValueType::V128, HostRegIndex(kResidentTarget), Imm{0u});
        auto result = block->VecFUnary(ValueType::V128, source, merge, Imm{64u}, Imm{0u}, Imm{1u});
        if (keep_merge_live) {
            block->StoreUniform(Uniform{16, ValueType::V128}, merge);
        }
        auto* publish = block->AppendInst(
                OpCode::SetHostFPR, result,
                HostRegIndex(kResidentTarget), Imm{0u});
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        const GPRSMask gprs{~((1u << 8) - 1u)};
        AllocationFixture baseline{block->MaxInstrId(), gprs, ResidentFPRs(), FeatureSet{}};
        RegisterAllocTestSupport::RunForXmmResidentTest(
                block.get(), &baseline, false);
        const auto baseline_size = EmitSize(block.get(), baseline);
        AllocationFixture alloc{block->MaxInstrId(), gprs, ResidentFPRs(), FeatureSet{}};
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
