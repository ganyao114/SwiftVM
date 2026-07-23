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

#include <cstring>
#include <string>
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

// Runs an ARM64 guest: x8 = syscall nr, x0-x5 = args, result -> x0.
static int RunArm64Guest(const linux::LoadedImage& image,
                         VAddr guest_sp,
                         linux::GuestMemory& memory) {
    auto* instance = translator::arm64::Arm64Instance::Make(
            reinterpret_cast<void*>(memory.GetBias()));
    auto* core = translator::arm64::Arm64Core::Make(instance);

    auto& ctx = core->GetContext();
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.pc = image.entry;
    ctx.sp = guest_sp;

    linux::SyscallHandler syscalls{&memory, image.brk_start, linux::GuestISA::kArm64, image.path};
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
static int RunX86Guest(const linux::LoadedImage& image,
                       VAddr guest_sp,
                       linux::GuestMemory& memory) {
    auto* instance = translator::x86::X86Instance::Make(
            reinterpret_cast<void*>(memory.GetBias()));
    auto* core = translator::x86::X86Core::Make(instance);

    auto& ctx = core->GetContext();
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.rip.qword = image.entry;
    ctx.rsp.qword = guest_sp;
    // Reset value of rflags at Linux process entry: bit 1 is always set,
    // plus IF (bit 9).
    ctx.ef.flags = 0x202;

    linux::SyscallHandler syscalls{&memory, image.brk_start, linux::GuestISA::kX86_64, image.path};
    // arch_prctl writes fs_base/gs_base straight into the frontend context.
    syscalls.SetX86Context(&ctx);
    int exit_code = 0;
    for (;;) {
        auto reason = core->Run();
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
                LOG_INFO("Guest exited with code {}", exit_code);
                break;
            }
        } else if (reason == translator::ExitReason::None) {
            LOG_INFO("Guest returned to host (rip = {:#x})", ctx.rip.qword);
            break;
        } else {
            LOG_ERROR("Guest halted: reason {} rip = {:#x} rax = {:#x}",
                      static_cast<u32>(reason),
                      ctx.rip.qword,
                      ctx.rax.qword);
            exit_code = 1;
            break;
        }
    }

    translator::x86::X86Core::Destroy(core);
    translator::x86::X86Instance::Destroy(instance);
    return exit_code;
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
