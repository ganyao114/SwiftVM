#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>

#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/execution_owner.h"
#include "runtime/backend/guest_memory_scope.h"
#include "runtime/backend/interrupt_request.h"
#include "runtime/backend/riscv64/memory_protocol.h"
#include "runtime/backend/runtime.h"
#include "runtime/frontend/ir_assembler.h"
#include "translator/arm64/translator.h"

using namespace swift;
using namespace swift::runtime;
namespace a64 = swift::translator::arm64;
namespace {
using T = ir::ValueType;
constexpr u32 uniform_size = 256;
constexpr u64 start_pc = 0x1000;
constexpr u64 helper_value = 0x8123456789abcdefULL;
constexpr u64 suffix_value = 0x7351928046abcdefULL;

struct Probe final : MemoryInterface {
    std::atomic<unsigned> reads{}, writes{}, pointers{}, fetches{}, decodes{};
    uintptr_t bias{};
    u64 mask{UINT64_MAX};
    bool Read(void*, size_t, size_t) override { ++reads; return false; }
    bool Write(void*, size_t, size_t) override { ++writes; return false; }
    void* GetPointer(void*) override { ++pointers; return nullptr; }
    CodeFetchResult ReadCode(void*, size_t, size_t) override { ++fetches; return CodeFetchResult::Unsupported; }
    bool MatchesCodeAddressSpace(uintptr_t expected_bias, u64 expected_mask) const override {
        return bias == expected_bias && mask == expected_mask;
    }
    static SingleDecodeResult Decode(const void* context, LocationDescriptor,
                                    MemoryInterface&, ir::Block&) {
        ++static_cast<Probe*>(const_cast<void*>(context))->decodes;
        return SingleDecodeResult::Unsupported;
    }
    bool Untouched() const {
        return !reads.load() && !writes.load() && !pointers.load() &&
               !fetches.load() && !decodes.load();
    }
    bool NoMemoryAccess() const {
        return !reads.load() && !writes.load() && !pointers.load() && !fetches.load();
    }
};

Config Configuration(bool native, Probe* probe = nullptr, bool service = false,
                     void* bias = nullptr, u64 mask = 0) {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = native,
            .backend_isa = kX86_64, .uniform_buffer_size = uniform_size,
            .static_program = true, .global_opts = Optimizations::None,
            .stack_alignment = 16, .memory_base = bias, .guest_addr_mask = mask,
            .memory = probe, .single_decode = service ? SingleDecodeService{&Probe::Decode, probe}
                                                   : SingleDecodeService{}};
}

