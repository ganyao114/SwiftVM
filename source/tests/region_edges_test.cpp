#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/compile/unit.h"
#include "runtime/compile/workspace.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <sys/mman.h>
#include <unistd.h>
#include "base/scope_exit.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/lower_edges.h"
#include "runtime/backend/arm64/jit/function_code_object_emitter.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/context.h"
#include "runtime/backend/riscv64/jit/compile.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/cast_utils.h"
#include "runtime/common/svm_config.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/ir/hir_builder.h"
#include "support/register_alloc_test_support.h"
#include "translator/x86/cpu.h"

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

TEST_CASE("frozen physical results and actions outlive their preparation writer",
          "[region-plan][ownership]") {
    ::swift::runtime::compile::CompileWorkspace rejected_workspace;
    Config config{.loc_start = 0,
                  .loc_end = 1ull << 48,
                  .enable_jit = true,
                  .has_local_operation = false,
                  .backend_isa = kArm64};
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    IntrusivePtr<Block> block{new Block(0, Location{0xf800})};
    const auto input = block->LoadImm(ValueType::U64, Imm{u64{41}});
    const auto result = block->Add(ValueType::U64, input, Operand{Imm{u64{7}}});
    block->StoreUniform(Uniform{0, ValueType::U64}, result);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    compile::CompileUnit unit{block};
    unit.SealIR();
    compile::Allocation physical{block->MaxInstrId(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    const auto view = physical.View();
    arm64::UnitActions actions{view};
    compile::SemanticFacts facts{block.get()};
    {
        compile::CompileWorkspace preparation_workspace;
        arm64::AllocationPreparation writer{physical, facts, preparation_workspace, actions};
        REQUIRE(&writer.Workspace() == &preparation_workspace);
        REQUIRE_THROWS_AS(
                (arm64::AllocationPreparation{physical, facts, preparation_workspace, actions}),
                std::logic_error);
        compile::AllocateBlock(block.get(),
                               &writer,
                               ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                                       block.get(), writer, features));
        REQUIRE(arm64::CompleteARM64UnitForEmission(module, writer, block.get(), nullptr));
        REQUIRE(view.RefersToSameResult(writer));
    }
    REQUIRE(actions.MatchesAllocation(view));
    REQUIRE(view.ValueType(result) == compile::Allocation::GPR);
    REQUIRE_THROWS_AS((arm64::AllocationPreparation{physical, facts, rejected_workspace, actions}),
                      std::logic_error);
    arm64::JitContext context{module, view, actions};
    arm64::JitTranslator translator{context};
    REQUIRE(translator.Translate(block.get()));
    context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);
}

TEST_CASE("caller prepares an ARM unit before any encoder", "[region-plan][explicit-preparation]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    IntrusivePtr<Block> block{new Block(0, Location{0xf810})};
    auto input = block->LoadImm(ValueType::U64, Imm{u64{41}});
    auto result = block->Add(ValueType::U64, input, Operand{Imm{u64{7}}});
    block->StoreUniform(Uniform{0, ValueType::U64}, result);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    REQUIRE_FALSE(arm64::CompleteARM64UnitForEmission(module, allocation, block.get(), nullptr));
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    const auto unprepared_attempt = allocation.AllocationAttempt();
    const auto unprepared_generation = allocation.GPRLocationGeneration();
    arm64::JitContext unprepared{module, allocation, allocation.Actions()};
    arm64::JitTranslator unprepared_translator{unprepared};
    REQUIRE_FALSE(unprepared.RequireBlockForEmission(block.get()));
    REQUIRE_FALSE(unprepared_translator.Translate(block.get()));
    REQUIRE(unprepared.CurrentBufferSize() == 0);
    REQUIRE_FALSE(allocation.Actions().Ready());
    REQUIRE(allocation.AllocationAttempt() == unprepared_attempt);
    REQUIRE(allocation.GPRLocationGeneration() == unprepared_generation);
    REQUIRE(arm64::CompleteARM64UnitForEmission(module, allocation, block.get(), nullptr));
    // An encoder created before completion cannot silently bind a later attempt.
    REQUIRE_FALSE(unprepared.RequireBlockForEmission(block.get()));
    REQUIRE_FALSE(unprepared_translator.Translate(block.get()));
    REQUIRE(unprepared.CurrentBufferSize() == 0);
    vixl::aarch64::MacroAssembler raw_assembler;
    arm64::JitContext raw_context{module,
                                  allocation,
                                  allocation.Actions(),
                                  raw_assembler,
                                  arm64::JitContext::AssemblerOnlyTag{}};
    arm64::JitTranslator raw_translator{raw_context};
    REQUIRE_FALSE(raw_context.RequireBlockForEmission(block.get()));
    REQUIRE_FALSE(raw_translator.Translate(block.get()));
    REQUIRE(raw_context.CurrentBufferSize() == 0);
    raw_assembler.Nop();
    REQUIRE(raw_context.CurrentBufferSize() == 4);
    REQUIRE(allocation.GPRLocationsFrozen());
    const auto attempt = allocation.AllocationAttempt();
    const auto generation = allocation.GPRLocationGeneration();
    const auto replays = allocation.FPRContractRetryCount();
    const auto owner = allocation.Actions().ForBlock(block.get());
    const auto certificate = *allocation.Actions().Certificate();
    REQUIRE(owner);
    REQUIRE(arm64::CompleteARM64UnitForEmission(module, allocation, block.get(), nullptr));
    IntrusivePtr<Block> foreign{new Block(0, Location{0xf820})};
    REQUIRE_FALSE(arm64::CompleteARM64UnitForEmission(module, allocation, foreign.get(), nullptr));
    REQUIRE(allocation.Actions().ForBlock(block.get()) == owner);
    // The caller must match an incomplete allocator's registered unit. The
    // completion entry does not reset that registration to its argument.
    foreign->AdvancePC(Imm{u64{1}}); foreign->SetTerminal(terminal::ReturnToHost{});
    foreign->ReIdInstr();
    AllocationFixture registered{foreign->MaxInstrId(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    ::swift::runtime::compile::AllocateBlock(
            foreign.get(),
            &registered,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    foreign.get(), registered, features),
            false);
    REQUIRE_FALSE(registered.Actions().Ready());
    REQUIRE_FALSE(arm64::CompleteARM64UnitForEmission(module, registered, block.get(), nullptr));
    REQUIRE(registered.Actions().Ready());
    REQUIRE(arm64::ARM64UnitReadyForEmission(
            module, registered, registered.Actions(), foreign.get(), nullptr));
    // Two independent allocations of the same IR can have identical counters
    // and homes. A certificate still belongs to exactly one physical result.
    AllocationFixture other{block->MaxInstrId(),
                            space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &other,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(block.get(), other, features),
            false);
    REQUIRE(arm64::CompleteARM64UnitForEmission(module, other, block.get(), nullptr));
    REQUIRE(other.AllocationAttempt() == allocation.AllocationAttempt());
    REQUIRE(other.GPRLocationGeneration() == allocation.GPRLocationGeneration());
    REQUIRE_FALSE(other.Actions().MatchesAllocation(allocation));
    REQUIRE_FALSE(arm64::ARM64UnitReadyForEmission(
            module, allocation, other.Actions(), block.get(), nullptr));
    REQUIRE_FALSE(certificate.MatchesUnit(
            block.get(), nullptr, allocation, other.Actions(), config, features));
    arm64::JitContext mismatched{module, allocation, other.Actions()};
    arm64::JitTranslator mismatched_translator{mismatched};
    REQUIRE_FALSE(mismatched_translator.Translate(block.get()));
    REQUIRE(mismatched.CurrentBufferSize() == 0);
    for (const bool direct : {true, false}) {
        arm64::JitContext context{module, allocation, allocation.Actions(), direct};
        arm64::JitTranslator translator{context};
        REQUIRE(translator.Translate(block.get())); context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
    }
    REQUIRE(allocation.AllocationAttempt() == attempt);
    REQUIRE(allocation.GPRLocationGeneration() == generation);
    REQUIRE(allocation.FPRContractRetryCount() == replays);
    // A fresh request invalidates old actions even when an empty block skips
    // the ordinary allocation scan. Old retained plans cannot be republished.
    IntrusivePtr<Block> empty{new Block(0, Location{0xf830})};
    empty->SetTerminal(terminal::ReturnToHost{});
    empty->ReIdInstr();
    compile::AllocateBlock(empty.get(),
                           &allocation,
                           ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                                   empty.get(), allocation, features));
    REQUIRE_FALSE(allocation.Actions().Ready());
    REQUIRE_FALSE(allocation.Actions().ForBlock(block.get()));
    REQUIRE_FALSE(certificate.MatchesUnit(
            block.get(), nullptr, allocation, allocation.Actions(), config, features));
    REQUIRE(arm64::CompleteARM64UnitForEmission(module, allocation, empty.get(), nullptr));
    REQUIRE(allocation.Actions().Ready());
    REQUIRE_FALSE(allocation.Actions().ForBlock(block.get()));
}

