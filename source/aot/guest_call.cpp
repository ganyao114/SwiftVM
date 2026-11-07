#include "guest_call.h"

#include "base/format.h"
#include "runtime/arch/x86/flags.h"

#include <algorithm>
#include <array>

namespace swift::guest_call {
const char* ToString(GuestCallStatus status) {
    switch (status) {
        case GuestCallStatus::Ok:
            return "Ok";
        case GuestCallStatus::GuestFault:
            return "GuestFault";
        case GuestCallStatus::UnexpectedSyscall:
            return "UnexpectedSyscall";
        case GuestCallStatus::BadReturnAddress:
            return "BadReturnAddress";
        case GuestCallStatus::StackImbalance:
            return "StackImbalance";
        case GuestCallStatus::SetupFailed:
            return "SetupFailed";
    }
    return "Unknown";
}

GuestCallError::GuestCallError(const GuestCallDiagnostic& diagnostic)
        : RuntimeError(ErrorKind::GuestFault,
                       Format("guest call {:#x}: {} (pc={:#x}, sp={:#x})",
                              diagnostic.entry,
                              ToString(diagnostic.status),
                              diagnostic.rip,
                              diagnostic.rsp))
        , diag_(diagnostic) {}

namespace {
struct CallFrame {
    GuestCallEnv& env;
    runtime::x86::GuestState saved;
    std::uint64_t buffer{}, size{};

