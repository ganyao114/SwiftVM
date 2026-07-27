//
// Runtime half of the host->guest call layer.  Everything here is driven by a
// CallPlan that was computed at compile time (abi_sysv.h); this file only
// moves bytes and validates the return path.
//

#include "guest_call.h"

#include <algorithm>
#include <cstring>

#include "fmt/format.h"

namespace swift::guest_call {

using swift::x86::Register;

const char* ToString(GuestCallStatus status) {
    switch (status) {
        case GuestCallStatus::Ok: return "Ok";
        case GuestCallStatus::GuestFault: return "GuestFault";
        case GuestCallStatus::UnexpectedSyscall: return "UnexpectedSyscall";
        case GuestCallStatus::BadReturnAddress: return "BadReturnAddress";
        case GuestCallStatus::StackImbalance: return "StackImbalance";
        case GuestCallStatus::SetupFailed: return "SetupFailed";
    }
    return "?";
}

GuestCallError::GuestCallError(const GuestCallDiagnostic& d)
        : std::runtime_error(fmt::format(
                  "guest call to {:#x} failed: {} (rip={:#x} rsp={:#x} exit_reason={} "
                  "interrupt={})",
                  d.entry, ToString(d.status), d.rip, d.rsp, d.exit_reason, d.interrupt))
        , diag_(d) {}

namespace {

// Distinctive junk written into every argument register the plan does NOT
// assign.  Without it a stale value from a previous call can make a
// mis-classified argument look correct; with it, reading the wrong register
// yields an unmistakable number.  (This is what makes the "integer and SSE
// counters share a sequence" mutation fail loudly instead of intermittently.)
constexpr std::uint64_t kIntPoison = 0xDEAD'BEEF'0BAD'0000ull;
constexpr std::uint64_t kSsePoison = 0x7FF8'DEAD'0BAD'0000ull;  // a quiet NaN pattern

std::uint64_t LoadEightbyte(const std::byte* data, unsigned size, unsigned index) {
    std::uint64_t v = 0;
    const unsigned off = index * 8;
    if (off >= size) {
        return 0;
    }
    const unsigned n = std::min<unsigned>(8, size - off);
    std::memcpy(&v, data + off, n);
    return v;
}

}  // namespace

GuestCallStatus PerformGuestCall(GuestCallEnv& env,
                                 std::uint64_t entry,
                                 const CallPlan& plan,
                                 const RawArg* args,
                                 unsigned nargs,
                                 std::byte* ret_out,
                                 GuestCallDiagnostic& diag) {
    auto& ctx = env.Context();
    diag = GuestCallDiagnostic{};
    diag.entry = entry;

    const auto fail = [&](GuestCallStatus s) {
        diag.status = s;
        diag.rip = ctx.rip.qword;
        diag.rsp = ctx.rsp.qword;
        return s;
    };

    if (entry == 0) {
        return fail(GuestCallStatus::SetupFailed);
    }

    // The call must leave the caller's view of the machine untouched: guest
    // TLS bases, callee-saved registers and the interrupted rip all belong to
    // whatever the environment was doing before.  Snapshot, then restore after
    // the return values have been read out.
    const x86::ThreadContext64 saved = ctx;

    // --- MEMORY return buffer (psABI: hidden first argument in %rdi) --------
    std::uint64_t ret_buffer = 0;
    const std::uint64_t ret_buffer_size = plan.ret_size;
    if (plan.ret_memory) {
        ret_buffer = env.ScratchAlloc(ret_buffer_size, std::max<std::uint64_t>(plan.ret_align, 8));
        if (ret_buffer == 0) {
            return fail(GuestCallStatus::SetupFailed);
        }
    }
    const auto release = [&] {
        if (ret_buffer != 0) {
            env.ScratchFree(ret_buffer, ret_buffer_size);
        }
    };

    // --- stack argument area ------------------------------------------------
    // psABI §3.2.2: the end of the input argument area is 16-byte aligned, so
    // %rsp is 16-byte aligned at the `call` and therefore ≡ 8 (mod 16) on
    // entry to the callee (the pushed return address).
    const std::uint64_t top = env.CallStackTop() & ~std::uint64_t{15};
    const std::uint64_t args_base = top - plan.stack_area;
    const std::uint64_t entry_sp = args_base - 8;

    std::vector<std::byte> image(plan.stack_area);

    // --- registers ----------------------------------------------------------
    for (unsigned i = 0; i < kMaxIntArgRegs; ++i) {
        ctx.regs[kIntArgRegs[i]].qword = kIntPoison | i;
    }
    for (unsigned i = 0; i < kMaxSseArgRegs; ++i) {
        ctx.xmms[i].l[0] = kSsePoison | i;
        ctx.xmms[i].l[1] = kSsePoison | 0x80 | i;
    }
    if (plan.ret_memory) {
        ctx.regs[kIntArgRegs[0]].qword = ret_buffer;  // %rdi
    }

    for (unsigned i = 0; i < nargs && i < plan.nargs; ++i) {
        const ArgPlan& a = plan.args[i];
        const RawArg& raw = args[i];
        if (a.n_eb == 0) {
            continue;  // empty aggregate: nothing is passed
        }
        if (a.on_stack) {
            if (a.stack_off + a.size > image.size()) {
                release();
                return fail(GuestCallStatus::SetupFailed);
            }
            std::memcpy(image.data() + a.stack_off, raw.data, a.size);
            continue;
        }
        for (unsigned e = 0; e < a.n_eb; ++e) {
            const std::uint64_t v = LoadEightbyte(raw.data, a.size, e);
            switch (a.kind[e]) {
                case SlotKind::IntReg:
                    ctx.regs[kIntArgRegs[a.reg[e]]].qword = v;
                    break;
                case SlotKind::SseReg: {
                    // A second eightbyte landing in the same vector register is
                    // the SSEUP case; it occupies the high half.
                    const bool high = e > 0 && a.kind[e - 1] == SlotKind::SseReg &&
                                      a.reg[e - 1] == a.reg[e];
                    auto& x = ctx.xmms[a.reg[e]];
                    if (high) {
                        x.l[1] = v;
                    } else {
                        x.l[0] = v;
                        x.l[1] = 0;
                    }
                    break;
                }
                case SlotKind::Stack:
                    break;  // unreachable: on_stack covers it
            }
        }
    }

    // psABI: "%al is used as hidden argument to specify the number of vector
    // registers used" for any call that may reach a varargs function.  It is
    // harmless for a prototyped call and required for a variadic one, so it is
    // always set.  RAX above AL is scratch on entry.
    ctx.rax.qword = plan.sse_used;

    // --- write the stack image and the sentinel return address ---------------
    if (!image.empty()) {
        void* host = env.HostPointer(args_base, image.size());
        if (host == nullptr) {
            release();
            return fail(GuestCallStatus::SetupFailed);
        }
        std::memcpy(host, image.data(), image.size());
    }
    const std::uint64_t sentinel = env.SentinelAddress();
    {
        void* host = env.HostPointer(entry_sp, 8);
        if (host == nullptr) {
            release();
            return fail(GuestCallStatus::SetupFailed);
        }
        std::memcpy(host, &sentinel, 8);
    }

    ctx.rsp.qword = entry_sp;
    ctx.rip.qword = entry;
    // Direction flag clear on entry, as the ABI requires of every call.
    ctx.direction = 0;
    ctx.ef.df = 0u;

    // --- run -----------------------------------------------------------------
    const GuestRunReport report = env.RunToSentinel();
    diag.exit_reason = report.exit_reason;
    diag.interrupt = report.interrupt;
    diag.rip = ctx.rip.qword;
    diag.rsp = ctx.rsp.qword;

    GuestCallStatus status = GuestCallStatus::Ok;
    switch (report.outcome) {
        case GuestRunOutcome::ReturnedToSentinel:
            break;
        case GuestRunOutcome::Faulted:
            status = GuestCallStatus::GuestFault;
            break;
        case GuestRunOutcome::UnexpectedSyscall:
            status = GuestCallStatus::UnexpectedSyscall;
            break;
        case GuestRunOutcome::HaltedElsewhere:
            status = GuestCallStatus::BadReturnAddress;
            break;
    }
    // `ret` pops the sentinel: %rsp must be exactly one slot above where the
    // callee was entered.  A mismatch means the guest unbalanced its own stack,
    // and whatever is in %rax is not a return value.
    if (status == GuestCallStatus::Ok && ctx.rsp.qword != entry_sp + 8) {
        status = GuestCallStatus::StackImbalance;
    }

    if (status != GuestCallStatus::Ok) {
        diag.status = status;
        ctx = saved;
        release();
        return status;
    }

    // --- return value --------------------------------------------------------
    if (plan.ret_memory) {
        const void* host = env.HostPointer(ret_buffer, ret_buffer_size);
        if (host == nullptr) {
            ctx = saved;
            release();
            return fail(GuestCallStatus::SetupFailed);
        }
        std::memcpy(ret_out, host, ret_buffer_size);
    } else if (plan.ret_n_eb > 0) {
        std::byte buf[16]{};
        unsigned int_taken = 0;
        unsigned sse_taken = 0;
        for (unsigned e = 0; e < plan.ret_n_eb && e < 2; ++e) {
            std::uint64_t v = 0;
            switch (plan.ret_eb[e]) {
                case Eightbyte::Integer:
                    // psABI: INTEGER return eightbytes take %rax then %rdx.
                    v = ctx.regs[kIntRetRegs[int_taken++]].qword;
                    break;
                case Eightbyte::Sse:
                    // ... and SSE eightbytes take %xmm0 then %xmm1.
                    v = ctx.xmms[sse_taken++].l[0];
                    break;
                case Eightbyte::SseUp:
                    v = ctx.xmms[sse_taken == 0 ? 0 : sse_taken - 1].l[1];
                    break;
                default:
                    break;
            }
            std::memcpy(buf + e * 8, &v, 8);
        }
        std::memcpy(ret_out, buf, std::min<unsigned>(plan.ret_size, 16));
    }

    ctx = saved;
    release();
    diag.status = GuestCallStatus::Ok;
    return GuestCallStatus::Ok;
}

// ---------------------------------------------------------------------------
// ScopedGuestBuffer
// ---------------------------------------------------------------------------

ScopedGuestBuffer::ScopedGuestBuffer(GuestCallEnv& env, std::uint64_t size, std::uint64_t align)
        : env_(&env), size_(size) {
    addr_ = env.ScratchAlloc(size, align);
    if (addr_ == 0) {
        size_ = 0;
    }
}

ScopedGuestBuffer::~ScopedGuestBuffer() {
    if (env_ != nullptr && addr_ != 0) {
        env_->ScratchFree(addr_, size_);
    }
}

ScopedGuestBuffer::ScopedGuestBuffer(ScopedGuestBuffer&& other) noexcept
        : env_(other.env_), addr_(other.addr_), size_(other.size_) {
    other.env_ = nullptr;
    other.addr_ = 0;
    other.size_ = 0;
}

ScopedGuestBuffer& ScopedGuestBuffer::operator=(ScopedGuestBuffer&& other) noexcept {
    if (this != &other) {
        if (env_ != nullptr && addr_ != 0) {
            env_->ScratchFree(addr_, size_);
        }
        env_ = other.env_;
        addr_ = other.addr_;
        size_ = other.size_;
        other.env_ = nullptr;
        other.addr_ = 0;
        other.size_ = 0;
    }
    return *this;
}

bool ScopedGuestBuffer::Write(const void* src, std::uint64_t size, std::uint64_t offset) {
    if (addr_ == 0 || offset + size > size_) {
        return false;
    }
    void* host = env_->HostPointer(addr_ + offset, size);
    if (host == nullptr) {
        return false;
    }
    std::memcpy(host, src, size);
    return true;
}

bool ScopedGuestBuffer::Read(void* dst, std::uint64_t size, std::uint64_t offset) const {
    if (addr_ == 0 || offset + size > size_) {
        return false;
    }
    const void* host = env_->HostPointer(addr_ + offset, size);
    if (host == nullptr) {
        return false;
    }
    std::memcpy(dst, host, size);
    return true;
}

ScopedGuestBuffer ScopedGuestBuffer::FromCString(GuestCallEnv& env, const char* s) {
    const std::uint64_t n = std::strlen(s) + 1;
    ScopedGuestBuffer buf{env, n, 1};
    if (buf.valid()) {
        buf.Write(s, n);
    }
    return buf;
}

}  // namespace swift::guest_call
