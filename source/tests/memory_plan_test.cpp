#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/backend/arm64/compile/residences.h"
#include "runtime/compile/workspace.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <string>
#include "runtime/common/svm_config.h"
#include <catch2/catch_test_macros.hpp>
#include "base/scope_exit.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/memory_preparation.h"
#if defined(__aarch64__) && defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
const arm64::MemoryAction* MemoryFor(const arm64::UnitActions& actions,
                                     const Inst* inst,
                                     arm64::MemoryAction::Flag flag) {
    const auto* row = inst ? actions.Memory(inst->Id()) : nullptr;
    return row && row->Has(flag) ? row : nullptr;
}
size_t CountActions(const arm64::MemoryActions& lowered, arm64::MemoryAction::Flag flag) {
    return std::ranges::count_if(lowered.actions,
                                 [flag](const auto& row) { return row.Has(flag); });
}
struct Fixture {
    IntrusivePtr<Block> block{new Block(0, Location{0xba00})};
    Value address, loaded, updated;
    const Inst* memory{};
    explicit Fixture(bool flags = false, bool live_old = false) {
        address = block->LoadUniform(ValueType::U64, Uniform{2048, ValueType::U64});
        loaded = block->LoadMemory(ValueType::U64, Operand{address});
        memory = loaded.Def();
        updated = block->Add(ValueType::U64, address, Operand{Imm{swift::u64{8}}});
        if (flags) block->SaveFlags(updated, Flags::Carry | Flags::Overflow);
        block->StoreUniform(Uniform{2056, ValueType::U64}, loaded);
        block->StoreUniform(Uniform{2064, ValueType::U64}, updated);
        if (live_old) block->StoreUniform(Uniform{2072, ValueType::U64}, address);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
    }
};
Config Configuration() {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    config.uniform_buffer_size = 4096; config.stack_alignment = 16;
    return config;
}

TEST_CASE("memory writeback contracts preserve observation windows offsets and prior coverage", "[memory-plan]") {
    for (unsigned scenario = 0; scenario < 28; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xba20})};
        const auto stored = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        Value delta;
        if (scenario == 24 || scenario == 25)
            delta = block->LoadUniform(
                    scenario == 25 ? ValueType::U32 : ValueType::U64,
                    Uniform{48, scenario == 25 ? ValueType::U32 : ValueType::U64});
        auto address = block->LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
        Value address_owner;
        if (scenario == 27) {
            address_owner = address;
            address = block->BitCast(ValueType::U64, address);
        }
        if (scenario == 23) block->UniformBarrier();
        const bool store = scenario == 1 || scenario == 3 || scenario == 5;
        const bool tso = scenario == 2 || scenario == 3 || scenario == 4 || scenario == 5;
        Value loaded;
        const Inst* memory;
        if (store) {
            if (tso) block->StoreMemoryTSO(Operand{address}, stored);
            else block->StoreMemory(Operand{address}, stored);
            memory = &block->GetInstList().back();
        } else {
            loaded = (tso ? block->LoadMemoryTSO(scenario == 7 ? ValueType::V128 : ValueType::U64,
                                                 Operand{address})
                          : block->LoadMemory(scenario == 7 ? ValueType::V128 : ValueType::U64,
                                              Operand{address}));
            memory = loaded.Def();
        }
        if (scenario == 10) block->UniformBarrier();
        if (scenario == 11) {
            const auto label = block->Goto(block->CondSet(Cond::EQ));
            block->BindLabel(label);
        }
        if (scenario == 12) (void)block->LoadMemory(ValueType::U64, Operand{Imm{swift::u64{0}}});
        if (scenario == 26) delta = block->LoadUniform(ValueType::U64, Uniform{48, ValueType::U64});
        const swift::s64 offset = scenario == 13   ? 256
                                  : scenario == 14 ? -257
                                  : scenario == 15 ? 255
                                  : scenario == 16 ? -256
                                  : scenario == 17 ? std::numeric_limits<swift::s64>::min()
                                                   : 8;
        const auto operand_delta = delta.Defined() ? Operand{delta} : Operand{Imm{offset}};
        const auto updated = (scenario == 17 ? block->Sub(ValueType::U64, address, operand_delta)
                                             : block->Add(ValueType::U64, address, operand_delta));
        if (scenario == 8) block->SaveFlags(updated, Flags::Carry | Flags::Overflow);
        if (loaded.Defined()) block->StoreUniform(Uniform{16, loaded.Type()}, loaded);
        block->StoreUniform(Uniform{32, ValueType::U64}, updated);
        if (scenario == 9) block->StoreUniform(Uniform{40, ValueType::U64}, address);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(stored.Id(), HostGPR{11});
        allocation.MapRegister(address.Id(), HostGPR{13});
        if (address_owner.Defined()) allocation.MapRegister(address_owner.Id(), HostGPR{13});
        if (delta.Defined()) allocation.MapRegister(delta.Id(), HostGPR{12});
        allocation.MapRegister(updated.Id(), HostGPR{static_cast<swift::u16>(scenario == 19 ? 14 : 13)});
        if (loaded.Defined()) allocation.MapRegister(loaded.Id(), HostGPR{static_cast<swift::u16>(scenario == 18 ? 13 : 8)});
        compile::BlockAnalysisIndex index;
        index.Build(block.get());
        compile::CompileWorkspace workspace;
        arm64::MemorySelectionBuilder memory_builder{workspace};
        memory_builder.Reset(index);
        if (scenario == 20)
            memory_builder.Record(
                    {memory->Id(), UINT16_MAX, 0, 0, arm64::MemoryTransferAction::Load});
        if (scenario == 21)
            memory_builder.Record(
                    {address.Id(), UINT16_MAX, 0, 0, arm64::MemoryTransferAction::PreIndex});
        const auto earlier = memory_builder.Finish();
        const arm64::MemoryOptions options{.biased = scenario == 6,
                                           .rcpc = scenario == 4 || scenario == 5};
        const auto shared = arm64::PrepareMemory(
                block.get(), allocation, FeatureSet{}, earlier, {}, options, index);
        compile::BlockAnalysisIndex independent;
        independent.Build(block.get());
        const auto own = arm64::PrepareMemory(
                block.get(), allocation, FeatureSet{}, earlier, {}, options, independent);
        const bool accepted = scenario <= 3 || scenario == 15 || scenario == 16 || scenario == 22 || scenario == 27;
        REQUIRE((shared.Find(memory->Id(), arm64::MemoryAction::PostIndex) != nullptr) == accepted);
        REQUIRE(shared.covered == own.covered);
        REQUIRE(shared.actions == own.actions);
        REQUIRE(shared.writes == own.writes);
        REQUIRE(shared.writes.size() == own.writes.size());
        REQUIRE(shared.Find(UINT32_MAX) == nullptr);
        if (accepted) {
            const auto& action = (*shared.Find(memory->Id(), arm64::MemoryAction::PostIndex));
            REQUIRE(shared.writes.front().update == updated.Id());
            REQUIRE(action.base == 13);
            REQUIRE(action.offset == offset);
            REQUIRE(action.access_bytes == 8);
            REQUIRE(shared.writes.front().consumed_value ==
                    (address_owner.Defined() ? address_owner.Id() : address.Id()));
        } else
            REQUIRE(std::find(shared.covered.begin(), shared.covered.end(), updated.Id()) ==
                    shared.covered.end());
    }
}
}

TEST_CASE("memory plans execute post-index updates at the memory instruction", "[memory-plan]") {
    AddressSpace space{Configuration()}; auto module = space.GetDefaultModule();
    Fixture item;
    AllocationFixture alloc{item.block->MaxInstrId(),
                            space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(),
                            FeatureSet{}};
    ::swift::runtime::compile::AllocateBlock(
            item.block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    item.block.get(), alloc, FeatureSet{}),
            false);
    alloc.MapRegister(item.address.Id(), HostGPR{13});
    alloc.MapRegister(item.updated.Id(), HostGPR{13});
    alloc.MapRegister(item.loaded.Id(), HostGPR{8});
    auto active = space.GetTrampolines().GetGPRRegs(); active.Mark(13); active.Mark(8);
    auto active_fprs = space.GetTrampolines().GetFPRRegs();
    for (auto& inst : item.block->GetInstList())
        alloc.SetActiveRegs(inst.Id(), active, active_fprs);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    const auto plan = alloc.Actions().ForBlock(item.block.get());
    const auto& memory_actions = alloc.Actions();
    REQUIRE(plan);
    REQUIRE(MemoryFor(memory_actions, item.memory, arm64::MemoryAction::PostIndex));
    REQUIRE(memory_actions.InstructionCovered(item.updated.Id()));
    REQUIRE((*MemoryFor(memory_actions, item.memory, arm64::MemoryAction::PostIndex)).offset == 8);
    arm64::JitTranslator translator{context}; translator.Translate(item.block.get()); context.Finish();
