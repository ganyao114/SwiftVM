#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/workspace.h"
using swift::runtime::compile::BlockAnalysisIndex;
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/lower_consumers.h"
#include "runtime/backend/arm64/jit/function_code_object_emitter.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/context.h"
#include "runtime/common/svm_config.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/ir/hir_builder.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::ir;
using namespace swift::runtime::backend;
using namespace swift::runtime::backend::arm64;

namespace {
ConsumerActions AuditPlan(Block& block, bool enabled = true) {
    block.ReIdInstr();
    BlockAnalysisIndex index; index.Build(&block);
    compile::CompileWorkspace workspace;
    return LowerConsumers(
            index,
            workspace,
            [](const Inst&) { return true; },
            [](const Inst&) { return false; },
            nullptr,
            enabled);
}

class AuditEnvironment {
public:
    AuditEnvironment() {
        for (size_t i = 0; i < names.size(); ++i)
            if (const auto* value = GetRawSvmConfigEnvForTest(names[i])) saved[i] = value;
    }
    ~AuditEnvironment() {
        for (size_t i = 0; i < names.size(); ++i) {
            if (saved[i]) SetSvmConfigEnvForTest(names[i], saved[i]->c_str(), 1);
            else UnsetSvmConfigEnvForTest(names[i]);
        }
    }
    void Set(bool enabled) {
        Set(enabled, enabled);
    }
    void Set(bool density, bool details) {
        REQUIRE(SetSvmConfigEnvForTest(names[0], density ? "1" : "0", 1) == 0);
        REQUIRE(SetSvmConfigEnvForTest(names[1], details ? "1" : "0", 1) == 0);
    }
private:
    static constexpr std::array<const char*, 2> names{{"SVM_DENSITY_PROF", "SVM_RA_HOT_COALESCE_ALL"}};
    std::array<std::optional<std::string>, 2> saved;
};
}

TEST_CASE("ALU audit facts are sparse and absent when diagnostics are disabled", "[alu-split-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbaa0})};
    auto input = block->LoadImm(ValueType::U64, Imm{u64{45}});
    const std::array<Value, 4> roots{{block->Sub(ValueType::U64, input, ir::Operand{Imm{u64{7}}}),
                                      block->And(ValueType::U64, input, ir::Operand{Imm{u64{7}}}),
                                      block->Or(ValueType::U64, input, ir::Operand{Imm{u64{7}}}),
                                      block->Xor(ValueType::U64, input, ir::Operand{Imm{u64{7}}})}};
    for (u16 i = 0; i < roots.size(); ++i)
        block->StoreUniform(Uniform{u16(i * 8), ValueType::U64}, roots[i]);
    auto disabled = AuditPlan(*block, false);
    REQUIRE(disabled.audit.empty());
    auto plan = AuditPlan(*block);
    REQUIRE(plan.valid);
    REQUIRE(plan.audit.size() == roots.size());
    REQUIRE_FALSE((plan.Audit(input.Id()) != nullptr));
    for (auto root : roots) {
        const auto& facts = (*plan.Audit(root.Id()));
        REQUIRE(facts.value_uses == 1); REQUIRE(facts.external_users == 1);
        REQUIRE(facts.family_count == 1); REQUIRE_FALSE(facts.family_capped);
        REQUIRE(facts.saw_value); REQUIRE_FALSE(facts.saw_address);
    }
}

TEST_CASE("ALU audit retains width bridge families and the thirty two member cap",
          "[alu-split-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbab0})};
    auto input = block->LoadImm(ValueType::U64, Imm{u64{64}});
    auto root = block->And(ValueType::U64, input, ir::Operand{Imm{u64{255}}});
    auto alias = root;
    for (unsigned i = 0; i < 35; ++i) alias = block->BitCast(ValueType::U64, alias);
    block->StoreUniform(Uniform{0, ValueType::U64}, alias);
    const auto capped = (*AuditPlan(*block).Audit(root.Id()));
    REQUIRE(capped.family_count == 32); REQUIRE(capped.family_capped);
    REQUIRE(capped.value_uses == 1); REQUIRE(capped.external_users == 1);
    REQUIRE(capped.saw_value); REQUIRE_FALSE(capped.saw_address);

    IntrusivePtr<Block> address{new Block(0, Location{0xbab8})};
    auto base = address->LoadImm(ValueType::U64, Imm{u64{64}});
    auto producer = address->Xor(ValueType::U64, base, ir::Operand{Imm{u64{0}}});
    auto narrow = address->BitExtract(ValueType::U8, producer, Imm{u8{0}}, Imm{u8{8}});
    auto wide = address->ZeroExtend64(ValueType::U64, narrow);
    address->LoadMemory(ValueType::U64, ir::Operand{wide});
    const auto facts = (*AuditPlan(*address).Audit(producer.Id()));
    REQUIRE(facts.family_count == 3); REQUIRE_FALSE(facts.family_capped);
    REQUIRE(facts.value_uses == 1); REQUIRE(facts.external_users == 1);
    REQUIRE(facts.saw_address); REQUIRE_FALSE(facts.saw_value);
}

