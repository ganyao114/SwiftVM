#include "runtime/execution/dispatch.h"
#include "runtime/execution/dispatch_cache.h"
#include "runtime/execution/fault.h"
#include "runtime/execution/vm.h"
#include "runtime/platform/linux/host_runtime.h"
#include "tests/support/test_mode.h"

#if defined(__aarch64__)
#include "runtime/backend/arm64/runtime.h"
#endif

#include <array>
#include <cerrno>
#include <cstring>
#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <elf.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

// These resolver ABIs return executable entries. Enter their shared lookup
// with the real guest target, without a compiler-generated host prologue.
#if defined(__aarch64__) || defined(__riscv) || defined(__loongarch64)
extern "C" void SvmTestDispatchInterruptEntry() noexcept;
asm(".pushsection .text\n"
    ".p2align 2\n"
    ".global SvmTestDispatchInterruptEntry\n"
    ".hidden SvmTestDispatchInterruptEntry\n"
    "SvmTestDispatchInterruptEntry:\n"
#if defined(__aarch64__)
    "ldr x0, [x19, #128]\n"
    "b SvmNativeDispatch\n"
#elif defined(__riscv)
    "ld a0, 128(s1)\n"
    "tail SvmNativeDispatch\n"
#else
    "ld.d $a0, $r22, 128\n"
    "b SvmNativeDispatch\n"
#endif
    ".popsection\n");
#endif

namespace {
namespace execution = swift::runtime::execution;
namespace platform = swift::runtime::platform;
namespace x86 = swift::runtime::x86;
using u64 = std::uint64_t;

u64 Ready(std::uint8_t*) { return 42; }

constexpr u64 image_address = 0x400000, entry_pc = image_address + 0x100,
              body_pc = image_address + 0x180, stop_pc = 0xdeadbeef0;

std::vector<std::uint8_t> Guest(std::span<const std::uint8_t> entry,
                                std::span<const std::uint8_t> body,
                                bool writable = false) {
    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_EXEC;
    header.e_machine = EM_X86_64;
    header.e_version = EV_CURRENT;
    header.e_entry = entry_pc;
    header.e_phoff = sizeof(header);
    header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(Elf64_Phdr);
    header.e_phnum = 1;
    Elf64_Phdr segment{};
    segment.p_type = PT_LOAD;
    segment.p_flags = PF_R | PF_X | (writable ? PF_W : 0);
    segment.p_vaddr = segment.p_paddr = image_address;
    segment.p_filesz = segment.p_memsz = 0x200;
    segment.p_align = 4096;
    std::vector<std::uint8_t> bytes(segment.p_filesz, 0xcc);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), &segment, sizeof(segment));
    std::memcpy(bytes.data() + 0x100, entry.data(), entry.size());
    std::memcpy(bytes.data() + 0x180, body.data(), body.size());
    return bytes;
}

struct CallMemory {
    u64 control, stack;
};

CallMemory PrepareCallMemory(execution::VM& vm) {
    const auto page = vm.Memory().PageSize();
    const auto address =
            vm.Memory().Map(0, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    REQUIRE(address != UINT64_MAX);
    const auto stack = address + page * 2 - sizeof(stop_pc);
    std::memcpy(vm.HostPointer(stack, sizeof(stop_pc), true), &stop_pc, sizeof(stop_pc));
    return {address, stack};
}
}  // namespace

TEST_CASE("thread dispatch cache owns sentinel and full-PC replacement",
          "[compile][dispatch-cache]") {
    execution::DispatchCache cache;
    REQUIRE(cache.Data() == nullptr);
    cache.Ensure();
    auto* const mapping = cache.Data();
    REQUIRE(mapping != nullptr);
    CHECK(mapping[0].pc == UINT64_MAX);
    CHECK(mapping[0].entry == nullptr);
    CHECK(mapping[execution::DispatchCache::Index(UINT64_MAX)].pc != UINT64_MAX);
    cache.Fill(0, &Ready);
    cache.Fill(UINT64_MAX, &Ready);
    CHECK(mapping[0].pc == 0);
    CHECK(mapping[0].entry == &Ready);
    CHECK(mapping[execution::DispatchCache::Index(UINT64_MAX)].entry == &Ready);
    constexpr u64 alias = u64{1} << SVM_DISPATCH_L1_BITS;
    REQUIRE(execution::DispatchCache::Index(alias) == 0);
    cache.Fill(alias, &Ready);
    CHECK(mapping[0].pc == alias);
    cache.ClearKey(alias);
    CHECK(mapping[0].pc == UINT64_MAX);
    CHECK(mapping[0].entry == nullptr);
    CHECK(mapping[execution::DispatchCache::Index(UINT64_MAX)].entry == &Ready);
    cache.Fill(alias, &Ready);
    cache.Reset();
    REQUIRE(cache.Data() == mapping);
    CHECK(mapping[0].pc == UINT64_MAX);
    CHECK(mapping[0].entry == nullptr);
    CHECK(mapping[execution::DispatchCache::Index(UINT64_MAX)].pc == 0);
    CHECK(mapping[execution::DispatchCache::Index(UINT64_MAX)].entry == nullptr);
    CHECK(cache.Release().Complete());
    CHECK(cache.Data() == nullptr);
}

