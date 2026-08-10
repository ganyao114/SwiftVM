#pragma once

namespace {

// ---------------------------------------------------------------------------
// Host-side layout of the virtual flags word (state.host_cpu_flags).
//
// This mirrors the JIT flags register (x26) layout bit-for-bit so that blocks
// executed by the interpreter and by the JIT observe identical flag state:
//  - guest N/Z/C/V live at the ARM64 host NZCV bit positions (31/30/29/28),
//    so JITed code can merge them with Mrs/Msr (GuestNZCVToHost);
//  - bits 7..0 hold the low byte of the last result whose SaveFlags pseudo
//    requested Parity (x86 PF is derived from it on test);
//  - bit 26 holds x86 AF exactly (carry/borrow into bit 4), matching the JIT's
//    HostFlagsBit::AuxiliaryCarry single-bit representation.
// See JitTranslator::SaveParity / SaveAuxiliaryCarry / HostFlagsBit.
// ---------------------------------------------------------------------------
constexpr u32 kHostFlagN = 31;
constexpr u32 kHostFlagZ = 30;
constexpr u32 kHostFlagC = 29;
constexpr u32 kHostFlagV = 28;
constexpr u32 kHostAF = 26;
constexpr u32 kHostParityByte = 0;  // width 8

u32 TypeBits(ValueType type) { return ir::GetValueSizeByte(type) * 8; }

bool IsVector(ValueType type) { return type >= ValueType::V8 && type <= ValueType::V256; }

u64 MaskBits(u32 bits) { return bits >= 64 ? ~u64(0) : ((u64(1) << bits) - 1); }

u64 SignExtendTo(u64 value, u32 bits) {
    if (bits >= 64) {
        return value;
    }
    const u64 sign = u64(1) << (bits - 1);
    return (value ^ sign) - sign;
}

}  // namespace