#if defined(__aarch64__)
    const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
    REQUIRE(id != INVALID_CACHE_ID); context.Flush(buffer);
    REQUIRE(module->Push(item.block.get()));
    space.PushCodeCache(item.block->GetStartLocation(), buffer.exec_data);
    std::array<swift::u64, 2> data{0x1122334455667788, 0x8877665544332211};
    const auto pointer = reinterpret_cast<uintptr_t>(data.data());
    Runtime runtime{&space};
    std::memcpy(runtime.GetUniformBuffer().data() + 2048, &pointer, sizeof(pointer));
    runtime.SetLocation(item.block->GetStartLocation().Value());
    REQUIRE(runtime.Run() == HaltReason::CallHost);
    swift::u64 loaded{}, updated{};
    std::memcpy(&loaded, runtime.GetUniformBuffer().data() + 2056, sizeof(loaded));
    std::memcpy(&updated, runtime.GetUniformBuffer().data() + 2064, sizeof(updated));
    REQUIRE(loaded == data[0]); REQUIRE(updated == pointer + 8);
#endif
}

TEST_CASE("TSO compound addresses reuse completed flag-observer scratch", "[memory-plan][preobserver-scratch]") {
    const char* old_flags = GetRawSvmConfigEnvForTest("SVM_FLAGS_REGS");
    const bool had_flags = old_flags != nullptr;
    const std::string saved_flags = old_flags ? old_flags : "";
    const auto restore = swift::ScopeExit([&] {
        if (had_flags) SetSvmConfigEnvForTest("SVM_FLAGS_REGS", saved_flags.c_str(), 1);
        else UnsetSvmConfigEnvForTest("SVM_FLAGS_REGS");
    });
    REQUIRE(SetSvmConfigEnvForTest("SVM_FLAGS_REGS", "1", 1) == 0);
    // Load, register store, constant store, and a store whose source is spilled.
    for (unsigned kind = 0; kind < 4; ++kind) DYNAMIC_SECTION("source kind " << kind) {
        auto config = Configuration();
        config.arm64_features = Arm64Features::RCpc | Arm64Features::RCpcImm;
        config.rcpc_fault_emulate = false;
        config.tso_mode = TsoMode::AcqRel;
        AddressSpace space{config};
        ModuleConfig module_config{};
        module_config.feature_overrides.Set(FeatureId::jit_scratch_xpool, true);
        auto module = space.MapModule(0xbc00, 0xbd00, module_config);
        REQUIRE(module);
        const auto features = ResolveFeatureSet(module_config);
        REQUIRE(features.jit_scratch_xpool);
        IntrusivePtr<Block> block{new Block(0, Location{0xbc00})};
        const auto base = block->LoadUniform(ValueType::U64, Uniform{2048, ValueType::U64});
        constexpr swift::u64 stored_bits = UINT64_C(0x8877665544332211);
        Value stored;
        if (kind) stored = kind == 2
                ? block->LoadImm(ValueType::U64, Imm{stored_bits})
                : block->LoadUniform(ValueType::U64, Uniform{2056, ValueType::U64});
        const auto left = block->LoadUniform(ValueType::U64, Uniform{2064, ValueType::U64});
        const auto right = block->LoadUniform(ValueType::U64, Uniform{2072, ValueType::U64});
        const auto sum = block->Add(ValueType::U64, left, Operand{right});
        block->SaveFlags(sum, Flags::All);
        block->ClearFlags(Flags::Carry);
        // No AdvancePC: MayFault observes the still-queued clear. The unknown
        // base and large displacement require address, immediate and span work.
        constexpr swift::u64 displacement = UINT64_C(0x100000000);
        const Operand address{base, Imm{displacement}, OperandPlus};
        Value result;
        if (kind) {
            block->StoreMemoryTSO(address, stored);
            result = stored;
        } else result = block->LoadMemoryTSO(ValueType::U64, address);
        block->StoreUniform(Uniform{2080, ValueType::U64}, result);
        const auto flags = block->GetFlags(ValueType::U64, sum, Flags::All);
        block->StoreUniform(Uniform{2088, ValueType::U64}, flags);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                                    space.GetTrampolines().GetFPRRegs(), features};
        compile::AllocateBlock(block.get(), &allocation,
                arm64::PrepareAllocationInputs(block.get(), allocation, features), false);
        if (kind == 3) allocation.MapMemSpill(stored.Id(), SpillSlot{2});
        arm64::CompleteARM64Allocation(module, allocation);
        arm64::JitContext context{module, allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        REQUIRE(translator.Translate(block.get()));
        context.Finish();
#if defined(__aarch64__)
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != INVALID_CACHE_ID);
        context.Flush(buffer);
        REQUIRE(module->Push(block.get()));
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        constexpr std::array<std::array<swift::u64, 2>, 3> inputs{{
                {UINT64_MAX, 1}, {UINT64_C(0x7fffffffffffffff), 1}, {0, 1}}};
        for (size_t offset : {size_t{0}, size_t{11}}) for (const auto& input : inputs) {
            CAPTURE(kind, offset, input[0], input[1]);
            alignas(16) std::array<swift::u8, 32> data{};
            constexpr swift::u64 initial = UINT64_C(0x1122334455667788);
            std::memcpy(data.data() + offset, &initial, sizeof(initial));
            const swift::u64 pointer = reinterpret_cast<uintptr_t>(data.data() + offset) - displacement;
            Runtime runtime{&space};
            auto* uniform = runtime.GetUniformBuffer().data();
            std::memcpy(uniform + 2048, &pointer, sizeof(pointer));
            std::memcpy(uniform + 2056, &stored_bits, sizeof(stored_bits));
            std::memcpy(uniform + 2064, &input[0], sizeof(input[0]));
            std::memcpy(uniform + 2072, &input[1], sizeof(input[1]));
            runtime.SetLocation(block->GetStartLocation().Value());
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            swift::u64 observed{}, captured{}, memory{};
            std::memcpy(&observed, uniform + 2080, sizeof(observed));
            std::memcpy(&captured, uniform + 2088, sizeof(captured));
            std::memcpy(&memory, data.data() + offset, sizeof(memory));
            REQUIRE(observed == (kind ? stored_bits : initial));
            REQUIRE(memory == (kind ? stored_bits : initial));
            const swift::u64 total = input[0] + input[1];
            const bool overflow = ((~(input[0] ^ input[1]) & (input[0] ^ total)) >> 63) != 0;
            const swift::u64 expected = (total & (swift::u64{1} << 63) ? swift::u64{1} << 31 : 0) |
                    (total == 0 ? swift::u64{1} << 30 : 0) | (swift::u64{overflow} << 28);
            REQUIRE((captured & UINT64_C(0xf0000000)) == expected); // CF was cleared after ADD.
            REQUIRE(((std::popcount(static_cast<swift::u8>(captured)) & 1) == 0) ==
                    ((std::popcount(static_cast<swift::u8>(total)) & 1) == 0));
            REQUIRE(((captured >> arm64::HostFlagsBit::AuxiliaryCarry) & 1) ==
                    swift::u64{((input[0] ^ input[1] ^ total) & 0x10) != 0});
        }
#endif
    }
}

TEST_CASE("repeated unaligned TSO accesses each take an alignment-safe path", "[memory-plan]") {
    // Regression: the RCpc fast path once skipped the alignment check of a
    // second access after an earlier dynamic check in the same block. A
    // failed check falls back and rejoins the same continuation, so it
    // proves nothing; the unchecked second access then faulted (SIGBUS)
    // on a naturally unaligned, window-crossing address.
    auto config = Configuration();
    config.arm64_features |= Arm64Features::RCpc | Arm64Features::RCpcImm;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xba30})};
    auto pointer = block->LoadUniform(ValueType::U64, Uniform{2048, ValueType::U64});
    auto stored = block->LoadUniform(ValueType::U64, Uniform{2056, ValueType::U64});
    block->StoreMemoryTSO(Operand{pointer}, stored);
    block->StoreMemoryTSO(Operand{pointer}, stored);
    auto first = block->LoadMemoryTSO(ValueType::U64, Operand{pointer});
    auto second = block->LoadMemoryTSO(ValueType::U64, Operand{pointer});
    block->StoreUniform(Uniform{2064, ValueType::U64}, first);
    block->StoreUniform(Uniform{2072, ValueType::U64}, second);
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
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
#if defined(__aarch64__)
    const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
    REQUIRE(id != INVALID_CACHE_ID); context.Flush(buffer); REQUIRE(module->Push(block.get()));
    space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
    alignas(16) std::array<swift::u8, 32> data{};
    const auto unaligned = reinterpret_cast<uintptr_t>(data.data()) + 11;
    const swift::u64 value = 0x8877665544332211;
    Runtime runtime{&space};
    std::memcpy(runtime.GetUniformBuffer().data() + 2048, &unaligned, sizeof(unaligned));
    std::memcpy(runtime.GetUniformBuffer().data() + 2056, &value, sizeof(value));
    runtime.SetLocation(block->GetStartLocation().Value());
    REQUIRE(runtime.Run() == HaltReason::CallHost);
    swift::u64 first_value{}, second_value{}, stored_value{};
    std::memcpy(&first_value, runtime.GetUniformBuffer().data() + 2064, sizeof(first_value));
    std::memcpy(&second_value, runtime.GetUniformBuffer().data() + 2072, sizeof(second_value));
    std::memcpy(&stored_value, data.data() + 11, sizeof(stored_value));
    REQUIRE(first_value == value); REQUIRE(second_value == value);
    REQUIRE(stored_value == value);