    ~CallFrame() {
        env.Context() = saved;
        if (buffer) env.ScratchFree(buffer, size);
    }
};

std::uint64_t EightbyteValue(const RawArg& raw, unsigned index) {
    std::uint64_t value{};
    const auto offset = index * 8;
    if (offset < raw.size) std::memcpy(&value, raw.data + offset, std::min(8u, raw.size - offset));
    return value;
}
}  // namespace

GuestCallStatus PerformGuestCall(GuestCallEnv& env,
                                 std::uint64_t entry,
                                 const CallRecipe& recipe,
                                 const RawArg* args,
                                 unsigned count,
                                 std::byte* result,
                                 GuestCallDiagnostic& diagnostic) {
    using namespace runtime::x86;
    auto& state = env.Context();
    diagnostic = {};
    diagnostic.entry = entry;
    const auto fail = [&](GuestCallStatus status) {
        diagnostic.status = status;
        diagnostic.rip = state.pc;
        diagnostic.rsp = state.gprs[RSP];
        return status;
    };
    if (!entry || count != recipe.nargs) return fail(GuestCallStatus::SetupFailed);
    CallFrame frame{env, state};
    if (recipe.ret_memory) {
        frame.size = recipe.ret_size;
        frame.buffer = env.ScratchAlloc(frame.size, std::max(8u, recipe.ret_align));
        if (!frame.buffer) return fail(GuestCallStatus::SetupFailed);
        state.gprs[RDI] = frame.buffer;
    }
    const auto top = env.CallStackTop() & ~UINT64_C(15);
    if (top < recipe.stack_area + 8) return fail(GuestCallStatus::SetupFailed);
    const auto arguments = top - recipe.stack_area;
    const auto stack = arguments - 8;
    auto* area = static_cast<std::byte*>(env.HostPointer(stack, recipe.stack_area + 8, true));
    if (!area) return fail(GuestCallStatus::SetupFailed);
    std::memset(area, 0, recipe.stack_area + 8);
    const auto sentinel = env.SentinelAddress();
    std::memcpy(area, &sentinel, 8);
    for (unsigned i = 0; i < count; ++i) {
        const auto& argument = recipe.args[i];
        if (args[i].size != argument.size) return fail(GuestCallStatus::SetupFailed);
        if (argument.on_stack) {
            std::memcpy(area + 8 + argument.stack_off, args[i].data, argument.size);
            continue;
        }
        for (unsigned lane = 0; lane < argument.n_eb; ++lane) {
            const auto value = EightbyteValue(args[i], lane);
            if (argument.kind[lane] == SlotKind::IntReg)
                state.gprs[kIntArgRegs[argument.reg[lane]]] = value;
            else if (argument.kind[lane] == SlotKind::SseReg) {
                const bool high = lane && argument.kind[lane - 1] == SlotKind::SseReg &&
                                  argument.reg[lane - 1] == argument.reg[lane];
                auto* vector = state.xmm[argument.reg[lane]];
                if (!high) std::memset(vector, 0, 16);
                std::memcpy(vector + (high ? 8 : 0), &value, 8);
            }
        }
    }
    state.gprs[RAX] = recipe.sse_used;
    state.gprs[RSP] = stack;
    state.pc = entry;
    state.packed_flags &= ~runtime::x86::flags::Direction;
    const auto report = env.RunToSentinel();
    diagnostic.exit_reason = report.exit_reason;
    diagnostic.interrupt = report.interrupt;
    if (report.outcome != GuestRunOutcome::ReturnedToSentinel) {
        const auto status = report.outcome == GuestRunOutcome::UnexpectedSyscall
                                    ? GuestCallStatus::UnexpectedSyscall
                            : report.outcome == GuestRunOutcome::HaltedElsewhere
                                    ? GuestCallStatus::BadReturnAddress
                                    : GuestCallStatus::GuestFault;
        return fail(status);
    }
    if (state.pc != sentinel) return fail(GuestCallStatus::BadReturnAddress);
    if (state.gprs[RSP] != arguments) return fail(GuestCallStatus::StackImbalance);
    if (recipe.ret_memory) {
        const auto* pointer = env.HostPointer(frame.buffer, frame.size);
        if (!pointer || state.gprs[RAX] != frame.buffer) return fail(GuestCallStatus::SetupFailed);
        std::memcpy(result, pointer, frame.size);
    } else if (recipe.ret_size) {
        std::array<std::byte, 16> returned{};
        unsigned integer{}, vector{};
        for (unsigned lane = 0; lane < recipe.ret_n_eb; ++lane) {
            if (recipe.ret_eb[lane] == Eightbyte::Integer) {
                const auto value = state.gprs[kIntRetRegs[integer++]];
                std::memcpy(returned.data() + lane * 8, &value, 8);
            } else if (recipe.ret_eb[lane] == Eightbyte::Sse) {
                std::memcpy(returned.data() + lane * 8, state.xmm[vector++], 8);
            } else if (recipe.ret_eb[lane] == Eightbyte::SseUp)
                std::memcpy(returned.data() + lane * 8, state.xmm[vector - 1] + 8, 8);
        }
        std::memcpy(result, returned.data(), recipe.ret_size);
    }
    return fail(GuestCallStatus::Ok);
}

ScopedGuestBuffer::ScopedGuestBuffer(GuestCallEnv& env, std::uint64_t size, std::uint64_t align)
        : env_(&env), size_(size) {
    addr_ = env.ScratchAlloc(size, align);
    if (!addr_) size_ = 0;
}

ScopedGuestBuffer::~ScopedGuestBuffer() {
    if (addr_) env_->ScratchFree(addr_, size_);
}

ScopedGuestBuffer::ScopedGuestBuffer(ScopedGuestBuffer&& other) noexcept
        : env_(other.env_), addr_(other.addr_), size_(other.size_) {
    other.addr_ = 0;
}

ScopedGuestBuffer& ScopedGuestBuffer::operator=(ScopedGuestBuffer&& other) noexcept {
    if (this == &other) return *this;
    if (addr_) env_->ScratchFree(addr_, size_);
    env_ = other.env_;
    addr_ = other.addr_;
    size_ = other.size_;
    other.addr_ = 0;
    return *this;
}

bool ScopedGuestBuffer::Write(const void* data, std::uint64_t size, std::uint64_t offset) {
    if (!addr_ || offset > size_ || size > size_ - offset) return false;
    auto* destination = env_->HostPointer(addr_ + offset, size, true);
    if (!destination) return false;
    std::memcpy(destination, data, size);
    return true;
}

bool ScopedGuestBuffer::Read(void* data, std::uint64_t size, std::uint64_t offset) const {
    if (!addr_ || offset > size_ || size > size_ - offset) return false;
    const auto* source = env_->HostPointer(addr_ + offset, size);
    if (!source) return false;
    std::memcpy(data, source, size);
    return true;
}

ScopedGuestBuffer ScopedGuestBuffer::FromCString(GuestCallEnv& env, const char* text) {
    ScopedGuestBuffer result{env, std::strlen(text) + 1, 1};
    if (result.valid()) result.Write(text, result.size());
    return result;
}
}  // namespace swift::guest_call