TEST_CASE("all frozen region and backedge option inputs reject before encoder output", "[region-plan][backedge-plan]") {
    for (unsigned option = 0; option < 9; ++option) {
        CAPTURE(option);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        AddressSpace space{config}; auto module = space.GetDefaultModule();
        const auto features = ResolveFeatureSet(module->GetModuleConfig());
        IntrusivePtr<Block> block{new Block(0, Location{0xf840})};
        block->AdvancePC(Imm{u64{1}}); block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
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
        REQUIRE(arm64::CompleteARM64UnitForEmission(module, allocation, block.get(), nullptr));
        const auto owner = allocation.Actions().ForBlock(block.get());
        const auto certificate = *allocation.Actions().Certificate();
        const auto attempt = allocation.AllocationAttempt();
        const auto generation = allocation.GPRLocationGeneration();
        auto changed_config = config; auto changed_module = module->GetModuleConfig();
        const char* environment = nullptr; bool previous = false;
        switch (option) {
        case 0: changed_config.region_edges = !config.region_edges; break;
        case 1:
            environment = "SVM_FLAGS_REGS";
            previous = certificate.options.flags_regs;
            break;
        case 2: changed_module.feature_overrides.Set(FeatureId::helper_leaf_abi, !features.helper_leaf_abi); break;
        case 3:
            environment = "SVM_BACKEDGE_LATCH";
            previous = certificate.options.backedge_latch;
            break;
        case 4:
            environment = "SVM_BACKEDGE_FLAGS";
            previous = certificate.options.backedge_flags;
            break;
        case 5: changed_module.feature_overrides.Set(FeatureId::flags_loop_lazy, !features.flags_loop_lazy); break;
        case 6: changed_module.feature_overrides.Set(FeatureId::flags_region_branch, !features.flags_region_branch); break;
        case 7:
            environment = "SVM_FLAGS_CFINV";
            previous = certificate.options.flags_cfinv;
            break;
        case 8:
            changed_config.arm64_features = static_cast<Arm64Features>(
                    static_cast<u64>(config.arm64_features) ^ static_cast<u64>(Arm64Features::FlagM));
            break;
        }
        const auto old_environment = environment ? GetRawSvmConfigEnvForTest(environment) : nullptr;
        const std::optional<std::string> saved = old_environment
                ? std::optional<std::string>{old_environment} : std::nullopt;
        auto restore = ScopeExit([&] {
            if (!environment) return;
            if (saved) (void)SetSvmConfigEnvForTest(environment, saved->c_str(), 1);
            else (void)UnsetSvmConfigEnvForTest(environment);
        });
        if (environment) REQUIRE(SetSvmConfigEnvForTest(environment, previous ? "0" : "1", 1) == 0);
        AddressSpace alternate_space{changed_config};
        auto alternate_module = alternate_space.MapModule(LocationDescriptor{0xf840},
                LocationDescriptor{0xf850}, changed_module);
        const auto changed_features = ResolveFeatureSet(alternate_module->GetModuleConfig());
        REQUIRE_FALSE(certificate.MatchesUnit(block.get(),
                                              nullptr,
                                              allocation,
                                              allocation.Actions(),
                                              alternate_space.GetConfig(),
                                              changed_features));
        arm64::JitContext context{alternate_module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        REQUIRE_FALSE(translator.Translate(block.get()));
        REQUIRE(context.CurrentBufferSize() == 0);
        REQUIRE(allocation.AllocationAttempt() == attempt);
        REQUIRE(allocation.GPRLocationGeneration() == generation);
        REQUIRE(allocation.Actions().ForBlock(block.get()) == owner);
    }
}

namespace {
void BackedgeClobberingHelper() {
#if defined(__aarch64__)
    asm volatile("cmp xzr, xzr" ::: "cc");
#endif
}
}

TEST_CASE("frozen region PF AF recipes preserve cold operands and ordinary fallback", "[region-plan][backedge-plan][region-pfaf-plan]") {
    for (unsigned variant = 0; variant < 4; ++variant) {
        CAPTURE(variant);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.region_edges = true; config.stack_alignment = 16;
        config.uniform_buffer_size = sizeof(swift::x86::ThreadContext64) + 64;
        AddressSpace space{config}; ModuleConfig module_config{};
        module_config.feature_overrides.Set(FeatureId::flags_region_branch, true);
        auto module = space.MapModule(LocationDescriptor{0xf860}, LocationDescriptor{0xf890}, module_config);
        const auto features = ResolveFeatureSet(module->GetModuleConfig());
        HIRBuilder builder{3, true, features};
        auto* function = builder.AppendFunction(Location{0xf860}, Location{0xf890});
        auto* source = &function->EditBlock(function->GetCurrentBlock());
        auto* hot = function->AppendBlock(Location{0xf870}, Location{0xf880});
        auto* cold = function->AppendBlock(Location{0xf880}, Location{0xf890});
        function->AddEdge(source, hot); function->AddEdge(source, cold);
        builder.SetCurBlock(source);
        const auto input = function->LoadUniform(ValueType::U16, Uniform{0, ValueType::U16});
        const auto sub = function->Sub(ValueType::U16, input, Operand{Imm{u64{1}}});
        function->SaveFlags(sub, Flags::All);
        Value condition;
        if (variant == 2) condition = function->LocalCondSet(ValueType::U8, Cond::EQ);
        if (variant == 1) {
            const auto zero = function->LoadImm(ValueType::U8, Imm{u8{0}});
            // Overlaps the uniform reread without changing the named Sub flags.
            function->StoreUniform(Uniform{0, ValueType::U8}, zero);
        } else if (variant == 2) {
            (void)function->CallLambda(ValueType::VOID, Lambda{Imm{u64{reinterpret_cast<uintptr_t>(&BackedgeClobberingHelper)}}});
        } else if (variant == 3) {
            const auto address = function->LoadUniform(ValueType::U64, Uniform{16, ValueType::U64});
            const auto loaded = function->LoadMemory(ValueType::U32, Operand{address});
            function->StoreUniform(Uniform{32, ValueType::U32}, loaded);
        }
        const bool canonical = GetSvmConfig().flags_cfinv && True(config.arm64_features & Arm64Features::FlagM);
        if (canonical) function->InvertCarry();
        else {
            const auto polarity = function->LoadImm(ValueType::U8, Imm{u8{0}});
            function->StoreUniform(
                    Uniform{offsetof(swift::x86::ThreadContext64, carry_inverted), ValueType::U8},
                    polarity);
        }
        function->AdvancePC(Imm{u64{1}});
        if (variant != 2) condition = function->LocalCondSet(ValueType::U8, Cond::EQ);
        function->EndBlock(terminal::If{condition,
                                        terminal::LinkBlock{Location{0xf870}},
                                        terminal::LinkBlock{Location{0xf880}}});
        builder.SetCurBlock(hot);
        const auto value = function->LoadImm(ValueType::U16, Imm{u16{2}});
        const auto next = function->Sub(ValueType::U16, value, Operand{Imm{u64{1}}});
        function->SaveFlags(next, Flags::All);
        function->AdvancePC(Imm{u64{1}});
        const auto marker = function->LoadImm(ValueType::U32, Imm{u32{111}});
        function->StoreUniform(Uniform{8, ValueType::U32}, marker);
        function->EndBlock(terminal::ReturnToHost{});
        builder.SetCurBlock(cold);
        const auto parity = function->TestFlags(ValueType::U32, Flags::Parity).SetCastType(ValueType::U32);
        const auto auxiliary = function->TestFlags(ValueType::U32, Flags::AuxiliaryCarry).SetCastType(ValueType::U32);
        const auto twice = function->Add(ValueType::U32, auxiliary, Operand{auxiliary});
        const auto packed = function->Or(ValueType::U32, parity, Operand{twice});
        function->StoreUniform(Uniform{8, ValueType::U32}, packed);
        function->StoreUniform(Uniform{24, ValueType::U32}, parity);
        function->StoreUniform(Uniform{28, ValueType::U32}, auxiliary);
        function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
        function->ComputeRPO(); function->IdByRPO();
        AllocationFixture allocation{function->MaxInstrCount(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateFunction(
                function,
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        function, allocation, features),
                {.block_intervals = false});
        REQUIRE(arm64::CompleteARM64UnitForEmission(module, allocation, nullptr, function));
        const auto actions = allocation.Actions().ForBlock(source->GetBlock());
        REQUIRE(actions);
        REQUIRE_FALSE(allocation.Actions().IntegerBranch(source->GetBlock()));
        if (variant < 2) {
            const auto& target = allocation.Actions().Edges().Block(0xf870)->edge_flags;
            CAPTURE(allocation.Actions().Edges().active,
                    target.overwrite_before_observe,
                    target.observed_nzcv_mask,
                    target.commits_before_fault,
                    target.barrier_before_commit);
            REQUIRE(allocation.Actions().BackedgeFor(source->GetBlock()));
            REQUIRE(allocation.Actions().BackedgeFor(source->GetBlock())->dead_successor);
            REQUIRE((allocation.Actions().PFAFFor(source->GetBlock()) != nullptr) ==
                    (variant == 0));
        } else
            REQUIRE_FALSE(allocation.Actions().BackedgeFor(source->GetBlock()));
        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        REQUIRE(translator.Translate(function)); context.Finish();
#if defined(__aarch64__)
        const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer);
        space.PushCodeCache(Location{0xf860}, buffer.exec_data + context.GetCodeOffset(0xf860));
        constexpr size_t memory_size = 4096;
        auto* memory = mmap(nullptr, memory_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(memory != MAP_FAILED); auto unmap = ScopeExit([&] { (void)munmap(memory, memory_size); });
        const u32 memory_value = 99; std::memcpy(memory, &memory_value, sizeof(memory_value));
        for (const u16 initial : {u16{0}, u16{1}, u16{2}, u16{128}}) {
            CAPTURE(initial);
            Runtime runtime{&space};
            std::memcpy(runtime.GetUniformBuffer().data(), &initial, sizeof(initial));
            const auto address = reinterpret_cast<u64>(memory);
            std::memcpy(runtime.GetUniformBuffer().data() + 16, &address, sizeof(address));
            runtime.SetLocation(0xf860); REQUIRE(runtime.Run() == HaltReason::CallHost);
            u32 actual{}; std::memcpy(&actual, runtime.GetUniformBuffer().data() + 8, sizeof(actual));
            const u32 expected = initial == 1 ? 111 : initial == 0 ? 3 : initial == 128 ? 2 : 0;
            if (initial != 1) {
                u32 observed_pf{}, observed_af{};
                std::memcpy(&observed_pf, runtime.GetUniformBuffer().data() + 24, sizeof(observed_pf));
                std::memcpy(&observed_af, runtime.GetUniformBuffer().data() + 28, sizeof(observed_af));
                const auto raw_flags = runtime.GetState()->host_cpu_flags;
                CAPTURE(observed_pf, observed_af, raw_flags);
                REQUIRE(observed_pf == (expected & 1));
                REQUIRE(observed_af == (expected >> 1));
                REQUIRE((raw_flags & 0xff) == static_cast<u8>(initial - 1));
                REQUIRE(((raw_flags >> arm64::HostFlagsBit::AuxiliaryCarry) & 1) == (expected >> 1));
            }
            REQUIRE(actual == expected);
            if (variant == 3) {
                u32 observed{}; std::memcpy(&observed, runtime.GetUniformBuffer().data() + 32, sizeof(observed));
                REQUIRE(observed == memory_value);
            }
        }
#endif
    }
}

TEST_CASE("compiler admits a code object only after every region has finished",
          "[region-plan][explicit-preparation]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder first_builder{1, true, features}, second_builder{1, true, features};
    auto* first = first_builder.AppendFunction(Location{0xf830}, Location{0xf840});
    first->AdvancePC(Imm{u64{1}});
    first->EndBlock(terminal::ReturnToHost{});
    first->EndFunction();
    auto* second = second_builder.AppendFunction(Location{0xf850}, Location{0xf860});
    second->AdvancePC(Imm{u64{1}});
    second->EndBlock(terminal::ReturnToHost{});
    const std::array<HIRFunction*, 2> inputs{first, second};
    REQUIRE_FALSE(arm64::PrepareFunctions(module, inputs).has_value());
    REQUIRE_FALSE(first->GetFunction()->IsIRReadOnly());
    REQUIRE(first->GetFunction()->GetBlocks().size() == 1);
    REQUIRE(space.GetCodeCache(0xf830) == nullptr);
    REQUIRE(space.GetCodeCache(0xf850) == nullptr);
    second->EndFunction();
    auto prepared = arm64::PrepareFunctions(module, inputs);
    REQUIRE(prepared.has_value());
    arm64::FunctionCodeObjectEmitter emitter{module, *prepared};
    REQUIRE(emitter.Emit()); REQUIRE(emitter.CurrentBufferSize() > 0);
}

TEST_CASE("fresh HIR compilation preserves a live SSA input", "[region-plan][hir-admission][explicit-preparation][hir-ownership]") {
#if defined(__aarch64__)
    constexpr size_t guest_size = 4096;
    auto* guest = static_cast<u8*>(mmap(nullptr, guest_size, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(guest != MAP_FAILED);
    Config config{.loc_start = 0, .loc_end = guest_size, .enable_jit = true,
                  .enable_asm_interp = false, .has_local_operation = false,
                  .backend_isa = kArm64, .uniform_buffer_size = 32,
                  .memory_base = guest, .guest_addr_mask = guest_size - 1};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    {
        HIRBuilder builder{1, true, features};
        auto* function = builder.AppendFunction(Location{0x200}, Location{0x210});
        auto* hir = &function->EditBlock(function->GetCurrentBlock());
        auto old = function->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        auto next = function->Add(ValueType::U64, old, Operand{Imm{u64{7}}});
        function->StoreUniform(Uniform{8, ValueType::U64}, next);
        function->StoreUniform(Uniform{16, ValueType::U64}, old);
        function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
        function->ComputeRPO(); function->IdByRPO();
        std::vector<u16> ids;
        for (auto& inst : hir->GetInstList()) ids.push_back(inst.Id());
        // Construct the already-bound local input contract; no ID changes even in
        // the fixture. The production admission never calls ReIdInstr.
        hir->GetBlock()->ReIdInstr();
        size_t index = 0;
        for (auto& inst : hir->GetInstList()) REQUIRE(inst.Id() == ids[index++]);
        REQUIRE(arm64::ARM64StandaloneHIRCapacity(*hir->GetBlock()) == ids.size());
        auto* code = TranslateIR(module, hir); REQUIRE(code != nullptr);
        REQUIRE(function->GetFunction()->FindBlock(Location{0x200}) == nullptr);
        REQUIRE(function->HasStandaloneBlocks());
        space.PushCodeCache(Location{0x200}, code);
        Runtime runtime{&space}; const u64 input = 41;
        std::memcpy(runtime.GetUniformBuffer().data(), &input, sizeof(input)); runtime.SetLocation(0x200);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        u64 next_value{}, old_value{};
        std::memcpy(&next_value, runtime.GetUniformBuffer().data() + 8, sizeof(next_value));
        std::memcpy(&old_value, runtime.GetUniformBuffer().data() + 16, sizeof(old_value));
        REQUIRE(next_value == 48); REQUIRE(old_value == 41);
        HIRBuilder empty_builder{1, true, features};
        auto* empty_function = empty_builder.AppendFunction(Location{0x240}, Location{0x250});
        auto* empty_hir = &empty_function->EditBlock(empty_function->GetCurrentBlock());
        empty_function->EndBlock(terminal::ReturnToHost{}); empty_function->EndFunction();
        empty_function->ComputeRPO(); empty_function->IdByRPO();
        REQUIRE(arm64::ARM64StandaloneHIRCapacity(*empty_hir->GetBlock()) == 0);
        auto* empty_code = TranslateIR(module, empty_hir); REQUIRE(empty_code != nullptr);
        REQUIRE(empty_function->GetFunction()->FindBlock(Location{0x240}) == nullptr);
        space.PushCodeCache(Location{0x240}, empty_code);
        Runtime empty_runtime{&space}; empty_runtime.SetLocation(0x240);
        REQUIRE(empty_runtime.Run() == HaltReason::CallHost);
    }
    REQUIRE(IsBlock(module->GetNode(Location{0x200})));
    REQUIRE(IsBlock(module->GetNode(Location{0x240})));
    Runtime after_builder{&space}; const u64 later_input = 51;
    std::memcpy(after_builder.GetUniformBuffer().data(), &later_input, sizeof(later_input));
    after_builder.SetLocation(0x200);
    REQUIRE(after_builder.Run() == HaltReason::CallHost);
    u64 later_next{}, later_old{};
    std::memcpy(&later_next, after_builder.GetUniformBuffer().data() + 8, sizeof(later_next));
    std::memcpy(&later_old, after_builder.GetUniformBuffer().data() + 16, sizeof(later_old));
    REQUIRE(later_next == 58); REQUIRE(later_old == 51);
    REQUIRE(munmap(guest, guest_size) == 0);
#else
    SUCCEED("native HIR execution requires AArch64");
#endif
}

TEST_CASE("standalone HIR publication rolls back an exact block owner", "[hir-admission][hir-ownership]") {
#if defined(__aarch64__)
    std::array<u8, 4096> guest{};
    Config config{.loc_start = 0, .loc_end = guest.size(), .enable_jit = true,
                  .enable_asm_interp = false, .has_local_operation = false,
                  .backend_isa = kArm64, .uniform_buffer_size = 32,
                  .memory_base = guest.data(), .guest_addr_mask = guest.size() - 1};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    {
        HIRBuilder builder{1, true, features};
        auto* function = builder.AppendFunction(Location{0x300}, Location{0x310});
        auto* hir = &function->EditBlock(function->GetCurrentBlock());
        auto* raw = hir->GetBlock();
        function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
        function->ComputeRPO(); function->IdByRPO();
        REQUIRE(function->GetFunction()->FindBlock(Location{0x300}) == raw);
        IntrusivePtr<Block> occupied{new Block(0, Location{0x300})};
        occupied->SetTerminal(terminal::ReturnToHost{});
        REQUIRE_FALSE(function->GetFunction()->DetachBlock(occupied.get()));
        REQUIRE(module->Push(occupied.get()));
        REQUIRE(TranslateIR(module, hir) == nullptr);
        REQUIRE(function->GetFunction()->FindBlock(Location{0x300}) == raw);
        REQUIRE_FALSE(function->HasStandaloneBlocks());
        REQUIRE(GetBlock(module->GetNode(Location{0x300})).get() == occupied.get());
        module->Remove(occupied.get());
        auto* code = TranslateIR(module, hir); REQUIRE(code != nullptr);
        space.PushCodeCache(Location{0x300}, code);
        REQUIRE(function->GetFunction()->FindBlock(Location{0x300}) == nullptr);
        REQUIRE(function->HasStandaloneBlocks());
        REQUIRE(GetBlock(module->GetNode(Location{0x300})).get() == raw);
        REQUIRE(TranslateIR(module, hir) == nullptr);
        REQUIRE(TranslateIR(module, function) == nullptr);
        REQUIRE_FALSE(PublishIRFunction(module, function));
        const std::array<HIRFunction*, 1> regions{{function}};
        REQUIRE(TranslateIR(module, std::span<HIRFunction* const>{regions}) == nullptr);
        REQUIRE(riscv64::CompileFunctions(module, std::span<HIRFunction* const>{regions}) == nullptr);
        HIRBuilder valid_builder{1, true, features};
        auto* valid = valid_builder.AppendFunction(Location{0x320}, Location{0x330});
        valid->EndBlock(terminal::ReturnToHost{}); valid->EndFunction();
        valid->ComputeRPO(); valid->IdByRPO();
        const std::array<HIRFunction*, 2> mixed{{valid, function}};
        REQUIRE(TranslateIR(module, std::span<HIRFunction* const>{mixed}) == nullptr);
        REQUIRE(riscv64::CompileFunctions(module, std::span<HIRFunction* const>{mixed}) == nullptr);
        REQUIRE(IsEmpty(module->GetNode(Location{0x320})));
        REQUIRE(valid->GetFunction()->FindBlock(Location{0x320}) != nullptr);
        REQUIRE(GetBlock(module->GetNode(Location{0x300})).get() == raw);
    }
    REQUIRE(IsBlock(module->GetNode(Location{0x300})));
    Runtime runtime{&space}; runtime.SetLocation(0x300);
    REQUIRE(runtime.Run() == HaltReason::CallHost);
#else
    SUCCEED("native HIR execution requires AArch64");
#endif
}

TEST_CASE("standalone HIR admission preserves unfinished and published function owners", "[hir-admission][hir-ownership]") {
#if defined(__aarch64__)
    Config config{.loc_start = 0, .loc_end = 4096, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    Block* retained{};
    {
        HIRBuilder builder{1, true, features};
        auto* function = builder.AppendFunction(Location{0x340}, Location{0x350});
        auto* hir = &function->EditBlock(function->GetCurrentBlock());
        retained = hir->GetBlock();
        function->EndBlock(terminal::ReturnToHost{});
        REQUIRE(TranslateIR(module, hir) == nullptr);
        REQUIRE(IsEmpty(module->GetNode(Location{0x340})));
        REQUIRE_FALSE(function->HasStandaloneBlocks());
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        REQUIRE(function->GetFunction()->FindBlock(Location{0x340}) == retained);
        REQUIRE(module->Push(function->GetFunction()));
        function->MarkFunctionPublished();
        REQUIRE(TranslateIR(module, hir) == nullptr);
        REQUIRE_FALSE(function->HasStandaloneBlocks());
        REQUIRE(function->GetFunction()->FindBlock(Location{0x340}) == retained);
        REQUIRE(IsFunction(module->GetNode(Location{0x340})));
    }
    auto node = module->GetNode(Location{0x340}); REQUIRE(IsFunction(node));
    REQUIRE(GetFunction(node)->FindBlock(Location{0x340}) == retained);
    REQUIRE(retained->HasTerminal());
#else
    SUCCEED("native HIR execution requires AArch64");
#endif
}

TEST_CASE("HIR admission rejects foreign and nonlocal inputs without renumbering", "[region-plan][hir-admission]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0xf890}, Location{0xf8b0});
    function->AdvancePC(Imm{u64{1}});
    auto* second = builder.LinkBlock(terminal::LinkBlock{Location{0xf8a0}});
    builder.SetCurBlock(second);
    auto input = function->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    auto result = function->Add(ValueType::U64, input, Operand{Imm{u64{7}}});
    function->StoreUniform(Uniform{8, ValueType::U64}, result);
    function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
    function->ComputeRPO(); function->IdByRPO();
    std::vector<u16> ids;
    for (auto& inst : second->GetInstList()) ids.push_back(inst.Id());
    const auto bound = second->GetBlock()->MaxInstrId(); const auto parent_bound = function->MaxInstrCount();
    REQUIRE(ids.front() > 0); REQUIRE(bound == 0);
    REQUIRE_FALSE(arm64::ARM64StandaloneHIRCapacity(*second->GetBlock()));
    REQUIRE(TranslateIR(module, second) == nullptr);
    REQUIRE(space.GetCodeCache(second->GetBlock()->GetStartLocation().Value()) == nullptr);
    size_t index = 0;
    for (auto& inst : second->GetInstList()) REQUIRE(inst.Id() == ids[index++]);
    REQUIRE(second->GetBlock()->MaxInstrId() == bound);
    REQUIRE(function->MaxInstrCount() == parent_bound);

    const unsigned shape = GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u);
    CAPTURE(shape);
    IntrusivePtr<Block> block{new Block(0, Location{0xf8c0})};
    auto a = block->LoadImm(ValueType::U64, Imm{u64{3}});
    auto b = block->Add(ValueType::U64, a, Operand{Imm{u64{4}}});
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    IntrusivePtr<Block> foreign{new Block(0, Location{0xf8d0})};
    auto outside = foreign->LoadImm(ValueType::U64, Imm{u64{9}});
    foreign->ReIdInstr();
    REQUIRE(arm64::ARM64StandaloneHIRCapacity(*block) == 2);
    compile::RewriteContext edit{block.get(), compilation_workspace, nullptr, false};
    if (shape == 0)
        edit.SetArg(block.get(), b.Def(), 0, outside);  // same ID, foreign pointer
    else if (shape == 1)
        edit.SetArg(block.get(), b.Def(), 0, b);  // self input
    else if (shape == 2)
        edit.Change(block.get(), a.Def(), [&](Inst& target) {
            target.SetInst(OpCode::BitCast, b);
        });  // forward alias/cycle
    else if (shape == 3)
        block->SetTerminal(terminal::Switch{outside, {}});
    else if (shape == 4)
        block->SetLoopHoistMetadata({.prefix_end = outside.Def()});
    else if (shape == 5)
        a.SetCastType(static_cast<ValueType>(255)), edit.SetArg(block.get(), b.Def(), 0, a);
    else if (shape == 6)
        edit.Change(block.get(), b.Def(), [](Inst& target) {
            target.SetReturn(static_cast<ValueType>(255));
        });
    else if (shape == 7) {
        block->SaveFlags(b, Flags::Zero);
        block->ReIdInstr();
    } else
        edit.Change(block.get(), b.Def(), [](Inst& target) {
            target.SetId(0);
        });  // duplicate/out-of-order domain
    REQUIRE_FALSE(arm64::ARM64StandaloneHIRCapacity(*block));
    // Restore only fixture mutations before teardown, not compiler side effects.
    if (shape == 2)
        edit.Change(block.get(), a.Def(), [](Inst& target) {
            target.SetInst(OpCode::LoadImm, Imm{u64{3}});
        });
}

TEST_CASE("HIR admission checks lambda params and recursive terminal references", "[region-plan][hir-admission]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    IntrusivePtr<Block> block{new Block(0, Location{0xf8e0})};
    auto address = block->LoadImm(ValueType::U64, Imm{u64{1}});
    auto input = block->LoadImm(ValueType::U64, Imm{u64{41}});
    auto call = block->CallLambda(ValueType::U64, Lambda{address}, input);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    REQUIRE(arm64::ARM64StandaloneHIRCapacity(*block) == 3);
    IntrusivePtr<Block> foreign{new Block(0, Location{0xf8f0})};
    auto outside = foreign->LoadImm(ValueType::U64, Imm{u64{1}});
    foreign->ReIdInstr();
    compile::RewriteContext{block.get(), compilation_workspace, nullptr, false}.SetArg(
            block.get(), call.Def(), 0, Lambda{outside});
    REQUIRE_FALSE(arm64::ARM64StandaloneHIRCapacity(*block));
    compile::RewriteContext{block.get(), compilation_workspace, nullptr, false}.SetArg(
            block.get(), call.Def(), 0, Lambda{address});
    REQUIRE(arm64::ARM64StandaloneHIRCapacity(*block) == 3);
    Params params;
    params.Push(input);
    params.Push(outside);
    auto phi = block->AddPhi(ValueType::U64, std::move(params));
    block->ReIdInstr();
    unsigned seen = 0;
    bool saw_foreign = false;
    phi.Def()->ForEachValue([&](Value value) { ++seen; saw_foreign |= value.Def() == outside.Def(); });
    REQUIRE(seen == 2); REQUIRE(saw_foreign);
    REQUIRE_FALSE(arm64::ARM64StandaloneHIRCapacity(*block)); // Phi is outside this policy.

    IntrusivePtr<Block> terminal_block{new Block(0, Location{0xf900})};
    auto condition = terminal_block->LoadImm(ValueType::U8, Imm{u8{1}});
    terminal_block->SetTerminal(terminal::Condition{
            Cond::EQ,
            terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToDispatch{}},
            terminal::CheckHalt{
                    terminal::Switch{condition, {{Imm{u64{1}}, terminal::ReturnToHost{}}}}}});
    terminal_block->ReIdInstr();
    REQUIRE(arm64::ARM64StandaloneHIRCapacity(*terminal_block) == 1);
    terminal_block->SetTerminal(terminal::Condition{Cond::EQ, terminal::ReturnToHost{},
            terminal::CheckHalt{terminal::Switch{outside,
                    {{Imm{u64{1}}, terminal::ReturnToHost{}}}}}});
    REQUIRE_FALSE(arm64::ARM64StandaloneHIRCapacity(*terminal_block));
    terminal_block->SetTerminal(terminal::If{BOOL{outside}, terminal::ReturnToHost{}, terminal::ReturnToDispatch{}});
    REQUIRE_FALSE(arm64::ARM64StandaloneHIRCapacity(*terminal_block));
    terminal_block->SetTerminal(terminal::Switch{condition, {}});
    terminal_block->SetLoopHoistMetadata(
            {.prefix_end = condition.Def(), .anchors = std::span<const Inst*>{}, .gpr_count = 1});
    REQUIRE_FALSE(arm64::ARM64StandaloneHIRCapacity(*terminal_block));
    std::array<const Inst*, 1> anchor{condition.Def()};
    terminal_block->SetLoopHoistMetadata(
            {.prefix_end = condition.Def(), .anchors = anchor, .gpr_count = 1});
    REQUIRE(arm64::ARM64StandaloneHIRCapacity(*terminal_block) == 1);
    anchor[0] = outside.Def();
    REQUIRE_FALSE(arm64::ARM64StandaloneHIRCapacity(*terminal_block));

    IntrusivePtr<Block> empty{new Block(0, Location{0xf910})};
    empty->SetTerminal(terminal::ReturnToHost{});
    REQUIRE(arm64::ARM64StandaloneHIRCapacity(*empty) == 0);
    empty->SetTerminal(terminal::If{condition, terminal::ReturnToHost{}, terminal::ReturnToDispatch{}});
    REQUIRE_FALSE(arm64::ARM64StandaloneHIRCapacity(*empty));
}

TEST_CASE("region DFS preserves ordered cuts across graph shapes", "[region-plan][regression][region-dfs]") {
    const unsigned shape = GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u);
    const bool enabled = GENERATE(false, true);
    CAPTURE(shape, enabled);
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64}; config.region_edges = enabled;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig()); HIRBuilder builder{1, true, features};
    constexpr u64 a=0xf600, b=0xf630, c=0xf610, d=0xf620, e=0xf640, outside=0xf700;
    auto* function = builder.AppendFunction(Location{a}, Location{e+16});
    std::array<const HIRBlock*, 5> nodes{function->GetCurrentBlock(),
                                         function->AppendBlock(Location{b}),
                                         function->AppendBlock(Location{c}),
                                         function->AppendBlock(Location{d}),
                                         function->AppendBlock(Location{e})};
    std::array<std::vector<u64>, 5> edges;
    std::set<std::pair<u64,u64>> expected;
    switch(shape) {
    case 0: edges[0]={a}; expected={{a,a}}; break;
    case 1: edges[0]={b,c}; edges[1]={d}; edges[2]={d}; edges[3]={a}; expected={{d,a}}; break;
    case 2: edges[0]={b,c}; edges[1]={a,d}; edges[2]={c}; edges[3]={b}; expected={{b,a},{c,c},{d,b}}; break;
    case 3: edges[0]={b,b,outside,c,b}; edges[1]={a}; expected={{b,a}}; break;
    case 4: edges[1]={c}; edges[2]={b}; edges[3]={d}; expected={{c,b},{d,d}}; break;
    case 5: edges[0]={outside,b}; edges[1]={outside}; break;
    case 6: edges[0]={b}; edges[2]={a}; edges[4]={c}; expected={{c,a}}; break;
    }
    for (size_t i=0; i<nodes.size(); ++i) {
        function->SetCurBlock(nodes[i]); function->AdvancePC(Imm{u64{1}});
        std::set<u64> added;
        for (const auto target : edges[i]) {
            const auto found = std::find_if(nodes.begin(), nodes.end(), [&](auto* node) {
                return node->GetBlock()->GetStartLocation().Value()==target;
            });
            if(found!=nodes.end() && added.insert(target).second) function->AddEdge(nodes[i], *found);
        }
        if(edges[i].empty()) function->EndBlock(terminal::ReturnToHost{});
        else if (shape == 3 && i == 0) {
            const auto selector = function->LoadImm(ValueType::U64, Imm{u64{0}});
            Vector<terminal::Switch::Case> cases;
            for(size_t arm=0; arm<edges[i].size(); ++arm) cases.push_back({Imm{static_cast<u64>(arm)},terminal::LinkBlock{Location{edges[i][arm]}}});
            function->EndBlock(terminal::Switch{selector,cases});
        } else if (edges[i].size() == 2) {
            if (shape == 1) {
                const auto condition =
                        function->TestNotZero(function->LoadImm(ValueType::U64, Imm{u64{1}}));
                function->EndBlock(terminal::If{condition,
                                                terminal::LinkBlock{Location{edges[i][0]}},
                                                terminal::LinkBlockFast{Location{edges[i][1]}}});
            } else
                function->EndBlock(
                        terminal::Condition{Cond::EQ,
                                            terminal::LinkBlock{Location{edges[i][0]}},
                                            terminal::LinkBlockFast{Location{edges[i][1]}}});
        } else
            function->EndBlock(terminal::CheckHalt{terminal::LinkBlock{Location{edges[i][0]}}});
    }
    function->EndFunction();
    if (shape == 6)
        HIRTestSupport::SetStartLocation(function->EditBlock(nodes[4]).GetBlock(), Location{a});
    // Include disconnected roots and preserve a deliberately non-location-sorted layout.
    HIRTestSupport::SetRPO(function, function->GetHIRBlocks());
    function->IdByRPO();
    AllocationFixture allocation{function->MaxInstrCount(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    const auto lowered = arm64::LowerUnitEdges(
            nullptr, function, allocation, allocation.Actions(), config, features);
    const auto& plan = lowered.actions;
    REQUIRE(lowered.certificate.valid);
    REQUIRE(plan.active == enabled);
    for (auto* node : nodes)
        REQUIRE(plan.ContainsInternalBlock(node->GetBlock()->GetStartLocation().Value()) ==
                enabled);
    REQUIRE_FALSE(plan.ContainsInternalBlock(outside));
    REQUIRE_FALSE(plan.ContainsInternalBlock(0));
    // Independent declared edges feed the retained map reference, not the production collector.
    std::map<u64,std::vector<u64>> reference;
    std::vector<u64> roots;
    for(auto& node : function->GetHIRBlocksRPO()) roots.push_back(node.GetBlock()->GetStartLocation().Value());
    reference[roots.front()]={a};
    for(size_t i=0; i<nodes.size(); ++i) {
        auto& targets=reference[nodes[i]->GetBlock()->GetStartLocation().Value()];
        for(const auto target : edges[i]) if(std::find(roots.begin(),roots.end(),target)!=roots.end() &&
            std::find(targets.begin(),targets.end(),target)==targets.end()) targets.push_back(target);
    }
    std::map<u64,unsigned> colors;
    std::set<std::pair<u64,u64>> cuts;
    std::function<void(u64)> visit=[&](u64 source) {
        colors[source]=1;
        for(const auto target:reference[source]) {
            if(colors[target]==1) cuts.emplace(source,target);
            else if(colors[target]==0) visit(target);
        }
        colors[source]=2;
    };
    for(const auto source:roots) if(colors[source]==0) visit(source);
    REQUIRE(cuts==expected);
    REQUIRE(std::set<std::pair<u64, u64>>(plan.cycle_edges.begin(), plan.cycle_edges.end()) ==
            (enabled ? cuts : std::set<std::pair<u64, u64>>{}));
    if(shape==6 && enabled) {
        REQUIRE(plan.BlockTransparency(nodes[0]->GetBlock()).has_value());
        REQUIRE_FALSE(plan.BlockTransparency(nodes[4]->GetBlock()).has_value());
    }
}

TEST_CASE("edge audit freezes commit suffixes with raw target multiplicity",
          "[region-plan][edge-audit]") {
    for (const bool enabled : {false, true})
        for (const bool audit : {false, true})
            for (unsigned scenario = 0; scenario < 11; ++scenario) {
                CAPTURE(enabled, audit, scenario);
                Config config{.loc_start = 0,
                              .loc_end = 1ull << 48,
                              .enable_jit = true,
                              .has_local_operation = false,
                              .backend_isa = kArm64};
                config.region_edges = enabled;
                AddressSpace space{config};
                auto features = ResolveFeatureSet(space.GetDefaultModule()->GetModuleConfig());
                features.flags_regs_audit = audit;
                HIRBuilder builder{1, false, features};
                constexpr u64 first_pc = 0xfa80, second_pc = 0xfaa0, outside = 0xfac0;
                auto* function = builder.AppendFunction(Location{first_pc}, Location{outside});
                auto* first = function->GetCurrentBlock();
                auto* second = function->AppendBlock(Location{second_pc});
                function->AddEdge(first, first);
                function->AddEdge(first, second);
                std::array<const Inst*, 3> advances{};
                const auto advance = [&](size_t index) {
                    function->AdvancePC(Imm{u64{1}});
                    advances[index] = &first->GetBlock()->GetInstList().back();
                };
                advance(0);
                if (scenario == 1) function->LoadMemory<U64>(Operand{Imm{u64{0x1000}}});
                if (scenario == 3 || scenario == 4 || scenario == 10) {
                    HelperCallTraits traits{.guest_state = HelperGuestStateEffect::None,
                                            .fault = HelperFaultEffect::NoDirectFault,
                                            .reentry = HelperReentryEffect::NoReentry};
                    if (scenario == 4) traits.fault = HelperFaultEffect::MayFault;
                    if (scenario == 10) traits = {};
                    function->CallLambda<U64>(Lambda{Imm{u64{0x1234}}, traits});
                }
                advance(1);
                advance(2);
                if (scenario == 2) function->CallLambda<U64>(Lambda{Imm{u64{0x1234}}});
                terminal::Condition pair{Cond::EQ,
                                         terminal::LinkBlock{Location{first_pc}},
                                         terminal::LinkBlockFast{Location{second_pc}}};
                if (scenario == 5) pair.else_ = terminal::LinkBlock{Location{first_pc}};
                if (scenario == 6) pair.else_ = terminal::LinkBlock{Location{outside}};
                if (scenario == 7)
                    function->EndBlock(terminal::Condition{
                            Cond::NE, pair, terminal::LinkBlock{Location{first_pc}}});
                else if (scenario == 8)
                    function->EndBlock(terminal::CheckHalt{pair});
                else if (scenario == 9)
                    function->EndBlock(terminal::LinkBlock{Location{second_pc}});
                else
                    function->EndBlock(pair);
                function->SetCurBlock(second);
                function->EndBlock(terminal::ReturnToHost{});
                function->EndFunction();
                function->ComputeRPO();
                function->IdByRPO();
                // Void IDs stay unique but cease to describe instruction
                // order. The suffix must use an identity transition, not >=.
                const auto first_id = advances[0]->Id(), last_id = advances[2]->Id();
                auto* body = function->EditBlock(first).GetBlock();
                compile::CompileWorkspace workspace;
                compile::RewriteContext rewrite{function, workspace, false};
                rewrite.Change(body, advances[0], [last_id](Inst& inst) { inst.SetId(last_id); });
                rewrite.Change(body, advances[2], [first_id](Inst& inst) { inst.SetId(first_id); });
                AllocationFixture allocation{function->MaxInstrCount(),
                                             space.GetTrampolines().GetGPRRegs(),
                                             space.GetTrampolines().GetFPRRegs(),
                                             features};
                const auto lowered = arm64::LowerUnitEdges(
                        nullptr, function, allocation, allocation.Actions(), config, features);
                REQUIRE(lowered.certificate.valid);
                const auto* row = lowered.actions.ForBlock(body);
                REQUIRE(row);
                const bool strict = enabled && audit && scenario != 2 && scenario != 6 &&
                                    scenario != 7 && scenario != 9;
                const auto expected = !strict ? UINT16_MAX
                                      : scenario == 1 || scenario == 4 || scenario == 10
                                              ? advances[1]->Id()
                                              : advances[0]->Id();
                REQUIRE(row->strict_advance_first == expected);
                REQUIRE(row->guest_instructions ==
                        (lowered.certificate.options.alu_split_audit ? 3 : 0));
                features.flags_regs_audit = !audit;
                REQUIRE(lowered.certificate.options != arm64::CaptureEdgeOptions(config, features));
            }
}

TEST_CASE("AllocationFixture freezes one shared region graph before emission", "[region-plan]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.region_edges = true;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0xfa00}, Location{0xfa30});
    function->AdvancePC(Imm{u64{1}});
    auto* first = &function->EditBlock(function->GetCurrentBlock());
    auto* second = builder.LinkBlock(terminal::LinkBlock{Location{0xfa10}});
    builder.SetCurBlock(second);
    function->AdvancePC(Imm{u64{1}});
    builder.LinkBlock(terminal::LinkBlock{Location{0xfa00}});
    function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
    AllocationFixture allocation{function->MaxInstrCount(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    ::swift::runtime::compile::AllocateFunction(
            function,
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, allocation, features));
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    const auto plan = allocation.Actions().ForBlock(first->GetBlock());
    REQUIRE(plan);
    const auto certificate = *allocation.Actions().Certificate();
    REQUIRE(certificate.valid);
    REQUIRE(allocation.Actions().ForBlock(second->GetBlock()));
    REQUIRE(allocation.Actions().Edges().active);
    REQUIRE(allocation.Actions().Edges().blocks.size() == 2);
    const std::set<std::pair<u64, u64>> expected_cycles{{0xfa10, 0xfa00}};
    REQUIRE(std::set<std::pair<u64, u64>>(allocation.Actions().Edges().cycle_edges.begin(),
                                          allocation.Actions().Edges().cycle_edges.end()) ==
            expected_cycles);
    REQUIRE(certificate.MatchesUnit(
            nullptr, function, allocation, allocation.Actions(), config, features));
    arm64::JitContext context{module, allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
    REQUIRE(translator.Translate(function));
    REQUIRE(context.CurrentBufferSize() > 0);
}

TEST_CASE("whole region certificate rejects changed structural fields before entry emission", "[region-plan][regression]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    for (const unsigned mutation : {2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u, 11u}) {
        CAPTURE(mutation);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.region_edges = true;
        AddressSpace space{config}; auto module = space.GetDefaultModule();
        const auto features = ResolveFeatureSet(module->GetModuleConfig());
        HIRBuilder builder{1, true, features};
        auto* function = builder.AppendFunction(Location{0xfb00}, Location{0xfb40});
        function->AdvancePC(Imm{u64{1}});
        auto* first = &function->EditBlock(function->GetCurrentBlock());
        auto* second = builder.LinkBlock(terminal::LinkBlock{Location{0xfb10}});
        builder.SetCurBlock(second); function->AdvancePC(Imm{u64{1}});
        builder.LinkBlock(terminal::LinkBlock{Location{0xfb00}});
        // An unreachable empty member is still part of the whole-unit proof.
        auto* empty = function->AppendBlock(Location{0xfb30});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        AllocationFixture allocation{function->MaxInstrCount(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateFunction(
                function,
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        function, allocation, features));
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        const auto region = allocation.Actions().Certificate();
        REQUIRE(region);
        REQUIRE(region->MatchesUnit(
                nullptr, function, allocation, allocation.Actions(), config, features));
        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        REQUIRE(context.CurrentBufferSize() == 0);
        switch (mutation) {
            case 0:
                compile::RewriteContext{function, compilation_workspace, false}.SetArg(
                        second->GetBlock(), &second->GetInstList().front(), 0, Imm{u64{2}});
                break;
            case 1: second->GetBlock()->SetTerminal(terminal::LinkBlock{Location{0xfb80}}); break;
        case 2:
            REQUIRE_FALSE(second->GetPredecessors().empty());
            HIRTestSupport::SetPredecessor(second, 0, second);
            break;
        case 3:
            REQUIRE_FALSE(first->GetSuccessors().empty());
            HIRTestSupport::SetSuccessor(first, 0, first);
            break;
        case 4:
            HIRTestSupport::SetRPO(function, {});
            break;
        case 5: function->RegisterExternalEntryRoot(second); break;
        case 6:
            function->SetCurBlock(second);
            function->RegisterExternalDirectLink(Location{0xfb80});
            break;
        case 7:
            function->SetCurBlock(second);
            function->RegisterCallReturn(Location{0xfb10});
            break;
        case 8: empty->GetBlock()->AdvancePC(Imm{u64{1}}); break;
        case 9: second->GetBlock()->SetEndLocation(Location{0xfb11}); break;
        case 10: allocation.BeginGPRLocationAnalysis(); break;
        case 11: allocation.ResetAllocations(); break;
        }
        REQUIRE_FALSE(region->MatchesUnit(
                nullptr, function, allocation, allocation.Actions(), config, features));
        REQUIRE_FALSE(translator.Translate(function));
        REQUIRE(context.CurrentBufferSize() == 0);
    }
}

TEST_CASE("stale standalone region plan cannot take the initial Require compatibility path", "[region-plan][regression]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    IntrusivePtr<Block> block{new Block(0, Location{0xfd00})};
    block->AdvancePC(Imm{u64{1}}); block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
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
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    const auto plan = allocation.Actions().ForBlock(block.get());
    const auto certificate = *allocation.Actions().Certificate();
    REQUIRE(certificate.valid);
    auto changed_config = config; changed_config.region_edges = !config.region_edges;
    REQUIRE_FALSE(certificate.MatchesUnit(
            block.get(), nullptr, allocation, allocation.Actions(), changed_config, features));
    auto changed_features = features; changed_features.helper_leaf_abi = !features.helper_leaf_abi;
    REQUIRE_FALSE(certificate.MatchesUnit(
            block.get(), nullptr, allocation, allocation.Actions(), config, changed_features));
    arm64::JitContext context{module, allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
    allocation.SetAllocationUnit(block.get(), nullptr);
    REQUIRE_FALSE(certificate.MatchesUnit(
            block.get(), nullptr, allocation, allocation.Actions(), config, features));
    REQUIRE_FALSE(translator.Translate(block.get()));
    REQUIRE_FALSE(allocation.Actions().Ready());
    REQUIRE(context.CurrentBufferSize() == 0);
}

TEST_CASE("prepared regions reject retained range writers before emission",
          "[region-plan][regression]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.region_edges = true; AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder first_builder{1, true, features}, second_builder{1, true, features};
    const auto make_function = [&](HIRBuilder& builder, u64 start) {
        auto* function = builder.AppendFunction(Location{start}, Location{start + 16});
        function->AdvancePC(Imm{u64{1}}); function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        return function;
    };
    auto* first = make_function(first_builder, 0xfe00);
    auto* second = make_function(second_builder, 0xfe20);
    auto* retained = second->EditBlock(&second->GetHIRBlocksRPO().front()).GetBlock();
    const std::array<HIRFunction*, 2> inputs{first, second};
    auto prepared = arm64::PrepareFunctions(module, inputs);
    REQUIRE(prepared.has_value());
    const auto end = retained->GetEndLocation();
    REQUIRE_THROWS_AS(retained->SetEndLocation(Location{0xfe2f}), std::logic_error);
    REQUIRE(retained->GetEndLocation() == end);
    arm64::FunctionCodeObjectEmitter emitter{module, *prepared};
    REQUIRE(emitter.Emit());
    REQUIRE(emitter.CurrentBufferSize() > 0);
    REQUIRE(space.GetCodeCache(0xfe00) == nullptr);
    REQUIRE(space.GetCodeCache(0xfe20) == nullptr);
}

TEST_CASE("region preparation rejects a foreign CFG predecessor before reading its IR", "[region-plan][regression]") {
    const unsigned foreign_blocks = GENERATE(1u, 8u);
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64}; config.region_edges = true;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    HIRBuilder builder{1, true, features}, foreign_builder{1, true, features};
    auto* foreign = foreign_builder.AppendFunction(Location{0xfec0}, Location{0xfed0});
    auto* foreign_block = &foreign->EditBlock(foreign->GetCurrentBlock());
    foreign->EndBlock(terminal::ReturnToHost{});
    for (unsigned i = 1; i < foreign_blocks; ++i)
        foreign_block = foreign->AppendBlock(Location{0xfed0 + i});
    auto* function = builder.AppendFunction(Location{0xfe80}, Location{0xfea0});
    function->AdvancePC(Imm{u64{1}});
    auto* first = &function->EditBlock(function->GetCurrentBlock());
    auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0xfe90}});
    builder.SetCurBlock(next); function->EndBlock(terminal::ReturnToHost{});
    HIRTestSupport::InsertForeignEdge(function, foreign_block, next);
    function->EndFunction(); foreign->EndFunction(); function->ComputeRPO(); function->IdByRPO();
    AllocationFixture allocation{function->MaxInstrCount(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    ::swift::runtime::compile::AllocateFunction(
            function,
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, allocation, features));
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    const auto plan = allocation.Actions().ForBlock(first->GetBlock());
    const auto certificate = *allocation.Actions().Certificate();
    REQUIRE_FALSE(certificate.valid);
    arm64::JitContext context{module, allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
    REQUIRE_FALSE(translator.Translate(function)); REQUIRE(context.CurrentBufferSize() == 0);
}

TEST_CASE("region roster rejects duplicate identities and null CFG edges", "[region-plan][regression]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64}; config.region_edges = true;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig()); HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0xfe40}, Location{0xfe60});
    function->AdvancePC(Imm{u64{1}});
    auto* first = &function->EditBlock(function->GetCurrentBlock());
    auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0xfe50}});
    builder.SetCurBlock(next); function->EndBlock(terminal::ReturnToHost{});
    auto* empty = function->AppendBlock(Location{0xfe5f});
    function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
    AllocationFixture allocation{function->MaxInstrCount(),
                                 space.GetTrampolines().GetGPRRegs(),
                                 space.GetTrampolines().GetFPRRegs(),
                                 features};
    ::swift::runtime::compile::AllocateFunction(
            function,
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, allocation, features));
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
    const auto build = [&] {
        return arm64::CaptureUnitCertificate(
                nullptr, function, allocation, allocation.Actions(), config, features);
    };
    REQUIRE(build().valid);
    const auto members = function->GetHIRBlocks();
    const auto empty_id = empty->GetOrderId(); REQUIRE(members[empty_id] == empty);
    HIRTestSupport::SetMember(function, empty_id, first);
    REQUIRE_FALSE(build().valid);
    HIRTestSupport::SetMember(function, empty_id, empty);
    REQUIRE(build().valid);
    REQUIRE_FALSE(next->GetPredecessors().empty());
    auto* predecessor = next->GetPredecessors().front();
    HIRTestSupport::SetPredecessor(next, 0, nullptr);
    REQUIRE_FALSE(build().valid);
    HIRTestSupport::SetPredecessor(next, 0, predecessor);
    REQUIRE(build().valid);
    const auto page_size = static_cast<size_t>(getpagesize());
    auto* protected_page = mmap(nullptr, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(protected_page != MAP_FAILED);
    const auto release_page = ScopeExit([&] { munmap(protected_page, page_size); });
    const auto* opaque = static_cast<const HIRBlock*>(protected_page);
    HIRTestSupport::SetPredecessor(next, 0, opaque);
    REQUIRE_FALSE(build().valid);
    HIRTestSupport::SetPredecessor(next, 0, predecessor);
    REQUIRE(build().valid);
    const auto* successor = first->GetSuccessors().front();
    HIRTestSupport::SetSuccessor(first, 0, opaque);
    REQUIRE_FALSE(build().valid);
    HIRTestSupport::SetSuccessor(first, 0, successor);
    REQUIRE(build().valid);
    REQUIRE(allocation.Actions().Certificate()->MatchesUnit(
            nullptr, function, allocation, allocation.Actions(), config, features));
}