TEST_CASE("ALU audit uses the first matching family member for store roles", "[alu-split-plan]") {
    for (bool root_is_data : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbac0})};
        auto input = block->LoadImm(ValueType::U64, Imm{u64{64}});
        auto root = block->Or(ValueType::U64, input, ir::Operand{Imm{u64{0}}});
        auto alias = block->BitCast(ValueType::U64, root);
        block->StoreMemory(ir::Operand{root_is_data ? alias : root}, root_is_data ? root : alias);
        const auto facts = (*AuditPlan(*block).Audit(root.Id()));
        REQUIRE(facts.family_count == 2); REQUIRE(facts.external_users == 1);
        REQUIRE(facts.value_uses == 2);
        REQUIRE(facts.saw_value == root_is_data); REQUIRE(facts.saw_address == !root_is_data);
    }
}

TEST_CASE("ALU audit separates operand multiplicity from flags and distinct users", "[alu-split-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbad0})};
    auto input = block->LoadImm(ValueType::U64, Imm{u64{45}});
    auto root = block->Sub(ValueType::U64, input, ir::Operand{Imm{u64{7}}});
    block->Add(ValueType::U64, root, ir::Operand{root});
    auto flags = block->And(ValueType::U64, input, ir::Operand{Imm{u64{7}}});
    block->SaveFlags(flags, Flags::NZ);
    auto plan = AuditPlan(*block);
    REQUIRE((*plan.Audit(root.Id())).value_uses == 2);
    REQUIRE((*plan.Audit(root.Id())).external_users == 1);
    REQUIRE((*plan.Audit(flags.Id())).value_uses == 0);
    REQUIRE((*plan.Audit(flags.Id())).external_users == 1);
    REQUIRE((*plan.Audit(flags.Id())).saw_value);
}

TEST_CASE("ALU audit admission rejects config and IR changes before bytes", "[alu-split-plan][alu-split-admission]") {
#if defined(__aarch64__)
    AuditEnvironment environment; environment.Set(false);
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    auto features = ResolveFeatureSet(module->GetModuleConfig());
    IntrusivePtr<Block> block{new Block(0, Location{0xbae0})};
    auto input = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    auto result = block->Sub(ValueType::U64, input, ir::Operand{Imm{u64{7}}});
    block->StoreUniform(Uniform{8, ValueType::U64}, result);
    auto* store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(CompleteARM64UnitForEmission(module, allocation, block.get(), nullptr));
    const auto* actions = allocation.Actions().ForBlock(block.get());
    REQUIRE_FALSE(allocation.Actions().AluSplit(result.Id()));
    JitContext old_context{module, allocation, allocation.Actions()};
    JitTranslator old{old_context};
    REQUIRE(old_context.RequireBlockForEmission(block.get()));
    environment.Set(true, false);
    JitContext split_context{module, allocation, allocation.Actions()};
    JitTranslator split{split_context};
    REQUIRE(split_context.RequireBlockForEmission(block.get()));
    environment.Set(false, true);
    REQUIRE_FALSE(split_context.RequireBlockForEmission(block.get()));
    REQUIRE_FALSE(split.Translate(block.get())); REQUIRE(split_context.CurrentBufferSize() == 0);
    environment.Set(true);
    REQUIRE_FALSE(ARM64UnitReadyForEmission(
            module, allocation, allocation.Actions(), block.get(), nullptr));
    REQUIRE_FALSE(old_context.RequireBlockForEmission(block.get()));
    REQUIRE_FALSE(old.Translate(block.get())); REQUIRE(old_context.CurrentBufferSize() == 0);
    JitContext new_context{module, allocation, allocation.Actions()};
    JitTranslator fresh{new_context};
    REQUIRE_FALSE(fresh.Translate(block.get())); REQUIRE(new_context.CurrentBufferSize() == 0);
    environment.Set(false);
    REQUIRE(ARM64UnitReadyForEmission(
            module, allocation, allocation.Actions(), block.get(), nullptr));
    REQUIRE(old.Translate(block.get())); REQUIRE(old_context.CurrentBufferSize() > 0);
#else
    SUCCEED("native ARM admission requires AArch64");
#endif
}

