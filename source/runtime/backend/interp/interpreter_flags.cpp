#include "interpreter.h"

namespace swift::runtime::backend::interp {

using ir::ValueType;

#include "interpreter_internal.h"

// ---------------------------------------------------------------------------
// Guest flags
// ---------------------------------------------------------------------------

void Interpreter::SaveGuestFlags(InterpStack& stack, ir::Inst* def, ir::Flags f) {
    const auto type = def->ReturnType();
    const u32 bits = TypeBits(type);
    if (bits == 0 || bits > 64) {
        return;
    }
    const u64 result = ReadScalar(stack, ir::Value{def});
    u64& fl = state.host_cpu_flags;
    auto set = [&](u32 bit, bool on) {
        fl = on ? (fl | (u64(1) << bit)) : (fl & ~(u64(1) << bit));
    };

    if (True(f & ir::Flags::Negate)) {
        set(kHostFlagN, (result >> (bits - 1)) & 1);
    }
    if (True(f & ir::Flags::Zero)) {
        set(kHostFlagZ, result == 0);
    }

    if (True(f & (ir::Flags::Carry | ir::Flags::Overflow))) {
        bool have{false}, carry{false}, overflow{false};
        switch (def->GetOp()) {
            case ir::OpCode::Add:
            case ir::OpCode::Adc: {
                // The incoming carry must be read before this save updates C.
                const u64 cin = def->GetOp() == ir::OpCode::Adc ? ((fl >> kHostFlagC) & 1) : 0;
                const u64 l = ReadScalar(stack, def->GetArg<ir::Value>(0)) & MaskBits(bits);
                const u64 r = EvalOperand(stack, def->GetArg<ir::Operand>(1)) & MaskBits(bits);
                const unsigned __int128 wide = static_cast<unsigned __int128>(l) + r + cin;
                carry = (wide >> bits) & 1;
                const u64 sl = SignExtendTo(l, bits);
                const u64 sr = SignExtendTo(r, bits);
                const u64 sres = SignExtendTo(result, bits);
                overflow = ((~(sl ^ sr) & (sl ^ sres)) >> (bits - 1)) & 1;
                have = true;
                break;
            }
            case ir::OpCode::Sub:
            case ir::OpCode::Sbb: {
                // ARM semantics (matching the JIT): C = NOT borrow.
                const u64 bin =
                        def->GetOp() == ir::OpCode::Sbb ? (1 - ((fl >> kHostFlagC) & 1)) : 0;
                const u64 l = ReadScalar(stack, def->GetArg<ir::Value>(0)) & MaskBits(bits);
                const u64 r = EvalOperand(stack, def->GetArg<ir::Operand>(1)) & MaskBits(bits);
                carry = static_cast<unsigned __int128>(l) >=
                        static_cast<unsigned __int128>(r) + bin;
                const u64 sl = SignExtendTo(l, bits);
                const u64 sr = SignExtendTo(r, bits);
                const u64 sres = SignExtendTo(result, bits);
                overflow = (((sl ^ sr) & (sl ^ sres)) >> (bits - 1)) & 1;
                have = true;
                break;
            }
            case ir::OpCode::And:
            case ir::OpCode::Or:
            case ir::OpCode::Xor:
            case ir::OpCode::AndNot:
                // Logical ops: C = V = 0 (host ANDS/BICS behaviour, which the
                // arm64 guest relies on; the x86 frontend additionally clears
                // C/V via a ClearFlags pseudo).
                carry = false;
                overflow = false;
                have = true;
                break;
            case ir::OpCode::Mul: {
                if (bits < 64) {
                    // Mirrors the JIT's want_cv path: widen the multiply and
                    // check the upper half. 64-bit Mul leaves C/V untouched
                    // (JitTranslator::SaveCV returns early for U64).
                    const u64 l = ReadScalar(stack, def->GetArg<ir::Value>(0)) & MaskBits(bits);
                    const u64 r = EvalOperand(stack, def->GetArg<ir::Operand>(1)) & MaskBits(bits);
                    if (ir::IsSignValueType(def->GetArg<ir::Value>(0).Type())) {
                        const __int128 wide =
                                static_cast<__int128>(static_cast<s64>(SignExtendTo(l, bits))) *
                                static_cast<s64>(SignExtendTo(r, bits));
                        overflow = carry =
                                wide !=
                                static_cast<__int128>(static_cast<s64>(SignExtendTo(result, bits)));
                    } else {
                        const unsigned __int128 wide = static_cast<unsigned __int128>(l) * r;
                        overflow = carry = (wide >> bits) != 0;
                    }
                    have = true;
                }
                break;
            }
            default:
                // Other ops do not define C/V; leave them untouched.
                break;
        }
        if (have) {
            if (True(f & ir::Flags::Carry)) {
                set(kHostFlagC, carry);
            }
            if (True(f & ir::Flags::Overflow)) {
                set(kHostFlagV, overflow);
            }
        }
    }

    if (True(f & ir::Flags::Parity)) {
        // JIT SaveParity: stash the low byte of the result; PF is derived
        // from it lazily in TestFlags.
        fl = (fl & ~(u64(0xFF) << kHostParityByte)) | ((result & 0xFF) << kHostParityByte);
    }
    if (True(f & ir::Flags::AuxiliaryCarry)) {
        switch (def->GetOp()) {
            case ir::OpCode::Add:
            case ir::OpCode::Adc:
            case ir::OpCode::Sub:
            case ir::OpCode::Sbb: {
                // JIT SaveAuxiliaryCarry: AF = bit4(left) ^ bit4(right) ^
                // bit4(result) (carry/borrow into bit 4), stored as one bit.
                const u64 l = ReadScalar(stack, def->GetArg<ir::Value>(0));
                const u64 r = EvalOperand(stack, def->GetArg<ir::Operand>(1));
                set(kHostAF, ((l ^ r ^ result) >> 4) & 1);
                break;
            }
            default:
                // The JIT never snapshots AF operands for non add/sub ops.
                break;
        }
    }
}

bool Interpreter::TestGuestFlags(ir::Flags f) {
    const u64 fl = state.host_cpu_flags;
    bool result{false};
    bool first{true};

    u64 nzcv{0};
    if (True(f & ir::Flags::Negate)) {
        nzcv |= u64(1) << kHostFlagN;
    }
    if (True(f & ir::Flags::Zero)) {
        nzcv |= u64(1) << kHostFlagZ;
    }
    if (True(f & ir::Flags::Carry)) {
        nzcv |= u64(1) << kHostFlagC;
    }
    if (True(f & ir::Flags::Overflow)) {
        nzcv |= u64(1) << kHostFlagV;
    }
    if (nzcv) {
        // JIT: Tst(flags, mask); Cset ne -> "any of the tested bits set".
        result = (fl & nzcv) != 0;
        first = false;
    }
    if (True(f & ir::Flags::Parity)) {
        // Mirrors JitTranslator::TestParityFlag: fold the saved byte, x86 PF
        // is set on even parity.
        u64 b = (fl >> kHostParityByte) & 0xFF;
        b ^= b >> 4;
        b ^= b >> 2;
        b ^= b >> 1;
        const bool pf = ((b & 1) ^ 1) != 0;
        result = first ? pf : (result && pf);
        first = false;
    }
    if (True(f & ir::Flags::AuxiliaryCarry)) {
        // Mirrors JitTranslator::TestAuxiliaryCarry: AF is a single bit.
        const bool af = (fl >> kHostAF) & 1;
        result = first ? af : (result && af);
        first = false;
    }
    // Empty mask -> false, like the JIT's `Mov(result, 0)` fallback.
    return result;
}

void Interpreter::ClearGuestFlags(ir::Flags f) {
    u64& fl = state.host_cpu_flags;
    if (True(f & ir::Flags::Negate)) {
        fl &= ~(u64(1) << kHostFlagN);
    }
    if (True(f & ir::Flags::Zero)) {
        fl &= ~(u64(1) << kHostFlagZ);
    }
    if (True(f & ir::Flags::Carry)) {
        fl &= ~(u64(1) << kHostFlagC);
    }
    if (True(f & ir::Flags::Overflow)) {
        fl &= ~(u64(1) << kHostFlagV);
    }
    if (True(f & ir::Flags::Parity)) {
        // JIT quirk (JitTranslator::ClearFlags): clearing Parity writes 1
        // into the saved parity byte rather than zeroing it.
        fl = (fl & ~(u64(0xFF) << kHostParityByte)) | (u64(1) << kHostParityByte);
    }
    if (True(f & ir::Flags::AuxiliaryCarry)) {
        fl &= ~(u64(1) << kHostAF);
    }
}

bool Interpreter::EvalCondition(ir::Cond cond) {
    const u64 fl = state.host_cpu_flags;
    const bool n = (fl >> kHostFlagN) & 1;
    const bool z = (fl >> kHostFlagZ) & 1;
    const bool c = (fl >> kHostFlagC) & 1;
    const bool v = (fl >> kHostFlagV) & 1;
    // ir::Cond values match the ARM condition encoding (JitTranslator::MapCond).
    switch (cond) {
        case ir::Cond::EQ:
            return z;
        case ir::Cond::NE:
            return !z;
        case ir::Cond::CS:
            return c;
        case ir::Cond::CC:
            return !c;
        case ir::Cond::MI:
            return n;
        case ir::Cond::PL:
            return !n;
        case ir::Cond::VS:
            return v;
        case ir::Cond::VC:
            return !v;
        case ir::Cond::HI:
            return c && !z;
        case ir::Cond::LS:
            return !c || z;
        case ir::Cond::GE:
            return n == v;
        case ir::Cond::LT:
            return n != v;
        case ir::Cond::GT:
            return !z && (n == v);
        case ir::Cond::LE:
            return z || (n != v);
        case ir::Cond::AL:
        case ir::Cond::NV:
        default:
            return true;
    }
}

}  // namespace swift::runtime::backend::interp
