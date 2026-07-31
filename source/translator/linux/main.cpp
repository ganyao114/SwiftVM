//
// SwiftVM Linux guest launcher (ARM64 + x86_64).
//
// Usage: svm_translator_linux <guest.elf> [guest args...]
//
// Loads a statically linked Linux ELF into the VM address space, builds the
// initial guest stack, then drives the translator core matching the ELF's
// e_machine. Guest system calls (`svc` / `syscall`) surface as
// ExitReason::Syscall and are emulated here.
//

#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "base/logging.h"
#include "loader.h"
#include "runtime/backend/signal_handler.h"
#include "syscalls.h"
#include "translator/arm64/translator.h"
#include "translator/x86/translator.h"

#ifndef SVM_DEFAULT_GUEST_ELF
#define SVM_DEFAULT_GUEST_ELF "tests/hello_aarch64"
#endif

using namespace swift;

// Interpreter wild-pointer guard thunk: ctx is a GuestMemory*.
static bool InterpRangeCheckThunk(void* ctx, u64 addr, u64 size) {
    return static_cast<linux::GuestMemory*>(ctx)->RangeIsMapped(addr, size);
}

// Runs an ARM64 guest: x8 = syscall nr, x0-x5 = args, result -> x0.
static int RunArm64Guest(const linux::LoadedImage& image,
                         VAddr guest_sp,
                         linux::GuestMemory& memory) {
    auto* instance = translator::arm64::Arm64Instance::Make(
            reinterpret_cast<void*>(memory.GetBias()), memory.Windowed() ? memory.Mask() : 0);
    // Wire the interpreter wild-pointer guard before creating the core
    // (the core's constructor copies it into the runtime State).
    instance->SetInterpRangeCheck(InterpRangeCheckThunk, &memory);
    auto* core = translator::arm64::Arm64Core::Make(instance);

    auto& ctx = core->GetContext();
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.pc = image.entry;
    ctx.sp = guest_sp;

    linux::SyscallHandler syscalls{&memory, image.brk_start, linux::GuestISA::kArm64, image.path};
    // SMC wiring: notify the runtime when the guest remaps/unmaps code pages.
    syscalls.SetSmcInvalidate([instance](VAddr s, VAddr e) { instance->InvalidateCodeRange(s, e); });
    int exit_code = 0;
    for (;;) {
        auto reason = core->Run();
        if (reason == translator::ExitReason::Syscall) {
            auto result = syscalls.Handle(ctx.r[8], ctx.r[0], ctx.r[1], ctx.r[2], ctx.r[3], ctx.r[4], ctx.r[5]);
            ctx.r[0] = static_cast<u64>(result.ret);
            if (result.exited) {
                exit_code = result.exit_code;
                LOG_INFO("Guest exited with code {}", exit_code);
                break;
            }
        } else if (reason == translator::ExitReason::None) {
            LOG_INFO("Guest returned to host (pc = {:#x})", ctx.pc);
            break;
        } else {
            LOG_ERROR("Guest halted: reason {} pc = {:#x} x8 = {:#x}",
                      static_cast<u32>(reason),
                      ctx.pc,
                      ctx.r[8]);
            exit_code = 1;
            break;
        }
    }

    translator::arm64::Arm64Core::Destroy(core);
    translator::arm64::Arm64Instance::Destroy(instance);
    return exit_code;
}

// Runs an x86_64 guest: rax = syscall nr, rdi/rsi/rdx/r10/r8/r9 = args,
// result -> rax.
namespace {

constexpr u64 kCloneSetTls = 0x00080000;
constexpr u64 kCloneParentSetTid = 0x00100000;
constexpr u64 kCloneChildClearTid = 0x00200000;
constexpr u64 kCloneChildSetTid = 0x01000000;

// Minimal Linux thread group for the x86_64 launcher: one shared translator
// Instance/AddressSpace and one Core/Runtime/ThreadContext per host thread.
class X86GuestProcess {
public:
    X86GuestProcess(const linux::LoadedImage& image, linux::GuestMemory& memory)
            : image(image)
            , memory(memory)
            , instance(translator::x86::X86Instance::Make(
                      reinterpret_cast<void*>(memory.GetBias()),
                      memory.Windowed() ? memory.Mask() : 0))
            , process(std::make_shared<linux::SyscallProcessState>(
                      &memory, image.brk_start)) {
        instance->SetInterpRangeCheck(InterpRangeCheckThunk, &memory);
        process->SetAlarmInterrupt([this] { InterruptAll(); });
    }

    ~X86GuestProcess() {
        process->ShutdownAlarm();
        JoinAll();
        translator::x86::X86Instance::Destroy(instance);
    }

