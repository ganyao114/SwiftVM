#include "fault.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <asm/sigcontext.h>
#endif

#include "guest_state.h"
#include "native_code.h"
#include "runtime/backend/arm64/abi.h"

namespace swift::runtime::execution {
namespace {
constexpr std::array signals{SIGSEGV, SIGBUS, SIGTRAP};
struct ServiceTimer {
    std::uint64_t& total;
    std::chrono::steady_clock::time_point begin{std::chrono::steady_clock::now()};
    ~ServiceTimer() {
        total += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
    }
};
std::atomic<FaultScope*> registered_scope{}; // One service, not a process directory.
static_assert(std::atomic<FaultScope*>::is_always_lock_free &&
              std::atomic<const NativeCode*>::is_always_lock_free &&
              std::atomic<std::uintptr_t>::is_always_lock_free &&
              std::atomic<std::uint64_t>::is_always_lock_free);
sigset_t ManagedSignals() noexcept {
    sigset_t set;
    sigemptyset(&set);
    for (const auto signal : signals) sigaddset(&set, signal);
    return set;
}
#if defined(__aarch64__)
const fpsimd_context* FindFPSIMD(const ucontext_t& machine) noexcept {
    const auto* records = machine.uc_mcontext.__reserved;
    constexpr auto bytes = sizeof(machine.uc_mcontext.__reserved);
    for (std::size_t at = 0; at + sizeof(_aarch64_ctx) <= bytes;) {
        const auto* head = reinterpret_cast<const _aarch64_ctx*>(records + at);
        if (head->size < sizeof(_aarch64_ctx) || (head->size & 15) || head->size > bytes - at) break;
        if (head->magic == FPSIMD_MAGIC && head->size >= sizeof(fpsimd_context))
            return reinterpret_cast<const fpsimd_context*>(head);
        at += head->size;
    }
    return nullptr;
}

bool PatchOrderedFallback(FaultContext& context, const NativeCode& code,
                          std::uint32_t fast_word, std::uint32_t fallback_word) noexcept {
    auto* execute = reinterpret_cast<std::uint8_t*>(code.entry) + std::size_t{fast_word} * 4;
    const auto fast = __atomic_load_n(reinterpret_cast<const std::uint32_t*>(execute), __ATOMIC_ACQUIRE);
    const auto opcode = fast & UINT32_C(0x3ffffc00);
    const auto imm9 = fast & 0x3fe00c00u;
    const bool ordered = opcode == 0x08dffc00u || opcode == 0x38bfc000u || opcode == 0x089ffc00u ||
            imm9 == 0x19400000u || imm9 == 0x19000000u || imm9 == 0x19c00000u || imm9 == 0x19800000u;
    // Inspect only this actual generated instruction. A later fault from an
    // already-patched plain cold access is fatal, not another alignment retry.
    const auto distance = static_cast<std::int64_t>(fallback_word) - fast_word;
    if (!ordered || distance < -(std::int64_t{1} << 25) || distance >= (std::int64_t{1} << 25))
        return false;
    const auto address = reinterpret_cast<std::uintptr_t>(execute);
    const auto page = address - address % context.page_size;
    if (!code.writable && syscall(SYS_mprotect, reinterpret_cast<void*>(page), context.page_size, PROT_READ | PROT_WRITE))
        return false;
    auto* write = code.writable ? code.writable + std::size_t{fast_word} * 4 : execute;
    const auto branch = 0x14000000u | (static_cast<std::uint32_t>(distance) & 0x03ffffffu);
    // The complete half-barrier stub was published with this same code owner.
    // Only the actual fast instruction changes, in one aligned atomic write.
    __atomic_store_n(reinterpret_cast<std::uint32_t*>(write), branch, __ATOMIC_RELEASE);
    __builtin___clear_cache(reinterpret_cast<char*>(write), reinterpret_cast<char*>(write + 4));
    if (!code.writable && syscall(SYS_mprotect, reinterpret_cast<void*>(page), context.page_size, PROT_READ | PROT_EXEC))
        return false;
    if (execute != write) __builtin___clear_cache(reinterpret_cast<char*>(execute), reinterpret_cast<char*>(execute + 4));
    return true;
}
#endif
} // namespace

FaultScope::FaultScope(FaultContext& context, GuestState& state) : context(context) {
    ServiceTimer elapsed{context.service_ns};
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) throw std::runtime_error("cannot get code page size for fault recovery");
    context.page_size = static_cast<std::uintptr_t>(page_size);
    const auto managed = ManagedSignals();
    sigset_t saved;
    if (sigprocmask(SIG_BLOCK, &managed, &saved)) throw std::runtime_error("cannot block signals for fault service");
    FaultScope* empty{};
    if (!registered_scope.compare_exchange_strong(empty, this)) {
        sigprocmask(SIG_SETMASK, &saved, nullptr);
        throw std::runtime_error("only one guest fault service may be registered");
    }
    context.state = &state;
    context.thread_id = static_cast<pid_t>(syscall(SYS_gettid));
    struct sigaction action{};
    action.sa_sigaction = Handle;
    action.sa_flags = SA_SIGINFO;
    action.sa_mask = managed;
    std::size_t installed{};
    for (; installed < signals.size(); ++installed)
        if (sigaction(signals[installed], &action, &previous[installed])) break;
    if (installed != signals.size()) {
        while (installed) { --installed; sigaction(signals[installed], &previous[installed], nullptr); }
        registered_scope.store(nullptr);
        sigprocmask(SIG_SETMASK, &saved, nullptr);
        throw std::runtime_error("cannot install native guest fault handlers");
    }
    context.registered = true;
    if (sigprocmask(SIG_SETMASK, &saved, nullptr)) {
        context.registered = false;
        registered_scope.store(nullptr);
        for (std::size_t i = 0; i < signals.size(); ++i) sigaction(signals[i], &previous[i], nullptr);
        throw std::runtime_error("cannot restore signal mask after fault registration");
    }
}

