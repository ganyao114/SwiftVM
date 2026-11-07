#include "riscv64_runtime.h"
#include "runtime/arch/x86/state.h"
#include "runtime/backend/riscv64/abi_layout.inc"
#include "runtime/backend/riscv64/patch.h"
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace swift::runtime::platform {
namespace {
using execution::FaultContext;
using execution::NativeRunContext;

bool RecoverNative(FaultContext& context,
                   int signal,
                   int signal_code,
                   std::uintptr_t address,
                   void* raw) noexcept {
#if defined(__riscv) && __riscv_xlen == 64
    const auto& machine = *static_cast<const ucontext_t*>(raw);
    // Linux's first general-context word is the faulting PC; x1..x31
    // retain their architectural indices, including the packed flags home.
    const auto host_pc = static_cast<std::uintptr_t>(machine.uc_mcontext.__gregs[0]);
    const auto* code = context.active.load();
    if (!code) return false;
    if (context.lookup_native) code = context.lookup_native(context.lookup_opaque, host_pc);
    if (!code) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(code->entry);
    if (host_pc < base || host_pc - base >= code->bytes) return false;
    const auto offset = host_pc - base;
    for (const auto& site : code->faults) {
        if (offset < site.first_offset || offset >= site.last_offset) continue;
        if (signal == SIGSEGV && signal_code == SEGV_ACCERR && context.recover_code_write) {
            const auto begin = std::chrono::steady_clock::now();
            const bool recovered = context.recover_code_write(context.code_write_opaque, address);
            context.service_ns +=
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                       std::chrono::steady_clock::now() - begin)
                                                       .count());
            if (recovered) {
                context.smc_write_faults.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        if (context.flags_offset != UINT32_MAX) {
            const auto flags = machine.uc_mcontext.__gregs[SVM_RV_FLAGS_REG];
            std::memcpy(reinterpret_cast<std::uint8_t*>(context.state) + context.flags_offset,
                        &flags,
                        sizeof flags);
        }
        execution::RaiseGuestFault(context, site, signal, signal_code, host_pc, address);
    }
#else
    (void)context;
    (void)signal;
    (void)signal_code;
    (void)address;
    (void)raw;
#endif
    return false;
}

void FlushCode(const std::uint8_t* address, std::size_t bytes) noexcept {
#if defined(__linux__) && defined(__riscv) && __riscv_xlen == 64
    // Flags=0 requests the Linux process-wide synchronization contract,
    // including migration. A failed synchronization cannot publish callable code.
    if (syscall(SYS_riscv_flush_icache, address, address + bytes, 0) != 0) std::abort();
#else
    __builtin___clear_cache(reinterpret_cast<char*>(const_cast<std::uint8_t*>(address)),
                            reinterpret_cast<char*>(const_cast<std::uint8_t*>(address + bytes)));
#endif
}

void SynchronizeCode(std::uint8_t* writable,
                     const std::uint8_t* executable,
                     std::size_t bytes) noexcept {
    FlushCode(writable, bytes);
    if (executable != writable) FlushCode(executable, bytes);
}
}  // namespace

Riscv64Runtime::Riscv64Runtime(bool diagnostic) noexcept {
    options.flags_offset = offsetof(x86::GuestState, packed_flags);
    options.pc_offset = offsetof(x86::GuestState, pc);
    options.mxcsr_offset = offsetof(x86::GuestState, mxcsr);
    options.native_fp_environment = true;
    options.dispatcher = reinterpret_cast<std::uintptr_t>(
            diagnostic ? &execution::SvmNativeDispatchCounted : &execution::SvmNativeDispatch);
    options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
}

execution::HostBackendOps Riscv64Runtime::Bind() noexcept {
    execution::HostBackendOps ops;
    ops.context = this;
    ops.target = backend::riscv64::KnownTargetInfo();
    ops.pinned_uniform_slots = 0;
    ops.pinned_vectors = false;
    ops.compile = [](void* raw,
                     const ir::Program& program,
                     execution::HostCompileRequest request) -> backend::CompileResult& {
        auto& self = *static_cast<Riscv64Runtime*>(raw);
        auto policy = self.options;
        policy.entry_pc = request.entry_pc;
        policy.fault_locations = request.fault_locations;
        backend::riscv64::Compile(program, policy, self.output, self.allocation);
        return self.output;
    };
    ops.trim = [](void* raw) noexcept {
        auto& self = *static_cast<Riscv64Runtime*>(raw);
        if (self.output.words.capacity() > 1024u * 1024)
            std::vector<std::uint32_t>{}.swap(self.output.words);
        if (self.output.entries.capacity() > 4096)
            std::vector<backend::EntryPoint>{}.swap(self.output.entries);
        if (self.allocation.homes.capacity() > 8u * 1024 * 1024 / sizeof(backend::riscv64::Home))
            std::vector<backend::riscv64::Home>{}.swap(self.allocation.homes);
        if (self.allocation.block_order.capacity() > 4096)
            std::vector<std::uint32_t>{}.swap(self.allocation.block_order);
    };
    ops.release = [](void* raw) noexcept {
        auto& self = *static_cast<Riscv64Runtime*>(raw);
        self.output = {};
        self.allocation = {};
    };
    ops.synchronize_code = SynchronizeCode;
    ops.initialize_link = [](void*,
                             std::span<std::uint8_t> write,
                             std::uintptr_t execute,
                             bool dual,
                             const backend::LinkSite& site,
                             std::uintptr_t ready) noexcept {
        return backend::riscv64::InitializeNativeLink(write, execute, site, ready, dual);
    };
    ops.patch_link = backend::riscv64::PatchNativeLink;
    ops.unlink = backend::riscv64::UnlinkNativeLink;
    ops.recover_native = RecoverNative;
    ops.update_fp = [](void*, NativeRunContext& native, std::uint32_t mxcsr) noexcept {
        constexpr unsigned rounding[]{0, 2, 3, 1};  // RNE, RDN, RUP, RTZ.
        native.guest_fpcr = rounding[(mxcsr >> 13) & 3] << 5;
    };
    ops.restore_host_fp = [](const NativeRunContext& native) noexcept {
#if defined(__riscv) && __riscv_xlen == 64
        asm volatile("fscsr %0" ::"r"(native.host_fpcr) : "memory");
#else
        (void)native;
#endif
    };
#if !defined(__riscv) || __riscv_xlen != 64
    ops.compile = nullptr;
    ops.target.mode = ir::TargetMode::Unknown;
#endif
    return ops;
}

#define SVM_CHECK_CONTEXT(field, macro)                                                            \
    static_assert(offsetof(execution::NativeRunContext, field) == macro)
SVM_CHECK_CONTEXT(state, SVM_RV_CTX_STATE);
SVM_CHECK_CONTEXT(opaque, SVM_RV_CTX_OPAQUE);
SVM_CHECK_CONTEXT(resolve, SVM_RV_CTX_RESOLVE);
SVM_CHECK_CONTEXT(slots, SVM_RV_CTX_SLOTS);
SVM_CHECK_CONTEXT(mask, SVM_RV_CTX_MASK);
SVM_CHECK_CONTEXT(active, SVM_RV_CTX_ACTIVE);
SVM_CHECK_CONTEXT(anchor, SVM_RV_CTX_ANCHOR);
SVM_CHECK_CONTEXT(dispatcher_entries, SVM_RV_CTX_DISPATCHES);
SVM_CHECK_CONTEXT(cache_hits, SVM_RV_CTX_HITS);
SVM_CHECK_CONTEXT(flags_offset, SVM_RV_CTX_FLAGS_OFFSET);
SVM_CHECK_CONTEXT(pc_offset, SVM_RV_CTX_PC_OFFSET);
SVM_CHECK_CONTEXT(host_fpcr, SVM_RV_CTX_HOST_FCSR);
SVM_CHECK_CONTEXT(guest_fpcr, SVM_RV_CTX_GUEST_FCSR);
SVM_CHECK_CONTEXT(fp_environment, SVM_RV_CTX_FP_ENVIRONMENT);
SVM_CHECK_CONTEXT(probe_steps, SVM_RV_CTX_PROBES);
SVM_CHECK_CONTEXT(prepare_atomic, SVM_RV_CTX_PREPARE_ATOMIC);
#undef SVM_CHECK_CONTEXT
}  // namespace swift::runtime::platform