TEST_CASE("merged physical IR ownership preserves each region's current HIR membership", "[region-plan][regression]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64}; config.region_edges = true;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig()); HIRBuilder builder{2, true, features};
    const auto make = [&](u64 start) {
        auto* function = builder.AppendFunction(Location{start}, Location{start + 16});
        function->AdvancePC(Imm{u64{1}}); function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO(); return function;
    };
    auto* first = make(0xfd80); auto* second = make(0xfda0);
    const std::array<HIRFunction*, 2> inputs{first, second};
    auto prepared = arm64::PrepareFunctions(module, inputs);
    REQUIRE(prepared.has_value());
    REQUIRE(second->GetFunction()->GetBlocks().empty());
    for (const auto& region : *prepared) {
        const auto* function = region.Function();
        const auto& allocation = region.Allocation();
        const auto* block = function->GetHIRBlocksRPO().front().GetBlock();
        const auto plan = region.Actions().Certificate();
        REQUIRE(plan->valid);
        REQUIRE(plan->MatchesUnit(
                nullptr, function, allocation, region.Actions(), config, features));
    }
    arm64::FunctionCodeObjectEmitter emitter{module, *prepared};
    REQUIRE(emitter.Emit(false)); REQUIRE(emitter.CurrentBufferSize() > 0);
}