#endif
}

TEST_CASE("fault-emulated RCpc accesses complete unaligned TSO accesses", "[memory-plan]") {
    // Same shape as the checked-path regression above, but with
    // rcpc_fault_emulate the emitter skips alignment dispatch entirely:
    // the window-crossing accesses raise SIGBUS and the runtime fault
    // handler emulates them. Requires the guest map oracle so the
    // handler can prove the access range is mapped before touching it.
    struct OracleGuard {
        ~OracleGuard() {
            backend::SignalHandler::SetGuestMapCheck(nullptr, nullptr);
            backend::SignalHandler::SetGuestRangeCheck(nullptr, nullptr);
        }
    } guard;
    alignas(16) std::array<swift::u8, 32> data{};
    const auto begin = reinterpret_cast<std::uintptr_t>(data.data());
    backend::SignalHandler::SetGuestMapCheck(
            [](void* ctx, std::uintptr_t addr) -> bool {
                const auto range = *static_cast<std::pair<std::uintptr_t, std::uintptr_t>*>(ctx);
                return addr >= range.first && addr < range.second;
            },
            new std::pair<std::uintptr_t, std::uintptr_t>(begin, begin + data.size()));
    backend::SignalHandler::SetGuestRangeCheck(
            [](void*, std::uintptr_t, swift::u64 length) -> swift::u64 { return length; }, nullptr);
    auto config = Configuration();
    config.arm64_features |= Arm64Features::RCpc | Arm64Features::RCpcImm;
    config.rcpc_fault_emulate = true;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xba34})};
    auto pointer = block->LoadUniform(ValueType::U64, Uniform{2048, ValueType::U64});
    auto stored = block->LoadUniform(ValueType::U64, Uniform{2056, ValueType::U64});
    block->StoreMemoryTSO(Operand{pointer}, stored);
    block->StoreMemoryTSO(Operand{pointer}, stored);
    auto first = block->LoadMemoryTSO(ValueType::U64, Operand{pointer});
    auto second = block->LoadMemoryTSO(ValueType::U64, Operand{pointer});
    block->StoreUniform(Uniform{2064, ValueType::U64}, first);
    block->StoreUniform(Uniform{2072, ValueType::U64}, second);
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
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
#if defined(__aarch64__)
    const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
    REQUIRE(id != INVALID_CACHE_ID); context.Flush(buffer); REQUIRE(module->Push(block.get()));
    space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
    const auto unaligned = begin + 11;
    const swift::u64 value = 0x8877665544332211;
    Runtime runtime{&space};
    std::memcpy(runtime.GetUniformBuffer().data() + 2048, &unaligned, sizeof(unaligned));
    std::memcpy(runtime.GetUniformBuffer().data() + 2056, &value, sizeof(value));
    runtime.SetLocation(block->GetStartLocation().Value());
    REQUIRE(runtime.Run() == HaltReason::CallHost);
    swift::u64 first_value{}, second_value{}, stored_value{};
    std::memcpy(&first_value, runtime.GetUniformBuffer().data() + 2064, sizeof(first_value));
    std::memcpy(&second_value, runtime.GetUniformBuffer().data() + 2072, sizeof(second_value));
    std::memcpy(&stored_value, data.data() + 11, sizeof(stored_value));
    REQUIRE(first_value == value); REQUIRE(second_value == value);
    REQUIRE(stored_value == value);
#endif
}

TEST_CASE("adaptive RCpc fallback recompiles fault-hot blocks with checks", "[memory-plan]") {
    // Under rcpc_fault_emulate a block starts unchecked and its
    // window-crossing accesses are emulated from SIGBUS. After
    // kRcpcFaultThreshold emulated faults charged to the block, the
    // dispatcher marks it checks-forced; the next compile of the same
    // block must then emit the checked form and produce no further
    // emulated faults while returning identical results.
    struct OracleGuard {
        ~OracleGuard() {
            backend::SignalHandler::SetGuestMapCheck(nullptr, nullptr);
            backend::SignalHandler::SetGuestRangeCheck(nullptr, nullptr);
        }
    } guard;
    alignas(16) std::array<swift::u8, 32> data{};
    const auto begin = reinterpret_cast<std::uintptr_t>(data.data());
    backend::SignalHandler::SetGuestMapCheck(
            [](void* ctx, std::uintptr_t addr) -> bool {
                const auto range = *static_cast<std::pair<std::uintptr_t, std::uintptr_t>*>(ctx);
                return addr >= range.first && addr < range.second;
            },
            new std::pair<std::uintptr_t, std::uintptr_t>(begin, begin + data.size()));
    backend::SignalHandler::SetGuestRangeCheck(
            [](void*, std::uintptr_t, swift::u64 length) -> swift::u64 { return length; }, nullptr);
    auto config = Configuration();
    config.arm64_features |= Arm64Features::RCpc | Arm64Features::RCpcImm;
    config.rcpc_fault_emulate = true;
    AddressSpace space{config}; auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xba38})};
    auto pointer = block->LoadUniform(ValueType::U64, Uniform{2048, ValueType::U64});
    auto stored = block->LoadUniform(ValueType::U64, Uniform{2056, ValueType::U64});
    block->StoreMemoryTSO(Operand{pointer}, stored);
    auto loaded = block->LoadMemoryTSO(ValueType::U64, Operand{pointer});
    block->StoreUniform(Uniform{2064, ValueType::U64}, loaded);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
#if defined(__aarch64__)
    REQUIRE(module->Push(block.get()));
    {
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
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
        arm64::JitContext context{module, alloc, alloc.Actions()};
        arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
        REQUIRE_FALSE(context.RcpcForceChecks());
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != INVALID_CACHE_ID); context.Flush(buffer);
        module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size,
                              block->GetStartLocation().Value());
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
    }
    const auto unaligned = begin + 11;
    const swift::u64 value = 0x8877665544332211;
    Runtime runtime{&space};
    std::memcpy(runtime.GetUniformBuffer().data() + 2048, &unaligned, sizeof(unaligned));
    std::memcpy(runtime.GetUniformBuffer().data() + 2056, &value, sizeof(value));
    // Two faulting accesses per run at distinct host PCs; counting is
    // per instruction, so run each site well past the 128 threshold.
    // The service may invalidate the manually pushed code, in which
    // case Run stops returning CallHost; either outcome is fine here.
    for (int run = 0; run < 200; ++run) {
        runtime.SetLocation(block->GetStartLocation().Value());
        if (runtime.Run() != HaltReason::CallHost) break;
    }
    REQUIRE(space.RcpcEmulatedFaultTotal() >= AddressSpace::kRcpcFaultThreshold);
    REQUIRE(space.RcpcForcedSiteCount() >= 1);
    REQUIRE(space.RcpcChecksForced(block->GetStartLocation().Value(),
                                   block->GetStartLocation().Value() + 1));
    // Recompiling the same block now yields the checked form: correct
    // results with zero additional emulated faults.
    {
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
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
        arm64::JitContext context{module, alloc, alloc.Actions()};
        arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
        REQUIRE(context.RcpcForceChecks());
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != INVALID_CACHE_ID); context.Flush(buffer);
        module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size,
                              block->GetStartLocation().Value());
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
    }
    const auto faults_before = space.RcpcEmulatedFaultTotal();
    runtime.SetLocation(block->GetStartLocation().Value());
    REQUIRE(runtime.Run() == HaltReason::CallHost);
    REQUIRE(space.RcpcEmulatedFaultTotal() == faults_before);
    swift::u64 loaded_value{}, stored_value{};
    std::memcpy(&loaded_value, runtime.GetUniformBuffer().data() + 2064, sizeof(loaded_value));
    std::memcpy(&stored_value, data.data() + 11, sizeof(stored_value));
    REQUIRE(loaded_value == value);
    REQUIRE(stored_value == value);
    // Rewritten guest code earns a fresh budget: invalidating the range
    // through the syscall-facing API clears the mark.
    space.InvalidateCodeRange(block->GetStartLocation().Value(),
                              block->GetStartLocation().Value() + 1);
    REQUIRE_FALSE(space.RcpcChecksForced(block->GetStartLocation().Value(),
                                         block->GetStartLocation().Value() + 1));