TEST_CASE("shared dispatch interrupt returns to its owner and accepts an in-flight stale view",
          "[native][dispatch-interrupt]") {
    REQUIRE(swift::tests::NativeMode());
    platform::HostRuntime host{false};
    const auto backend = host.Bind();
    REQUIRE(backend.recover_native != nullptr);
    execution::DispatchCache cache;
    execution::DispatchInterrupt interrupt;
    cache.Ensure();
    interrupt.Ensure();
    REQUIRE(interrupt.Data() != nullptr);
    const auto base = reinterpret_cast<std::uintptr_t>(interrupt.Data());
    std::uint8_t byte{};
    for (const auto offset : {std::size_t{0}, execution::DispatchCache::Bytes - 1}) {
        iovec local{&byte, 1}, remote{reinterpret_cast<void*>(base + offset), 1};
        errno = 0;
        CHECK(syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == -1);
        CHECK(errno == EFAULT);
    }

    struct Resolver {
        execution::DispatchEntry* interrupt;
        unsigned calls{};
    } resolver{interrupt.Data()};

    execution::FaultContext faults;
    execution::DispatchSlot slot;
    execution::NativeRunContext native, other;
    other.l1.store(cache.Data());
    native.opaque = &resolver;
    native.resolve = [](void* raw,
                        execution::NativeRunContext* context) noexcept -> std::uintptr_t {
        auto& resolver = *static_cast<Resolver*>(raw);
        ++resolver.calls;
#if !defined(__x86_64__)
        if (context->l1.load() == resolver.interrupt)
            return reinterpret_cast<std::uintptr_t>(&SvmTestDispatchInterruptEntry);
#else
        (void)context;
#endif
        return 0;
    };
    native.slots = &slot;
    native.active = &faults.active;
    native.faults = &faults;
#if defined(__x86_64__)
    native.guest_fpcr = 0x1f80;
#endif
    faults.native = &native;
    faults.recover_native = backend.recover_native;
    faults.dispatch_interrupt_bytes = execution::DispatchCache::Bytes;
    faults.dispatch_interrupt_base.store(base);
    {
        execution::FaultScope scope{faults, native.state};
        for (const auto pending : {u64{1}, u64{0}}) {
            const auto target = (u64{0x12345} << SVM_DISPATCH_L1_BITS) |
                                (pending ? 0 : execution::DispatchCache::Entries - 1);
            native.state.pc = target;
#if defined(__x86_64__)
            native.native_eflags = 2;
            native.flags_offset = faults.flags_offset = offsetof(x86::GuestState, packed_flags);
            native.state.packed_flags = (u64{1} << 55) | (pending ? 0x90000025 : 0x600000a5);
            const auto expected_flags = native.state.packed_flags;
#endif
            native.pending_events.store(pending);
            native.l1.store(interrupt.Data());
            const auto before = faults.dispatch_interrupts.load();
            REQUIRE(execution::SvmRunNative(&native) ==
                    swift::runtime::ExitValue(swift::runtime::NativeExit::GuestEvents));
            CHECK(native.state.pc == target);
#if defined(__x86_64__)
            CHECK(native.state.packed_flags == expected_flags);
#endif
            CHECK(!faults.exception_pending);
            CHECK(faults.signal_number == 0);
            CHECK(faults.dispatch_interrupts.load() == before + 1);
            CHECK(!faults.active.load());
            CHECK(native.root_sp == 0);
            CHECK(native.phase.load() == SVM_NATIVE_PHASE_RUNTIME);
            CHECK(other.l1.load() == cache.Data());
        }
        native.pending_events.store(0);
        native.l1.store(cache.Data());
        native.state.pc = entry_pc;
        const auto resolves = resolver.calls;
        REQUIRE(execution::SvmRunNative(&native) ==
                swift::runtime::ExitValue(swift::runtime::NativeExit::ResolveFailure));
        CHECK(resolver.calls == resolves + 1);
        CHECK(native.state.pc == entry_pc);
        CHECK(!faults.exception_pending);
        CHECK(faults.dispatch_interrupts.load() == 2);
        CHECK(other.l1.load() == cache.Data());
    }
    {
        execution::RuntimeStats stats;
        execution::VM vm{{}, stats, backend};
        // Borrow the real owner through the fixed native ABI, without a test
        // accessor in the public VM interface. Model cancellation after a
        // lookup has already selected the protected view.
        auto& owner = *reinterpret_cast<execution::NativeRunContext*>(
                reinterpret_cast<std::uint8_t*>(&vm.State()) -
                offsetof(execution::NativeRunContext, state));
        owner.faults->dispatch_interrupt_bytes = execution::DispatchCache::Bytes;
        owner.faults->dispatch_interrupt_base.store(base);
        owner.faults->dispatch_resume_l1.store(reinterpret_cast<std::uintptr_t>(cache.Data()));
        owner.l1.store(interrupt.Data());
        auto& signals = *static_cast<platform::GuestSignals*>(owner.faults->signal_opaque);
        swift::runtime::GuestExceptionInfo event;
        REQUIRE(!signals.TakePending(event));
        CHECK(owner.pending_events.load() == 0);
        CHECK(owner.l1.load() == cache.Data());
        CHECK(vm.Close().Complete());
    }
    CHECK(interrupt.Release().Complete());
    CHECK(cache.Release().Complete());
}

