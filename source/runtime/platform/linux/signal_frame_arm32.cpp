#include "signal_frame.h"

#include <cstring>
#include "abi_arm32.h"
#include "runtime/arch/arm32/numeric.h"
#include "runtime/execution/memory.h"

namespace swift::runtime::platform {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct MachineContext {
    u32 trap{}, error{}, old_mask{}, r[16]{}, cpsr{}, address{};
};

struct alignas(8) UserContext {
    u32 flags{}, link{};
    abi32::Stack stack;
    MachineContext machine;
    u32 mask[2]{}, unused[30]{};
    alignas(8) std::uint8_t records[512]{};
};

struct Frame {
    abi32::SignalInfo info;
    UserContext context;
    u32 retcode[2]{};
};

struct VFP {
    u32 magic{0x56465001}, size{288};
    u64 d[32]{};
    u32 fpscr{}, pad{}, fpexc{}, fpinst{}, fpinst2{}, pad2{};
};

static_assert(sizeof(MachineContext) == 84 && offsetof(UserContext, machine) == 20);
static_assert(offsetof(UserContext, mask) == 104 && offsetof(UserContext, records) == 232);
static_assert(sizeof(UserContext) == 744 && sizeof(Frame) == 880 && sizeof(VFP) == 288);
}  // namespace

bool EncodeArm32SignalFrame(const arm32::GuestState& state,
                            const GuestExceptionInfo& event,
                            const GuestSignalInfo& info,
                            const GuestSignalAction& action,
                            const SignalFrameContext& saved,
                            u64 restorer,
                            EncodedSignalFrame& encoded,
                            arm32::GuestState& handler) {
    const bool realtime = action.flags & 4;
    const auto frame_bytes = realtime ? sizeof(Frame) : sizeof(UserContext) + 8;
    u64 top = std::uint32_t(state.r[13]);
    const bool use_alt = (action.flags & 0x08000000) && !(saved.stack.flags & 2) &&
                         !OnSignalStack(top, saved.stack);
    if (use_alt) top = saved.stack.address + saved.stack.size;
    if (top > (u64{1} << 32) || top < frame_bytes) return false;
    encoded.address = (top - frame_bytes) & ~u64{7};
    if ((use_alt || OnSignalStack(state.r[13], saved.stack)) &&
        (encoded.address < saved.stack.address || saved.stack.size < frame_bytes ||
         encoded.address - saved.stack.address > saved.stack.size - frame_bytes))
        return false;
    Frame frame{};
    frame.info = abi32::Encode(info);
    frame.context.stack = abi32::Encode(saved.stack);
    frame.context.mask[0] = saved.mask;
    frame.context.mask[1] = saved.mask >> 32;
    auto& mc = frame.context.machine;
    for (unsigned i = 0; i < 15; ++i) mc.r[i] = state.r[i];
    mc.r[15] = std::uint32_t(state.pc);
    mc.cpsr = state.Cpsr();
    mc.old_mask = saved.mask;
    mc.address = event.address_valid ? event.address : 0;
    VFP vfp;
    std::memcpy(vfp.d, state.q, sizeof(state.q));
    vfp.fpscr = state.fpscr;
    vfp.fpexc = 1u << 30;
    std::memcpy(frame.context.records, &vfp, sizeof(vfp));
    encoded.bytes.resize(frame_bytes);
    if (realtime)
        std::memcpy(encoded.bytes.data(), &frame, sizeof(frame));
    else
        std::memcpy(encoded.bytes.data(), &frame.context, sizeof(frame.context));
    handler = state;
    handler.r[0] = info.number;
    if (realtime) {
        handler.r[1] = encoded.address + offsetof(Frame, info);
        handler.r[2] = encoded.address + offsetof(Frame, context);
    }
    handler.r[13] = encoded.address;
    handler.r[14] = std::uint32_t(restorer);
    handler.pc =
            arm32::Location::Make(action.handler & ~1u, action.handler & 1u, 0, state.fpscr).Key();
    handler.reservation_valid = handler.service_word = handler.helper_status = 0;
    handler.service_pc = 0;
    return true;
}

bool DecodeArm32SignalFrame(const execution::GuestMemory& memory,
                            const arm32::GuestState& current,
                            arm32::GuestState& restored,
                            SignalFrameContext& saved,
                            bool realtime) {
    if (current.r[13] & 7) return false;
    UserContext context;
    if (!memory.CopyIn(&context, current.r[13] + (realtime ? 128 : 0), sizeof(context)))
        return false;
    const auto& mc = context.machine;
    const bool thumb = mc.cpsr & 32;
    const auto it = std::uint8_t(((mc.cpsr >> 25) & 3) | ((mc.cpsr >> 8) & 252));
    if ((mc.cpsr & 31) != 0x10 || (mc.cpsr & 0x010003c0) || (mc.r[15] & (thumb ? 1u : 3u)) ||
        (!thumb && it))
        return false;
    VFP vfp;
    std::memcpy(&vfp, context.records, sizeof(vfp));
    u32 end[2];
    std::memcpy(end, context.records + sizeof(vfp), sizeof(end));
    if (vfp.magic != 0x56465001 || vfp.size != sizeof(vfp) || end[0] || end[1] ||
        !arm32::IsSupportedFPSCR(vfp.fpscr))
        return false;
    restored = current;
    for (unsigned i = 0; i < 15; ++i) restored.r[i] = mc.r[i];
    restored.fpscr = vfp.fpscr;
    std::memcpy(restored.q, vfp.d, sizeof(restored.q));
    restored.pc = arm32::Location::Make(mc.r[15], thumb, it, vfp.fpscr).Key();
    restored.SetCpsr(mc.cpsr);
    restored.reservation_valid = restored.service_word = restored.helper_status = 0;
    restored.service_pc = 0;
    saved = {context.mask[0] | (u64(context.mask[1]) << 32), abi32::Decode(context.stack)};
    return true;
}
}  // namespace swift::runtime::platform