FaultScope::~FaultScope() {
    ServiceTimer elapsed{context.service_ns};
    const auto managed = ManagedSignals();
    sigset_t saved;
    const bool blocked = sigprocmask(SIG_BLOCK, &managed, &saved) == 0;
    if (!blocked) context.restore_error = errno;
    context.active.store(nullptr);
    context.registered = false;
    for (std::size_t i = 0; i < signals.size(); ++i)
        if (sigaction(signals[i], &previous[i], nullptr)) context.restore_error = errno;
    registered_scope.store(nullptr);
    if (blocked && sigprocmask(SIG_SETMASK, &saved, nullptr)) context.restore_error = errno;
}

void FaultScope::Chain(int signal, siginfo_t* info, void* raw) const noexcept {
    std::size_t index{};
    while (signals[index] != signal) ++index;
    const auto& old = previous[index];
    if (old.sa_handler == SIG_IGN) return;
    if (old.sa_handler == SIG_DFL) {
        sigaction(signal, &old, nullptr);
        syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), signal);
    } else if (old.sa_flags & SA_SIGINFO) old.sa_sigaction(signal, info, raw);
    else old.sa_handler(signal);
}

void FaultScope::Handle(int signal, siginfo_t* info, void* raw) noexcept {
    const auto saved_errno = errno;
    auto* scope = registered_scope.load();
    if (!scope) {
        // Only reachable if restoring an old action failed. Never swallow
        // a host fault after the registration object's lifetime has ended.
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(signal, &action, nullptr);
        syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), signal);
        return;
    }
    auto& context = scope->context;
