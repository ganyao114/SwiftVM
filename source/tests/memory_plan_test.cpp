#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include "base/scope_exit.h"
#if defined(__aarch64__) && defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
struct Fixture {
    IntrusivePtr<Block> block{new Block(0, Location{0xba00})};
    Value address, loaded, updated;
    Inst* memory{};
    explicit Fixture(bool flags = false, bool live_old = false) {
        address = block->LoadUniform(Uniform{2048, ValueType::U64}).SetType(ValueType::U64);
        loaded = block->LoadMemory(Operand{address}).SetType(ValueType::U64);
        memory = loaded.Def();
        updated = block->Add(address, Operand{Imm{swift::u64{8}}}).SetType(ValueType::U64);
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
        const auto stored = block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        Value delta;
        if (scenario == 24 || scenario == 25)
            delta = block->LoadUniform(Uniform{48, scenario == 25 ? ValueType::U32 : ValueType::U64})
                    .SetType(scenario == 25 ? ValueType::U32 : ValueType::U64);
        auto address = block->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
        Value address_owner;
        if (scenario == 27) {
            address_owner = address;
            address = block->BitCast(address).SetType(ValueType::U64);
        }
        if (scenario == 23) block->UniformBarrier();
        const bool store = scenario == 1 || scenario == 3 || scenario == 5;
        const bool tso = scenario == 2 || scenario == 3 || scenario == 4 || scenario == 5;
        Value loaded;
        Inst* memory;
        if (store) {
            if (tso) block->StoreMemoryTSO(Operand{address}, stored);
            else block->StoreMemory(Operand{address}, stored);
            memory = &block->GetInstList().back();
        }
        else {
            loaded = (tso ? block->LoadMemoryTSO(Operand{address}) : block->LoadMemory(Operand{address}))
                    .SetType(scenario == 7 ? ValueType::V128 : ValueType::U64);
            memory = loaded.Def();
        }
        if (scenario == 10) block->UniformBarrier();
        if (scenario == 11) {
            const auto label = block->Goto(block->CondSet(Cond::EQ));
            block->BindLabel(label);
        }
        if (scenario == 12) (void)block->LoadMemory(Operand{Imm{swift::u64{0}}}).SetType(ValueType::U64);
        if (scenario == 26) delta = block->LoadUniform(Uniform{48, ValueType::U64}).SetType(ValueType::U64);
        const swift::s64 offset = scenario == 13 ? 256 : scenario == 14 ? -257 :
                scenario == 15 ? 255 : scenario == 16 ? -256 :
                scenario == 17 ? std::numeric_limits<swift::s64>::min() : 8;
        const auto operand_delta = delta.Defined() ? Operand{delta} : Operand{Imm{offset}};
        const auto updated = (scenario == 17 ? block->Sub(address, operand_delta)
                                            : block->Add(address, operand_delta)).SetType(ValueType::U64);
        if (scenario == 8) block->SaveFlags(updated, Flags::Carry | Flags::Overflow);
        if (loaded.Defined()) block->StoreUniform(Uniform{16, loaded.Type()}, loaded);
        block->StoreUniform(Uniform{32, ValueType::U64}, updated);
        if (scenario == 9) block->StoreUniform(Uniform{40, ValueType::U64}, address);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(stored.Id(), HostGPR{11});
        allocation.MapRegister(address.Id(), HostGPR{13});
        if (address_owner.Defined()) allocation.MapRegister(address_owner.Id(), HostGPR{13});
        if (delta.Defined()) allocation.MapRegister(delta.Id(), HostGPR{12});
        allocation.MapRegister(updated.Id(), HostGPR{static_cast<swift::u16>(scenario == 19 ? 14 : 13)});
        if (loaded.Defined()) allocation.MapRegister(loaded.Id(), HostGPR{static_cast<swift::u16>(scenario == 18 ? 13 : 8)});
        arm64::GPRBlockPlan earlier;
        if (scenario == 20) earlier.load_updates.emplace(memory, arm64::GPRLoadUpdate{});
        if (scenario == 21) earlier.preindex_updates.emplace(address.Def(), arm64::GPRMemoryUpdate{});
        arm64::BlockAnalysisIndex index;
        index.Build(block.get());
        const arm64::MemoryLoweringOptions options{.biased = scenario == 6, .rcpc = scenario == 4 || scenario == 5};
        const auto shared = arm64::BuildMemoryLoweringPlan(block.get(), allocation, FeatureSet{}, earlier, {}, options, &index);
        const auto own = arm64::BuildMemoryLoweringPlan(block.get(), allocation, FeatureSet{}, earlier, {}, options);
        const bool accepted = scenario <= 3 || scenario == 15 || scenario == 16 || scenario == 22 || scenario == 27;
        REQUIRE(shared.writebacks.contains(memory) == accepted);
        REQUIRE(shared.covered == own.covered);
        REQUIRE(shared.writebacks.size() == own.writebacks.size());
        REQUIRE(shared.writebacks.find(nullptr) == shared.writebacks.end());
        if (accepted) {
            const auto& action = shared.writebacks.at(memory);
            REQUIRE(action.update == updated.Def());
            REQUIRE(action.base == 13);
            REQUIRE(action.offset == offset);
            REQUIRE(action.access_bytes == 8);
            REQUIRE(action.consumed_value == (address_owner.Defined() ? address_owner.Id() : address.Id()));
        } else REQUIRE(std::find(shared.covered.begin(), shared.covered.end(), updated.Def()) == shared.covered.end());
    }
}
}