struct StateSnapshot {
    std::array<u8, sizeof(backend::State) + uniform_size> state;
    std::array<u8, sizeof(backend::RuntimeProfileInterface)> profile;
    bool operator==(const StateSnapshot&) const = default;
};
StateSnapshot Capture(Runtime& runtime) {
    StateSnapshot result;
    std::memcpy(result.state.data(), runtime.GetState(), result.state.size());
    std::memcpy(result.profile.data(), runtime.GetState()->interface, result.profile.size());
    return result;
}
void Put(Runtime& runtime, u32 offset, u64 value) {
    std::memcpy(runtime.GetUniformBuffer().data() + offset, &value, 8);
}
u64 Get(Runtime& runtime, u32 offset) {
    u64 value;
    std::memcpy(&value, runtime.GetUniformBuffer().data() + offset, 8);
    return value;
}
void Seed(Runtime& runtime) {
    const auto bytes = runtime.GetUniformBuffer();
    for (u32 i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(i * 37 + 0xa5);
    runtime.GetState()->host_cpu_flags = 0xabcde98765432109ULL;
    runtime.GetState()->prev_loc = ir::Location{0x8123};
    for (u32 i = 0; i < runtime.GetState()->spill_area.size(); ++i)
        runtime.GetState()->spill_area[i] = 0xa500000000000000ULL + i;
    runtime.SetLocation(start_pc);
}
void Publish(backend::AddressSpace& space, const IntrusivePtr<ir::Block>& block) {
    REQUIRE(space.GetDefaultModule()->Push(block.get()));
    if (space.GetConfig().enable_jit)
        REQUIRE(backend::TranslateIR(space.GetDefaultModule(), block) != nullptr);
}
template<class Predicate> bool Poll(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

struct Mapping {
    void* address = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ~Mapping() { if (address != MAP_FAILED) munmap(address, 0x10000); }
};
using ArmInstance = std::unique_ptr<a64::Arm64Instance, decltype(&a64::Arm64Instance::Destroy)>;
using ArmCore = std::unique_ptr<a64::Arm64Core, decltype(&a64::Arm64Core::Destroy)>;
struct CoreSnapshot {
    std::array<u8, sizeof(swift::arm64::ThreadContext64)> context;
    u64 syscall{};
    bool operator==(const CoreSnapshot&) const = default;
};
CoreSnapshot Capture(a64::Arm64Core& core) {
    CoreSnapshot result;
    std::memcpy(result.context.data(), &core.GetContext(), result.context.size());
    result.syscall = core.GetSyscallNumber();
    return result;
}
void Prime(a64::Arm64Instance& instance, a64::Arm64Core& core, u64 pc, u64 syscall) {
    // Publish a known host-return unit, so initialization never relies on
    // unchecked ordinary frontend fetch or the global mapping oracle.
    auto& space = *instance.GetAddressSpace();
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{pc})};
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Publish(space, block);
    auto& context = core.GetContext();
    auto* bytes = reinterpret_cast<u8*>(&context);
    for (size_t i = 0; i < sizeof(context); ++i) bytes[i] = static_cast<u8>(i * 19 + syscall);
    context.pc = pc;
    context.r[8] = syscall;
    REQUIRE(core.Run() == swift::translator::ExitReason::Syscall);
    REQUIRE(core.GetSyscallNumber() == syscall);
}

struct NestedRequest {
    Runtime* outer{};
    Runtime* other{};
    std::array<a64::Arm64Core*, 3> cores{};
    bool rendezvous{};
    std::atomic<bool> entered{}, rejections_done{}, verified{}, release{};
    std::atomic<unsigned> calls{};
    std::array<HaltReason, 4> reasons{};
    std::array<swift::translator::ExitReason, 6> core_reasons{};
    bool active_before{}, active_after{}, outer_unchanged{}, other_unchanged{};
    bool mapping_unchanged{}, fp_unchanged{}, kernel_unchanged{}, cores_unchanged{true};
    bool concurrent_state_unchanged{};
    u64 observed_bias{}, observed_mask{};
};

