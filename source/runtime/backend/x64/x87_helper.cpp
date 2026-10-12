#include "x87_helper.h"

#include <stdexcept>
#include "runtime/frontend/x86/x87.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::x64 {

namespace {
bool ValidRegisterCommand(u64 command) {
    using namespace swift::x86;
    const auto action = static_cast<X87Action>(command & 0xff);
    const auto format = static_cast<X87Format>((command >> 8) & 0xff);
    const auto index = (command >> 16) & 0xff;
    const auto operation = (command >> 24) & 0xff;
    const u64 flags = command >> 32;
    if (index > 7) return false;
    const bool plain = format == X87Format::None && index == 0 && operation == 0 && flags == 0;
    switch (action) {
        case X87Action::Init: case X87Action::ClearExceptions:
        case X87Action::Scale: case X87Action::Extract:
            return plain;
        case X87Action::LoadReg: case X87Action::Exchange:
            return format == X87Format::Register && operation == 0 && flags == 0;
        case X87Action::StoreReg: case X87Action::Free:
            return format == X87Format::Register && operation == 0 && (flags & ~u64(X87Pop)) == 0;
        case X87Action::Binary:
            return format == X87Format::Register && operation <= u64(X87Binary::Div) &&
                   (flags & ~u64(X87Pop | X87Reverse | X87DestIndex)) == 0 &&
                   (!(flags & X87Pop) || (flags & X87DestIndex));
        case X87Action::Compare:
            return format == X87Format::Register && operation == 0 &&
                   (flags & ~u64(X87Pop | X87PopTwice | X87Unordered | X87ToEFlags)) == 0 &&
                   (!(flags & X87PopTwice) || (index == 1 && !(flags & (X87Pop | X87ToEFlags))));
        case X87Action::Unary:
            return format == X87Format::None && index == 0 && flags == 0 && operation <= u64(X87Unary::Round);
        case X87Action::Remainder:
            return format == X87Format::None && index == 0 && flags == 0 && operation <= u64(X87Remainder::Nearest);
        case X87Action::Transcendental:
            return format == X87Format::None && index == 0 && flags == 0 && operation <= u64(X87Transcendental::TwoToXMinusOne);
        case X87Action::LoadConstant:
            return format == X87Format::None && index == 0 && flags == 0 && operation <= u64(X87Constant::Zero);
        case X87Action::AdjustTop:
            return format == X87Format::None && index == 0 && operation == 0 && (flags & ~u64(X87IncrementTop)) == 0;
        case X87Action::StoreStatus:
            return format == X87Format::Register && index == 0 && operation == 0 && flags == 0;
        default: return false;
    }
}
}  // namespace

u64 ValidateX87RegisterOp(ir::Inst* inst, u64 uniform_size) {
    if (uniform_size < sizeof(swift::x86::ThreadContext64))
        throw std::runtime_error("x64 X87Op requires a complete x86 uniform context");
    if (!inst->ArgAt(0).IsValue() || !inst->ArgAt(1).IsImm() || !inst->ArgAt(2).IsValue())
        throw std::runtime_error("x64 X87Op requires context/address values and an immediate command");
    const auto command = inst->GetArg<ir::Imm>(1).Get();
    if (!ValidRegisterCommand(command))
        throw std::runtime_error("x64 X87Op requires a valid register-only command (memory forms unsupported)");
    for (u32 index : {0u, 2u}) {
        const auto input = inst->GetArg<ir::Value>(index);
        if (!input.Defined() || input.Type() != ir::ValueType::U64 || input.Def()->ReturnType() != ir::ValueType::U64)
            throw std::runtime_error("x64 X87Op requires U64 context/address inputs");
    }
    const bool narrow_status = (command & 0xff) == u64(swift::x86::X87Action::StoreStatus) &&
                               inst->ReturnType() == ir::ValueType::U16;
    if (inst->ReturnType() != ir::ValueType::U64 && !narrow_status)
        throw std::runtime_error("x64 X87Op requires a U64 result (or FNSTSW U16)");
    return command;
}

u64 EvaluateX87Register(State* state, u64 context, u64 command) noexcept {
    // Keep this state in the host wrapper, so non-X87 blocks retain their
    // existing frame and FP prologue. No guest memory access can abandon it.
    alignas(16) unsigned char saved[512];
    const u32 default_mxcsr = 0x1f80;
    asm volatile("fxsave64 %0" : "=m"(saved) : : "memory");
    asm volatile("fninit\n\tldmxcsr %0" : : "m"(default_mxcsr) : "memory",
                 "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
    u64 result{};
    try {
        if (!ValidRegisterCommand(command) || context != reinterpret_cast<u64>(state->uniform_buffer_begin))
            state->halt_reason = HaltReason::IllegalCode;
        else result = swift::x86::X87Dispatch(context, command, 0);
    } catch (...) {
        state->halt_reason = HaltReason::IllegalCode;
    }
    // FXRSTOR restores both x87 payload/control/status and MXCSR. Describe all
    // restored FP registers to the compiler; the U64 result stays in GPRs.
    asm volatile("fxrstor64 %0" : : "m"(saved) : "memory",
                 "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)",
                 "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                 "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    return result;
}

}  // namespace swift::runtime::backend::x64