TEST_CASE("restoring exact region inputs restores the frozen certificate without reallocation", "[region-plan][regression]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    for (const unsigned field : {1u, 2u, 3u}) {
        CAPTURE(field);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64}; config.region_edges = true;
        AddressSpace space{config}; auto module = space.GetDefaultModule();
        const auto features = ResolveFeatureSet(module->GetModuleConfig()); HIRBuilder builder{1, true, features};
        auto* function = builder.AppendFunction(Location{0xff00}, Location{0xff20});
        function->AdvancePC(Imm{u64{1}});
        auto* first = &function->EditBlock(function->GetCurrentBlock());
        function->EndBlock(terminal::ReturnToHost{});
        const FunctionEntryProvenance entry{.target = Location{0xff00}, .owner_start = Location{0xff00},
                .owner_end = Location{0xff20}, .dependencies = {{Location{0xff00}, Location{0xff01}}},
                .disposition = FunctionEntryDisposition::Accepted, .external_root = true};
        function->SetFunctionEntryProvenance({entry});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        AllocationFixture allocation{function->MaxInstrCount(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateFunction(
                function,
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        function, allocation, features));
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        const auto region = allocation.Actions().Certificate();
        const auto attempt = allocation.AllocationAttempt(), generation = allocation.GPRLocationGeneration();
        const auto root_location = function->GetFunction()->GetStartLocation();
        const auto root_size = function->GetFunction()->GetNodeSize();
        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        switch (field) {
            case 0:
                compile::RewriteContext{function, compilation_workspace, false}.SetArg(
                        first->GetBlock(), &first->GetInstList().front(), 0, Imm{u64{2}});
                break;
            case 1:
                HIRTestSupport::SetStartLocation(function->GetFunction(), Location{0xff40});
                break;
            case 2: function->GetFunction()->SetEndLocation(Location{0xff21}); break;
        case 3: {
            auto changed = entry; changed.dependencies[0].end = Location{0xff02};
            function->SetFunctionEntryProvenance({changed}); break;
        }
        }
        REQUIRE_FALSE(translator.Translate(function)); REQUIRE(context.CurrentBufferSize() == 0);
        compile::RewriteContext{function, compilation_workspace, false}.SetArg(
                first->GetBlock(), &first->GetInstList().front(), 0, Imm{u64{1}});
        HIRTestSupport::SetStartLocation(function->GetFunction(), root_location);
        function->GetFunction()->SetEndLocation(root_location + root_size);
        function->SetFunctionEntryProvenance({entry});
        REQUIRE(region->MatchesUnit(
                nullptr, function, allocation, allocation.Actions(), config, features));
        REQUIRE(allocation.AllocationAttempt() == attempt); REQUIRE(allocation.GPRLocationGeneration() == generation);
        REQUIRE(translator.Translate(function)); REQUIRE(context.CurrentBufferSize() > 0);
    }
}