TEST_CASE("memory plans execute post-index updates at the memory instruction", "[memory-plan]") {
    AddressSpace space{Configuration()}; auto module = space.GetDefaultModule();
    Fixture item;
    RegAlloc alloc{item.block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                   space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    RegisterAllocPass::Run(item.block.get(), &alloc, false, FeatureSet{});
    alloc.MapRegister(item.address.Id(), HostGPR{13});
    alloc.MapRegister(item.updated.Id(), HostGPR{13});
    alloc.MapRegister(item.loaded.Id(), HostGPR{8});
    auto active = space.GetTrampolines().GetGPRRegs(); active.Mark(13); active.Mark(8);
    auto active_fprs = space.GetTrampolines().GetFPRRegs();
    for (auto& inst : item.block->GetInstList())
        alloc.SetActiveRegs(inst.Id(), active, active_fprs);
    alloc.CompleteGPRAllocation(module);
    arm64::JitContext context{module, alloc};
    const auto plan = alloc.GPRPlanFor(item.block.get());
    REQUIRE(plan->memory);
    REQUIRE(plan->memory->writebacks.contains(item.memory));
    REQUIRE(plan->memory->writebacks.at(item.memory).update == item.updated.Def());
    REQUIRE(plan->memory->writebacks.at(item.memory).offset == 8);
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
    auto pointer = block->LoadUniform(Uniform{2048, ValueType::U64}).SetType(ValueType::U64);
    auto stored = block->LoadUniform(Uniform{2056, ValueType::U64}).SetType(ValueType::U64);
    block->StoreMemoryTSO(Operand{pointer}, stored);
    block->StoreMemoryTSO(Operand{pointer}, stored);
    auto first = block->LoadMemoryTSO(Operand{pointer}).SetType(ValueType::U64);
    auto second = block->LoadMemoryTSO(Operand{pointer}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{2064, ValueType::U64}, first);
    block->StoreUniform(Uniform{2072, ValueType::U64}, second);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc alloc{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                   space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &alloc, false, FeatureSet{});
    alloc.CompleteGPRAllocation(module);
    arm64::JitContext context{module, alloc};
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
    auto pointer = block->LoadUniform(Uniform{2048, ValueType::U64}).SetType(ValueType::U64);
    auto stored = block->LoadUniform(Uniform{2056, ValueType::U64}).SetType(ValueType::U64);
    block->StoreMemoryTSO(Operand{pointer}, stored);
    block->StoreMemoryTSO(Operand{pointer}, stored);
    auto first = block->LoadMemoryTSO(Operand{pointer}).SetType(ValueType::U64);
    auto second = block->LoadMemoryTSO(Operand{pointer}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{2064, ValueType::U64}, first);
    block->StoreUniform(Uniform{2072, ValueType::U64}, second);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc alloc{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                   space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &alloc, false, FeatureSet{});
    alloc.CompleteGPRAllocation(module);
    arm64::JitContext context{module, alloc};
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
    auto pointer = block->LoadUniform(Uniform{2048, ValueType::U64}).SetType(ValueType::U64);
    auto stored = block->LoadUniform(Uniform{2056, ValueType::U64}).SetType(ValueType::U64);
    block->StoreMemoryTSO(Operand{pointer}, stored);
    auto loaded = block->LoadMemoryTSO(Operand{pointer}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{2064, ValueType::U64}, loaded);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
#if defined(__aarch64__)
    REQUIRE(module->Push(block.get()));
    {
        RegAlloc alloc{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                       space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
        RegisterAllocPass::Run(block.get(), &alloc, false, FeatureSet{});
        alloc.CompleteGPRAllocation(module);
        arm64::JitContext context{module, alloc};
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
        RegAlloc alloc{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                       space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
        RegisterAllocPass::Run(block.get(), &alloc, false, FeatureSet{});
        alloc.CompleteGPRAllocation(module);
        arm64::JitContext context{module, alloc};
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
        RegAlloc alloc{item.block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                       space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
        RegisterAllocPass::Run(item.block.get(), &alloc, false, FeatureSet{});
        // Force the same physical candidate. The pure builder must still
        // reject the effect/lifetime violation before any instructions emit.
        alloc.MapRegister(item.address.Id(), HostGPR{13});
        alloc.MapRegister(item.updated.Id(), HostGPR{13});
        alloc.CompleteGPRAllocation(space.GetDefaultModule());
        arm64::JitContext context{space.GetDefaultModule(), alloc};
        REQUIRE(context.CurrentBufferSize() == 0);
        const auto plan = alloc.GPRPlanFor(item.block.get());
        REQUIRE_FALSE((plan->memory && plan->memory->writebacks.contains(item.memory)));
    }
}

TEST_CASE("fixed-home writeback transfers the last covered SSA use", "[memory-plan]") {
    AddressSpace space{Configuration()}; auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0xba10})};
    auto initial = block->LoadUniform(Uniform{2048, ValueType::U64}).SetType(ValueType::U64);
    block->SetHostGPR(initial, HostRegIndex(4), Imm{0u});
    auto address = block->GetHostGPR(HostRegIndex(4), Imm{0u}).SetType(ValueType::U64);
    auto loaded = block->LoadMemory(Operand{address}).SetType(ValueType::U64);
    auto updated = block->Add(address, Operand{Imm{swift::u64{8}}}).SetType(ValueType::U64);
    block->SetHostGPR(updated, HostRegIndex(4), Imm{0u});
    block->StoreUniform(Uniform{2056, ValueType::U64}, loaded);
    auto current = block->GetHostGPR(HostRegIndex(4), Imm{0u}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{2064, ValueType::U64}, current);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc alloc{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                   space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &alloc, false, FeatureSet{});
    alloc.CompleteGPRAllocation(module);
    arm64::JitContext context{module, alloc};
    const auto plan = alloc.GPRPlanFor(block.get()); REQUIRE(plan->memory);
    REQUIRE(plan->memory->writebacks.contains(loaded.Def()));
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
    auto address = block->LoadUniform(Uniform{2048, ValueType::U64}).SetType(ValueType::U64);
    auto loaded = block->LoadMemory(Operand{address}).SetType(ValueType::U16);
    auto extended = block->SignExtend(loaded).SetType(ValueType::S64);
    block->StoreUniform(Uniform{2056, ValueType::S64}, extended);
    block->SetTerminal(terminal::ReturnToHost{}); block->ReIdInstr();
    RegAlloc alloc{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                   space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &alloc, false, FeatureSet{});
    alloc.MapRegister(address.Id(), HostGPR{13});
    alloc.MapRegister(loaded.Id(), HostGPR{8});
    alloc.MapRegister(extended.Id(), HostGPR{9});
    alloc.CompleteGPRAllocation(space.GetDefaultModule());
    arm64::JitContext context{space.GetDefaultModule(), alloc};
    REQUIRE(context.CurrentBufferSize() == 0);
    const auto plan = alloc.GPRPlanFor(block.get()); REQUIRE(plan->memory);
    const auto& action = plan->memory->narrow_loads.at(loaded.Def());
    REQUIRE(action.extension == extended.Def()); REQUIRE(action.direct);
    REQUIRE(action.sign); REQUIRE(action.access_bytes == 2); REQUIRE(action.result_bytes == 8);
}

TEST_CASE("narrow load contracts reject incompatible views users and earlier writeback homes", "[memory-plan]") {
    for (unsigned scenario = 0; scenario < 12; ++scenario) {
        CAPTURE(scenario);
        IntrusivePtr<Block> block{new Block(0, Location{0xba60})};
        const auto address = block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        const auto type = scenario == 1 ? ValueType::U8 : scenario == 6 ? ValueType::U32 :
                          scenario == 8 ? ValueType::V16 : ValueType::U16;
        const auto loaded = (scenario == 5 ? block->LoadMemoryTSO(Operand{address}) : block->LoadMemory(Operand{address})).SetType(type);
        Value updated;
        if (scenario == 2) block->UniformBarrier();
        if (scenario == 11) updated = block->Add(address, Operand{Imm{swift::u64{2}}}).SetType(ValueType::U64);
        const auto extended = (scenario == 1 ? block->ZeroExtend32(loaded) : block->SignExtend(loaded))
                .SetType(scenario == 1 ? ValueType::U32 : scenario == 7 ? ValueType::S16 : ValueType::S64);
        if (scenario == 4) block->SaveFlags(extended, Flags::Carry);
        block->StoreUniform(Uniform{16, extended.Type()}, extended);
        if (scenario == 3) block->StoreUniform(Uniform{24, loaded.Type()}, loaded);
        if (updated.Defined()) block->StoreUniform(Uniform{32, ValueType::U64}, updated);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(address.Id(), HostGPR{13});
        allocation.MapRegister(loaded.Id(), HostGPR{8});
        allocation.MapRegister(extended.Id(), HostGPR{static_cast<swift::u16>(scenario == 11 ? 8 : scenario == 9 || scenario == 10 ? 13 : 9)});
        if (updated.Defined()) allocation.MapRegister(updated.Id(), HostGPR{13});
        arm64::GPRBlockPlan earlier;
        if (scenario == 9) earlier.load_updates.emplace(loaded.Def(), arm64::GPRLoadUpdate{.target = 13});
        if (scenario == 10) earlier.preindex_updates.emplace(address.Def(), arm64::GPRMemoryUpdate{
                .memory = loaded.Def(), .base = vixl::aarch64::XRegister{13}, .offset = 2});
        arm64::BlockAnalysisIndex index;
        index.Build(block.get());
        const arm64::MemoryLoweringOptions options{.narrow_fuse = true};
        const auto shared = arm64::BuildMemoryLoweringPlan(block.get(), allocation, FeatureSet{}, earlier, {}, options, &index);
        const auto own = arm64::BuildMemoryLoweringPlan(block.get(), allocation, FeatureSet{}, earlier, {}, options);
        const bool accepted = scenario == 0 || scenario == 1 || scenario == 11;
        REQUIRE(shared.narrow_loads.contains(loaded.Def()) == accepted);
        REQUIRE(shared.covered == own.covered);
        REQUIRE(shared.narrow_loads.size() == own.narrow_loads.size());
        if (accepted) {
            const auto& action = shared.narrow_loads.at(loaded.Def());
            REQUIRE(action.extension == extended.Def());
            REQUIRE(action.direct == (scenario != 11));
            REQUIRE(action.sign == (scenario != 1));
            REQUIRE(action.access_bytes == (scenario == 1 ? 1 : 2));
            REQUIRE(action.result_bytes == (scenario == 1 ? 4 : 8));
            if (scenario == 11) REQUIRE(shared.writebacks.contains(loaded.Def()));
        } else REQUIRE(std::find(shared.covered.begin(), shared.covered.end(), extended.Def()) == shared.covered.end());
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
        if (dynamic) delta = block->LoadUniform(Uniform{2032, ValueType::U64}).SetType(ValueType::U64);
        if (store) stored = block->LoadUniform(Uniform{2040, ValueType::U64}).SetType(ValueType::U64);
        const auto initial = block->LoadUniform(Uniform{2048, ValueType::U64}).SetType(ValueType::U64);
        block->SetHostGPR(initial, HostRegIndex(4), Imm{0u});
        const auto address = block->GetHostGPR(HostRegIndex(4), Imm{0u}).SetType(ValueType::U64);
        Value loaded;
        Inst* memory;
        if (store) {
            block->StoreMemory(Operand{address}, stored);
            memory = &block->GetInstList().back();
        } else {
            loaded = block->LoadMemory(Operand{address}).SetType(ValueType::U64);
            memory = loaded.Def();
        }
        const auto updated = block->Add(address, dynamic ? Operand{delta} : Operand{Imm{swift::u64{8}}})
                .SetType(ValueType::U64);
        block->SetHostGPR(updated, HostRegIndex(4), Imm{0u});
        if (!store) block->StoreUniform(Uniform{2056, ValueType::U64}, loaded);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
        RegisterAllocPass::Run(block.get(), &allocation, false, FeatureSet{});
        allocation.CompleteGPRAllocation(module);
        arm64::JitContext context{module, allocation};
        const auto plan = allocation.GPRPlanFor(block.get());
        if (dynamic) {
            // Scalar register-delta updates stay as ordinary Add instructions;
            // only immediate updates have a native post-index recipe.
            REQUIRE_FALSE((plan->memory && plan->memory->writebacks.contains(memory)));
        } else {
            REQUIRE(plan->memory);
            REQUIRE(plan->memory->writebacks.contains(memory));
            REQUIRE(plan->memory->writebacks.at(memory).offset == 8);
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
        const auto address = block->LoadUniform(Uniform{2048, ValueType::U64}).SetType(ValueType::U64);
        const auto loaded = block->LoadMemory(Operand{address}).SetType(ValueType::U16);
        const auto extended = block->SignExtend(loaded).SetType(ValueType::S64);
        block->StoreUniform(Uniform{2056, ValueType::S64}, extended);
        const auto guard = block->LoadUniform(Uniform{2064, ValueType::U64}).SetType(ValueType::U64);
        const auto second = block->LoadMemory(Operand{guard}).SetType(ValueType::U8);
        block->StoreUniform(Uniform{2072, ValueType::U8}, second);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
        RegisterAllocPass::Run(block.get(), &allocation, false, FeatureSet{});
        allocation.MapRegister(address.Id(), HostGPR{13});
        allocation.MapRegister(loaded.Id(), HostGPR{8});
        allocation.MapRegister(extended.Id(), HostGPR{9});
        allocation.CompleteGPRAllocation(module);
        arm64::JitContext context{module, allocation};
        const auto plan = allocation.GPRPlanFor(block.get());
        REQUIRE(plan->memory);
        const auto& action = plan->memory->narrow_loads.at(loaded.Def());
        REQUIRE(action.direct);
        REQUIRE(action.access_bytes == 2);
        REQUIRE(action.result_bytes == 8);
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
        Inst *memory{}, *narrow{};
        arm64::MemoryWritebackAction writeback;
        arm64::NarrowLoadAction narrow_action;
        std::vector<swift::u32> covered, aliases;
        size_t writebacks{}, narrow_loads{};
    };
    const auto snapshot_ids = [](const std::vector<Inst*>& nodes) {
        std::vector<swift::u32> ids;
        for (const auto* node : nodes) ids.push_back(node->Id());
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const auto build = [&](bool second_first) {
        IntrusivePtr<Block> block{new Block(0, Location{0xba80})};
        Value addr_a, loaded_a, updated_a, addr_b, loaded_b, extended_b;
        const auto make_a = [&] {
            addr_a = block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
            loaded_a = block->LoadMemory(Operand{addr_a}).SetType(ValueType::U64);
            updated_a = block->Add(addr_a, Operand{Imm{swift::u64{8}}}).SetType(ValueType::U64);
            block->StoreUniform(Uniform{16, ValueType::U64}, loaded_a);
            block->StoreUniform(Uniform{24, ValueType::U64}, updated_a);
        };
        const auto make_b = [&] {
            addr_b = block->LoadUniform(Uniform{32, ValueType::U64}).SetType(ValueType::U64);
            loaded_b = block->LoadMemory(Operand{addr_b}).SetType(ValueType::U16);
            extended_b = block->SignExtend(loaded_b).SetType(ValueType::S64);
            block->StoreUniform(Uniform{40, ValueType::S64}, extended_b);
        };
        if (second_first) { make_b(); make_a(); } else { make_a(); make_b(); }
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
        allocation.MapRegister(addr_a.Id(), HostGPR{13});
        allocation.MapRegister(updated_a.Id(), HostGPR{13});
        allocation.MapRegister(loaded_a.Id(), HostGPR{8});
        allocation.MapRegister(addr_b.Id(), HostGPR{15});
        allocation.MapRegister(loaded_b.Id(), HostGPR{9});
        allocation.MapRegister(extended_b.Id(), HostGPR{10});
        arm64::GPRBlockPlan earlier;
        arm64::BlockAnalysisIndex index;
        index.Build(block.get());
        const arm64::MemoryLoweringOptions options{.narrow_fuse = true};
        const auto shared = arm64::BuildMemoryLoweringPlan(block.get(), allocation, FeatureSet{}, earlier, {}, options, &index);
        const auto own = arm64::BuildMemoryLoweringPlan(block.get(), allocation, FeatureSet{}, earlier, {}, options);
        REQUIRE(shared.writebacks.size() == own.writebacks.size());
        REQUIRE(shared.narrow_loads.size() == own.narrow_loads.size());
        REQUIRE(shared.covered == own.covered);
        REQUIRE(shared.writebacks.contains(loaded_a.Def()));
        REQUIRE(shared.narrow_loads.contains(loaded_b.Def()));
        Snapshot snapshot;
        snapshot.memory = loaded_a.Def();
        snapshot.narrow = loaded_b.Def();
        snapshot.writeback = shared.writebacks.at(loaded_a.Def());
        snapshot.narrow_action = shared.narrow_loads.at(loaded_b.Def());
        snapshot.covered = snapshot_ids(shared.covered);
        snapshot.aliases = snapshot_ids(shared.address_aliases);
        snapshot.writebacks = shared.writebacks.size();
        snapshot.narrow_loads = shared.narrow_loads.size();
        REQUIRE(snapshot.writeback.update == updated_a.Def());
        REQUIRE(snapshot.narrow_action.extension == extended_b.Def());
        return snapshot;
    };
    const auto forward = build(false);
    REQUIRE(forward.writebacks == 1);
    REQUIRE(forward.narrow_loads == 1);
    REQUIRE(forward.writeback.base == 13);
    REQUIRE(forward.writeback.offset == 8);
    REQUIRE(forward.writeback.access_bytes == 8);
    REQUIRE(forward.narrow_action.direct);
    REQUIRE(forward.narrow_action.sign);
    REQUIRE(forward.narrow_action.access_bytes == 2);
    REQUIRE(forward.narrow_action.result_bytes == 8);
    const auto swapped = build(true);
    REQUIRE(swapped.writebacks == forward.writebacks);
    REQUIRE(swapped.narrow_loads == forward.narrow_loads);
    REQUIRE(swapped.writeback.base == forward.writeback.base);
    REQUIRE(swapped.writeback.offset == forward.writeback.offset);
    REQUIRE(swapped.writeback.access_bytes == forward.writeback.access_bytes);
    REQUIRE(swapped.writeback.consumed_value != UINT32_MAX);
    REQUIRE(swapped.narrow_action.direct == forward.narrow_action.direct);
    REQUIRE(swapped.narrow_action.sign == forward.narrow_action.sign);
    REQUIRE(swapped.narrow_action.access_bytes == forward.narrow_action.access_bytes);
    REQUIRE(swapped.narrow_action.result_bytes == forward.narrow_action.result_bytes);
    REQUIRE(swapped.covered.size() == forward.covered.size());
    REQUIRE(swapped.aliases.size() == forward.aliases.size());
}

TEST_CASE("memory plans reject stale indexes and follow reallocation", "[memory-plan]") {
    IntrusivePtr<Block> block{new Block(0, Location{0xba88})};
    const auto address = block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    const auto loaded = block->LoadMemory(Operand{address}).SetType(ValueType::U64);
    const auto updated = block->Add(address, Operand{Imm{swift::u64{8}}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{16, ValueType::U64}, loaded);
    block->StoreUniform(Uniform{24, ValueType::U64}, updated);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    RegAlloc allocation{block->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    const auto map_candidates = [&](swift::u16 updated_home) {
        allocation.MapRegister(address.Id(), HostGPR{13});
        allocation.MapRegister(loaded.Id(), HostGPR{8});
        allocation.MapRegister(updated.Id(), HostGPR{updated_home});
    };
    map_candidates(13);
    arm64::GPRBlockPlan earlier;
    const arm64::MemoryLoweringOptions options{};
    IntrusivePtr<Block> other{new Block(0, Location{0xba90})};
    other->LoadUniform(Uniform{0, ValueType::U64});
    other->SetTerminal(terminal::ReturnToHost{});
    other->ReIdInstr();
    arm64::BlockAnalysisIndex stale;
    stale.Build(other.get());
    const auto foreign = arm64::BuildMemoryLoweringPlan(block.get(), allocation, FeatureSet{}, earlier, {}, options, &stale);
    REQUIRE(foreign.writebacks.empty());
    REQUIRE(foreign.narrow_loads.empty());
    REQUIRE(foreign.covered.empty());
    arm64::BlockAnalysisIndex index;
    index.Build(block.get());
    const auto fresh = arm64::BuildMemoryLoweringPlan(block.get(), allocation, FeatureSet{}, earlier, {}, options, &index);
    REQUIRE(fresh.writebacks.contains(loaded.Def()));
    REQUIRE(fresh.writebacks.at(loaded.Def()).offset == 8);
    allocation.ResetAllocations();
    map_candidates(14);
    const auto remapped = arm64::BuildMemoryLoweringPlan(block.get(), allocation, FeatureSet{}, earlier, {}, options, &index);
    REQUIRE(remapped.writebacks.empty());
    REQUIRE(std::find(remapped.covered.begin(), remapped.covered.end(), updated.Def()) == remapped.covered.end());
    allocation.ResetAllocations();
    map_candidates(13);
    const auto restored = arm64::BuildMemoryLoweringPlan(block.get(), allocation, FeatureSet{}, earlier, {}, options, &index);
    REQUIRE(restored.writebacks.size() == fresh.writebacks.size());
    REQUIRE(restored.writebacks.at(loaded.Def()).update == fresh.writebacks.at(loaded.Def()).update);
    REQUIRE(restored.writebacks.at(loaded.Def()).base == fresh.writebacks.at(loaded.Def()).base);
    REQUIRE(restored.writebacks.at(loaded.Def()).offset == fresh.writebacks.at(loaded.Def()).offset);
    REQUIRE(restored.covered == fresh.covered);
    IntrusivePtr<Block> plain{new Block(0, Location{0xba98})};
    const auto plain_address = plain->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    const auto plain_loaded = plain->LoadMemory(Operand{plain_address}).SetType(ValueType::U64);
    plain->StoreUniform(Uniform{16, ValueType::U64}, plain_loaded);
    plain->SetTerminal(terminal::ReturnToHost{});
    plain->ReIdInstr();
    RegAlloc plain_allocation{plain->MaxInstrId(), GPRSMask{}, FPRSMask{}, FeatureSet{}};
    plain_allocation.MapRegister(plain_address.Id(), HostGPR{13});
    plain_allocation.MapRegister(plain_loaded.Id(), HostGPR{8});
    arm64::BlockAnalysisIndex plain_index;
    plain_index.Build(plain.get());
    const arm64::MemoryLoweringOptions fused{.narrow_fuse = true};
    const auto zero = arm64::BuildMemoryLoweringPlan(plain.get(), plain_allocation, FeatureSet{}, earlier, {}, fused, &plain_index);
    REQUIRE(zero.writebacks.empty());
    REQUIRE(zero.narrow_loads.empty());
    REQUIRE(zero.covered.empty());
}