TEST_CASE("thread dispatch follows repeated guest FP-domain changes", "[native][dispatch-cache]") {
    REQUIRE(swift::tests::NativeMode());
    // LDMXCSR [RDI]; MOVQ XMM0,RSI; MOVQ XMM1,RDX; JMP RBX.
    constexpr std::uint8_t entry[]{0x0f,
                                   0xae,
                                   0x17,
                                   0x66,
                                   0x48,
                                   0x0f,
                                   0x6e,
                                   0xc6,
                                   0x66,
                                   0x48,
                                   0x0f,
                                   0x6e,
                                   0xca,
                                   0xff,
                                   0xe3};
    // MULSD XMM0,XMM1; MOVQ RAX,XMM0; RET.
    constexpr std::uint8_t body[]{0xf2, 0x0f, 0x59, 0xc1, 0x66, 0x48, 0x0f, 0x7e, 0xc0, 0xc3};

    struct Case {
        std::uint32_t control;
        u64 lhs, rhs, expected;
    };

    constexpr u64 normal = 0x0010000000000000, subnormal = 0x0008000000000000,
                  two = 0x4000000000000000, half = 0x3fe0000000000000;
    constexpr std::array cases{Case{0x1f80, subnormal, two, normal},
                               Case{0x1fc0, subnormal, two, 0},
                               Case{0x9fc0, normal, half, 0},
                               Case{0x9f80, normal, half, 0}};
    for (const auto scope :
         {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function}) {
        platform::HostRuntime host{true};
#if defined(__aarch64__)
        auto capabilities = host.Bind().capabilities;
        capabilities.afp = false;
        swift::runtime::backend::arm64::Runtime software_flush{true, capabilities};
        auto backend = software_flush.Bind();
#else
        auto backend = host.Bind();
#endif
        // Exercise all four owner domains even on hosts with native flush control.
        // ARM uses actual software-flush code, so a stale L1 entry changes results.
        backend.specializes_fp_flush = true;
        execution::RuntimeOptions options;
        options.scope = scope;
        options.decode_timing = true;
        execution::RuntimeStats stats;
        execution::VM vm{options, stats, backend};
        vm.PrepareGuest(Guest(entry, body), "dispatch-fp-domains.elf");
        const auto memory = PrepareCallMemory(vm);
        const auto call = [&](const Case& test) {
            std::memcpy(vm.HostPointer(memory.control, sizeof(test.control), true),
                        &test.control,
                        sizeof(test.control));
            auto& state = vm.State();
            state.gprs[x86::RDI] = memory.control;
            state.gprs[x86::RSI] = test.lhs;
            state.gprs[x86::RDX] = test.rhs;
            state.gprs[x86::RBX] = body_pc;
            vm.CallGuest(entry_pc, memory.stack, stop_pc);
            CHECK(state.gprs[x86::RAX] == test.expected);
        };
        for (unsigned revisit = 0; revisit != 2; ++revisit)
            for (const auto& test : cases) {
                INFO("scope=" << static_cast<unsigned>(scope) << " control=" << test.control);
                const auto before = stats.compile_attempts;
                // The second call also warms the entry prefix in the new domain.
                call(test);
                call(test);
                const auto warmed = stats.compile_attempts;
                call(test);
                CHECK(stats.compile_attempts == warmed);
                if (revisit)
                    CHECK(stats.compile_attempts == before);
                else
                    CHECK(stats.compile_attempts > before);
            }
        const auto warmed = stats.compile_attempts;
        // RC changes while DAZ/FTZ remain unchanged; the multiplication is exact.
        call({0xbf80, normal, two, 0x0020000000000000});
        CHECK(stats.compile_attempts == warmed);
        CHECK(stats.interpreted_steps == 0);
        CHECK(vm.Close().Complete());
        CHECK(stats.thread_dispatch_cache);
        CHECK(stats.thread_l1_bytes == execution::DispatchCache::Bytes);
        CHECK(stats.cache_hits > 0);
    }
}