TEST_CASE("region unit certificate includes complete external entry provenance", "[region-plan][regression]") {
    for (const unsigned field : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u}) {
        CAPTURE(field);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64}; config.region_edges = true;
        AddressSpace space{config}; auto module = space.GetDefaultModule();
        const auto features = ResolveFeatureSet(module->GetModuleConfig()); HIRBuilder builder{1, true, features};
        auto* function = builder.AppendFunction(Location{0xfc00}, Location{0xfc30});
        function->AdvancePC(Imm{u64{1}});
        auto* first = &function->EditBlock(function->GetCurrentBlock());
        auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0xfc10}});
        builder.SetCurBlock(next); function->EndBlock(terminal::ReturnToHost{});
        FunctionEntryProvenance entry{.target = Location{0xfc10}, .owner_start = Location{0xfc00},
                .owner_end = Location{0xfc30}, .dependencies = {{Location{0xfc10}, Location{0xfc11}}},
                .disposition = FunctionEntryDisposition::Accepted, .external_root = true};
        function->SetFunctionEntryProvenance({entry});
        function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
        AllocationFixture allocation{function->MaxInstrCount(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateFunction(
                function,
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        function, allocation, features));
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        const auto region = allocation.Actions().Certificate();
        REQUIRE(region);
        REQUIRE(region->MatchesUnit(
                nullptr, function, allocation, allocation.Actions(), config, features));
        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        switch (field) {
        case 0: entry.target = Location{0xfc20}; break;
        case 1: entry.owner_start = Location{0xfc01}; break;
        case 2: entry.owner_end = Location{0xfc31}; break;
        case 3: entry.dependencies[0].start = Location{0xfc0f}; break;
        case 4: entry.dependencies[0].end = Location{0xfc12}; break;
        case 5: entry.disposition = FunctionEntryDisposition::Rejected; break;
        case 6: entry.rejection = FunctionEntryRejection::BoundaryMismatch; break;
        case 7: entry.call_return_owned = true; break;
        case 8: entry.call_return_boundary = true; break;
        case 9: entry.external_root = false; break;
        }
        function->SetFunctionEntryProvenance({entry});
        REQUIRE_FALSE(region->MatchesUnit(
                nullptr, function, allocation, allocation.Actions(), config, features));
        REQUIRE_FALSE(translator.Translate(function)); REQUIRE(context.CurrentBufferSize() == 0);
    }
}