u64 NestedHelper(u64 pointer, u64, u64) {
    auto& request = *reinterpret_cast<NestedRequest*>(pointer);
    ++request.calls;
    request.active_before = backend::RuntimeExecutionActiveOnCurrentThread();
    const auto outer = Capture(*request.outer);
    const auto other = Capture(*request.other);
    const auto* mapping = &backend::GuestMemoryScope::Current();
    request.observed_bias = mapping->bias;
    request.observed_mask = mapping->mask;
    const auto fp = _mm_getcsr();
    std::atomic<unsigned> gate{};
    {
        // A real helper memory admission is still owned by the parent. Nested
        // rejected execution must not run its abandoned-kernel cleanup.
        backend::riscv64::MemoryKernelGuard kernel{gate};
        const auto depth = backend::riscv64::memory_kernel_depth.load();
        request.reasons = {request.outer->Run(), request.outer->Step(),
                           request.other->Run(), request.other->Step()};
        for (u32 i = 0; i < request.cores.size(); ++i) if (request.cores[i]) {
            const auto before = Capture(*request.cores[i]);
            request.core_reasons[i * 2] = request.cores[i]->Run();
            request.core_reasons[i * 2 + 1] = request.cores[i]->Step();
            request.cores_unchanged &= before == Capture(*request.cores[i]);
        }
        request.kernel_unchanged = depth == backend::riscv64::memory_kernel_depth.load();
    }
    request.active_after = backend::RuntimeExecutionActiveOnCurrentThread();
    request.outer_unchanged = outer == Capture(*request.outer);
    request.other_unchanged = other == Capture(*request.other);
    const auto& after_mapping = backend::GuestMemoryScope::Current();
    request.mapping_unchanged = mapping == &after_mapping && mapping->bias == request.observed_bias &&
                                mapping->mask == request.observed_mask;
    request.fp_unchanged = _mm_getcsr() == fp;
    request.entered.store(true, std::memory_order_release);
    if (request.rendezvous) {
        while (!request.rejections_done.load(std::memory_order_acquire)) std::this_thread::yield();
        // The remote rejection calls have completed, before any permitted
        // SignalInterrupt writes. Only this execution owner snapshots State.
        request.concurrent_state_unchanged = outer == Capture(*request.outer);
        request.verified.store(true, std::memory_order_release);
        while (!request.release.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    return helper_value;
}

IntrusivePtr<ir::Block> HelperBlock(bool fault = false) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{start_pc})};
    ir::Assembler assembler{block.get()};
    auto pointer = assembler.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto result = assembler.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&NestedHelper)}},
                                      pointer, pointer, pointer).SetType(T::U64);
    assembler.StoreUniform(ir::Uniform{8, T::U64}, result);
    if (fault) {
        assembler.AdvancePC(ir::Imm{u32{4}});
        auto address = assembler.LoadImm(ir::Imm{u64{0}}).SetType(T::U64);
        auto value = assembler.LoadMemory(ir::Operand{address}).SetType(T::U64);
        assembler.StoreUniform(ir::Uniform{16, T::U64}, value);
    }
    assembler.StoreUniform(ir::Uniform{24, T::U64}, assembler.LoadImm(ir::Imm{suffix_value}).SetType(T::U64));
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}
void CheckRequest(const NestedRequest& request, void* bias, u64 mask) {
    REQUIRE(request.active_before);
    REQUIRE(request.active_after);
    for (const auto reason : request.reasons) REQUIRE(reason == HaltReason::EntryRejected);
    REQUIRE(request.outer_unchanged);
    REQUIRE(request.other_unchanged);
    REQUIRE(request.mapping_unchanged);
    REQUIRE(request.fp_unchanged);
    REQUIRE(request.kernel_unchanged);
    REQUIRE(request.observed_bias == reinterpret_cast<u64>(bias));
    REQUIRE(request.observed_mask == mask);
    REQUIRE_FALSE(backend::RuntimeExecutionActiveOnCurrentThread());
}
}

TEST_CASE("Runtime Step missing capability or unsupported fresh decode preserves state and publication", "[runtime][step][ownership]") {
    REQUIRE(static_cast<u32>(HaltReason::Unsupported) == 0x200);
    REQUIRE(static_cast<u32>(HaltReason::EntryRejected) == 0x400);
    for (bool native : {false, true}) for (bool service : {false, true})
    for (bool has_memory : {false, true}) {
        CAPTURE(native, service, has_memory);
        Probe probe;
        auto config = Configuration(native, &probe, service);
        if (!has_memory) config.memory = nullptr;
        backend::AddressSpace space{config};
        Runtime runtime{&space};
        Seed(runtime);
        const auto module = space.GetDefaultModule();
        for (const auto reason : {HaltReason::None, HaltReason::PageFatal, HaltReason::CallHost}) {
            runtime.GetState()->halt_reason = reason;
            runtime.GetState()->exit_request = 17;
            const auto before = Capture(runtime);
            const auto decodes = probe.decodes.load();
            REQUIRE(runtime.Step() == HaltReason::Unsupported);
            REQUIRE(Capture(runtime) == before);
            REQUIRE(runtime.GetLocation() == start_pc);
            REQUIRE(probe.NoMemoryAccess());
            REQUIRE(probe.decodes.load() == decodes + unsigned(native && service && has_memory));
            REQUIRE(backend::IsEmpty(module->GetNode(ir::Location{start_pc})));
            REQUIRE(space.GetCodeCache(ir::Location{start_pc}) == nullptr);
            REQUIRE_FALSE(backend::RuntimeExecutionActiveOnCurrentThread());
        }
        // Even an existing IR/native cache entry must not become a Step unit.
        runtime.GetState()->halt_reason = HaltReason::None;
        runtime.GetState()->exit_request = 0;
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{start_pc})};
        ir::Assembler assembler{block.get()};
        assembler.StoreUniform(ir::Uniform{8, T::U64}, assembler.LoadImm(ir::Imm{helper_value}).SetType(T::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Publish(space, block);
        const auto cache = space.GetCodeCache(ir::Location{start_pc});
        const auto before = Capture(runtime);
        const auto decodes = probe.decodes.load();
        REQUIRE(runtime.Step() == HaltReason::Unsupported);
        REQUIRE(Capture(runtime) == before);
        REQUIRE(space.GetCodeCache(ir::Location{start_pc}) == cache);
        REQUIRE(probe.NoMemoryAccess());
        REQUIRE(probe.decodes.load() == decodes + unsigned(native && service && has_memory));
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(Get(runtime, 8) == helper_value);
    }
}