    int Run(VAddr guest_sp) {
        x86::ThreadContext64 initial{};
        initial.rip.qword = image.entry;
        initial.rsp.qword = guest_sp;
        initial.ef.flags = 0x202;
        const int leader_code = RunThread(initial, 1000, 0, 0, true);
        JoinAll();
        const int exit_code = process->IsExiting() ? process->GetExitCode() : leader_code;
        LOG_INFO("Guest process exited with code {}", exit_code);
        return exit_code;
    }

private:
    s64 Spawn(const linux::SyscallHandler::CloneRequest& request,
              const x86::ThreadContext64& parent_context) {
        std::call_once(multithreaded, [this] { instance->PrepareForMultithreading(); });

        const s64 child_tid = process->AllocateTid();
        auto child_context = parent_context;
        child_context.rax.qword = 0;
        child_context.rsp.qword = request.child_stack;
        if (request.flags & kCloneSetTls) {
            child_context.fs_base = request.tls;
        }
        const VAddr clear_child_tid =
                (request.flags & kCloneChildClearTid) ? request.child_tid : 0;
        const u64 signal_mask = request.signal_mask;

        // Both TID stores are visible before the child is allowed to run.
        if ((request.flags & kCloneParentSetTid) &&
            !process->StoreGuestU32(request.parent_tid, static_cast<u32>(child_tid))) {
            return -linux::EFAULT_;
        }
        if ((request.flags & kCloneChildSetTid) &&
            !process->StoreGuestU32(request.child_tid, static_cast<u32>(child_tid))) {
            return -linux::EFAULT_;
        }

        active_threads.fetch_add(1, std::memory_order_acq_rel);
        try {
            std::thread host_thread(
                    [this, child_context, child_tid, clear_child_tid, signal_mask] {
                RunThread(child_context, child_tid, clear_child_tid, signal_mask, false);
            });
            {
                std::lock_guard guard(threads_mutex);
                threads.push_back(std::move(host_thread));
            }
            threads_changed.notify_all();
        } catch (...) {
            active_threads.fetch_sub(1, std::memory_order_acq_rel);
            threads_changed.notify_all();
            return -linux::EAGAIN_;
        }
        return child_tid;
    }

    int RunThread(x86::ThreadContext64 initial,
                  s64 tid,
                  VAddr initial_clear_child_tid,
                  u64 initial_signal_mask,
                  bool leader) {
        auto* core = translator::x86::X86Core::Make(instance);
        {
            std::lock_guard guard(cores_mutex);
            active_cores.push_back(core);
        }
        auto& ctx = core->GetContext();
        std::memcpy(&ctx, &initial, sizeof(ctx));

        linux::SyscallHandler syscalls{
                &memory,
                image.brk_start,
                linux::GuestISA::kX86_64,
                image.path,
                process,
                tid,
                initial_signal_mask};
        syscalls.SetClearChildTid(initial_clear_child_tid);
        syscalls.SetSmcInvalidate(
                [this](VAddr start, VAddr end) { instance->InvalidateCodeRange(start, end); });
        syscalls.SetX86Context(&ctx);
        syscalls.SetCloneCallback(
                [this, &ctx](const auto& request) { return Spawn(request, ctx); });

        int exit_code = 0;
        for (;;) {
            if (process->IsExiting()) {
                exit_code = process->GetExitCode();
                break;
            }
            const auto pending = syscalls.DeliverPendingSignal();
            if (pending.terminated) {
                exit_code = pending.exit_code;
                process->RequestExitGroup(exit_code);
                InterruptAll();
                break;
            }
            if (pending.delivered) {
                // alarm expiry stops the runtime at a translated block
                // boundary. The guest frame is now installed, so re-enable
                // execution with the handler RIP as the next location.
                core->ClearInterrupt();
            }
            const auto reason = core->Run();
            if (reason == translator::ExitReason::Syscall) {
                auto result = syscalls.Handle(ctx.rax.qword,
                                              ctx.rdi.qword,
                                              ctx.rsi.qword,
                                              ctx.rdx.qword,
                                              ctx.r10.qword,
                                              ctx.r8.qword,
                                              ctx.r9.qword);
                if (!result.context_restored) {
                    ctx.rax.qword = static_cast<u64>(result.ret);
                }
                if (result.exited) {
                    exit_code = result.exit_code;
                    if (result.exit_group) {
                        InterruptAll();
                    }
                    break;
                }
            } else if (reason == translator::ExitReason::Signal) {
                // The timer thread and exit_group use the runtime's
                // block-boundary interrupt. Clear this core's latch; the loop
                // above either injects a pending guest signal or observes the
                // process-wide exit request.
                core->ClearInterrupt();
            } else if (reason == translator::ExitReason::None) {
                break;
            } else {
                LOG_ERROR("Guest thread {} halted: reason {} rip = {:#x} rax = {:#x}",
                          tid,
                          static_cast<u32>(reason),
                          ctx.rip.qword,
                          ctx.rax.qword);
                exit_code = 1;
                if (leader) {
                    process->RequestExitGroup(exit_code);
                }
                break;
            }
        }

        const VAddr clear_child_tid = syscalls.GetClearChildTid();
        if (clear_child_tid) {
            if (process->StoreGuestU32(clear_child_tid, 0)) {
                process->WakeFutex(clear_child_tid, UINT32_MAX);
            }
        }
        {
            std::lock_guard guard(cores_mutex);
            std::erase(active_cores, core);
        }
        translator::x86::X86Core::Destroy(core);
        active_threads.fetch_sub(1, std::memory_order_acq_rel);
        threads_changed.notify_all();
        return exit_code;
    }

