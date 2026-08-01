#pragma once

#include <cstddef>

#include "runtime/common/types.h"

namespace swift::x86 {

// Non-architectural X87Reg::reserved[0] marker used only by the opt-in ARM64
// reduced pipeline. SoftFloat writes clear it; certified native/f64 values set
// it. FXSAVE/FXRSTOR never expose or restore the padding.
constexpr u8 kX87ReducedMarker = 0xA5;

// X87Dispatch command encoding.  The frontend supplies one command word and
// one optional guest address; the helper updates the architectural x87 fields
// in ThreadContext64 and returns a scalar result only for FNSTSW AX / FCOMI.
enum class X87Action : u8 {
    Init,
    ClearExceptions,
    LoadFloat,
    StoreFloat,
    LoadInt,
    StoreInt,
    LoadReg,
    StoreReg,
    Binary,
    Compare,
    Unary,
    Remainder,
    Scale,
    Extract,
    Transcendental,
    LoadConstant,
    Exchange,
    Free,
    AdjustTop,
    StoreControl,
    LoadControl,
    StoreStatus,
    StoreEnvironment,
    LoadEnvironment,
};

enum class X87Format : u8 {
    None,
    Float32,
    Float64,
    Float80,
    Int16,
    Int32,
    Int64,
    Register,
};

enum class X87Binary : u8 {
    Add,
    Mul,
    Sub,
    Div,
};

enum class X87Unary : u8 {
    ChangeSign,
    Abs,
    Test,
    Examine,
    Sqrt,
    Round,
};

enum class X87Remainder : u8 {
    Truncate,
    Nearest,
};

enum class X87Transcendental : u8 {
    Sin,
    Cos,
    SinCos,
    Tan,
    Atan,
    YLog2X,
    YLog2XPlusOne,
    TwoToXMinusOne,
};

enum class X87Constant : u8 {
    One,
    Log2Ten,
    Log2E,
    Pi,
    Log10Two,
    LnTwo,
    Zero,
};

enum X87CommandFlag : u32 {
    X87Pop = 1u << 0,
    X87PopTwice = 1u << 1,
    X87Reverse = 1u << 2,
    X87DestIndex = 1u << 3,
    X87Unordered = 1u << 4,
    X87ToEFlags = 1u << 5,
    X87Truncate = 1u << 6,
    X87IncrementTop = 1u << 7,
};

constexpr u64 MakeX87Command(X87Action action,
                             X87Format format = X87Format::None,
                             u8 index = 0,
                             u8 operation = 0,
                             u32 flags = 0) {
    return static_cast<u64>(action) | (static_cast<u64>(format) << 8) |
           (static_cast<u64>(index & 7) << 16) |
           (static_cast<u64>(operation) << 24) |
           (static_cast<u64>(flags) << 32);
}

// Actions whose dedicated dispatch path was audited to use integer/SoftFloat
// code only and may therefore execute while AFP guest FPCR remains installed.
// Keep this a closed allowlist: an unreviewed action is conservative by
// default, even if its implementation currently looks similar to one below.
constexpr bool X87ActionFPCRTransparent(X87Action action) {
    switch (action) {
        case X87Action::LoadFloat:
        case X87Action::StoreFloat:
        case X87Action::StoreReg:
        case X87Action::Remainder:
        case X87Action::LoadConstant:
        case X87Action::StoreControl:
        case X87Action::StoreStatus:
            return true;
        default:
            return false;
    }
}

constexpr bool X87CommandFPCRTransparent(u64 command) {
    return X87ActionFPCRTransparent(
            static_cast<X87Action>(command & 0xFF));
}

// Bit the x87/fxsave helpers set in their return value when a guest address
// they had to dereference is not backed by a guest mapping. The architectural
// outcome is #PF, but the fault happens in a live *host* frame that
// runtime.cpp's HandleFault cannot unwind, so the helper validates instead,
// declines to touch the memory, and reports the fault through its result; the
// emitted code turns that bit into a guest-visible PageFatal (decoder_x87.cc's
// CallX87, decoder_xsave.cc). Bit 63 is free in every helper result: the only
// values the helpers return are FSW (16 bits) and the FCOMI flag triple.
constexpr u64 kX87GuestFault = u64(1) << 63;

// Guest -> host pointer for helpers that dereference guest memory from host
// code. Applies the bounded-guest-window mask (isolation: host memory is
// unreachable) and, when the embedder installed one, its guest-mapping oracle
// (availability: an unmapped address returns nullptr instead of faulting in an
// unrecoverable host frame, and latches kX87GuestFault for the caller).
// Returns nullptr unless [address, address + size) is fully backed.
u8* GuestPointer(u64 address, size_t size);

u64 X87Dispatch(u64 context, u64 command, u64 guest_address);
u64 X87DispatchFPFree(u64 context, u64 command, u64 guest_address);
u64 X87Fxsave(u64 context, u64 guest_address);
u64 X87Fxrstor(u64 context, u64 guest_address);

}  // namespace swift::x86