TEST_CASE("Runtime Step observes sticky Signal without consuming state or fetching", "[runtime][step][interrupt]") {
    for (bool native : {false, true}) for (bool service : {false, true}) {
        Probe probe;
        backend::AddressSpace space{Configuration(native, &probe, service)};
        Runtime runtime{&space};
        Seed(runtime);
        runtime.GetState()->halt_reason = HaltReason::PageFatal;
        runtime.GetState()->exit_request = 17;
        runtime.SignalInterrupt();
        const auto requested = Capture(runtime);
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            REQUIRE(runtime.Step() == HaltReason::Signal);
            REQUIRE(Capture(runtime) == requested);
            REQUIRE(probe.Untouched());
            REQUIRE_FALSE(backend::RuntimeExecutionActiveOnCurrentThread());
        }
        REQUIRE((runtime.GetState()->exit_request & kBackedgeSignalRequest) != 0);
        runtime.ClearInterrupt();  // stopped owner only
        REQUIRE(runtime.GetState()->exit_request == 17);
        REQUIRE(runtime.GetState()->halt_reason == HaltReason::None);
        const auto cleared = Capture(runtime);
        REQUIRE(runtime.Step() == HaltReason::Unsupported);
        REQUIRE(Capture(runtime) == cleared);
        REQUIRE(probe.NoMemoryAccess());
        REQUIRE(probe.decodes.load() == unsigned(native && service));
    }
}

