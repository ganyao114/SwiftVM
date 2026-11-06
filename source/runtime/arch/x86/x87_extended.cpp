#include "x87_detail.h"

namespace swift::runtime::x86::detail {
namespace {
X87Result Binary(X87State& s, unsigned index, X87Format format, X87Binary operation, u64 options, u64 raw) {
    const auto destination = (options & X87DestIndex) ? index : 0;
    const auto source = (options & X87DestIndex) ? 0 : index;
    s.status &= ~c1;
    if (Missing(s, destination) || (format == X87Format::Register && Missing(s, source))) {
        if (StackFault(s, false)) return X87Result::RaisedException;
        Write(s, destination, indefinite);
        if (options & X87Pop) Pop(s);
        return X87Result::Ok;
    }
    auto env = Environment(s);
    u16 extra{};
    auto left = Read(s, destination);
    auto right = format == X87Format::Register ? Read(s, source) : FromMemory(env, format, raw, extra);
    if (UnsupportedValue(left) || UnsupportedValue(right)) {
        if (Raise(s, invalid)) return X87Result::RaisedException;
        Write(s, destination, indefinite);
        if (options & X87Pop) Pop(s);
        return X87Result::Ok;
    }
    extra |= Denormal(left) || Denormal(right) ? denormal : 0;
    if (Raise(s, extra | Exceptions(env.exceptionFlags))) return X87Result::RaisedException;
    left = CanonicalOperand(left); right = CanonicalOperand(right);
    if (options & X87Reverse) { const auto temporary = left; left = right; right = temporary; }
    extFloat80_t result{};
    switch (operation) {
        case X87Binary::Add: result = extF80_add(&env, left, right); break;
        case X87Binary::Mul: result = extF80_mul(&env, left, right); break;
        case X87Binary::Sub: result = extF80_sub(&env, left, right); break;
        case X87Binary::Div: result = extF80_div(&env, left, right); break;
    }
    if (env.roundedUp) s.status |= c1;
    if (Raise(s, Exceptions(env.exceptionFlags))) return X87Result::RaisedException;
    Write(s, destination, result);
    if (options & X87Pop) Pop(s);
    return X87Result::Ok;
}

X87Result Compare(X87State& s, unsigned index, X87Format format, u64 options, u64 raw) {
    s.status &= ~c1;
    const bool empty = Missing(s, 0) || (format == X87Format::Register && Missing(s, index));
    auto env = Environment(s);
    u16 extra{};
    const auto left = empty ? indefinite : Read(s, 0);
    const auto right = empty ? indefinite : format == X87Format::Register ? Read(s, index) :
                                                          FromMemory(env, format, raw, extra);
    const bool unsupported = UnsupportedValue(left) || UnsupportedValue(right);
    const bool nan = IsNaN(left) || IsNaN(right);
    if (empty) extra |= invalid | stack_fault;
    else if (unsupported || ((options & X87QuietCompare) ?
             IsSignaling(left) || IsSignaling(right) : nan)) extra |= invalid;
    if (!empty && (Denormal(left) || Denormal(right))) extra |= denormal;
    extra |= Exceptions(env.exceptionFlags);
    if (Raise(s, extra)) return X87Result::RaisedException;

    unsigned flags{};
    if (empty || unsupported || nan) flags = 7;
    else {
        const auto a = CanonicalOperand(left), b = CanonicalOperand(right);
        const bool both_zero = IsZero(a) && IsZero(b);
        const bool equal = both_zero || (a.signExp == b.signExp && a.signif == b.signif);
        bool less{};
        if (!equal) {
            const bool sign_a = (a.signExp & 0x8000) != 0, sign_b = (b.signExp & 0x8000) != 0;
            less = sign_a != sign_b ? sign_a : sign_a ? MagnitudeGreater(a, b) : MagnitudeGreater(b, a);
        }
        flags = equal ? 4 : less ? 1 : 0;
    }
    if (options & X87ToFlags) s.result = flags;
    else s.status = (s.status & ~(c0 | c2 | c3)) |
                    ((flags & 1) ? c0 : 0) | ((flags & 2) ? c2 : 0) | ((flags & 4) ? c3 : 0);
    if (options & X87PopTwice) { Pop(s); Pop(s); }
    else if (options & X87Pop) Pop(s);
    return X87Result::Ok;
}

X87Result Unary(X87State& s, X87Unary operation) {
    if (operation == X87Unary::Test)
        return Compare(s, 0, X87Format::Float64, 0, 0);
    if (operation == X87Unary::Examine) {
        const bool empty = Missing(s, 0);
        const auto value = Read(s, 0);
        u16 flags{};
        if (empty) flags = c3 | c0;
        else {
            if (value.signExp & 0x8000) flags |= c1;
            if (UnsupportedValue(value)) {}
            else if (IsNaN(value)) flags |= c0;
            else if (IsInfinity(value)) flags |= c2 | c0;
            else if (IsZero(value)) flags |= c3;
            else if (Denormal(value)) flags |= c3 | c2;
            else flags |= c2;
        }
        s.status = (s.status & ~conditions) | flags;
        return X87Result::Ok;
    }
    s.status &= ~c1;
    if (Missing(s, 0)) {
        if (StackFault(s, false)) return X87Result::RaisedException;
        Write(s, 0, indefinite); return X87Result::Ok;
    }
    auto value = Read(s, 0);
    if (operation == X87Unary::ChangeSign) value.signExp ^= 0x8000;
    else if (operation == X87Unary::Abs) value.signExp &= 0x7fff;
    else {
        if (UnsupportedValue(value)) {
            if (Raise(s, invalid)) return X87Result::RaisedException;
            Write(s, 0, indefinite); return X87Result::Ok;
        }
        if (Denormal(value) && Raise(s, denormal)) return X87Result::RaisedException;
        auto env = Environment(s);
        const auto result = extF80_roundToInt(&env, CanonicalOperand(value), env.roundingMode, true);
        if (MagnitudeGreater(result, value)) s.status |= c1;
        if (Raise(s, Exceptions(env.exceptionFlags))) return X87Result::RaisedException;
        value = result;
    }
    Write(s, 0, value); return X87Result::Ok;
}

extFloat80_t Constant(const X87State& s, X87Constant kind) {
    if (kind == X87Constant::One) return one;
    if (kind == X87Constant::Zero) return zero;
    // These are the adjacent ext80 representations around the exact positive
    // constants. FLD constants honor RC, ignore PC and never raise precision.
    u64 floor{}; u16 exponent{}; bool nearest_up = true;
    switch (kind) {
        case X87Constant::Log2Ten: floor = UINT64_C(0xd49a784bcd1b8afe); exponent = 0x4000; nearest_up = false; break;
        case X87Constant::Log2E: floor = UINT64_C(0xb8aa3b295c17f0bb); exponent = 0x3fff; break;
        case X87Constant::Pi: floor = UINT64_C(0xc90fdaa22168c234); exponent = 0x4000; break;
        case X87Constant::Log10Two: floor = UINT64_C(0x9a209a84fbcff798); exponent = 0x3ffd; break;
        default: floor = UINT64_C(0xb17217f7d1cf79ab); exponent = 0x3ffe; break;
    }
    const auto rounding = (s.control >> 10) & 3;
    return {floor + (rounding == 2 || (rounding == 0 && nearest_up)), exponent};
}
} // namespace

X87Result ExecuteExtended(X87State& s, u64 encoded, X87Command command, unsigned index, u64 raw) {
    const bool operation = command == X87Command::Binary || command == X87Command::Compare;
    const auto format = static_cast<X87Format>((encoded >> 11) & 7);
    const auto binary = static_cast<X87Binary>((encoded >> 14) & 3);
    const auto options = encoded & UINT64_C(0x3f0000);
    if (operation) {
        if ((encoded & ~UINT64_C(0x3fffff)) || format > X87Format::Int64) return X87Result::Unsupported;
        if (command == X87Command::Binary) {
            if (options & ~(X87Pop | X87Reverse | X87DestIndex)) return X87Result::Unsupported;
            if (format != X87Format::Register && (index || (options & (X87DestIndex | X87Pop)))) return X87Result::Unsupported;
        } else if (binary != X87Binary::Add || (options & ~(X87Pop | X87PopTwice | X87QuietCompare | X87ToFlags)) ||
                   ((options & X87Pop) && (options & X87PopTwice)) ||
                   (format != X87Format::Register && index)) return X87Result::Unsupported;
    } else {
        if (encoded & ~UINT64_C(0x7ff)) return X87Result::Unsupported;
        const bool indexed = command == X87Command::PushReg || command == X87Command::StoreReg ||
                             command == X87Command::Free || command == X87Command::QueryRegExt ||
                             command == X87Command::RestoreRegExtHigh || command == X87Command::ConditionalMove;
        if (!indexed && index) return X87Result::Unsupported;
        if ((command == X87Command::Unary && raw > static_cast<u64>(X87Unary::Round)) ||
            (command == X87Command::PushConstant && raw > static_cast<u64>(X87Constant::Zero)) ||
            (command == X87Command::Transcendental && raw > static_cast<u64>(X87Transcendental::TwoToXMinusOne)) ||
            ((command == X87Command::CommitPrepared || command == X87Command::CommitExt80 ||
              command == X87Command::Free || command == X87Command::AdjustTop || command == X87Command::Remainder ||
              command == X87Command::PrepareI16 || command == X87Command::PrepareI32 || command == X87Command::PrepareI64 ||
              command == X87Command::ConditionalMove) && raw > 1) ||
            ((command == X87Command::LoadControlWord || command == X87Command::PushExtHigh ||
              command == X87Command::RestoreRegExtHigh || command == X87Command::PushBcdHigh) && raw > 0xffff)) return X87Result::Unsupported;
    }
    // The non-waiting environment operations are also used to finish a fully
    // loaded FLDENV/FRSTOR transaction. They do not deliver a new #MF halfway.
    switch (command) {
        case X87Command::ClearExceptions:
            s.status &= ~u16{0x80ff}; return X87Result::Ok;
        case X87Command::RefreshStatus:
            Pending(s); return X87Result::Ok;
        case X87Command::MaskExceptions:
            s.control |= 0x3f; Pending(s); return X87Result::Ok;
        case X87Command::QueryRegExt: {
            const auto value = Read(s, index);
            s.result = value.signif; s.reserved = value.signExp; return X87Result::Ok;
        }
        case X87Command::RestoreRegExtHigh:
            s.registers[Physical(s, index)] = {s.result, static_cast<u16>(raw), {}};
            return X87Result::Ok;
        case X87Command::CommitPrepared:
            s.status = s.reserved;
            if (raw) Pop(s);
            return X87Result::Ok;
        case X87Command::CommitBcd:
            CommitBcd(s); return X87Result::Ok;
        case X87Command::CommitExt80:
            s.status &= ~c1;
            if (Missing(s, 0) && StackFault(s, false)) return X87Result::RaisedException;
            if (raw) Pop(s);
            return X87Result::Ok;
        default: break;
    }
    if (Pending(s)) return X87Result::UnmaskedPending;
    switch (command) {
        case X87Command::Wait: return X87Result::Ok;
        case X87Command::LoadControlWord:
            s.control = static_cast<u16>(raw); Pending(s); return X87Result::Ok;
        case X87Command::StageExtLow:
            s.result = raw; return X87Result::Ok;
        case X87Command::PushExtHigh:
            return Push(s, {s.result, static_cast<u16>(raw)});
        case X87Command::PushBcdHigh: {
            if (Tag(s, (Top(s) - 1) & 7) != 3) return Push(s, indefinite);
            // The ISA does not validate A..F digit nibbles. Keep their
            // deterministic conversion in the architecturally undefined case.
            u64 magnitude = ((raw >> 4) & 15) * 10 + (raw & 15);
            for (unsigned byte = 8; byte--;) {
                const auto packed = (s.result >> (byte * 8)) & 255;
                magnitude = magnitude * 100 + (packed >> 4) * 10 + (packed & 15);
            }
            auto value = FromSigned(magnitude, 64);
            value.signExp |= raw & 0x8000; // Also preserves the sign of zero.
            return Push(s, value);
        }
        case X87Command::PushF32: case X87Command::PushI16: case X87Command::PushI32: case X87Command::PushI64: {
            if (Tag(s, (Top(s) - 1) & 7) != 3) return Push(s, indefinite);
            s.status &= ~c1;
            auto env = Environment(s); u16 extra{};
            const auto input = command == X87Command::PushF32 ? X87Format::Float32 :
                               command == X87Command::PushI16 ? X87Format::Int16 :
                               command == X87Command::PushI32 ? X87Format::Int32 : X87Format::Int64;
            const auto value = FromMemory(env, input, raw, extra);
            if (Raise(s, extra | Exceptions(env.exceptionFlags))) return X87Result::RaisedException;
            return Push(s, value);
        }
        case X87Command::PushReg: {
            if (Tag(s, (Top(s) - 1) & 7) != 3) return Push(s, indefinite);
            if (Missing(s, index)) {
                if (StackFault(s, false)) return X87Result::RaisedException;
                return Push(s, indefinite);
            }
            return Push(s, Read(s, index));
        }
        case X87Command::StoreReg:
            s.status &= ~c1;
            if (Missing(s, 0)) {
                if (StackFault(s, false)) return X87Result::RaisedException;
                Write(s, index, indefinite);
            } else Write(s, index, Read(s, 0));
            return X87Result::Ok;
        case X87Command::ConditionalMove:
            // Both operands are read for stack exceptions independently of
            // the condition. Normal FCMOV leaves x87 condition bits unchanged.
            if (Missing(s, 0) || Missing(s, index)) {
                if (StackFault(s, false)) return X87Result::RaisedException;
                Write(s, 0, indefinite);
            } else if (raw) Write(s, 0, Read(s, index));
            return X87Result::Ok;
        case X87Command::PrepareF32: case X87Command::PrepareF64: case X87Command::PrepareExt80:
        case X87Command::PrepareI16: case X87Command::PrepareI32: case X87Command::PrepareI64:
            return PrepareStore(s, command, raw);
        case X87Command::PrepareBcd: return PrepareBcd(s);
        case X87Command::Binary: return Binary(s, index, format, binary, options, raw);
        case X87Command::Compare: return Compare(s, index, format, options, raw);
        case X87Command::Unary: return Unary(s, static_cast<X87Unary>(raw));
        case X87Command::PushConstant: return Push(s, Constant(s, static_cast<X87Constant>(raw)));
        case X87Command::Free:
            SetTag(s, Physical(s, index), 3);
            if (raw) Pop(s);
            return X87Result::Ok;
        case X87Command::AdjustTop:
            s.status &= ~c1; SetTop(s, Top(s) + (raw ? 1 : 7)); return X87Result::Ok;
        case X87Command::Remainder: return ExecuteRemainder(s, raw != 0);
        case X87Command::Scale: return ExecuteScale(s);
        case X87Command::Extract: return ExecuteExtract(s);
        case X87Command::Transcendental: return ExecuteTranscendental(s, static_cast<X87Transcendental>(raw));
        default: return X87Result::Unsupported;
    }
}
} // namespace swift::runtime::x86::detail