#endif
}

TEST_CASE("memory writeback rejects flags and live old addresses before emission", "[memory-plan]") {
    for (bool flags : {false, true}) {
        AddressSpace space{Configuration()};
        Fixture item{flags, !flags};
        AllocationFixture alloc{item.block->MaxInstrId(),
                                space.GetTrampolines().GetGPRRegs(),
                                space.GetTrampolines().GetFPRRegs(),
                                FeatureSet{}};
        ::swift::runtime::compile::AllocateBlock(
                item.block.get(),
                &alloc,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        item.block.get(), alloc, FeatureSet{}),
                false);
        // Force the same physical candidate. The pure builder must still
        // reject the effect/lifetime violation before any instructions emit.
        alloc.MapRegister(item.address.Id(), HostGPR{13});
        alloc.MapRegister(item.updated.Id(), HostGPR{13});
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(space.GetDefaultModule(), alloc);
        arm64::JitContext context{space.GetDefaultModule(), alloc, alloc.Actions()};
        REQUIRE(context.CurrentBufferSize() == 0);
        const auto plan = alloc.Actions().ForBlock(item.block.get());
        const auto& memory_actions = alloc.Actions();
        REQUIRE_FALSE(MemoryFor(memory_actions, item.memory, arm64::MemoryAction::PostIndex));
    }
}

TEST_CASE("fixed-home writeback transfers the last covered SSA use", "[memory-plan]") {
    AddressSpace space{Configuration()}; auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xba10})};
    auto initial = block->LoadUniform(ValueType::U64, Uniform{2048, ValueType::U64});
    block->SetHostGPR(initial, HostRegIndex(4), Imm{0u});
    auto address = block->GetHostGPR(ValueType::U64, HostRegIndex(4), Imm{0u});
    auto loaded = block->LoadMemory(ValueType::U64, Operand{address});
    auto updated = block->Add(ValueType::U64, address, Operand{Imm{swift::u64{8}}});
    block->SetHostGPR(updated, HostRegIndex(4), Imm{0u});
    block->StoreUniform(Uniform{2056, ValueType::U64}, loaded);
    auto current = block->GetHostGPR(ValueType::U64, HostRegIndex(4), Imm{0u});
    block->StoreUniform(Uniform{2064, ValueType::U64}, current);
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
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    const auto plan = alloc.Actions().ForBlock(block.get());
    const auto& memory_actions = alloc.Actions();
    REQUIRE(plan);
    REQUIRE(MemoryFor(memory_actions, loaded.Def(), arm64::MemoryAction::PostIndex));
    arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
#if defined(__aarch64__)
    const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
    REQUIRE(id != INVALID_CACHE_ID); context.Flush(buffer); REQUIRE(module->Push(block.get()));
    space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
    std::array<swift::u64, 2> data{0x1122334455667788, 0x8877665544332211};
    const auto pointer = reinterpret_cast<uintptr_t>(data.data());
    Runtime runtime{&space}; std::memcpy(runtime.GetUniformBuffer().data() + 2048, &pointer, sizeof(pointer));
    runtime.SetLocation(block->GetStartLocation().Value()); REQUIRE(runtime.Run() == HaltReason::CallHost);
    swift::u64 loaded_value{}, address_value{};
    std::memcpy(&loaded_value, runtime.GetUniformBuffer().data() + 2056, sizeof(loaded_value));
    std::memcpy(&address_value, runtime.GetUniformBuffer().data() + 2064, sizeof(address_value));
    REQUIRE(loaded_value == data[0]); REQUIRE(address_value == pointer + 8);
#endif
}

TEST_CASE("narrow load recipes freeze width and destination before emission", "[memory-plan]") {
    AddressSpace space{Configuration()};
    IntrusivePtr<Block> block{new Block(0, Location{0xba20})};
    auto address = block->LoadUniform(ValueType::U64, Uniform{2048, ValueType::U64});
    auto loaded = block->LoadMemory(ValueType::U16, Operand{address});
    auto extended = block->SignExtend(ValueType::S64, loaded);
    block->StoreUniform(Uniform{2056, ValueType::S64}, extended);
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
    alloc.MapRegister(address.Id(), HostGPR{13});
    alloc.MapRegister(loaded.Id(), HostGPR{8});
    alloc.MapRegister(extended.Id(), HostGPR{9});
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(space.GetDefaultModule(), alloc);
    arm64::JitContext context{space.GetDefaultModule(), alloc, alloc.Actions()};
    REQUIRE(context.CurrentBufferSize() == 0);
    const auto plan = alloc.Actions().ForBlock(block.get());
    const auto& memory_actions = alloc.Actions();
    REQUIRE(plan);
    const auto* row = MemoryFor(memory_actions, loaded.Def(), arm64::MemoryAction::Narrow);
    REQUIRE(row);
    const auto& action = *row;
    REQUIRE(memory_actions.InstructionCovered(extended.Id()));
    REQUIRE(action.Has(arm64::MemoryAction::Direct));
    REQUIRE(action.target == 9);
    REQUIRE(action.Has(arm64::MemoryAction::Sign));
    REQUIRE(action.access_bytes == 2);
    REQUIRE(action.ResultBytes() == 8);
}

TEST_CASE("narrow load contracts reject incompatible views users and earlier writeback homes", "[memory-plan]") {
    for (unsigned scenario = 0; scenario < 12; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xba60})};
        const auto address = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        const auto type = scenario == 1   ? ValueType::U8
                          : scenario == 6 ? ValueType::U32
                          : scenario == 8 ? ValueType::V16
                                          : ValueType::U16;
        const auto loaded = (scenario == 5 ? block->LoadMemoryTSO(type, Operand{address})
                                           : block->LoadMemory(type, Operand{address}));
        Value updated;
        if (scenario == 2) block->UniformBarrier();
        if (scenario == 11)
            updated = block->Add(ValueType::U64, address, Operand{Imm{swift::u64{2}}});
        const auto extended = (scenario == 1 ? block->ZeroExtend32(scenario == 1   ? ValueType::U32
                                                                   : scenario == 7 ? ValueType::S16
                                                                                   : ValueType::S64,
                                                                   loaded)
                                             : block->SignExtend(scenario == 1   ? ValueType::U32
                                                                 : scenario == 7 ? ValueType::S16
                                                                                 : ValueType::S64,
                                                                 loaded));
        if (scenario == 4) block->SaveFlags(extended, Flags::Carry);
        block->StoreUniform(Uniform{16, extended.Type()}, extended);
        if (scenario == 3) block->StoreUniform(Uniform{24, loaded.Type()}, loaded);
        if (updated.Defined()) block->StoreUniform(Uniform{32, ValueType::U64}, updated);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(address.Id(), HostGPR{13});
        allocation.MapRegister(loaded.Id(), HostGPR{8});
        allocation.MapRegister(extended.Id(), HostGPR{static_cast<swift::u16>(scenario == 11 ? 8 : scenario == 9 || scenario == 10 ? 13 : 9)});
        if (updated.Defined()) allocation.MapRegister(updated.Id(), HostGPR{13});
        compile::BlockAnalysisIndex index;
        index.Build(block.get());
        compile::CompileWorkspace workspace;
        arm64::MemorySelectionBuilder memory_builder{workspace};
        memory_builder.Reset(index);
        if (scenario == 9)
            memory_builder.Record(
                    {loaded.Id(), UINT16_MAX, 0, 13, arm64::MemoryTransferAction::Load});
        if (scenario == 10)
            memory_builder.Record(
                    {address.Id(), loaded.Id(), 2, 13, arm64::MemoryTransferAction::PreIndex});
        const auto earlier = memory_builder.Finish();
        const arm64::MemoryOptions options{.narrow_fuse = true};
        const auto shared = arm64::PrepareMemory(
                block.get(), allocation, FeatureSet{}, earlier, {}, options, index);
        compile::BlockAnalysisIndex independent;
        independent.Build(block.get());
        const auto own = arm64::PrepareMemory(
                block.get(), allocation, FeatureSet{}, earlier, {}, options, independent);
        const bool accepted = scenario == 0 || scenario == 1 || scenario == 11;
        REQUIRE((shared.Find(loaded.Id(), arm64::MemoryAction::Narrow) != nullptr) == accepted);
        REQUIRE(shared.covered == own.covered);
        REQUIRE(shared.actions == own.actions);
        REQUIRE(shared.writes == own.writes);
        REQUIRE(CountActions(shared, arm64::MemoryAction::Narrow) ==
                CountActions(own, arm64::MemoryAction::Narrow));
        if (accepted) {
            const auto& action = (*shared.Find(loaded.Id(), arm64::MemoryAction::Narrow));
            REQUIRE(std::ranges::find(shared.covered, extended.Id()) != shared.covered.end());
            REQUIRE(action.Has(arm64::MemoryAction::Direct) == (scenario != 11));
            REQUIRE(action.target == (scenario == 11 ? UINT8_MAX : 9));
            REQUIRE(action.Has(arm64::MemoryAction::Sign) == (scenario != 1));
            REQUIRE(action.access_bytes == (scenario == 1 ? 1 : 2));
            REQUIRE(action.ResultBytes() == (scenario == 1 ? 4 : 8));
            if (scenario == 11)
                REQUIRE((shared.Find(loaded.Id(), arm64::MemoryAction::PostIndex) != nullptr));
        } else
            REQUIRE(std::find(shared.covered.begin(), shared.covered.end(), extended.Id()) ==
                    shared.covered.end());
    }
}