TEST_CASE("Runtime native and interpreter helpers reject recursive and nested Runtime and Core entry", "[runtime][ownership][reentry]") {
    Mapping outer_memory, other_memory, core_a_memory, core_b_memory;
    REQUIRE(outer_memory.address != MAP_FAILED);
    REQUIRE(other_memory.address != MAP_FAILED);
    REQUIRE(core_a_memory.address != MAP_FAILED);
    REQUIRE(core_b_memory.address != MAP_FAILED);
    Probe core_a_probe, core_b_probe;
    core_a_probe.bias = reinterpret_cast<uintptr_t>(core_a_memory.address); core_a_probe.mask = 0xffff;
    core_b_probe.bias = reinterpret_cast<uintptr_t>(core_b_memory.address); core_b_probe.mask = 0xffff;
    ArmInstance arm_a{a64::Arm64Instance::Make(core_a_memory.address, 0xffff, &core_a_probe), &a64::Arm64Instance::Destroy};
    ArmInstance arm_b{a64::Arm64Instance::Make(core_b_memory.address, 0xffff, &core_b_probe), &a64::Arm64Instance::Destroy};
    ArmCore core_a{a64::Arm64Core::Make(arm_a.get()), &a64::Arm64Core::Destroy};
    ArmCore core_same_instance{a64::Arm64Core::Make(arm_a.get()), &a64::Arm64Core::Destroy};
    ArmCore core_other_instance{a64::Arm64Core::Make(arm_b.get()), &a64::Arm64Core::Destroy};
    Prime(*arm_a, *core_a, 0x3000, 0xface123);
    Prime(*arm_a, *core_same_instance, 0x4000, 0xfade456);
    Prime(*arm_b, *core_other_instance, 0x5000, 0xbeef789);
    for (bool native : {false, true}) {
        CAPTURE(native);
        Probe probe;
        backend::AddressSpace outer_space{Configuration(native, &probe, true, outer_memory.address, 0xffff)};
        backend::AddressSpace other_space{Configuration(native, &probe, true, other_memory.address, 0xfff)};
        Runtime outer{&outer_space}, other{&other_space};
        Seed(outer); Seed(other);
        NestedRequest request{.outer = &outer, .other = &other,
            .cores = {core_a.get(), core_same_instance.get(), core_other_instance.get()}};
        Publish(outer_space, HelperBlock());
        Put(outer, 0, reinterpret_cast<u64>(&request));
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const auto decodes = probe.decodes.load();
            const auto core_a_fetches = core_a_probe.fetches.load();
            const auto core_b_fetches = core_b_probe.fetches.load();
            outer.SetLocation(start_pc);
            REQUIRE(outer.Run() == HaltReason::CallHost);
            CheckRequest(request, outer_memory.address, 0xffff);
            REQUIRE(Get(outer, 8) == helper_value);
            REQUIRE(Get(outer, 24) == suffix_value);
            REQUIRE(request.cores_unchanged);
            for (auto reason : request.core_reasons)
                REQUIRE(reason == swift::translator::ExitReason::EntryRejected);
            REQUIRE(probe.NoMemoryAccess());
            REQUIRE(probe.decodes.load() == decodes);
            REQUIRE(core_a_probe.fetches.load() == core_a_fetches);
            REQUIRE(core_b_probe.fetches.load() == core_b_fetches);
            REQUIRE(core_a_probe.reads.load() == 0);
            REQUIRE(core_a_probe.pointers.load() == 0);
            REQUIRE(core_b_probe.reads.load() == 0);
            REQUIRE(core_b_probe.pointers.load() == 0);
            REQUIRE(other.Step() == HaltReason::Unsupported);
            REQUIRE(probe.decodes.load() == decodes + unsigned(native));
            REQUIRE(other.Run() == HaltReason::CodeMiss);
            REQUIRE(core_a->Step() == swift::translator::ExitReason::Unsupported);
            REQUIRE(core_same_instance->Step() == swift::translator::ExitReason::Unsupported);
            REQUIRE(core_other_instance->Step() == swift::translator::ExitReason::Unsupported);
            REQUIRE(core_a_probe.fetches.load() == core_a_fetches +
                2 * unsigned(arm_a->GetAddressSpace()->GetConfig().enable_jit));
            REQUIRE(core_b_probe.fetches.load() == core_b_fetches +
                unsigned(arm_b->GetAddressSpace()->GetConfig().enable_jit));
        }
        REQUIRE(request.calls.load() == 2);
        REQUIRE(backend::GuestMemoryScope::Current().bias == 0);
        REQUIRE(backend::GuestMemoryScope::Current().mask == UINT64_MAX);
        REQUIRE(backend::riscv64::memory_kernel_depth.load() == 0);
        REQUIRE(backend::riscv64::anonymous_memory_readers.load() == 0);
    }
}

