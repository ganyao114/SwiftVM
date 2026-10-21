#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/alloc/consumer_plan.h"
#include "runtime/backend/arm64/alloc/target_allocation_planner.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/arm64/jit/function_code_object_emitter.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "runtime/ir/hir_builder.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::ir;
using namespace swift::runtime::backend;
using namespace swift::runtime::backend::arm64;

namespace {
ConsumerPlan AuditPlan(Block& block, bool enabled = true) {
    block.ReIdInstr();
    BlockAnalysisIndex index; index.Build(&block);
    return BuildConsumerPlan(index, [](const Inst&) { return true; },
                             [](const Inst&) { return false; }, nullptr, enabled);
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
    auto input = block->LoadImm(Imm{u64{45}}).SetType(ValueType::U64);
    const std::array<Value, 4> roots{{
        block->Sub(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64),
        block->And(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64),
        block->Or(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64),
        block->Xor(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64)}};
    for (u16 i = 0; i < roots.size(); ++i) block->StoreUniform(Uniform{u16(i * 8), ValueType::U64}, roots[i]);
    auto disabled = AuditPlan(*block, false); REQUIRE(disabled.alu_split.empty());
    auto plan = AuditPlan(*block); REQUIRE(plan.valid); REQUIRE(plan.alu_split.size() == roots.size());
    REQUIRE_FALSE(plan.alu_split.contains(input.Def()));
    for (auto root : roots) {
        const auto& facts = plan.alu_split.at(root.Def());
        REQUIRE(facts.value_uses == 1); REQUIRE(facts.external_users == 1);
        REQUIRE(facts.family_count == 1); REQUIRE_FALSE(facts.family_capped);
        REQUIRE(facts.saw_value); REQUIRE_FALSE(facts.saw_address);
    }
}

TEST_CASE("ALU audit retains width bridge families and the thirty two member cap", "[alu-split-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbab0})};
    auto input = block->LoadImm(Imm{u64{64}}).SetType(ValueType::U64);
    auto root = block->And(input, ir::Operand{Imm{u64{255}}}).SetType(ValueType::U64);
    auto alias = root;
    for (unsigned i = 0; i < 35; ++i) alias = block->BitCast(alias).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, alias);
    const auto capped = AuditPlan(*block).alu_split.at(root.Def());
    REQUIRE(capped.family_count == 32); REQUIRE(capped.family_capped);
    REQUIRE(capped.value_uses == 1); REQUIRE(capped.external_users == 1);
    REQUIRE(capped.saw_value); REQUIRE_FALSE(capped.saw_address);

    IntrusivePtr<Block> address{new Block(0, Location{0xbab8})};
    auto base = address->LoadImm(Imm{u64{64}}).SetType(ValueType::U64);
    auto producer = address->Xor(base, ir::Operand{Imm{u64{0}}}).SetType(ValueType::U64);
    auto narrow = address->BitExtract(producer, Imm{u8{0}}, Imm{u8{8}}).SetType(ValueType::U8);
    auto wide = address->ZeroExtend64(narrow).SetType(ValueType::U64);
    address->LoadMemory(ir::Operand{wide}).SetType(ValueType::U64);
    const auto facts = AuditPlan(*address).alu_split.at(producer.Def());
    REQUIRE(facts.family_count == 3); REQUIRE_FALSE(facts.family_capped);
    REQUIRE(facts.value_uses == 1); REQUIRE(facts.external_users == 1);
    REQUIRE(facts.saw_address); REQUIRE_FALSE(facts.saw_value);
}

TEST_CASE("ALU audit uses the first matching family member for store roles", "[alu-split-plan]") {
    for (bool root_is_data : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0xbac0})};
        auto input = block->LoadImm(Imm{u64{64}}).SetType(ValueType::U64);
        auto root = block->Or(input, ir::Operand{Imm{u64{0}}}).SetType(ValueType::U64);
        auto alias = block->BitCast(root).SetType(ValueType::U64);
        block->StoreMemory(ir::Operand{root_is_data ? alias : root}, root_is_data ? root : alias);
        const auto facts = AuditPlan(*block).alu_split.at(root.Def());
        REQUIRE(facts.family_count == 2); REQUIRE(facts.external_users == 1);
        REQUIRE(facts.value_uses == 2);
        REQUIRE(facts.saw_value == root_is_data); REQUIRE(facts.saw_address == !root_is_data);
    }
}

TEST_CASE("ALU audit separates operand multiplicity from flags and distinct users", "[alu-split-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbad0})};
    auto input = block->LoadImm(Imm{u64{45}}).SetType(ValueType::U64);
    auto root = block->Sub(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64);
    block->Add(root, ir::Operand{root}).SetType(ValueType::U64);
    auto flags = block->And(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64);
    block->SaveFlags(flags, Flags::NZ);
    auto plan = AuditPlan(*block);
    REQUIRE(plan.alu_split.at(root.Def()).value_uses == 2);
    REQUIRE(plan.alu_split.at(root.Def()).external_users == 1);
    REQUIRE(plan.alu_split.at(flags.Def()).value_uses == 0);
    REQUIRE(plan.alu_split.at(flags.Def()).external_users == 1);
    REQUIRE(plan.alu_split.at(flags.Def()).saw_value);
}