#if defined(__aarch64__) && defined(__linux__)
TEST_CASE("writeback load and store preserve fault state and publish successful updates", "[memory-plan][memory-fault]") {
    for (bool dynamic : {false, true}) for (bool store : {false, true}) {
        CAPTURE(dynamic, store);
        const auto page_size = sysconf(_SC_PAGESIZE);
        REQUIRE(page_size > 0);
        auto* guard = mmap(nullptr, static_cast<size_t>(page_size), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(guard != MAP_FAILED);
        auto cleanup = swift::ScopeExit([&] { munmap(guard, static_cast<size_t>(page_size)); });
        std::array<UniformMapDesc, 1> mapped{{UniformMapDesc{128, 8, 4, false}}};
        auto config = Configuration();
        config.buffers_static_alloc = mapped;
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block(0, Location{0xba30})};
        Value delta, stored;
        if (dynamic) delta = block->LoadUniform(ValueType::U64, Uniform{2032, ValueType::U64});
        if (store) stored = block->LoadUniform(ValueType::U64, Uniform{2040, ValueType::U64});
        const auto initial = block->LoadUniform(ValueType::U64, Uniform{2048, ValueType::U64});
        block->SetHostGPR(initial, HostRegIndex(4), Imm{0u});
        const auto address = block->GetHostGPR(ValueType::U64, HostRegIndex(4), Imm{0u});
        Value loaded;
        const Inst* memory;
        if (store) {
            block->StoreMemory(Operand{address}, stored);
            memory = &block->GetInstList().back();
        } else {
            loaded = block->LoadMemory(ValueType::U64, Operand{address});
            memory = loaded.Def();
        }
        const auto updated = block->Add(
                ValueType::U64, address, dynamic ? Operand{delta} : Operand{Imm{swift::u64{8}}});
        block->SetHostGPR(updated, HostRegIndex(4), Imm{0u});
        if (!store) block->StoreUniform(Uniform{2056, ValueType::U64}, loaded);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     FeatureSet{}};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, FeatureSet{}),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        arm64::JitContext context{module, allocation, allocation.Actions()};
        const auto plan = allocation.Actions().ForBlock(block.get());
        const auto& memory_actions = allocation.Actions();
        if (dynamic) {
            // Scalar register-delta updates stay as ordinary Add instructions;
            // only immediate updates have a native post-index recipe.
            REQUIRE_FALSE(MemoryFor(memory_actions, memory, arm64::MemoryAction::PostIndex));
        } else {
            REQUIRE(plan);
            REQUIRE(MemoryFor(memory_actions, memory, arm64::MemoryAction::PostIndex));
            REQUIRE((*MemoryFor(memory_actions, memory, arm64::MemoryAction::PostIndex)).offset ==
                    8);
        }
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != INVALID_CACHE_ID);
        context.Flush(buffer);
        module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size,
                              block->GetStartLocation().Value(), buffer.exec_data);
        for (const auto& fault : translator.GetFaultMetadata())
            module->AddFaultEntry(buffer.exec_data + fault.host_begin, buffer.exec_data + fault.host_end,
                                  fault.guest_start, buffer.exec_data,
                                  fault.recovery_offset ? buffer.exec_data + fault.recovery_offset : nullptr,
                                  fault.recovery_kind);
        REQUIRE(module->Push(block.get()));
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);

        Runtime runtime{&space};
        const auto pointer = reinterpret_cast<uintptr_t>(guard);
        const swift::u64 sentinel = 0x9a8b7c6d5e4f3021ull;
        auto uniform = runtime.GetUniformBuffer();
        const swift::u64 dynamic_delta = 24;
        const swift::u64 stored_value = 0x1122334455667788ull;
        std::memcpy(uniform.data() + 2032, &dynamic_delta, sizeof(dynamic_delta));
        std::memcpy(uniform.data() + 2040, &stored_value, sizeof(stored_value));
        std::memcpy(uniform.data() + 2048, &pointer, sizeof(pointer));
        std::memcpy(uniform.data() + 2056, &sentinel, sizeof(sentinel));
        runtime.SetLocation(block->GetStartLocation().Value());
        REQUIRE(runtime.Run() == HaltReason::PageFatal);
        swift::u64 result{};
        std::memcpy(&result, uniform.data() + 2056, sizeof(result));
        REQUIRE(result == sentinel);
        std::memcpy(&result, uniform.data() + 128, sizeof(result));
        REQUIRE(result == pointer);
        REQUIRE(mprotect(guard, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE) == 0);
        // Reuse the emitted code with zero, positive and wrapping negative
        // register deltas. Immediate recipes need only their fixed offset.
        const std::array<swift::u64, 3> deltas{0, dynamic_delta, swift::u64{0} - dynamic_delta};
        for (unsigned run = 0; run < (dynamic ? deltas.size() : 1); ++run) {
            const auto success_delta = dynamic ? deltas[run] : 8;
            const swift::u64 initial_memory = 0x8877665544332211ull;
            std::memcpy(guard, &initial_memory, sizeof(initial_memory));
            Runtime successful{&space};
            auto success_uniform = successful.GetUniformBuffer();
            std::memcpy(success_uniform.data() + 2032, &success_delta, sizeof(success_delta));
            std::memcpy(success_uniform.data() + 2040, &stored_value, sizeof(stored_value));
            std::memcpy(success_uniform.data() + 2048, &pointer, sizeof(pointer));
            std::memcpy(success_uniform.data() + 2056, &sentinel, sizeof(sentinel));
            successful.SetLocation(block->GetStartLocation().Value());
            REQUIRE(successful.Run() == HaltReason::CallHost);
            std::memcpy(&result, success_uniform.data() + 128, sizeof(result));
            REQUIRE(result == pointer + success_delta);
            std::memcpy(&result, success_uniform.data() + 2056, sizeof(result));
            REQUIRE(result == (store ? sentinel : initial_memory));
            std::memcpy(&result, guard, sizeof(result));
            REQUIRE(result == (store ? stored_value : initial_memory));
        }
    }
}
TEST_CASE("fused narrow loads preserve access width and fault publication order", "[memory-plan][memory-fault]") {
    const auto page_size = sysconf(_SC_PAGESIZE);
    REQUIRE(page_size > 0);
    auto* data = static_cast<swift::u8*>(mmap(nullptr, static_cast<size_t>(page_size) * 2,
                                            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(data != MAP_FAILED);
    auto cleanup = swift::ScopeExit([&] { munmap(data, static_cast<size_t>(page_size) * 2); });
    data[page_size - 2] = 1;
    data[page_size - 1] = 0x80;
    REQUIRE(mprotect(data + page_size, static_cast<size_t>(page_size), PROT_NONE) == 0);
    for (bool first_fault : {false, true}) {
        CAPTURE(first_fault);
        AddressSpace space{Configuration()};
        auto module = space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block(0, Location{0xba50})};
        const auto address = block->LoadUniform(ValueType::U64, Uniform{2048, ValueType::U64});
        const auto loaded = block->LoadMemory(ValueType::U16, Operand{address});
        const auto extended = block->SignExtend(ValueType::S64, loaded);
        block->StoreUniform(Uniform{2056, ValueType::S64}, extended);
        const auto guard = block->LoadUniform(ValueType::U64, Uniform{2064, ValueType::U64});
        const auto second = block->LoadMemory(ValueType::U8, Operand{guard});
        block->StoreUniform(Uniform{2072, ValueType::U8}, second);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     FeatureSet{}};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, FeatureSet{}),
                false);
        allocation.MapRegister(address.Id(), HostGPR{13});
        allocation.MapRegister(loaded.Id(), HostGPR{8});
        allocation.MapRegister(extended.Id(), HostGPR{9});
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
        arm64::JitContext context{module, allocation, allocation.Actions()};
        const auto plan = allocation.Actions().ForBlock(block.get());
        const auto& memory_actions = allocation.Actions();
        REQUIRE(plan);
        const auto* row = MemoryFor(memory_actions, loaded.Def(), arm64::MemoryAction::Narrow);
        REQUIRE(row);
        const auto& action = *row;
        REQUIRE(action.Has(arm64::MemoryAction::Direct));
        REQUIRE(action.access_bytes == 2);
        REQUIRE(action.ResultBytes() == 8);
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        const auto [id, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(id != INVALID_CACHE_ID);
        context.Flush(buffer);
        module->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size,
                              block->GetStartLocation().Value(), buffer.exec_data);
        for (const auto& fault : translator.GetFaultMetadata())
            module->AddFaultEntry(buffer.exec_data + fault.host_begin, buffer.exec_data + fault.host_end,
                                  fault.guest_start, buffer.exec_data,
                                  fault.recovery_offset ? buffer.exec_data + fault.recovery_offset : nullptr,
                                  fault.recovery_kind);
        REQUIRE(module->Push(block.get()));
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        Runtime runtime{&space};
        const auto pointer = reinterpret_cast<uintptr_t>(data + page_size - (first_fault ? 1 : 2));
        const auto protected_pointer = reinterpret_cast<uintptr_t>(data + page_size);
        constexpr swift::u64 sentinel = 0x9a8b7c6d5e4f3021ull;
        auto uniform = runtime.GetUniformBuffer();
        std::memcpy(uniform.data() + 2048, &pointer, sizeof(pointer));
        std::memcpy(uniform.data() + 2056, &sentinel, sizeof(sentinel));
        std::memcpy(uniform.data() + 2064, &protected_pointer, sizeof(protected_pointer));
        std::memcpy(uniform.data() + 2072, &sentinel, sizeof(sentinel));
        runtime.SetLocation(block->GetStartLocation().Value());
        REQUIRE(runtime.Run() == HaltReason::PageFatal);
        swift::u64 result{};
        std::memcpy(&result, uniform.data() + 2056, sizeof(result));
        REQUIRE(result == (first_fault ? sentinel : 0xffffffffffff8001ull));
        std::memcpy(&result, uniform.data() + 2072, sizeof(result));
        REQUIRE(result == sentinel);
    }
}
#endif