TEST_CASE("Runtime concurrent entry is rejected before state access and permits owner Signal and retry", "[runtime][ownership][concurrent]") {
    for (bool native : {false, true}) for (bool interrupt : {false, true}) {
        CAPTURE(native, interrupt);
        Mapping memory;
        REQUIRE(memory.address != MAP_FAILED);
        Probe probe;
        backend::AddressSpace outer_space{Configuration(native, &probe, true, memory.address, 0xffff)};
        backend::AddressSpace other_space{Configuration(native, &probe, true)};
        Runtime outer{&outer_space}, other{&other_space};
        Seed(outer); Seed(other);
        NestedRequest request{.outer = &outer, .other = &other, .rendezvous = true};
        Publish(outer_space, HelperBlock());
        Put(outer, 0, reinterpret_cast<u64>(&request));
        HaltReason owner_reason = HaltReason::None;
        bool owner_tls_released{};
        std::thread owner([&] {
            owner_reason = outer.Run();
            owner_tls_released = !backend::RuntimeExecutionActiveOnCurrentThread();
        });
        const bool entered = Poll([&] { return request.entered.load(std::memory_order_acquire); });
        // No rejected thread reads ordinary State/context while the owner runs.
        std::array<HaltReason, 4> rejected{};
        bool caller_inactive = !backend::RuntimeExecutionActiveOnCurrentThread();
        if (entered) {
            rejected = {outer.Run(), outer.Step(), outer.Run(), outer.Step()};
        }
        request.rejections_done.store(true, std::memory_order_release);
        const bool verified = entered && Poll([&] { return request.verified.load(std::memory_order_acquire); });
        if (verified && interrupt) outer.SignalInterrupt();
        request.release.store(true, std::memory_order_release);
        owner.join();
        REQUIRE(entered);
        REQUIRE(verified);
        REQUIRE(request.concurrent_state_unchanged);
        REQUIRE(caller_inactive);
        for (const auto reason : rejected) REQUIRE(reason == HaltReason::EntryRejected);
        CheckRequest(request, memory.address, 0xffff);
        REQUIRE(owner_tls_released);
        REQUIRE(owner_reason == (interrupt ? HaltReason::Signal : HaltReason::CallHost));
        REQUIRE(probe.Untouched());
        if (interrupt) {
            const auto before = Capture(outer);  // stopped after join
            REQUIRE(outer.Step() == HaltReason::Signal);
            REQUIRE(Capture(outer) == before);
            outer.ClearInterrupt();
        }
        request.rendezvous = false;
        outer.SetLocation(start_pc);
        REQUIRE(outer.Step() == HaltReason::Unsupported);
        REQUIRE(outer.Run() == HaltReason::CallHost);
        REQUIRE(Get(outer, 8) == helper_value);
        REQUIRE(Get(outer, 24) == suffix_value);
        REQUIRE(request.calls.load() == 2);
        REQUIRE_FALSE(backend::RuntimeExecutionActiveOnCurrentThread());
    }
}

TEST_CASE("Runtime rejected helper entry leaves native fault recovery attached to its parent", "[runtime][ownership][fault]") {
    Mapping memory;
    REQUIRE(memory.address != MAP_FAILED);
    REQUIRE(mprotect(memory.address, 4096, PROT_NONE) == 0);
    backend::AddressSpace outer_space{Configuration(true, nullptr, false, memory.address, 0xffff)};
    backend::AddressSpace other_space{Configuration(true)};
    Runtime outer{&outer_space}, other{&other_space};
    NestedRequest request{.outer = &outer, .other = &other};
    Publish(outer_space, HelperBlock(true));
    Put(outer, 0, reinterpret_cast<u64>(&request));
    Put(outer, 16, 0xdead1111);
    Put(outer, 24, 0xdead2222);
    outer.SetLocation(start_pc);
    REQUIRE(outer.Run() == HaltReason::PageFatal);
    CheckRequest(request, memory.address, 0xffff);
    REQUIRE(outer.GetLocation() == start_pc + 4);
    REQUIRE(Get(outer, 8) == helper_value);
    REQUIRE(Get(outer, 16) == 0xdead1111);
    REQUIRE(Get(outer, 24) == 0xdead2222);
    REQUIRE(outer.GetState()->halt_reason == HaltReason::None);
    REQUIRE(backend::riscv64::memory_kernel_depth.load() == 0);
    REQUIRE(backend::riscv64::anonymous_memory_readers.load() == 0);
    REQUIRE(mprotect(memory.address, 4096, PROT_READ | PROT_WRITE) == 0);
    const u64 loaded = 0x173256abcdef1234;
    std::memcpy(memory.address, &loaded, 8);
    outer.SetLocation(start_pc);
    REQUIRE(outer.Run() == HaltReason::CallHost);
    CheckRequest(request, memory.address, 0xffff);
    REQUIRE(Get(outer, 16) == loaded);
    REQUIRE(Get(outer, 24) == suffix_value);
    REQUIRE(other.Step() == HaltReason::Unsupported);
    REQUIRE(other.Run() == HaltReason::CodeMiss);
}