    void JoinAll() {
        for (;;) {
            std::thread thread;
            {
                std::unique_lock lock(threads_mutex);
                if (threads.empty()) {
                    if (active_threads.load(std::memory_order_acquire) == 0) {
                        break;
                    }
                    threads_changed.wait(lock, [this] {
                        return !threads.empty() ||
                               active_threads.load(std::memory_order_acquire) == 0;
                    });
                    continue;
                }
                thread = std::move(threads.back());
                threads.pop_back();
            }
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void InterruptAll() {
        std::lock_guard guard(cores_mutex);
        for (auto* core : active_cores) {
            core->SignalInterrupt();
        }
    }

    const linux::LoadedImage& image;
    linux::GuestMemory& memory;
    translator::x86::X86Instance* instance;
    std::shared_ptr<linux::SyscallProcessState> process;
    std::once_flag multithreaded;
    std::mutex threads_mutex;
    std::condition_variable threads_changed;
    std::vector<std::thread> threads;
    std::atomic<u32> active_threads{1};
    std::mutex cores_mutex;
    std::vector<translator::x86::X86Core*> active_cores;
};

}  // namespace

static int RunX86Guest(const linux::LoadedImage& image,
                       VAddr guest_sp,
                       linux::GuestMemory& memory) {
    X86GuestProcess process{image, memory};
    return process.Run(guest_sp);
}

int main(int argc, char** argv) {
    std::string guest_path;
    std::vector<std::string> guest_args;
    if (argc >= 2) {
        guest_path = argv[1];
        for (int i = 1; i < argc; ++i) {
            guest_args.emplace_back(argv[i]);
        }
    } else {
        guest_path = SVM_DEFAULT_GUEST_ELF;
        guest_args = {guest_path};
        LOG_INFO("No guest ELF given, falling back to {}", guest_path);
    }
    std::vector<std::string> guest_envs = {"PATH=/usr/bin:/bin", "HOME=/root"};
    // Keep the historical minimal guest environment, but allow the dynamic
    // linker regression suite to request eager binding explicitly.
    if (const char* bind_now = std::getenv("LD_BIND_NOW")) {
        guest_envs.emplace_back(std::string("LD_BIND_NOW=") + bind_now);
    }
    // Keep OpenSSL's guest-side capability override available to the launcher.
    // It is both a standard compatibility knob and useful for selecting one
    // advertised x86 crypto subset while leaving the translator's own SVM_*
    // controls host-only.
    if (const char* ia32cap = std::getenv("OPENSSL_ia32cap")) {
        guest_envs.emplace_back(std::string("OPENSSL_ia32cap=") + ia32cap);
    }

    // 1. Guest address space. The default is a bounded, biased reservation:
    //    every guest address is truncated to the window before the bias is
    //    added, so no wild guest pointer can name unrelated host memory.
    //    SVM_GUEST_BITS overrides the window size.
    //
    //    Linux hosts may explicitly request SVM_MEM_IDENTITY=ON. That skips
    //    the window and maps guest addresses directly at the same host
    //    addresses with MAP_FIXED_NOREPLACE. It releases the JIT's pt (x24)
    //    and mem_scratch (x10) reservations, but a wild guest pointer can then
    //    reach translator/DSO/stack mappings. Identity is therefore opt-in
    //    and never silently falls back to bias mode on a fixed-map collision.
    //    macOS does not inspect this switch and always follows the historical
    //    bounded-bias path (its 4GB pagezero prevents low ET_EXEC identity).
    //
    //    SVM_GUEST_BITS=0 restores the old unbounded bias mode, in which the
    //    guest can read and write arbitrary host memory. That is the defect
    //    the window removed, not a supported mode, so it is gated at COMPILE
    //    time (-DSWIFT_ALLOW_UNBOUNDED_GUEST=1, cmake option
    //    SWIFT_ALLOW_UNBOUNDED_GUEST) and is absent from ordinary builds.
    //    run_isolation_tests.sh uses a dedicated build to demonstrate it.
    linux::GuestMemory memory;
    {
        bool identity_mode = false;
#if defined(__linux__)
        if (const char* env = std::getenv("SVM_MEM_IDENTITY")) {
            identity_mode = std::strcmp(env, "0") != 0 &&
                            std::strcmp(env, "OFF") != 0 &&
                            std::strcmp(env, "off") != 0;
        }
        if (identity_mode) {
            memory.EnableIdentityMode();
            LOG_WARNING(
                    "SVM_MEM_IDENTITY=ON: Linux guest addresses map directly onto the "
                    "host address space. Guest wild pointers can access translator "
                    "mappings; fixed-address conflicts are fatal.");
        }
#endif
        u32 window_bits = linux::GuestMemory::kDefaultWindowBits;
        if (!identity_mode) {
            if (const char* env = std::getenv("SVM_GUEST_BITS")) {
                const long v = std::strtol(env, nullptr, 0);
                if (v == 0) {
#ifdef SWIFT_ALLOW_UNBOUNDED_GUEST
                    window_bits = 0;
#else
                    LOG_ERROR(
                            "SVM_GUEST_BITS=0 (unbounded guest address space) is not compiled "
                            "into this build. It lets the guest read and write host memory and "
                            "exists only so run_isolation_tests.sh can demonstrate the defect; "
                            "rebuild with -DSWIFT_ALLOW_UNBOUNDED_GUEST=ON to get it.");
                    return 2;
#endif
                } else if (v >= 20 && v <= 47) {
                    window_bits = static_cast<u32>(v);
                } else {
                    LOG_ERROR("SVM_GUEST_BITS={} out of range (0 or 20..47); using {}",
                              env,
                              window_bits);
                }
            }
            if (!memory.ReserveWindow(window_bits)) {
                PANIC("Failed to reserve the {}-bit guest address window", window_bits);
            }
            if (window_bits == 0) {
                LOG_WARNING(
                        "SVM_GUEST_BITS=0: guest address space is UNBOUNDED — the guest can "
                        "read and write host memory. Diagnostics only.");
            }
        }
    }

    // Tell the runtime's host signal handler how to distinguish a wild guest
    // pointer (fault host address not backed by any guest mapping -> guest
    // PageFatal) from a protection fault on a mapped guest page (SMC, host
    // bug -> let the default handler crash with diagnostics).
    runtime::backend::SignalHandler::SetGuestMapProbe(
            [](void* ctx, std::uintptr_t fault_host_addr) -> bool {
                auto* mem = static_cast<linux::GuestMemory*>(ctx);
                const VAddr guest =
                        mem->ToGuest(reinterpret_cast<const void*>(fault_host_addr));
                // Windowed: a host address outside the reservation is not a
                // guest address at all. RangeIsMapped truncates, so without
                // this check a host fault below the window would alias onto a
                // mapped guest page and be misreported as guest memory.
                if (mem->Windowed() && guest > mem->Mask()) {
                    return false;
                }
                return mem->RangeIsMapped(guest, 1);
            },
            &memory);
    // Range form, used by the helpers that must validate before they
    // dereference (x87/fxsave, rep-string walks). Same truncation rule.
    runtime::backend::SignalHandler::SetGuestRangeProbe(
            [](void* ctx, std::uintptr_t host_addr, u64 length) -> u64 {
                auto* mem = static_cast<linux::GuestMemory*>(ctx);
                const VAddr guest = mem->ToGuest(reinterpret_cast<const void*>(host_addr));
                if (mem->Windowed() && guest > mem->Mask()) {
                    return 0;
                }
                return mem->MappedBytesFrom(guest, length);
            },
            &memory);

    // 2. Load the guest ELF.
    linux::ElfLoader loader{&memory};
    linux::LoadedImage image;
    try {
        image = loader.Load(guest_path);
    } catch (const std::exception& e) {
        LOG_ERROR("Cannot start guest: {}", e.what());
        return 2;
    }
    if (image.isa == linux::GuestISA::kX86_64 && image.interpreter_base != 0) {
        // glibc's GNU-property x86-64-baseline test includes the architectural
        // MMX CPUID bit. Keep this internal compatibility marker scoped to
        // PT_INTERP launches so static-guest CPUID and unit fingerprints stay
        // byte-for-byte unchanged.
        ::setenv("SVM_X86_64_ABI_BASELINE", "1", 1);
    }

    // 3. Guest main stack (argc/argv/envp/auxv).
    const VAddr guest_sp = linux::SetupInitialStack(memory, image, guest_args, guest_envs);

    // 4. Translator core for the guest ISA + syscall loop.
    switch (image.isa) {
        case linux::GuestISA::kArm64:
            return RunArm64Guest(image, guest_sp, memory);
        case linux::GuestISA::kX86_64:
            return RunX86Guest(image, guest_sp, memory);
    }
    return 1;
}
