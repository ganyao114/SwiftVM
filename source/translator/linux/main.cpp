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
            reinterpret_cast<void*>(memory.GetBias()));
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
                      reinterpret_cast<void*>(memory.GetBias())))
            , process(std::make_shared<linux::SyscallProcessState>(
                      &memory, image.brk_start)) {
        instance->SetInterpRangeCheck(InterpRangeCheckThunk, &memory);
    }

    ~X86GuestProcess() {
        JoinAll();
        translator::x86::X86Instance::Destroy(instance);
    }

    int Run(VAddr guest_sp) {
        x86::ThreadContext64 initial{};
        initial.rip.qword = image.entry;
        initial.rsp.qword = guest_sp;
        initial.ef.flags = 0x202;
        const int leader_code = RunThread(initial, 1000, 0, true);
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
            std::thread host_thread([this, child_context, child_tid, clear_child_tid] {
                RunThread(child_context, child_tid, clear_child_tid, false);
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
                  bool leader) {
        auto* core = translator::x86::X86Core::Make(instance);
        {
            std::lock_guard guard(cores_mutex);
            active_cores.push_back(core);
        }
        auto& ctx = core->GetContext();
        std::memcpy(&ctx, &initial, sizeof(ctx));

        linux::SyscallHandler syscalls{
                &memory, image.brk_start, linux::GuestISA::kX86_64, image.path, process, tid};
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
            const auto reason = core->Run();
            if (reason == translator::ExitReason::Syscall) {
                auto result = syscalls.Handle(ctx.rax.qword,
                                              ctx.rdi.qword,
                                              ctx.rsi.qword,
                                              ctx.rdx.qword,
                                              ctx.r10.qword,
                                              ctx.r8.qword,
                                              ctx.r9.qword);
                ctx.rax.qword = static_cast<u64>(result.ret);
                if (result.exited) {
                    exit_code = result.exit_code;
                    if (result.exit_group) {
                        InterruptAll();
                    }
                    break;
                }
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

    // 1. Guest address space (guest addresses virtualized through a
    //    guest->host bias; see guest_memory.h).
    linux::GuestMemory memory;

    // Tell the runtime's host signal handler how to distinguish a wild guest
    // pointer (fault host address not backed by any guest mapping -> guest
    // PageFatal) from a protection fault on a mapped guest page (SMC, host
    // bug -> let the default handler crash with diagnostics).
    runtime::backend::SignalHandler::SetGuestMapProbe(
            [](void* ctx, std::uintptr_t fault_host_addr) -> bool {
                auto* mem = static_cast<linux::GuestMemory*>(ctx);
                const VAddr guest =
                        mem->ToGuest(reinterpret_cast<const void*>(fault_host_addr));
                return mem->RangeIsMapped(guest, 1);
            },
            &memory);

    // 2. Load the guest ELF.
    linux::ElfLoader loader{&memory};
    auto image = loader.Load(guest_path);

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