TEST_CASE("watched guest write revokes a warm indirect target before reentry",
          "[native][dispatch-cache]") {
    REQUIRE(swift::tests::NativeMode());
    // A first trip through body arms its page. The static writer changes EDI's
    // immediate, then revisits body and exits42 without any indirect guest edge.
    std::array<std::uint8_t, 25> static_entry{0xb9, 0,  0,    0,    0, 0xeb, 0x79, 0x48, 0xbf,
                                              0,    0,  0,    0,    0, 0,    0,    0,    0xc6,
                                              0x07, 42, 0xe9, 0x67, 0, 0,    0};
    const u64 immediate = body_pc + 1;
    std::memcpy(static_entry.data() + 9, &immediate, sizeof(immediate));
    constexpr std::uint8_t static_body[]{0xbf, 17, 0,    0,    0,    0xff, 0xc1, 0x83,
                                         0xf9, 1,  0x0f, 0x84, 0x77, 0xff, 0xff, 0xff,
                                         0xb8, 60, 0,    0,    0,    0x0f, 0x05};
    // MOV BYTE [RDI],42; JMP RBX. The target's immediate is already write-watched.
    constexpr std::uint8_t writer[]{0xc6, 0x07, 42, 0xff, 0xe3};
    constexpr std::uint8_t body[]{0xb8, 17, 0, 0, 0, 0xc3};  // MOV EAX,17; RET.
    for (const auto scope :
         {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function}) {
        {
            platform::HostRuntime static_host{true};
            execution::RuntimeOptions static_options;
            static_options.scope = scope;
            execution::RuntimeStats static_stats;
            execution::VM static_vm{static_options, static_stats, static_host.Bind()};
            static_vm.PrepareGuest(Guest(static_entry, static_body, true),
                                   "dispatch-static-smc.elf");
            REQUIRE(static_vm.RunPrepared() == 42);
            REQUIRE(static_vm.Close().Complete());
            CHECK(static_stats.thread_dispatch_cache);
            CHECK(static_stats.thread_l1_bytes == 0);
            CHECK(static_stats.interpreted_steps == 0);
            CHECK(static_stats.smc_write_faults == 1);
            CHECK(static_stats.cache_invalidations > 0);
        }
        platform::HostRuntime host{true};
        execution::RuntimeOptions options;
        options.scope = scope;
        options.decode_timing = true;
        execution::RuntimeStats stats;
        execution::VM vm{options, stats, host.Bind()};
        vm.PrepareGuest(Guest(writer, body, true), "dispatch-smc.elf");
        const auto memory = PrepareCallMemory(vm);
        vm.CallGuest(body_pc, memory.stack, stop_pc);
        CHECK(vm.State().gprs[x86::RAX] == 17);
        const auto warmed = stats.compile_attempts;
        vm.CallGuest(body_pc, memory.stack, stop_pc);
        REQUIRE(vm.State().gprs[x86::RAX] == 17);
        REQUIRE(stats.compile_attempts == warmed);
        vm.State().gprs[x86::RDI] = body_pc + 1;
        vm.State().gprs[x86::RBX] = body_pc;
        vm.CallGuest(entry_pc, memory.stack, stop_pc);
        REQUIRE(vm.State().gprs[x86::RAX] == 42);
        CHECK(stats.cache_invalidations > 0);
        const auto rebuilt = stats.compile_attempts;
        CHECK(rebuilt > warmed);
        vm.CallGuest(body_pc, memory.stack, stop_pc);
        CHECK(vm.State().gprs[x86::RAX] == 42);
        CHECK(stats.compile_attempts == rebuilt);
        CHECK(stats.interpreted_steps == 0);
        CHECK(vm.Close().Complete());
        CHECK(stats.smc_write_faults == 1);
        CHECK(stats.thread_dispatch_cache);
        CHECK(stats.thread_l1_bytes == execution::DispatchCache::Bytes);
    }
}
