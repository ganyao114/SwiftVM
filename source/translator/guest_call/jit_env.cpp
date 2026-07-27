#include "jit_env.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime/backend/signal_handler.h"
#include "translator/x86/translator.h"

namespace swift::guest_call {

using swift::translator::ExitReason;
using swift::translator::x86::X86Core;
using swift::translator::x86::X86Instance;

namespace {

// The one live environment, so the process-global probes the runtime installs
// (SignalHandler map/range oracles, and translator.cpp's file-static
// MemoryImpl bias) always describe the space that is actually running.  Only
// one JitGuestEnv may exist at a time; the constructor asserts it.
JitGuestEnv* g_active = nullptr;
GuestSpace* g_active_space = nullptr;

constexpr std::uint64_t kGuestPageSize = 0x1000;

// x86-64 Linux syscall numbers used below.
enum : std::uint64_t {
    SYS_read = 0,
    SYS_write = 1,
    SYS_close = 3,
    SYS_fstat = 5,
    SYS_mmap = 9,
    SYS_mprotect = 10,
    SYS_munmap = 11,
    SYS_brk = 12,
    SYS_rt_sigaction = 13,
    SYS_rt_sigprocmask = 14,
    SYS_ioctl = 16,
    SYS_writev = 20,
    SYS_access = 21,
    SYS_madvise = 28,
    SYS_getpid = 39,
    SYS_exit = 60,
    SYS_uname = 63,
    SYS_readlink = 89,
    SYS_getuid = 102,
    SYS_getgid = 104,
    SYS_geteuid = 107,
    SYS_getegid = 108,
    SYS_sigaltstack = 131,
    SYS_arch_prctl = 158,
    SYS_futex = 202,
    SYS_set_tid_address = 218,
    SYS_clock_gettime = 228,
    SYS_exit_group = 231,
    SYS_openat = 257,
    SYS_newfstatat = 262,
    SYS_set_robust_list = 273,
    SYS_prlimit64 = 302,
    SYS_getrandom = 318,
    SYS_readlinkat = 267,
    SYS_statx = 332,
    SYS_rseq = 334,
};

constexpr std::int64_t kEnosys = -38;
constexpr std::int64_t kEnoent = -2;
constexpr std::int64_t kEbadf = -9;
constexpr std::int64_t kEinval = -22;

constexpr std::uint64_t ARCH_SET_FS = 0x1002;
constexpr std::uint64_t ARCH_SET_GS = 0x1001;

}  // namespace

JitGuestEnv::JitGuestEnv() {
    // Two live environments would race over the runtime's process-global
    // memory bias; fail fast instead of producing wrong addresses.
    if (g_active != nullptr) {
        std::abort();
    }
    g_active = this;
}

JitGuestEnv::~JitGuestEnv() {
    if (core_ != nullptr) {
        X86Core::Destroy(core_);
    }
    if (instance_ != nullptr) {
        X86Instance::Destroy(instance_);
    }
    g_active = nullptr;
    g_active_space = nullptr;
    runtime::backend::SignalHandler::SetGuestMapProbe(nullptr, nullptr);
    runtime::backend::SignalHandler::SetGuestRangeProbe(nullptr, nullptr);
}

bool JitGuestEnv::Init(const std::string& elf_path, std::string& error) {
    if (!space_.ReserveWindow(32)) {
        error = "cannot reserve the 32-bit guest window";
        return false;
    }
    if (!space_.LoadElf(elf_path, image_, error)) {
        return false;
    }
    if (!space_.MapFixed(layout_.call_stack_base, layout_.call_stack_size)) {
        error = "cannot map the call stack";
        return false;
    }
    if (!space_.MapFixed(layout_.sentinel_page, kGuestPageSize)) {
        error = "cannot map the sentinel page";
        return false;
    }
    if (!space_.CreateArena(layout_.arena_base, layout_.arena_size)) {
        error = "cannot map the scratch arena";
        return false;
    }
    if (!space_.MapFixed(layout_.main_stack_top - layout_.main_stack_size,
                         layout_.main_stack_size)) {
        error = "cannot map the guest main stack";
        return false;
    }
    // The whole sentinel page is `hlt`, so even a return address that is a few
    // bytes off still stops the run instead of decoding whatever follows.
    std::memset(space_.ToHost(layout_.sentinel_page), 0xF4, kGuestPageSize);

    brk_start_ = (image_.max_vaddr + kGuestPageSize - 1) & ~(kGuestPageSize - 1);
    brk_ = brk_start_;
    if (!space_.MapFixed(brk_start_, 4u << 20)) {
        error = "cannot map the guest brk region";
        return false;
    }
    mmap_next_ = 0x20000000;

    g_active_space = &space_;
    // Wild guest pointers must surface as a guest PageFatal, not a host crash:
    // without these probes a bad pointer inside JIT code kills the test binary
    // and the "guest crashed" case could not be tested at all.
    runtime::backend::SignalHandler::SetGuestMapProbe(
            [](void* ctx, std::uintptr_t host_addr) -> bool {
                auto* s = static_cast<GuestSpace*>(ctx);
                const std::uint64_t guest = host_addr - s->Bias();
                if (guest > s->Mask()) {
                    return false;
                }
                return s->RangeIsMapped(guest, 1);
            },
            &space_);
    runtime::backend::SignalHandler::SetGuestRangeProbe(
            [](void* ctx, std::uintptr_t host_addr, u64 len) -> u64 {
                auto* s = static_cast<GuestSpace*>(ctx);
                const std::uint64_t guest = host_addr - s->Bias();
                if (guest > s->Mask()) {
                    return 0;
                }
                return s->MappedBytesFrom(guest, len);
            },
            &space_);

    instance_ = X86Instance::Make(reinterpret_cast<void*>(space_.Bias()), space_.Mask());
    instance_->SetInterpRangeCheck(
            [](void* ctx, std::uint64_t addr, std::uint64_t size) -> bool {
                return static_cast<GuestSpace*>(ctx)->RangeIsMapped(addr, size);
            },
            &space_);
    core_ = X86Core::Make(instance_);
    std::memset(&core_->GetContext(), 0, sizeof(swift::x86::ThreadContext64));
    auto& ctx = core_->GetContext();
    ctx.ef.flags = 0x202;
    ctx.x87_fcw = 0x037F;
    ctx.x87_ftw = 0xFFFF;
    ctx.mxcsr = 0x1F80;
    return true;
}

swift::x86::ThreadContext64& JitGuestEnv::Context() { return core_->GetContext(); }

void* JitGuestEnv::HostPointer(std::uint64_t guest_addr, std::uint64_t size) {
    if (!space_.RangeIsMapped(guest_addr, size)) {
        return nullptr;
    }
    return space_.ToHost(guest_addr);
}

std::uint64_t JitGuestEnv::CallStackTop() {
    return layout_.call_stack_base + layout_.call_stack_size - 256;
}

std::uint64_t JitGuestEnv::ScratchAlloc(std::uint64_t size, std::uint64_t align) {
    return space_.ArenaAlloc(size, align);
}

void JitGuestEnv::ScratchFree(std::uint64_t addr, std::uint64_t size) {
    space_.ArenaFree(addr, size);
}

std::uint64_t JitGuestEnv::LookupSymbol(const std::string& name) {
    if (auto c = resolved_.find(name); c != resolved_.end()) {
        return c->second;
    }
    std::uint64_t addr = 0;
    auto it = image_.functions.find(name);
    if (it != image_.functions.end()) {
        addr = it->second;
    } else if (auto ot = image_.objects.find(name); ot != image_.objects.end()) {
        addr = ot->second;
    }
    // STT_GNU_IFUNC: st_value names a RESOLVER that returns the address of the
    // implementation to use.  Calling it as if it were `strlen` would quietly
    // hand back a pointer instead of a length -- a wrong answer that looks like
    // a plausible number.  Run the resolver once (through this very call
    // layer) and cache what it picks, which is also what the guest's own
    // IRELATIVE relocations did at startup.
    if (addr != 0 && image_.ifuncs.count(name) != 0) {
        GuestFn<std::uint64_t()> resolver{this, addr};
        auto r = resolver.Try();
        addr = r.ok() ? r.value() : 0;
    }
    resolved_.emplace(name, addr);
    return addr;
}

std::uint64_t JitGuestEnv::RawSymbol(const std::string& name) const {
    auto it = image_.functions.find(name);
    return it != image_.functions.end() ? it->second : 0;
}

GuestRunReport JitGuestEnv::RunToSentinel() {
    GuestRunReport report{};
    auto& ctx = core_->GetContext();
    for (;;) {
        const auto reason = core_->Run();
        report.exit_reason = static_cast<std::uint32_t>(reason);
        report.interrupt = static_cast<std::uint32_t>(ctx.interrupt);
        if (reason == ExitReason::None) {
            // The sentinel page is all `hlt`, and the frontend reports rip
            // *after* the trapping instruction.
            const std::uint64_t rip = ctx.rip.qword;
            if (rip >= layout_.sentinel_page && rip <= layout_.sentinel_page + kGuestPageSize) {
                report.outcome = GuestRunOutcome::ReturnedToSentinel;
            } else {
                report.outcome = GuestRunOutcome::HaltedElsewhere;
            }
            return report;
        }
        if (reason == ExitReason::Syscall) {
            if (!HandleSyscall()) {
                report.outcome = GuestRunOutcome::UnexpectedSyscall;
                return report;
            }
            continue;
        }
        report.outcome = GuestRunOutcome::Faulted;
        return report;
    }
}

// ---------------------------------------------------------------------------
// Minimal Linux syscall emulation -- only what a static glibc needs to get
// from _start to main, plus write/exit so a guest can report and terminate.
// Anything else returns -ENOSYS, which glibc handles gracefully for the
// optional ones (rseq, ARCH_CET_STATUS, ...).
// ---------------------------------------------------------------------------
bool JitGuestEnv::HandleSyscall() {
    auto& ctx = core_->GetContext();
    const std::uint64_t nr = ctx.rax.qword;
    const std::uint64_t a0 = ctx.rdi.qword;
    const std::uint64_t a1 = ctx.rsi.qword;
    const std::uint64_t a2 = ctx.rdx.qword;
    [[maybe_unused]] const std::uint64_t a3 = ctx.r10.qword;
    std::int64_t ret = kEnosys;

    switch (nr) {
        case SYS_write: {
            if (!space_.RangeIsMapped(a1, a2)) {
                ret = -14;  // EFAULT
                break;
            }
            // Guest stdout/stderr is forwarded so a failing guest can say why.
            if (a0 == 1 || a0 == 2) {
                std::fwrite(space_.ToHost(a1), 1, a2, a0 == 1 ? stdout : stderr);
                ret = static_cast<std::int64_t>(a2);
            } else {
                ret = kEbadf;
            }
            break;
        }
        case SYS_writev: {
            std::int64_t total = 0;
            for (std::uint64_t i = 0; i < a2; ++i) {
                const std::uint64_t iov = a1 + i * 16;
                if (!space_.RangeIsMapped(iov, 16)) {
                    break;
                }
                std::uint64_t base = 0;
                std::uint64_t len = 0;
                std::memcpy(&base, space_.ToHost(iov), 8);
                std::memcpy(&len, space_.ToHost(iov + 8), 8);
                if (len == 0 || !space_.RangeIsMapped(base, len)) {
                    continue;
                }
                if (a0 == 1 || a0 == 2) {
                    std::fwrite(space_.ToHost(base), 1, len, a0 == 1 ? stdout : stderr);
                }
                total += static_cast<std::int64_t>(len);
            }
            ret = total;
            break;
        }
        case SYS_brk: {
            if (a0 == 0) {
                ret = static_cast<std::int64_t>(brk_);
            } else if (a0 >= brk_start_ && a0 < brk_start_ + (4u << 20)) {
                brk_ = a0;
                ret = static_cast<std::int64_t>(brk_);
            } else {
                ret = static_cast<std::int64_t>(brk_);
            }
            break;
        }
        case SYS_mmap: {
            const std::uint64_t len = (a1 + kGuestPageSize - 1) & ~(kGuestPageSize - 1);
            const std::uint64_t addr = a0 != 0 ? a0 : mmap_next_;
            if (!space_.MapFixed(addr, len)) {
                ret = -12;  // ENOMEM
                break;
            }
            if (a0 == 0) {
                mmap_next_ += (len + GuestSpace::kHostPageSize - 1) &
                              ~(GuestSpace::kHostPageSize - 1);
            }
            ret = static_cast<std::int64_t>(addr);
            break;
        }
        case SYS_munmap:
        case SYS_mprotect:
        case SYS_madvise:
        case SYS_rt_sigaction:
        case SYS_rt_sigprocmask:
        case SYS_sigaltstack:
        case SYS_set_robust_list:
        case SYS_close:
            ret = 0;
            break;
        case SYS_arch_prctl:
            if (a0 == ARCH_SET_FS) {
                ctx.fs_base = a1;
                ret = 0;
            } else if (a0 == ARCH_SET_GS) {
                ctx.gs_base = a1;
                ret = 0;
            } else {
                ret = kEinval;
            }
            break;
        case SYS_set_tid_address:
        case SYS_getpid:
            ret = 1000;
            break;
        case SYS_getuid:
        case SYS_geteuid:
        case SYS_getgid:
        case SYS_getegid:
            ret = 0;
            break;
        case SYS_uname: {
            if (space_.RangeIsMapped(a0, 6 * 65)) {
                auto* p = static_cast<char*>(space_.ToHost(a0));
                std::memset(p, 0, 6 * 65);
                std::strcpy(p + 0 * 65, "Linux");
                std::strcpy(p + 1 * 65, "swiftvm");
                std::strcpy(p + 2 * 65, "6.1.0");
                std::strcpy(p + 3 * 65, "#1 SMP");
                std::strcpy(p + 4 * 65, "x86_64");
                ret = 0;
            } else {
                ret = -14;
            }
            break;
        }
        case SYS_getrandom: {
            if (space_.RangeIsMapped(a0, a1)) {
                auto* p = static_cast<unsigned char*>(space_.ToHost(a0));
                // Deterministic: reproducibility beats entropy in a test VM.
                for (std::uint64_t i = 0; i < a1; ++i) {
                    p[i] = static_cast<unsigned char>(0x5A + i);
                }
                ret = static_cast<std::int64_t>(a1);
            } else {
                ret = -14;
            }
            break;
        }
        case SYS_prlimit64: {
            // rlim_cur / rlim_max for RLIMIT_STACK; glibc reads it for the
            // thread stack size.
            if (a3 != 0 && space_.RangeIsMapped(a3, 16)) {
                const std::uint64_t soft = 8u << 20;
                const std::uint64_t hard = ~std::uint64_t{0};
                std::memcpy(space_.ToHost(a3), &soft, 8);
                std::memcpy(static_cast<char*>(space_.ToHost(a3)) + 8, &hard, 8);
            }
            ret = 0;
            break;
        }
        case SYS_clock_gettime: {
            if (space_.RangeIsMapped(a1, 16)) {
                const std::uint64_t sec = 1700000000;
                const std::uint64_t nsec = 0;
                std::memcpy(space_.ToHost(a1), &sec, 8);
                std::memcpy(static_cast<char*>(space_.ToHost(a1)) + 8, &nsec, 8);
            }
            ret = 0;
            break;
        }
        case SYS_readlink:
        case SYS_readlinkat:
        case SYS_openat:
        case SYS_access:
        case SYS_statx:
        case SYS_newfstatat:
        case SYS_fstat:
        case SYS_read:
        case SYS_ioctl:
            ret = kEnoent;
            break;
        case SYS_futex:
        case SYS_rseq:
            ret = kEnosys;
            break;
        case SYS_exit:
        case SYS_exit_group:
            exited_ = true;
            exit_code_ = a0;
            ctx.rax.qword = 0;
            return false;
        default:
            unknown_syscall_ = nr;
            ret = kEnosys;
            break;
    }
    ctx.rax.qword = static_cast<std::uint64_t>(ret);
    return true;
}

// ---------------------------------------------------------------------------
// Startup: build the Linux initial stack and run until `stop_symbol`.
// ---------------------------------------------------------------------------
runtime::backend::AddressSpace* JitGuestEnv::AddressSpace() const {
    return instance_ != nullptr ? instance_->GetAddressSpace() : nullptr;
}

bool JitGuestEnv::RunStartupUntil(const std::string& stop_symbol, std::string& error,
                                  bool patch_stop) {
    auto it = image_.functions.find(stop_symbol);
    if (it == image_.functions.end()) {
        error = "no symbol named " + stop_symbol;
        return false;
    }
    const std::uint64_t stop = it->second;

    // Patch the first byte of the stop symbol with `hlt`.  Done before any
    // translation so the SMC tracker has nothing to invalidate; deliberately
    // never restored (see the header).
    if (patch_stop) {
        *static_cast<std::uint8_t*>(space_.ToHost(stop)) = 0xF4;
    } else if (*static_cast<const std::uint8_t*>(space_.ToHost(stop)) != 0xF4) {
        // "Already patched" must be a fact, not an assumption: without this the
        // guest would run past `main` into a full program run and the failure
        // would surface far away from its cause.
        error = "the loaded image is not pre-patched at " + stop_symbol;
        return false;
    }

    // --- initial stack: argc, argv, envp, auxv ------------------------------
    std::uint64_t sp = layout_.main_stack_top - 4096;
    const auto push_str = [&](const char* s) {
        const std::uint64_t n = std::strlen(s) + 1;
        sp -= n;
        std::memcpy(space_.ToHost(sp), s, n);
        return sp;
    };
    const std::uint64_t argv0 = push_str("guest");
    const std::uint64_t env0 = push_str("SWIFTVM=1");
    const std::uint64_t platform = push_str("x86_64");
    sp -= 16;
    const std::uint64_t random = sp;
    for (int i = 0; i < 16; ++i) {
        static_cast<unsigned char*>(space_.ToHost(random))[i] =
                static_cast<unsigned char>(0xA5 + i);
    }
    sp &= ~std::uint64_t{15};

    std::vector<std::uint64_t> words;
    words.push_back(1);       // argc
    words.push_back(argv0);   // argv[0]
    words.push_back(0);       // argv NULL
    words.push_back(env0);    // envp[0]
    words.push_back(0);       // envp NULL
    const auto aux = [&](std::uint64_t k, std::uint64_t v) {
        words.push_back(k);
        words.push_back(v);
    };
    aux(3, image_.phdr);            // AT_PHDR
    aux(4, image_.phentsize);       // AT_PHENT
    aux(5, image_.phnum);           // AT_PHNUM
    aux(6, kGuestPageSize);         // AT_PAGESZ
    aux(7, 0);                      // AT_BASE
    aux(8, 0);                      // AT_FLAGS
    aux(9, image_.entry);           // AT_ENTRY
    aux(11, 0);                     // AT_UID
    aux(12, 0);                     // AT_EUID
    aux(13, 0);                     // AT_GID
    aux(14, 0);                     // AT_EGID
    aux(15, platform);              // AT_PLATFORM
    aux(16, 0);                     // AT_HWCAP (x86 glibc uses CPUID instead)
    aux(17, 100);                   // AT_CLKTCK
    aux(23, 0);                     // AT_SECURE
    aux(25, random);                // AT_RANDOM
    aux(26, 0);                     // AT_HWCAP2
    aux(31, argv0);                 // AT_EXECFN
    aux(0, 0);                      // AT_NULL

    // Keep %rsp 16-byte aligned at the guest entry point.
    if ((words.size() % 2) != 0) {
        words.push_back(0);
    }
    sp -= words.size() * 8;
    sp &= ~std::uint64_t{15};
    std::memcpy(space_.ToHost(sp), words.data(), words.size() * 8);

    auto& ctx = core_->GetContext();
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ef.flags = 0x202;
    ctx.x87_fcw = 0x037F;
    ctx.x87_ftw = 0xFFFF;
    ctx.mxcsr = 0x1F80;
    ctx.rip.qword = image_.entry;
    ctx.rsp.qword = sp;

    for (;;) {
        const auto reason = core_->Run();
        if (reason == ExitReason::None) {
            if (ctx.rip.qword == stop || ctx.rip.qword == stop + 1) {
                started_ = true;
                return true;
            }
            error = "guest halted at " + std::to_string(ctx.rip.qword) + ", expected " +
                    std::to_string(stop);
            return false;
        }
        if (reason == ExitReason::Syscall) {
            if (!HandleSyscall()) {
                error = "guest exited during startup with code " + std::to_string(exit_code_);
                return false;
            }
            continue;
        }
        error = "guest faulted during startup: exit_reason=" +
                std::to_string(static_cast<int>(reason)) +
                " rip=" + std::to_string(ctx.rip.qword);
        return false;
    }
}

}  // namespace swift::guest_call
