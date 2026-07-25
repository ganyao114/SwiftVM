#pragma once

#include "runtime/common/types.h"

namespace swift::x86 {

// Non-architectural X87Reg::reserved[0] marker used only by the opt-in ARM64
// reduced pipeline. SoftFloat writes clear it; certified native/f64 values set
// it. FXSAVE/FXRSTOR never expose or restore the padding.
constexpr u8 kX87ReducedMarker = 0xA5;
// An exact helper result that is finite and inside the normal binary64 range.
// Its architectural ext80 payload remains untouched, but a later opt-in
// reduced operation may explicitly round it to f64 instead of keeping the
// whole arithmetic chain on the helper path.
constexpr u8 kX87ReducedReadyMarker = 0xA6;

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

u64 X87Dispatch(u64 context, u64 command, u64 guest_address);
u64 X87Fxsave(u64 context, u64 guest_address);
u64 X87Fxrstor(u64 context, u64 guest_address);

}  // namespace swift::x86