TEST_CASE("memory plans are order-independent over independent candidates", "[memory-plan]") {
    struct Snapshot {
        const Inst *memory{}, *narrow{};
        arm64::MemoryAction writeback;
        arm64::MemoryActions::Write proof;
        arm64::MemoryAction narrow_action;
        std::vector<swift::u16> covered;
        size_t writebacks{}, narrow_loads{};
    };
    const auto build = [&](bool second_first) {
        IntrusivePtr<Block> block{new Block(0, Location{0xba80})};
        Value addr_a, loaded_a, updated_a, addr_b, loaded_b, extended_b;
        const auto make_a = [&] {
            addr_a = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
            loaded_a = block->LoadMemory(ValueType::U64, Operand{addr_a});
            updated_a = block->Add(ValueType::U64, addr_a, Operand{Imm{swift::u64{8}}});
            block->StoreUniform(Uniform{16, ValueType::U64}, loaded_a);
            block->StoreUniform(Uniform{24, ValueType::U64}, updated_a);
        };
        const auto make_b = [&] {
            addr_b = block->LoadUniform(ValueType::U64, Uniform{32, ValueType::U64});
            loaded_b = block->LoadMemory(ValueType::U16, Operand{addr_b});
            extended_b = block->SignExtend(ValueType::S64, loaded_b);
            block->StoreUniform(Uniform{40, ValueType::S64}, extended_b);
        };
        if (second_first) { make_b(); make_a(); } else { make_a(); make_b(); }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(addr_a.Id(), HostGPR{13});
        allocation.MapRegister(updated_a.Id(), HostGPR{13});
        allocation.MapRegister(loaded_a.Id(), HostGPR{8});
        allocation.MapRegister(addr_b.Id(), HostGPR{15});
        allocation.MapRegister(loaded_b.Id(), HostGPR{9});
        allocation.MapRegister(extended_b.Id(), HostGPR{10});
        const arm64::MemorySelections earlier;
        compile::BlockAnalysisIndex index;
        index.Build(block.get());
        const arm64::MemoryOptions options{.narrow_fuse = true};
        const auto shared = arm64::PrepareMemory(
                block.get(), allocation, FeatureSet{}, earlier, {}, options, index);
        compile::BlockAnalysisIndex independent;
        independent.Build(block.get());
        const auto own = arm64::PrepareMemory(
                block.get(), allocation, FeatureSet{}, earlier, {}, options, independent);
        REQUIRE(shared.writes.size() == own.writes.size());
        REQUIRE(CountActions(shared, arm64::MemoryAction::Narrow) ==
                CountActions(own, arm64::MemoryAction::Narrow));
        REQUIRE(shared.covered == own.covered);
        REQUIRE(shared.actions == own.actions);
        REQUIRE(shared.writes == own.writes);
        REQUIRE((shared.Find(loaded_a.Id(), arm64::MemoryAction::PostIndex) != nullptr));
        REQUIRE((shared.Find(loaded_b.Id(), arm64::MemoryAction::Narrow) != nullptr));
        Snapshot snapshot;
        snapshot.memory = loaded_a.Def();
        snapshot.narrow = loaded_b.Def();
        snapshot.writeback = (*shared.Find(loaded_a.Id(), arm64::MemoryAction::PostIndex));
        snapshot.proof = shared.writes.front();
        snapshot.narrow_action = (*shared.Find(loaded_b.Id(), arm64::MemoryAction::Narrow));
        snapshot.covered = shared.covered;
        std::sort(snapshot.covered.begin(), snapshot.covered.end());
        snapshot.writebacks = shared.writes.size();
        snapshot.narrow_loads = CountActions(shared, arm64::MemoryAction::Narrow);
        REQUIRE(snapshot.proof.update == updated_a.Id());
        REQUIRE(std::ranges::find(snapshot.covered, extended_b.Id()) != snapshot.covered.end());
        return snapshot;
    };
    const auto forward = build(false);
    REQUIRE(forward.writebacks == 1);
    REQUIRE(forward.narrow_loads == 1);
    REQUIRE(forward.writeback.base == 13);
    REQUIRE(forward.writeback.offset == 8);
    REQUIRE(forward.writeback.access_bytes == 8);
    REQUIRE(forward.narrow_action.Has(arm64::MemoryAction::Direct));
    REQUIRE(forward.narrow_action.Has(arm64::MemoryAction::Sign));
    REQUIRE(forward.narrow_action.access_bytes == 2);
    REQUIRE(forward.narrow_action.ResultBytes() == 8);
    const auto swapped = build(true);
    REQUIRE(swapped.writebacks == forward.writebacks);
    REQUIRE(swapped.narrow_loads == forward.narrow_loads);
    REQUIRE(swapped.writeback.base == forward.writeback.base);
    REQUIRE(swapped.writeback.offset == forward.writeback.offset);
    REQUIRE(swapped.writeback.access_bytes == forward.writeback.access_bytes);
    REQUIRE(swapped.proof.consumed_value != UINT16_MAX);
    REQUIRE(swapped.narrow_action.Has(arm64::MemoryAction::Direct) ==
            forward.narrow_action.Has(arm64::MemoryAction::Direct));
    REQUIRE(swapped.narrow_action.Has(arm64::MemoryAction::Sign) ==
            forward.narrow_action.Has(arm64::MemoryAction::Sign));
    REQUIRE(swapped.narrow_action.access_bytes == forward.narrow_action.access_bytes);
    REQUIRE(swapped.narrow_action.ResultBytes() == forward.narrow_action.ResultBytes());
    REQUIRE(swapped.covered.size() == forward.covered.size());
}