#if defined(__aarch64__)
    auto* machine = static_cast<ucontext_t*>(raw);
    const auto host_pc = static_cast<std::uintptr_t>(machine->uc_mcontext.pc);
    const auto* code = context.active.load();
    if (code && info->si_code > 0 && context.thread_id == static_cast<pid_t>(syscall(SYS_gettid))) {
        if (context.lookup_native) code = context.lookup_native(context.lookup_opaque, host_pc);
    }
    else code = nullptr;
    if (code) {
        const auto base = reinterpret_cast<std::uintptr_t>(code->entry);
        if (host_pc >= base && host_pc - base < code->bytes && !((host_pc - base) & 3)) {
            const auto word = (host_pc - base) / 4;
            for (const auto& site : code->faults) {
                const auto fallback = std::uint64_t{site.unaligned_fallback_word};
                const bool has_fallback = fallback != UINT32_MAX && fallback + 3 <= code->bytes / 4;
                const bool cold = has_fallback && word >= fallback && word - fallback < 3;
                if (!cold && (word < site.first_word || word >= site.last_word)) continue;
                if (signal == SIGSEGV && info->si_code == SEGV_ACCERR && context.recover_code_write) {
                    ServiceTimer service{context.service_ns};
                    if (context.recover_code_write(context.code_write_opaque, reinterpret_cast<std::uintptr_t>(info->si_addr))) {
                        context.smc_write_faults.fetch_add(1, std::memory_order_relaxed);
                        errno = saved_errno;
                        return; // Retry the actual store; retirement waits for the cold dispatcher.
                    }
                }
                if (signal == SIGBUS && info->si_code == BUS_ADRALN &&
                    word == site.unaligned_word && has_fallback) {
                    if (PatchOrderedFallback(context, *code, site.unaligned_word, site.unaligned_fallback_word)) {
                        context.unaligned_patches.fetch_add(1, std::memory_order_relaxed);
                        context.unaligned_recoveries.fetch_add(1, std::memory_order_relaxed);
                        machine->uc_mcontext.pc = base + static_cast<std::uintptr_t>(fallback) * 4;
                        errno = saved_errno;
                        return;
                    }
                }
                if (context.pinned_vector_offset != UINT32_MAX) {
                    const auto* vectors = FindFPSIMD(*machine);
                    if (!vectors) break; // Do not publish incomplete guest state; chain the original signal.
                    auto* slots = reinterpret_cast<std::uint8_t*>(context.state) + context.pinned_vector_offset;
                    std::memcpy(slots, &vectors->vregs[16], 16 * sizeof(vectors->vregs[0]));
                }
                if (context.pinned_uniform_offset != UINT32_MAX) {
                    auto* slots = reinterpret_cast<std::uint8_t*>(context.state) + context.pinned_uniform_offset;
                    for (unsigned i = 0; i < context.pinned_uniform_slots; ++i) {
                        // Low eight are callee-saved; pin16 maps high slots
                        // directly to x8..x15. Preserve the legacy pin12 map.
                        const auto reg = backend::arm64::abi::PinnedUniformRegister(i, context.pinned_uniform_slots);
                        std::memcpy(slots + i * 8, &machine->uc_mcontext.regs[reg], 8);
                    }
                }
                if (context.flags_offset != UINT32_MAX) {
                    auto packed = machine->uc_mcontext.regs[1];
                    if (context.native_nzcv) {
                        std::memcpy(&packed, reinterpret_cast<const std::uint8_t*>(context.state) + context.flags_offset, 8);
                        packed &= ~backend::arm64::abi::PackedDynamicFlags;
                        if (site.test_bits && (machine->uc_mcontext.regs[29] & backend::arm64::abi::DeferredTestTag)) {
                            // The compiler records the actual still-valid
                            // entry snapshot, which can differ from the pin
                            // already overwritten by a successful load.
                            const auto raw = machine->uc_mcontext.regs[site.test_register];
                            const auto value = site.test_bits == 32 ? std::uint64_t{static_cast<std::uint32_t>(raw)} : raw;
                            packed |= (value & UINT64_C(0xff)) | (std::uint64_t{value == 0} << 30) |
                                      (((value >> (site.test_bits - 1)) & 1) << 31);
                        } else {
                            const auto pf = machine->uc_mcontext.regs[site.parity_register ? site.parity_register : 1];
                            const auto carry_flip = site.carry_direct ? UINT64_C(0) : UINT64_C(0x20000000);
                            packed |= (pf & UINT64_C(0xff)) |
                                      ((machine->uc_mcontext.pstate ^ carry_flip) & UINT64_C(0xf0000000)) |
                                      ((((machine->uc_mcontext.regs[29] ^ pf) >> 4) & 1) << 26);
                        }
                    }
                    std::memcpy(reinterpret_cast<std::uint8_t*>(context.state) + context.flags_offset,
                                &packed, 8);
                }
                context.state->pc = site.guest_pc;
                context.signal_number = signal;
                context.signal_code = info->si_code;
                context.host_pc = host_pc;
                context.address = reinterpret_cast<std::uintptr_t>(info->si_addr);
                context.active.store(nullptr);
                siglongjmp(context.jump, 1);
            }
        }
    }
#else
    (void)context;
#endif
    errno = saved_errno;
    scope->Chain(signal, info, raw);
}
} // namespace swift::runtime::execution