TEST_CASE("ALU diagnostics preserve generated bytes and native results", "[alu-split-plan][alu-split-native]") {
#if defined(__aarch64__)
    AuditEnvironment environment;
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 32;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    for (unsigned op = 0; op < 4; ++op) for (bool flags : {false, true}) {
        std::vector<u8> before;
        for (bool audit : {false, true}) {
            environment.Set(audit);
            IntrusivePtr<Block> block{new Block(0, Location{0xbaf0})};
            auto input = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
            Value result;
            if (op == 0)
                result = block->Sub(ValueType::U64, input, ir::Operand{Imm{u64{7}}});
            else if (op == 1)
                result = block->And(ValueType::U64, input, ir::Operand{Imm{u64{7}}});
            else if (op == 2)
                result = block->Or(ValueType::U64, input, ir::Operand{Imm{u64{7}}});
            else
                result = block->Xor(ValueType::U64, input, ir::Operand{Imm{u64{7}}});
            if (flags) block->SaveFlags(result, Flags::NZ);
            block->StoreUniform(Uniform{16, ValueType::U64}, result);
            block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
            AllocationFixture allocation{block->MaxInstrId(),
                                         space.GetTrampolines().GetGPRRegs(),
                                         space.GetTrampolines().GetFPRRegs(),
                                         features};
            ::swift::runtime::compile::AllocateBlock(
                    block.get(),
                    &allocation,
                    ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                            block.get(), allocation, features),
                    false);
            REQUIRE(CompleteARM64UnitForEmission(module, allocation, block.get(), nullptr));
            const auto* actions = allocation.Actions().ForBlock(block.get());
            REQUIRE(allocation.Actions().Certificate()->options.alu_split_audit == audit);
            if (audit) REQUIRE(allocation.Actions().AluSplit(result.Id()));
            JitContext context{module, allocation, allocation.Actions()};
            JitTranslator translator{context};
            REQUIRE(translator.Translate(block.get())); context.Finish();
            const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
            const std::vector<u8> code(bytes, bytes + context.CurrentBufferSize());
            if (!audit) before = code;
            else REQUIRE(code == before);
            const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
            REQUIRE(id != INVALID_CACHE_ID); context.Flush(buffer);
            space.PushCodeCache(Location{0xbaf0}, buffer.exec_data);
            Runtime runtime{&space}; const u64 value = 45;
            std::memcpy(runtime.GetUniformBuffer().data(), &value, sizeof(value));
            runtime.SetLocation(0xbaf0); REQUIRE(runtime.Run() == HaltReason::CallHost);
            u64 output{}; std::memcpy(&output, runtime.GetUniformBuffer().data() + 16, sizeof(output));
            const u64 expected = op == 0 ? value - 7 : op == 1 ? value & 7 : op == 2 ? value | 7 : value ^ 7;
            REQUIRE(output == expected);
        }
    }
#else
    SUCCEED("native ARM execution requires AArch64");
#endif
}

TEST_CASE("ALU audit facts remain block local across region preflight and retry", "[alu-split-plan][alu-split-admission]") {
#if defined(__aarch64__)
    AuditEnvironment environment;
    environment.Set(true);
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{2, true, features};
    auto* first = builder.AppendFunction(Location{0xbb00}, Location{0xbb10});
    auto a = first->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    auto first_root = first->Sub(ValueType::U64, a, ir::Operand{Imm{u64{7}}});
    first->StoreUniform(Uniform{8, ValueType::U64}, first_root);
    first->EndBlock(terminal::ReturnToHost{}); first->EndFunction(); first->ComputeRPO(); first->IdByRPO();
    auto* second = builder.AppendFunction(Location{0xbb20}, Location{0xbb40});
    second->AdvancePC(Imm{u64{1}});
    auto* later = builder.LinkBlock(terminal::LinkBlock{Location{0xbb30}});
    builder.SetCurBlock(later);
    auto b = second->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    auto late_root = second->Or(ValueType::U64, b, ir::Operand{Imm{u64{7}}});
    second->StoreUniform(Uniform{8, ValueType::U64}, late_root);
    second->EndBlock(terminal::ReturnToHost{}); second->EndFunction(); second->ComputeRPO(); second->IdByRPO();
    auto first_prepared = PrepareFunction(module, first);
    REQUIRE(first_prepared.has_value());
    environment.Set(false);
    auto second_prepared = PrepareFunction(module, second);
    REQUIRE(second_prepared.has_value());
    environment.Set(true);
    std::vector<Arm64PreparedUnit> prepared;
    prepared.push_back(std::move(*first_prepared));
    prepared.push_back(std::move(*second_prepared));
    {
        FunctionCodeObjectEmitter rejected{module, prepared};
        REQUIRE_FALSE(rejected.Emit());
        REQUIRE(rejected.CurrentBufferSize() == 0);
    }
    prepared.pop_back();
    auto retry = PrepareFunction(module, second);
    REQUIRE(retry.has_value());
    prepared.push_back(std::move(*retry));
    REQUIRE(first_root.Id() != late_root.Id());
    REQUIRE(prepared[0].Actions().AluSplit(first_root.Id()));
    REQUIRE_FALSE(prepared[0].Actions().AluSplit(late_root.Id()));
    REQUIRE(prepared[1].Actions().AluSplit(late_root.Id()));
    REQUIRE_FALSE(prepared[1].Actions().AluSplit(first_root.Id()));
    FunctionCodeObjectEmitter emitter{module, prepared};
    REQUIRE(emitter.Emit()); REQUIRE(emitter.CurrentBufferSize() > 0);
#else
    SUCCEED("native ARM region admission requires AArch64");
#endif
}