TEST_CASE("memory preparation requests only the analyses each candidate needs", "[memory-plan]") {
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xba80})};
        const auto unrelated = block->LoadUniform(ValueType::U64, Uniform{80, ValueType::U64});
        const auto alias = block->BitCast(ValueType::U32, unrelated);
        REQUIRE(alias.Def()->IsBitCastOperation());
        const auto base = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        auto address = base;
        if (scenario == 1) address = block->GetOperand(ValueType::U64, Operand{base});
        if (scenario == 2) address = block->Add(ValueType::U64, base, Operand{Imm{swift::u64{12}}});
        const bool narrow = scenario == 3 || scenario == 7;
        const bool update_candidate = scenario >= 4;
        const bool preservation_conflict = scenario == 5 || scenario == 7;
        const auto loaded = scenario == 2 ? block->LoadMemoryTSO(ValueType::U32, Operand{address})
                                          : block->LoadMemory(narrow             ? ValueType::U8
                                                              : update_candidate ? ValueType::U64
                                                                                 : ValueType::U32,
                                                              Operand{address});
        Value updated;
        if (update_candidate)
            updated = block->Add(ValueType::U64, address, Operand{Imm{swift::u64{8}}});
        const auto result = narrow ? block->SignExtend(ValueType::U64, loaded) : loaded;
        block->StoreUniform(Uniform{16, result.Type()}, result);
        if (updated.Defined()) block->StoreUniform(Uniform{24, ValueType::U64}, updated);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();

        AllocationFixture allocation{
                block->MaxInstrId(), GPRSMask{UINT32_MAX}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(base.Id(), HostGPR{22});
        if (address.Id() != base.Id())
            allocation.MapRegister(address.Id(),
                                   HostGPR{static_cast<swift::u16>(scenario == 2 ? 14 : 22)});
        allocation.MapRegister(loaded.Id(), HostGPR{8});
        if (narrow) allocation.MapRegister(result.Id(), HostGPR{8});
        if (updated.Defined())
            allocation.MapRegister(updated.Id(),
                                   HostGPR{static_cast<swift::u16>(scenario == 4 ? 23 : 22)});
        if (preservation_conflict)
            allocation.PlanFixedGPRDefinition(base.Id(),
                                              {.home = 22,
                                               .end = updated.Id() + 1u,
                                               .transfer = updated.Id() + 1u,
                                               .last_use = block->MaxInstrId() - 1u,
                                               .width = 8,
                                               .preserved = true});
        auto& index = allocation.PrepareFacts(block.get()).PrepareIndex(block.get());
        REQUIRE_FALSE(index.UsesReady());
        REQUIRE_FALSE(index.ResolvedUsesReady());
        REQUIRE(allocation.FindGPRPreservations() == nullptr);
        const arm64::MemoryOptions options{.narrow_fuse = scenario == 1 || narrow,
                                           .address_tie = scenario == 1,
                                           .rcpc = scenario == 2};
        const arm64::MemorySelections selections;
        const auto lowered = arm64::PrepareMemory(
                block.get(), allocation, FeatureSet{}, selections, {}, options, index);
        REQUIRE(index.UsesReady() == (narrow || scenario == 6));
        REQUIRE(index.ResolvedUsesReady() == (scenario == 6));
        REQUIRE((allocation.FindGPRPreservations() != nullptr) == (scenario >= 5));
        REQUIRE((lowered.Find(loaded.Id(), arm64::MemoryAction::Narrow) != nullptr) == narrow);
        REQUIRE((lowered.Find(loaded.Id(), arm64::MemoryAction::Offset) != nullptr) ==
                (scenario == 2));
        REQUIRE((lowered.Find(loaded.Id(), arm64::MemoryAction::PostIndex) != nullptr) ==
                (scenario == 6));
        REQUIRE(lowered.writes.size() == (scenario == 6 ? 1u : 0u));
        if (scenario == 1 || scenario == 2)
            REQUIRE(lowered.covered == std::vector<swift::u16>{address.Id()});
        if (scenario == 2)
            REQUIRE(lowered.Find(loaded.Id(), arm64::MemoryAction::Offset)->offset == 12);
        if (scenario == 6) {
            REQUIRE(lowered.writes.front().consumed_value == address.Id());
            REQUIRE(lowered.writes.front().update == updated.Id());
        }
        const auto repeated = arm64::PrepareMemory(
                block.get(), allocation, FeatureSet{}, selections, {}, options, index);
        REQUIRE(repeated.actions == lowered.actions);
        REQUIRE(repeated.writes == lowered.writes);
        REQUIRE(repeated.covered == lowered.covered);
    }
}