std::atomic_bool g_region_fault_armed{};
std::atomic_bool g_region_fault_done{};
std::atomic<u64> g_region_loop_entries{};
std::atomic<u64> g_region_selector{};
std::atomic<u8*> g_region_fault_address{};

u64 RegionSelector() {
    return g_region_selector.fetch_add(1, std::memory_order_seq_cst) & 1u;
}

u64 RegionLoopCheck() {
    g_region_loop_entries.fetch_add(1, std::memory_order_seq_cst);
    if (g_region_fault_armed.exchange(false, std::memory_order_seq_cst)) {
        auto* address = g_region_fault_address.load(std::memory_order_acquire);
        const auto value = *static_cast<volatile u8*>(address);
        *static_cast<volatile u8*>(address) = static_cast<u8>(value ^ 1u);
        g_region_fault_done.store(true, std::memory_order_seq_cst);
    }
    return 0;
}

template <typename Predicate>
bool WaitRegion(Predicate&& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

struct RegionFunction {
    HIRBuilder builder{1, true, swift::runtime::FeatureSet{}};
    HIRFunction* function{};
};

std::unique_ptr<RegionFunction> BuildRegionLoop(VAddr a, VAddr b, VAddr c) {
    auto result = std::make_unique<RegionFunction>();
    result->function = result->builder.AppendFunction(Location{a}, Location{c + 1});
    auto* function = result->function;
    function->AdvancePC(Imm{u64{1}});
    const auto selector = function->CallLambda(
            ValueType::U64,
            Lambda{Imm{static_cast<u64>(reinterpret_cast<uintptr_t>(FptrCast(&RegionSelector)))}});
    const auto condition = function->TestNotZero(selector);
    auto [else_block, then_block] = result->builder.If(terminal::If{
            condition,
            terminal::LinkBlock{Location{b}},
            terminal::LinkBlock{Location{c}},
    });

    result->builder.SetCurBlock(then_block);
    function->AdvancePC(Imm{u64{1}});
    (void)function->CallLambda(
            ValueType::U64,
            Lambda{Imm{static_cast<u64>(reinterpret_cast<uintptr_t>(FptrCast(&RegionLoopCheck)))}});
    result->builder.LinkBlock(terminal::LinkBlock{Location{a}});

    result->builder.SetCurBlock(else_block);
    function->AdvancePC(Imm{u64{1}});
    result->builder.LinkBlock(terminal::LinkBlock{Location{a}});
    function->EndFunction();
    return result;
}

std::unique_ptr<RegionFunction> BuildMixedRegion(VAddr a,
                                                 VAddr internal,
                                                 VAddr external) {
    auto result = std::make_unique<RegionFunction>();
    result->function =
            result->builder.AppendFunction(Location{a}, Location{internal + 1});
    auto* function = result->function;
    function->AdvancePC(Imm{u64{1}});
    const auto selector = function->CallLambda(
            ValueType::U64,
            Lambda{Imm{static_cast<u64>(reinterpret_cast<uintptr_t>(FptrCast(&RegionSelector)))}});
    const auto condition = function->TestNotZero(selector);
    auto [external_block, internal_block] = result->builder.If(terminal::If{
            condition,
            terminal::LinkBlock{Location{internal}},
            terminal::LinkBlock{Location{external}},
    });

    result->builder.SetCurBlock(internal_block);
    function->AdvancePC(Imm{u64{1}});
    result->builder.ReturnToHost();
    (void)external_block;
    function->EndFunction();
    return result;
}

IntrusivePtr<Block> BuildRegionReplacement(VAddr guest, u64 fingerprint) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    const auto value = block->LoadImm(ValueType::U64, Imm{fingerprint});
    block->StoreUniform(Uniform{0, ValueType::U64}, value);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    return block;
}

}  // namespace