TEST_CASE("ALU audit admission rejects config and IR changes before bytes", "[alu-split-plan][alu-split-admission]") {
#if defined(__aarch64__)
    AuditEnvironment environment; environment.Set(false);
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    auto features = ResolveFeatureSet(module->GetModuleConfig());
    IntrusivePtr<Block> block{new Block(0, Location{0xbae0})};
    auto input = block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto result = block->Sub(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{8, ValueType::U64}, result);
    auto* store = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE(CompleteARM64UnitForEmission(module, allocation, block.get(), nullptr));
    const auto* actions = allocation.GPRPlanViewFor(block.get());
    REQUIRE_FALSE((actions->consumers && !actions->consumers->alu_split.empty()));
    JitContext old_context{module, allocation}; JitTranslator old{old_context};
    REQUIRE(old_context.RequireGPRPlan(block.get()));
    environment.Set(true, false);
    JitContext split_context{module, allocation}; JitTranslator split{split_context};
    REQUIRE(split_context.RequireGPRPlan(block.get()));
    environment.Set(false, true);
    REQUIRE_FALSE(split_context.RequireGPRPlan(block.get()));
    REQUIRE_FALSE(split.Translate(block.get())); REQUIRE(split_context.CurrentBufferSize() == 0);
    environment.Set(true);
    REQUIRE_FALSE(ARM64UnitReadyForEmission(module, allocation, block.get(), nullptr));
    REQUIRE_FALSE(old_context.RequireGPRPlan(block.get()));
    REQUIRE_FALSE(old.Translate(block.get())); REQUIRE(old_context.CurrentBufferSize() == 0);
    JitContext new_context{module, allocation}; JitTranslator fresh{new_context};
    REQUIRE_FALSE(fresh.Translate(block.get())); REQUIRE(new_context.CurrentBufferSize() == 0);
    environment.Set(false);
    REQUIRE(ARM64UnitReadyForEmission(module, allocation, block.get(), nullptr));
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
            auto input = block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
            Value result;
            if (op == 0) result = block->Sub(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64);
            else if (op == 1) result = block->And(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64);
            else if (op == 2) result = block->Or(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64);
            else result = block->Xor(input, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64);
            if (flags) block->SaveFlags(result, Flags::NZ);
            block->StoreUniform(Uniform{16, ValueType::U64}, result);
            block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
            RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                                space.GetTrampolines().GetFPRRegs(), features};
            RegisterAllocPass::Run(block.get(), &allocation, false, features);
            REQUIRE(CompleteARM64UnitForEmission(module, allocation, block.get(), nullptr));
            const auto* actions = allocation.GPRPlanViewFor(block.get());
            REQUIRE(actions->region->options.alu_split_audit == audit);
            if (audit) REQUIRE(actions->consumers->alu_split.contains(result.Def()));
            JitContext context{module, allocation}; JitTranslator translator{context};
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
    AuditEnvironment environment; environment.Set(true);
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{2, true, features};
    auto* first = builder.AppendFunction(Location{0xbb00}, Location{0xbb10});
    auto* first_hir = first->GetCurrentBlock();
    auto a = first->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto first_root = first->Sub(a, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64);
    first->StoreUniform(Uniform{8, ValueType::U64}, first_root);
    first->EndBlock(terminal::ReturnToHost{}); first->EndFunction(); first->ComputeRPO(); first->IdByRPO();
    auto* second = builder.AppendFunction(Location{0xbb20}, Location{0xbb40});
    second->AdvancePC(Imm{u64{1}});
    auto* later = builder.LinkBlock(terminal::LinkBlock{Location{0xbb30}}); builder.SetCurBlock(later);
    auto b = second->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto late_root = second->Or(b, ir::Operand{Imm{u64{7}}}).SetType(ValueType::U64);
    second->StoreUniform(Uniform{8, ValueType::U64}, late_root);
    second->EndBlock(terminal::ReturnToHost{}); second->EndFunction(); second->ComputeRPO(); second->IdByRPO();
    RegAlloc first_allocation{first->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                              space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(first, &first_allocation, features);
    REQUIRE(CompleteARM64UnitForEmission(module, first_allocation, nullptr, first));
    environment.Set(false);
    RegAlloc second_allocation{second->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                               space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(second, &second_allocation, features);
    REQUIRE(CompleteARM64UnitForEmission(module, second_allocation, nullptr, second));
    environment.Set(true);
    const std::array<FunctionRegionEmission, 2> mixed{{{first, &first_allocation}, {second, &second_allocation}}};
    FunctionCodeObjectEmitter unprepared{module, mixed};
    REQUIRE_FALSE(unprepared.Emit()); REQUIRE(unprepared.CurrentBufferSize() == 0);
    RegAlloc retry{second->MaxInstrCount(), space.GetTrampolines().GetGPRRegs(),
                   space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(second, &retry, features);
    REQUIRE(CompleteARM64UnitForEmission(module, retry, nullptr, second));
    const auto* first_plan = first_allocation.GPRPlanViewFor(first_hir->GetBlock());
    const auto* later_plan = retry.GPRPlanViewFor(later->GetBlock());
    REQUIRE(first_plan->consumers->alu_split.contains(first_root.Def()));
    REQUIRE_FALSE(first_plan->consumers->alu_split.contains(late_root.Def()));
    REQUIRE(later_plan->consumers->alu_split.contains(late_root.Def()));
    REQUIRE_FALSE(later_plan->consumers->alu_split.contains(first_root.Def()));
    const std::array<FunctionRegionEmission, 2> prepared{{{first, &first_allocation}, {second, &retry}}};
    FunctionCodeObjectEmitter emitter{module, prepared};
    REQUIRE(emitter.Emit()); REQUIRE(emitter.CurrentBufferSize() > 0);
#else
    SUCCEED("native ARM region admission requires AArch64");
#endif
}