TEST_CASE("memory plans reject stale indexes and follow reallocation", "[memory-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xba88})};
    const auto address = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    const auto loaded = block->LoadMemory(ValueType::U64, Operand{address});
    const auto updated = block->Add(ValueType::U64, address, Operand{Imm{swift::u64{8}}});
    block->StoreUniform(Uniform{16, ValueType::U64}, loaded);
    block->StoreUniform(Uniform{24, ValueType::U64}, updated);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    const auto map_candidates = [&](swift::u16 updated_home) {
        allocation.MapRegister(address.Id(), HostGPR{13});
        allocation.MapRegister(loaded.Id(), HostGPR{8});
        allocation.MapRegister(updated.Id(), HostGPR{updated_home});
    };
    map_candidates(13);
    const arm64::MemorySelections earlier;
    const arm64::MemoryOptions options{};
    IntrusivePtr<Block> other{new Block(0, Location{0xba90})};
    other->LoadUniform(Uniform{0, ValueType::U64});
    other->SetTerminal(terminal::ReturnToHost{});
    other->ReIdInstr();
    compile::BlockAnalysisIndex stale;
    stale.Build(other.get());
    const auto foreign = arm64::PrepareMemory(
            block.get(), allocation, FeatureSet{}, earlier, {}, options, stale);
    REQUIRE(foreign.writes.empty());
    REQUIRE((CountActions(foreign, arm64::MemoryAction::Narrow) == 0));
    REQUIRE(foreign.covered.empty());
    compile::BlockAnalysisIndex index;
    index.Build(block.get());
    const auto fresh = arm64::PrepareMemory(
            block.get(), allocation, FeatureSet{}, earlier, {}, options, index);
    REQUIRE((fresh.Find(loaded.Id(), arm64::MemoryAction::PostIndex) != nullptr));
    REQUIRE((*fresh.Find(loaded.Id(), arm64::MemoryAction::PostIndex)).offset == 8);
    allocation.ResetAllocations();
    map_candidates(14);
    const auto remapped = arm64::PrepareMemory(
            block.get(), allocation, FeatureSet{}, earlier, {}, options, index);
    REQUIRE(remapped.writes.empty());
    REQUIRE(std::find(remapped.covered.begin(), remapped.covered.end(), updated.Id()) ==
            remapped.covered.end());
    allocation.ResetAllocations();
    map_candidates(13);
    const auto restored = arm64::PrepareMemory(
            block.get(), allocation, FeatureSet{}, earlier, {}, options, index);
    REQUIRE(restored.writes.size() == fresh.writes.size());
    REQUIRE(restored.writes.front().update == fresh.writes.front().update);
    REQUIRE((*restored.Find(loaded.Id(), arm64::MemoryAction::PostIndex)).base ==
            (*fresh.Find(loaded.Id(), arm64::MemoryAction::PostIndex)).base);
    REQUIRE((*restored.Find(loaded.Id(), arm64::MemoryAction::PostIndex)).offset ==
            (*fresh.Find(loaded.Id(), arm64::MemoryAction::PostIndex)).offset);
    REQUIRE(restored.covered == fresh.covered);
    IntrusivePtr<Block> plain{new Block(0, Location{0xba98})};
    const auto plain_address = plain->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    const auto plain_loaded = plain->LoadMemory(ValueType::U64, Operand{plain_address});
    plain->StoreUniform(Uniform{16, ValueType::U64}, plain_loaded);
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    AllocationFixture plain_allocation{plain->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    plain_allocation.MapRegister(plain_address.Id(), HostGPR{13});
    plain_allocation.MapRegister(plain_loaded.Id(), HostGPR{8});
    compile::BlockAnalysisIndex plain_index;
    plain_index.Build(plain.get());
    const arm64::MemoryOptions fused{.narrow_fuse = true};
    const auto zero = arm64::PrepareMemory(
            plain.get(), plain_allocation, FeatureSet{}, earlier, {}, fused, plain_index);
    REQUIRE(zero.writes.empty());
    REQUIRE((CountActions(zero, arm64::MemoryAction::Narrow) == 0));
    REQUIRE(zero.covered.empty());
}

TEST_CASE("memory actions preserve sealed offset views and allocation identity", "[memory-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xbaa0})};
    const auto base = block->LoadUniform(ValueType::U32, Uniform{0, ValueType::U32});
    auto view = base;
    view = view.SetCastType(ValueType::U64);
    const auto address = block->Add(ValueType::U64, view, Operand{Imm{swift::u64{12}}});
    const auto loaded = block->LoadMemoryTSO(ValueType::U32, Operand{address});
    block->StoreUniform(Uniform{16, ValueType::U32}, loaded);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    AllocationFixture allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    allocation.MapRegister(base.Id(), HostGPR{13});
    allocation.MapRegister(address.Id(), HostGPR{14});
    allocation.MapRegister(loaded.Id(), HostGPR{8});
    compile::BlockAnalysisIndex index;
    index.Build(block.get());
    const arm64::MemorySelections earlier;
    const auto lowered = arm64::PrepareMemory(
            block.get(), allocation, FeatureSet{}, earlier, {}, {.rcpc = true}, index);
    REQUIRE(lowered.actions.size() == 1);
    REQUIRE(lowered.writes.empty());
    allocation.BeginTargetPlanning();
    allocation.StoreMemory(lowered);
    const auto& actions = allocation.Actions();
    const auto* action = actions.Memory(loaded.Id());
    REQUIRE(action);
    REQUIRE(action->Has(arm64::MemoryAction::Offset));
    REQUIRE_FALSE(action->Has(arm64::MemoryAction::PostIndex));
    REQUIRE_FALSE(action->Has(arm64::MemoryAction::Narrow));
    REQUIRE(action->offset == 12);
    REQUIRE(action->OffsetBase(*loaded.Def()).Def() == base.Def());
    REQUIRE(action->OffsetBase(*loaded.Def()).Type() == ValueType::U64);
    REQUIRE(base.Def()->ReturnType() == ValueType::U32);
    REQUIRE(actions.InstructionCovered(address.Id()));
    REQUIRE_FALSE(actions.InstructionCovered(loaded.Id()));
    allocation.EndTargetPlanning();
    REQUIRE(actions.Ready());
    REQUIRE(actions.Memory(loaded.Id())->OffsetBase(*loaded.Def()).Type() == ValueType::U64);
    allocation.ResetAllocations();
    REQUIRE_FALSE(actions.Memory(loaded.Id()));
    REQUIRE_FALSE(actions.InstructionCovered(address.Id()));
}

TEST_CASE("publication and memory selections preserve views offsets and scoped receivers",
          "[memory-plan][regression]") {
    struct Fixture {
        IntrusivePtr<Block> block;
        Value source, extended, extract, update, address, loaded, load_update;
        const Inst* publication{};
        const Inst* store{};
        const Inst* writeback{};
        const Inst* update_publication{};
        const Inst* load_publication{};
    };
    const auto make = [](swift::u64 location) {
        Fixture f{IntrusivePtr<Block>{new Block(0, Location{location})}};
        f.source = f.block->LoadUniform(ValueType::U8, Uniform{0, ValueType::U8});
        const auto view = Value{f.source}.SetCastType(ValueType::U32);
        f.extended = f.block->ZeroExtend32To64(ValueType::U64, view);
        f.block->SetHostGPR(
                Value{f.extended}.SetCastType(ValueType::U32), HostRegIndex(23), Imm{1u});
        f.publication = &f.block->GetInstList().back();
        f.extract = f.block->BitExtract(ValueType::U8, view, Imm{0u}, Imm{8u});
        f.block->StoreMemory(Operand{Imm{swift::u64{0x1000}}}, f.extract);
        f.store = &f.block->GetInstList().back();
        const auto base = f.block->GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
        f.update = f.block->Sub(ValueType::U64, base, Operand{Imm{1u}});
        f.block->StoreMemory(Operand{f.update}, f.source);
        f.writeback = &f.block->GetInstList().back();
        f.block->SetHostGPR(f.update, HostRegIndex(22), Imm{0u});
        f.update_publication = &f.block->GetInstList().back();
        f.address = f.block->GetOperand(ValueType::U64, Operand{base, Imm{8u}});
        f.loaded = f.block->LoadMemory(ValueType::U8, Operand{f.address});
        f.load_update = f.block->Add(ValueType::U64, base, Operand{Imm{1u}});
        f.block->SetHostGPR(f.load_update, HostRegIndex(22), Imm{0u});
        f.load_publication = &f.block->GetInstList().back();
        f.block->SetTerminal(terminal::ReturnToHost{});
        f.block->ReIdInstr();
        return f;
    };
    for (const bool fused : {false, true}) {
        auto local = make(0xbb40);
        auto foreign = make(0xbb44);
        REQUIRE(local.extract.Id() == foreign.extract.Id());
        REQUIRE(local.update.Id() == foreign.update.Id());
        const auto large = std::numeric_limits<swift::s64>::max();
        compile::BlockAnalysisIndex index;
        index.Build(local.block.get());
        compile::CompileWorkspace workspace;
        arm64::MemorySelectionBuilder selected{workspace};
        selected.Reset(index);
        selected.Record(
                {local.extract.Id(), local.store->Id(), 0, 13, arm64::MemoryTransferAction::Value});
        selected.Record({local.update.Id(),
                         local.writeback->Id(),
                         -1,
                         22,
                         arm64::MemoryTransferAction::PreIndex});
        selected.Record({local.update.Id(),
                         local.writeback->Id(),
                         -2,
                         23,
                         arm64::MemoryTransferAction::PreIndex});
        selected.Record(arm64::AddressAction{large, local.address.Id(), local.loaded.Id(), 22});
        selected.Record({local.loaded.Id(),
                         local.load_update.Id(),
                         1,
                         22,
                         arm64::MemoryTransferAction::Load});
        selected.Record({local.load_update.Id(),
                         local.load_publication->Id(),
                         1,
                         22,
                         arm64::MemoryTransferAction::LoadUpdate});
        selected.LinkPublication(local.update.Id(), local.update_publication->Id());
        selected.LinkPublication(local.update.Id(), local.publication->Id());
        REQUIRE(selected.PublicationForUpdate(local.update.Def()) ==
                local.update_publication->Id());
        REQUIRE(selected.IsLoadPublication(local.load_publication));
        REQUIRE(selected.IsLoadUpdateInstruction(local.load_update.Def()));
        REQUIRE(selected.IsLoadUpdateInstruction(local.load_publication));
        REQUIRE_FALSE(selected.IsLoadUpdateInstruction(local.loaded.Def()));
        REQUIRE_FALSE(selected.IsLoadPublication(foreign.load_publication));
        const auto* update =
                selected.Transfer(local.update.Def(), arm64::MemoryTransferAction::PreIndex);
        REQUIRE(update);
        REQUIRE(update->offset == -1);
        REQUIRE(update->home == 22);
        REQUIRE(update->MatchesMemory(*local.writeback));
        REQUIRE_FALSE(
                selected.Transfer(foreign.update.Def(), arm64::MemoryTransferAction::PreIndex));
        const auto* value =
                selected.Transfer(local.extract.Def(), arm64::MemoryTransferAction::Value);
        REQUIRE(value);
        REQUIRE(value->MatchesValue(*local.store, local.extract));
        REQUIRE_FALSE(value->MatchesValue(*local.store, foreign.extract));
        REQUIRE(selected.Address(local.address.Def())->offset == large);
        REQUIRE_FALSE(selected.Address(foreign.address.Def()));
        arm64::PublicationAction publication{.instruction = local.publication->Id()};
        publication.kind = arm64::PublicationAction::Value;
        publication.target = 23;
        publication.SetView(32, 8);
        if (fused) publication.properties |= arm64::PublicationAction::FusedSource;
        const auto expected = fused ? Value{local.source}.SetCastType(ValueType::U32)
                                    : Value{local.extended}.SetCastType(ValueType::U32);
        REQUIRE(publication.Source(*local.publication).Def() == expected.Def());
        REQUIRE(publication.Source(*local.publication).Type() == expected.Type());
        REQUIRE(publication.Source(*local.publication).Type() != expected.Def()->ReturnType());
        AllocationFixture allocation{
                local.block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.BeginTargetPlanning();
        allocation.StorePublications(std::span{&publication, 1});
        const auto selections = selected.Finish();
        allocation.StoreMemorySelections(selections);
        allocation.EndTargetPlanning();
        const auto& actions = allocation.Actions();
        REQUIRE(actions.Ready());
        REQUIRE(actions.Publication(publication.instruction)->Width() == 32);
        REQUIRE(actions.Publication(publication.instruction)->BitOffset() == 8);
        REQUIRE(actions.Publication(publication.instruction)->Source(*local.publication).Def() ==
                expected.Def());
        REQUIRE(actions.Address(local.address.Id())->offset == large);
        REQUIRE(actions.MemoryTransfer(local.update.Id())->MatchesMemory(*local.writeback));
        REQUIRE_FALSE(actions.MemoryTransfer(local.extract.Id())
                              ->MatchesValue(*local.store, foreign.extract));
        REQUIRE_FALSE(actions.Publication(UINT32_MAX));
        REQUIRE_FALSE(actions.Address(UINT32_MAX));
        compile::BlockAnalysisIndex foreign_index;
        foreign_index.Build(foreign.block.get());
        selected.Reset(foreign_index);
        const auto empty = selected.Finish();
        REQUIRE(empty.Transfers().empty());
        REQUIRE(empty.Addresses().empty());
        REQUIRE_FALSE(empty.Transfer(local.update.Def(), arm64::MemoryTransferAction::PreIndex));
        REQUIRE(selections.Transfer(local.update.Def(), arm64::MemoryTransferAction::PreIndex)
                        ->offset == -1);
        REQUIRE(selections.IsLoadUpdateInstruction(local.load_publication));
        REQUIRE_FALSE(selections.IsLoadUpdateInstruction(foreign.load_publication));
        allocation.ResetAllocations();
        REQUIRE_FALSE(actions.Publication(publication.instruction));
        REQUIRE_FALSE(actions.Address(local.address.Id()));
        REQUIRE_FALSE(actions.MemoryTransfer(local.update.Id()));
    }
}