TEST_CASE("region local cycle exits boundedly on whole-function SMC invalidation",
          "[region-edges][production][smc]") {
#if defined(__aarch64__)
    const bool same_thread_fault = GENERATE(false, true);
    DYNAMIC_SECTION("faulting runtime thread=" << same_thread_fault) {
        const size_t page_size = static_cast<size_t>(getpagesize());
        const size_t guest_size = 4 * page_size;
        void* guest_memory = mmap(nullptr,
                                  guest_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANON,
                                  -1,
                                  0);
        REQUIRE(guest_memory != MAP_FAILED);
        {
            const VAddr guest_a = page_size + 0x100;
            const VAddr guest_b = page_size + 0x180;
            const VAddr guest_c = page_size + 0x200;
            Config config{
                    .loc_start = 0,
                    .loc_end = guest_size,
                    .enable_jit = true,
                    .enable_asm_interp = false,
                    .has_local_operation = false,
                    .backend_isa = kArm64,
                    .uniform_buffer_size = 64,
                    .global_opts = Optimizations::BlockLink |
                                   Optimizations::FunctionBaseCompile,
                    .region_edges = true,
                    .memory_base = guest_memory,
                    .guest_addr_mask = guest_size - 1,
            };
            AddressSpace space{config};
            space.GetSmcTracker().EnableMultithreading();
            auto module = space.GetDefaultModule();
            auto region = BuildRegionLoop(guest_a, guest_b, guest_c);
            REQUIRE(TranslateIR(module, region->function) != nullptr);

            // 三个 external entry 都发布，但 region 内边不登记 direct-link site。
            REQUIRE(space.GetCodeCache(guest_a) != nullptr);
            REQUIRE(space.GetCodeCache(guest_b) != nullptr);
            REQUIRE(space.GetCodeCache(guest_c) != nullptr);
            REQUIRE(space.GetLinkManager().GetStats().sites == 0);

            g_region_selector.store(0, std::memory_order_seq_cst);
            g_region_loop_entries.store(0, std::memory_order_seq_cst);
            g_region_fault_done.store(false, std::memory_order_seq_cst);
            g_region_fault_armed.store(same_thread_fault, std::memory_order_seq_cst);
            g_region_fault_address.store(
                    static_cast<u8*>(guest_memory) + guest_b,
                    std::memory_order_release);

            std::atomic_bool runner_done{};
            std::atomic<u32> runner_halt{};
            std::thread runner([&] {
                Runtime runtime{&space};
                runtime.SetLocation(guest_a);
                runner_halt.store(static_cast<u32>(runtime.Run()),
                                  std::memory_order_release);
                runner_done.store(true, std::memory_order_release);
            });

            if (!same_thread_fault) {
                REQUIRE(WaitRegion(
                        [] {
                            return g_region_loop_entries.load(
                                           std::memory_order_seq_cst) > 32;
                        },
                        std::chrono::seconds(2)));
                std::thread writer([&] {
                    Runtime writer_runtime{&space};
                    auto* address = static_cast<volatile u8*>(guest_memory) + guest_b;
                    const auto value = *address;
                    *address = static_cast<u8>(value ^ 1u);
                    g_region_fault_done.store(true, std::memory_order_seq_cst);
                });
                writer.join();
            }

            REQUIRE(WaitRegion(
                    [] { return g_region_fault_done.load(std::memory_order_seq_cst); },
                    std::chrono::seconds(2)));
            const u64 entries_after_fault =
                    g_region_loop_entries.load(std::memory_order_seq_cst);
            const bool bounded = WaitRegion(
                    [&] { return runner_done.load(std::memory_order_acquire); },
                    std::chrono::seconds(2));
            if (!bounded) {
                space.InvalidateCodeRange(guest_b, guest_b + 1);
                REQUIRE(WaitRegion(
                        [&] { return runner_done.load(std::memory_order_acquire); },
                        std::chrono::seconds(2)));
            }
            runner.join();
            REQUIRE(bounded);
            REQUIRE(runner_halt.load(std::memory_order_acquire) ==
                    static_cast<u32>(HaltReason::CodeMiss));
            // Bounded stop: forward edges no longer poll, so after the
            // fault the runner may execute the one in-flight path to the
            // next backedge poll, entering the counting block at most
            // once more before it exits.
            const u64 final_entries =
                    g_region_loop_entries.load(std::memory_order_seq_cst);
            REQUIRE(final_entries >= entries_after_fault);
            REQUIRE(final_entries <= entries_after_fault + 1);

            // 同一 function node 的任一页失效都清除三个 external entry。
            REQUIRE(space.GetCodeCache(guest_a) == nullptr);
            REQUIRE(space.GetCodeCache(guest_b) == nullptr);
            REQUIRE(space.GetCodeCache(guest_c) == nullptr);

            constexpr u64 kReplacement = 0xC1C1E5AFEull;
            auto replacement = BuildRegionReplacement(guest_a, kReplacement);
            auto* replacement_code = TranslateIR(module, replacement);
            REQUIRE(replacement_code != nullptr);
            space.PushCodeCache(Location{guest_a}, replacement_code);
            Runtime verify{&space};
            verify.SetLocation(guest_a);
            REQUIRE(verify.Run() == HaltReason::CallHost);
            u64 observed{};
            std::memcpy(&observed, verify.GetUniformBuffer().data(), sizeof(observed));
            REQUIRE(observed == kReplacement);
        }
        REQUIRE(munmap(guest_memory, guest_size) == 0);
    }
#else
    SUCCEED("region edge execution requires an AArch64 host");
#endif
}

