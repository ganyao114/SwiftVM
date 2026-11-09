#include "signal_frame.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include "runtime/arch/x86/flags.h"
#include "runtime/execution/memory.h"

namespace swift::runtime::platform {
namespace {
using u64 = std::uint64_t;

struct MachineContext {
    u64 gregs[23]{};
    u64 fpstate{}, reserved[8]{};
};

struct UserContext {
    u64 flags{}, link{};
    GuestSignalStack stack;
    MachineContext machine;
    u64 mask{};
};

struct Frame {
    u64 restorer{};
    UserContext context;
    GuestSignalInfo info;
};

struct FPState {
    std::uint16_t control{}, status{}, tags{}, opcode{};
    u64 ip{}, dp{};
    std::uint32_t mxcsr{}, mxcsr_mask{};
    std::uint8_t st[8][16]{}, xmm[16][16]{};
    std::uint32_t reserved[12]{};
    std::uint32_t magic{}, extended_size{};
    u64 features{};
    std::uint32_t xstate_size{}, padding[7]{};
};

struct XState {
    FPState fp;
    u64 features{}, compacted{}, reserved[6]{};
    std::uint8_t ymm_high[16][16]{};
    std::uint32_t magic2{};
};

static_assert(sizeof(MachineContext) == 256 && sizeof(UserContext) == 304 && sizeof(Frame) == 440);
static_assert(sizeof(FPState) == 512 && offsetof(XState, ymm_high) == 576 &&
              offsetof(XState, magic2) == 832);
constexpr unsigned register_order[]{x86::R8,
                                    x86::R9,
                                    x86::R10,
                                    x86::R11,
                                    x86::R12,
                                    x86::R13,
                                    x86::R14,
                                    x86::R15,
                                    x86::RDI,
                                    x86::RSI,
                                    x86::RBP,
                                    x86::RBX,
                                    x86::RDX,
                                    x86::RAX,
                                    x86::RCX,
                                    x86::RSP};
constexpr std::uint32_t magic1 = 0x46505853, magic2 = 0x46505845;
constexpr u64 xstate_features = 7;

void SaveFP(const x86::GuestState& state, XState& out) {
    out.fp.control = state.x87.control;
    out.fp.status = state.x87.status;
    out.fp.opcode = state.x87_opcode;
    out.fp.ip = state.x87_ip;
    out.fp.dp = state.x87_dp;
    out.fp.mxcsr = state.mxcsr;
    out.fp.mxcsr_mask = 0xffff;
    const auto top = (state.x87.status >> 11) & 7;
    for (unsigned i = 0; i < 8; ++i) {
        if (((state.x87.tags >> (i * 2)) & 3) != 3) out.fp.tags |= 1u << i;
        std::memcpy(out.fp.st[i], &state.x87.registers[(top + i) & 7], 10);
    }
    std::memcpy(out.fp.xmm, state.xmm, sizeof(state.xmm));
    out.fp.magic = magic1;
    out.fp.extended_size = 836;
    out.fp.features = xstate_features;
    out.fp.xstate_size = 832;
    out.features = xstate_features;
    std::memcpy(out.ymm_high, state.ymm_high, sizeof(state.ymm_high));
    out.magic2 = magic2;
}

void RestoreFP(const XState& in, bool extended, x86::GuestState& state) {
    state.x87 = {};
    state.x87_ip = state.x87_dp = 0;
    state.x87_opcode = 0;
    if (!extended || (in.features & 1)) {
        state.x87.control = in.fp.control;
        state.x87.status = in.fp.status;
        state.x87_opcode = in.fp.opcode;
        state.x87_ip = in.fp.ip;
        state.x87_dp = in.fp.dp;
        state.x87.tags = 0;
        const auto top = (in.fp.status >> 11) & 7;
        for (unsigned i = 0; i < 8; ++i) {
            const auto physical = (top + i) & 7;
            auto& value = state.x87.registers[physical];
            std::memcpy(&value, in.fp.st[i], 10);
            unsigned tag = 3;
            if (in.fp.tags & (1u << physical)) {
                const auto exponent = value.sign_exp & 0x7fff;
                tag = !exponent && !value.significand                                 ? 1
                      : !exponent || exponent == 0x7fff || !(value.significand >> 63) ? 2
                                                                                      : 0;
            }
            state.x87.tags |= tag << (physical * 2);
        }
    }
    state.mxcsr = 0x1f80;
    std::memset(state.xmm, 0, sizeof(state.xmm));
    std::memset(state.ymm_high, 0, sizeof(state.ymm_high));
    if (!extended || (in.features & 2)) {
        state.mxcsr = in.fp.mxcsr;
        std::memcpy(state.xmm, in.fp.xmm, sizeof(state.xmm));
    }
    if (extended && (in.features & 4))
        std::memcpy(state.ymm_high, in.ymm_high, sizeof(state.ymm_high));
}
}  // namespace

bool EncodeX86SignalFrame(const x86::GuestState& state,
                          const GuestExceptionInfo& event,
                          const GuestSignalInfo& info,
                          const GuestSignalAction& action,
                          const SignalFrameContext& saved,
                          u64 restorer,
                          EncodedSignalFrame& encoded,
                          x86::GuestState& handler) {
    u64 top = state.gprs[x86::RSP];
    const bool use_alt = (action.flags & 0x08000000) && !(saved.stack.flags & 2) &&
                         !OnSignalStack(top, saved.stack);
    if (use_alt) {
        if (saved.stack.size > UINT64_MAX - saved.stack.address) return false;
        top = saved.stack.address + saved.stack.size;
    }
    constexpr auto space = sizeof(Frame) + 63 + 836 + 128 + 8;
    if (top < space) return false;
    encoded.address = ((top - space) & ~u64{63}) + 56;
    const auto fp_address = (encoded.address + sizeof(Frame) + 63) & ~u64{63};
    const auto size = static_cast<std::size_t>(fp_address - encoded.address) + 836;
    if ((use_alt || OnSignalStack(state.gprs[x86::RSP], saved.stack)) &&
        (encoded.address < saved.stack.address || size > saved.stack.size ||
         encoded.address - saved.stack.address > saved.stack.size - size))
        return false;
    encoded.bytes.assign(size, 0);
    Frame frame;
    frame.restorer = restorer;
    frame.context.flags = 7;  // Extended FP state and explicit user SS.
    frame.context.stack = saved.stack;
    frame.context.mask = saved.mask;
    auto& machine = frame.context.machine;
    for (unsigned i = 0; i < 16; ++i) machine.gregs[i] = state.gprs[register_order[i]];
    machine.gregs[16] = state.pc;
    machine.gregs[17] = x86::flags::ToRflags(state.packed_flags);
    machine.gregs[18] = UINT64_C(0x002b000000000033);
    // FP syndrome bits describe the raised causes, not a hardware exception
    // error code. Only the reconstructed page-fault syndrome is an ERR here.
    machine.gregs[19] =
            event.kind == GuestExceptionKind::Memory && event.syndrome_valid ? event.syndrome : 0;
    machine.gregs[20] = event.exception_vector != UINT16_MAX       ? event.exception_vector
                        : event.kind == GuestExceptionKind::Memory ? 14
                        : event.kind == GuestExceptionKind::Divide ? 0
                        : event.kind == GuestExceptionKind::IllegalInstruction ? 6
                        : event.kind == GuestExceptionKind::Breakpoint         ? 3
                        : event.kind == GuestExceptionKind::Alignment          ? 13
                                                                               : 19;
    machine.gregs[21] = saved.mask;
    machine.gregs[22] = event.address_valid ? event.address : 0;
    machine.fpstate = fp_address;
    frame.info = info;
    std::memcpy(encoded.bytes.data(), &frame, sizeof(frame));
    XState fp{};
    SaveFP(state, fp);
    std::memcpy(encoded.bytes.data() + fp_address - encoded.address, &fp, 836);
    handler = state;
    handler.gprs[x86::RSP] = encoded.address;
    handler.gprs[x86::RDI] = info.number;
    handler.gprs[x86::RSI] = encoded.address + offsetof(Frame, info);
    handler.gprs[x86::RDX] = encoded.address + offsetof(Frame, context);
    handler.packed_flags &= ~x86::flags::Direction;
    // Linux x86 handle_signal() clears user FPU states after saving the frame.
    handler.x87 = {};
    handler.x87_ip = handler.x87_dp = 0;
    handler.x87_opcode = 0;
    handler.mxcsr = 0x1f80;
    std::memset(handler.xmm, 0, sizeof(handler.xmm));
    std::memset(handler.ymm_high, 0, sizeof(handler.ymm_high));
    handler.pc = action.handler;
    return true;
}

bool DecodeX86SignalFrame(const execution::GuestMemory& memory,
                          const x86::GuestState& current,
                          x86::GuestState& restored,
                          SignalFrameContext& saved) {
    // RET has consumed pretcode before the restorer executes rt_sigreturn.
    if (current.gprs[x86::RSP] < 8) return false;
    Frame frame;
    if (!memory.CopyIn(&frame, current.gprs[x86::RSP] - 8, sizeof(frame))) return false;
    const auto& machine = frame.context.machine;
    if (machine.gregs[16] >= (u64{1} << 48) || machine.gregs[15] >= (u64{1} << 48) ||
        (machine.gregs[17] & ~u64{0xed7}) || (machine.gregs[18] & 0xffff) != 0x33 ||
        ((machine.gregs[18] >> 48) & 0xffff) != 0x2b)
        return false;
    restored = current;
    for (unsigned i = 0; i < 16; ++i) restored.gprs[register_order[i]] = machine.gregs[i];
    restored.pc = machine.gregs[16];
    restored.packed_flags = x86::flags::FromRflags(machine.gregs[17], current.packed_flags);
    if (machine.fpstate) {
        XState fp{};
        if ((machine.fpstate & 15) || !memory.CopyIn(&fp.fp, machine.fpstate, sizeof(FPState)) ||
            (fp.fp.mxcsr & ~0xffffu))
            return false;
        const bool extended = fp.fp.magic == magic1;
        if (extended) {
            if (fp.fp.xstate_size != 832 || fp.fp.extended_size != 836 ||
                (fp.fp.features & ~xstate_features) ||
                !memory.CopyIn(
                        reinterpret_cast<std::uint8_t*>(&fp) + 512, machine.fpstate + 512, 324) ||
                (fp.features & ~xstate_features) || fp.compacted || fp.magic2 != magic2 ||
                std::any_of(
                        std::begin(fp.reserved), std::end(fp.reserved), [](u64 x) { return x; }))
                return false;
        } else if (fp.fp.magic)
            return false;
        RestoreFP(fp, extended, restored);
    } else {
        XState empty{};
        RestoreFP(empty, true, restored);
    }
    saved = {frame.context.mask, frame.context.stack};
    return true;
}
}  // namespace swift::runtime::platform
