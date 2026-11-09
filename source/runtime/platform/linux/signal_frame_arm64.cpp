#include "signal_frame.h"

#include <cstring>
#include "runtime/arch/arm64/numeric.h"
#include "runtime/execution/memory.h"

namespace swift::runtime::platform {
namespace {
using u64 = std::uint64_t;

struct alignas(16) MachineContext {
    u64 address{}, registers[31]{}, sp{}, pc{}, pstate{};
    alignas(16) std::uint8_t records[4096]{};
};

struct UserContext {
    u64 flags{}, link{};
    GuestSignalStack stack;
    u64 mask{};
    std::uint8_t unused_mask[120]{};
    MachineContext machine;
};

struct Frame {
    GuestSignalInfo info;
    UserContext context;
    u64 previous_fp{}, previous_lr{};
};

struct RecordHeader {
    std::uint32_t magic{}, size{};
};

struct FPSIMD {
    RecordHeader header{0x46508001, 528};
    std::uint32_t fpsr{}, fpcr{};
    std::uint8_t vectors[32][16]{};
};

static_assert(sizeof(MachineContext) == 4384 && offsetof(MachineContext, records) == 288 &&
              offsetof(UserContext, machine) == 176 && sizeof(UserContext) == 4560 &&
              sizeof(Frame) == 4704 && sizeof(FPSIMD) == 528);
constexpr u64 nzcv = 0xf0000000;
}  // namespace

bool EncodeArm64SignalFrame(const arm64::GuestState& state,
                            const GuestExceptionInfo& event,
                            const GuestSignalInfo& info,
                            const GuestSignalAction& action,
                            const SignalFrameContext& saved,
                            u64 restorer,
                            EncodedSignalFrame& encoded,
                            arm64::GuestState& handler) {
    u64 top = state.gprs[31];
    const bool use_alt = (action.flags & 0x08000000) && !(saved.stack.flags & 2) &&
                         !OnSignalStack(top, saved.stack);
    if (use_alt) {
        if (saved.stack.size > UINT64_MAX - saved.stack.address) return false;
        top = saved.stack.address + saved.stack.size;
    }
    if (top < sizeof(Frame)) return false;
    encoded.address = (top - sizeof(Frame)) & ~u64{15};
    if ((use_alt || OnSignalStack(state.gprs[31], saved.stack)) &&
        (encoded.address < saved.stack.address || saved.stack.size < sizeof(Frame) ||
         encoded.address - saved.stack.address > saved.stack.size - sizeof(Frame)))
        return false;
    Frame frame{};
    frame.info = info;
    frame.context.stack = saved.stack;
    frame.context.mask = saved.mask;
    auto& machine = frame.context.machine;
    machine.address = event.address_valid ? event.address : 0;
    std::memcpy(machine.registers, state.gprs, sizeof(machine.registers));
    machine.sp = state.gprs[31];
    machine.pc = state.pc;
    machine.pstate = state.packed_flags & nzcv;
    FPSIMD fp;
    fp.fpcr = state.fpcr;
    fp.fpsr = state.fpsr;
    std::memcpy(fp.vectors, state.q, sizeof(state.q));
    std::memcpy(machine.records, &fp, sizeof(fp));
    frame.previous_fp = state.gprs[29];
    frame.previous_lr = state.gprs[30];
    encoded.bytes.resize(sizeof(frame));
    std::memcpy(encoded.bytes.data(), &frame, sizeof(frame));
    handler = state;
    handler.gprs[0] = info.number;
    handler.gprs[1] = encoded.address + offsetof(Frame, info);
    handler.gprs[2] = encoded.address + offsetof(Frame, context);
    handler.gprs[29] = encoded.address + offsetof(Frame, previous_fp);
    handler.gprs[30] = restorer;
    handler.gprs[31] = encoded.address;
    handler.pc = action.handler;
    handler.reservation_valid = 0;
    return true;
}

bool DecodeArm64SignalFrame(const execution::GuestMemory& memory,
                            const arm64::GuestState& current,
                            arm64::GuestState& restored,
                            SignalFrameContext& saved) {
    if (current.gprs[31] & 15) return false;
    Frame frame;
    if (!memory.CopyIn(&frame, current.gprs[31], sizeof(frame))) return false;
    const auto& machine = frame.context.machine;
    if ((machine.pc & 3) || (machine.sp & 15) || (machine.pstate & ~nzcv) ||
        machine.pc >= (u64{1} << 48) || machine.sp >= (u64{1} << 48))
        return false;
    FPSIMD fp;
    bool found{}, ended{};
    for (std::size_t at = 0; at + sizeof(RecordHeader) <= sizeof(machine.records);) {
        RecordHeader head;
        std::memcpy(&head, machine.records + at, sizeof(head));
        if (!head.magic && !head.size) {
            ended = true;
            break;
        }
        if (head.size < 16 || (head.size & 15) || head.size > sizeof(machine.records) - at)
            return false;
        if (head.magic == 0x46508001) {
            if (found || head.size != sizeof(fp)) return false;
            std::memcpy(&fp, machine.records + at, sizeof(fp));
            found = true;
        } else if (head.magic != 0x45535201) {
            // No SVE/SME/extra-context feature is exposed by this guest profile.
            return false;
        }
        at += head.size;
    }
    if (!found || !ended || !arm64::IsSupportedFPCR(fp.fpcr) || (fp.fpsr & ~0xf800009fu))
        return false;
    restored = current;
    std::memcpy(restored.gprs, machine.registers, sizeof(machine.registers));
    restored.gprs[31] = machine.sp;
    restored.pc = machine.pc;
    restored.packed_flags = (current.packed_flags & ~nzcv) | machine.pstate;
    std::memcpy(restored.q, fp.vectors, sizeof(fp.vectors));
    restored.fpcr = fp.fpcr;
    restored.fpsr = fp.fpsr;
    restored.helper_status = restored.service_word = restored.reservation_valid = 0;
    saved = {frame.context.mask, frame.context.stack};
    return true;
}
}  // namespace swift::runtime::platform