TEST_CASE("region localizes only targets emitted in the same unit",
          "[region-edges][production][fallback]") {
#if defined(__aarch64__)
    const size_t page_size = static_cast<size_t>(getpagesize());
    const size_t guest_size = 2 * page_size;
    void* guest_memory = mmap(nullptr,
                              guest_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON,
                              -1,
                              0);
    REQUIRE(guest_memory != MAP_FAILED);
    {
        const VAddr guest_a = 0x100;
        const VAddr guest_internal = 0x180;
        const VAddr guest_external = page_size + 0x100;
        Config config{
                .loc_start = 0,
                .loc_end = guest_size,
                .enable_jit = true,
                .enable_asm_interp = false,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .uniform_buffer_size = 64,
                .global_opts = Optimizations::BlockLink |
                               Optimizations::FunctionBaseCompile,
                .region_edges = true,
                .memory_base = guest_memory,
                .guest_addr_mask = guest_size - 1,
        };
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        auto region = BuildMixedRegion(guest_a, guest_internal, guest_external);
        REQUIRE(TranslateIR(module, region->function) != nullptr);

        // 同 unit 的入口直接走本地标签；未发射的目标继续登记外部 direct-link site。
        REQUIRE(space.GetCodeCache(guest_a) != nullptr);
        REQUIRE(space.GetCodeCache(guest_internal) != nullptr);
        REQUIRE(space.GetCodeCache(guest_external) == nullptr);
        REQUIRE(space.GetLinkManager().GetStats().sites == 1);
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("region edge execution requires an AArch64 host");
#endif
}
