#include "x87.h"

#include "x87_detail.h"

namespace swift::runtime::x86::detail {
unsigned Top(const X87State& s) { return (s.status >> 11) & 7; }

unsigned Physical(const X87State& s, unsigned i) { return (Top(s) + i) & 7; }

unsigned Tag(const X87State& s, unsigned physical) { return (s.tags >> (physical * 2)) & 3; }

void SetTop(X87State& s, unsigned top) { s.status = (s.status & ~top_mask) | ((top & 7) << 11); }

void SetTag(X87State& s, unsigned physical, unsigned tag) {
    const auto shift = physical * 2;
    s.tags = (s.tags & ~(3u << shift)) | (tag << shift);
}

bool Pending(X87State& s) {
    const bool pending = (s.status & ~s.control & 0x3f) != 0;
    s.status = (s.status & ~summary) | (pending ? summary : 0);
    return pending;
}

bool Raise(X87State& s, u16 flags) {
    s.status |= flags;
    return Pending(s);
}

extFloat80_t Read(const X87State& s, unsigned i) {
    const auto& v = s.registers[Physical(s, i)];
    return {v.significand, v.sign_exp};
}

bool Denormal(extFloat80_t v) {
    return !(v.signExp & 0x7fff) && v.signif && !(v.signif & integer_bit);
}

bool UnsupportedValue(extFloat80_t v) { return (v.signExp & 0x7fff) && !(v.signif & integer_bit); }

unsigned Classify(extFloat80_t v) {
    const auto exponent = v.signExp & 0x7fff;
    if (!exponent && !v.signif) return 1;
    return !exponent || exponent == 0x7fff || !(v.signif & integer_bit) ? 2 : 0;
}

void WritePhysical(X87State& s, unsigned physical, extFloat80_t v) {
    s.registers[physical] = {v.signif, v.signExp, {}};
    SetTag(s, physical, Classify(v));
}

void Write(X87State& s, unsigned i, extFloat80_t v) { WritePhysical(s, Physical(s, i), v); }

void Pop(X87State& s) {
    SetTag(s, Top(s), 3);
    SetTop(s, Top(s) + 1);
}

bool StackFault(X87State& s, bool overflow) {
    s.status = (s.status & ~c1) | (overflow ? c1 : 0);
    return Raise(s, invalid | stack_fault);
}

bool Missing(const X87State& s, unsigned i) { return Tag(s, Physical(s, i)) == 3; }

X87Result Push(X87State& s, extFloat80_t v) {
    const auto destination = (Top(s) - 1) & 7;
    if (Tag(s, destination) != 3) {
        if (StackFault(s, true)) return X87Result::RaisedException;
        v = indefinite;
    } else
        s.status &= ~c1;
    SetTop(s, destination);
    WritePhysical(s, destination, v);
    return X87Result::Ok;
}

softfloat_state Environment(const X87State& s) {
    softfloat_state env{};
    env.detectTininess = softfloat_tininess_afterRounding;
    const auto precision = (s.control >> 8) & 3;
    // Reserved PC=01 uses extended precision, matching the preserved helper.
    env.roundingPrecision = precision == 0 ? 32 : precision == 2 ? 64 : 80;
    const auto rounding = (s.control >> 10) & 3;
    env.roundingMode = rounding == 0   ? softfloat_round_near_even
                       : rounding == 1 ? softfloat_round_min
                       : rounding == 2 ? softfloat_round_max
                                       : softfloat_round_minMag;
    return env;
}

u16 Exceptions(unsigned flags) {
    return (flags & softfloat_flag_invalid ? 1 : 0) | (flags & softfloat_flag_infinite ? 4 : 0) |
           (flags & softfloat_flag_overflow ? 8 : 0) | (flags & softfloat_flag_underflow ? 16 : 0) |
           (flags & softfloat_flag_inexact ? 32 : 0);
}

extFloat80_t Calculate(softfloat_state& env, X87Command command, extFloat80_t a, extFloat80_t b) {
    switch (command) {
        case X87Command::Add:
            return extF80_add(&env, a, b);
        case X87Command::Mul:
            return extF80_mul(&env, a, b);
        case X87Command::Div:
            return extF80_div(&env, a, b);
        default:
            return extF80_sqrt(&env, a);
    }
}

X87Result Arithmetic(X87State& s, X87Command command, unsigned index) {
    s.status &= ~c1;
    if (Missing(s, 0) || (command != X87Command::Sqrt && Missing(s, index))) {
        if (StackFault(s, false)) return X87Result::RaisedException;
        Write(s, 0, indefinite);
        return X87Result::Ok;
    }
    const auto left = Read(s, 0), right = Read(s, index);
    if (UnsupportedValue(left) || (command != X87Command::Sqrt && UnsupportedValue(right))) {
        if (Raise(s, invalid)) return X87Result::RaisedException;
        Write(s, 0, indefinite);
        return X87Result::Ok;
    }
    if ((Denormal(left) || (command != X87Command::Sqrt && Denormal(right))) && Raise(s, denormal))
        return X87Result::RaisedException;
    auto env = Environment(s);
    const auto result = Calculate(env, command, left, right);
    // The actual roundPack records the effective magnitude increment after
    // tie-to-even and underflow/overflow handling; no second arithmetic call.
    if (env.roundedUp) s.status |= c1;
    if (Raise(s, Exceptions(env.exceptionFlags))) return X87Result::RaisedException;
    Write(s, 0, result);
    return X87Result::Ok;
}

X87Result Execute(X87State& s, X87Command command, unsigned index, u64 raw) {
    if (command == X87Command::Init) {
        // FNINIT empties the stack through tags; physical register payloads
        // survive. Keep this bounded field reset free of a whole-state copy.
        s.control = 0x037f;
        s.status = 0;
        s.tags = 0xffff;
        s.reserved = 0;
        s.result = 0;
        return X87Result::Ok;
    }
    // FNSTSW/FNSTCW are non-waiting observations of pending exception state.
    if (command == X87Command::Status) {
        Pending(s);
        s.result = s.status;
        return X87Result::Ok;
    }
    if (command == X87Command::ControlWord) {
        s.result = s.control;
        return X87Result::Ok;
    }
    if (Pending(s)) return X87Result::UnmaskedPending;
    switch (command) {
        case X87Command::SetControlWord:
            if (raw > 0xffff) return X87Result::Unsupported;
            s.control = static_cast<u16>(raw);
            return Pending(s) ? X87Result::UnmaskedPending : X87Result::Ok;
        case X87Command::PushOne:
            return Push(s, one);
        case X87Command::PushOnePop: {
            const auto result = Push(s, one);
            if (result != X87Result::Ok) return result;
            // Push delivers exact one or the masked-overflow quiet
            // indefinite. Sqrt changes neither; StoreRegPop(0) clears C1
            // then empties the real pushed slot, retaining its payload.
            s.status &= ~c1;
            Pop(s);
            return X87Result::Ok;
        }
        case X87Command::PushF64: {
            // Check stack overflow before interpreting the memory operand.
            if (Tag(s, (Top(s) - 1) & 7) != 3) return Push(s, indefinite);
            s.status &= ~c1;
            if (!(raw & 0x7ff0000000000000ULL) && (raw & 0x000fffffffffffffULL) &&
                Raise(s, denormal))
                return X87Result::RaisedException;
            auto env = Environment(s);
            const auto result = f64_to_extF80(&env, float64_t{raw});
            if (Raise(s, Exceptions(env.exceptionFlags))) return X87Result::RaisedException;
            return Push(s, result);
        }
        case X87Command::Exchange:
        case X87Command::ExchangeTwice: {
            s.status &= ~c1;
            const bool empty_left = Missing(s, 0), empty_right = Missing(s, index);
            if (empty_left || empty_right) {
                if (StackFault(s, false)) return X87Result::RaisedException;
                Write(s, 0, indefinite);
                Write(s, index, indefinite);
                return X87Result::Ok;
            }
            const auto left = Read(s, 0), right = Read(s, index);
            // A double exchange still classifies the final values and
            // clears payload padding; it is not an unconditional no-op.
            const bool twice = command == X87Command::ExchangeTwice;
            Write(s, 0, twice ? left : right);
            Write(s, index, twice ? right : left);
            return X87Result::Ok;
        }
        case X87Command::StoreRegPop:
            s.status &= ~c1;
            if (Missing(s, 0)) {
                if (StackFault(s, false)) return X87Result::RaisedException;
                Write(s, index, indefinite);
            } else
                Write(s, index, Read(s, 0));
            Pop(s);
            return X87Result::Ok;
        case X87Command::Pop:
            // A separate memory-store continuation must retain ReadF64's
            // rounding direction; only an empty-stack fault changes C1.
            if (Missing(s, 0) && StackFault(s, false)) return X87Result::RaisedException;
            Pop(s);
            return X87Result::Ok;
        case X87Command::ReadF64: {
            s.status &= ~c1;
            if (Missing(s, 0)) {
                if (StackFault(s, false)) return X87Result::RaisedException;
                s.result = 0xfff8000000000000ULL;
                return X87Result::Ok;
            }
            const auto value = Read(s, 0);
            if (UnsupportedValue(value)) {
                if (Raise(s, invalid)) return X87Result::RaisedException;
                s.result = 0xfff8000000000000ULL;
                return X87Result::Ok;
            }
            auto env = Environment(s);
            const auto result = extF80_to_f64(&env, value);
            if (env.roundedUp) s.status |= c1;
            if (Raise(s, Exceptions(env.exceptionFlags))) return X87Result::RaisedException;
            s.result = result.v;
            return X87Result::Ok;
        }
        case X87Command::Add:
        case X87Command::Mul:
        case X87Command::Div:
        case X87Command::Sqrt:
            return Arithmetic(s, command, index);
        default:
            return X87Result::Unsupported;
    }
}
}  // namespace swift::runtime::x86::detail

extern "C" std::uint64_t SvmX87Execute(void* state,
                                       std::uint64_t encoded,
                                       std::uint64_t raw) noexcept {
    using namespace swift::runtime::x86;
    using namespace swift::runtime::x86::detail;
    if (!state) return static_cast<u64>(X87Result::Unsupported);
    const auto command = static_cast<X87Command>(encoded & 255);
    if (command > X87Command::CommitBcd) return static_cast<u64>(X87Result::Unsupported);
    const auto index = static_cast<unsigned>((encoded >> 8) & 7);
    if (command > X87Command::PushOnePop)
        return static_cast<u64>(
                ExecuteExtended(*static_cast<X87State*>(state), encoded, command, index, raw));
    if (encoded & ~std::uint64_t{0x7ff}) return static_cast<u64>(X87Result::Unsupported);
    const bool indexed = command == X87Command::Exchange || command == X87Command::Add ||
                         command == X87Command::Mul || command == X87Command::Div ||
                         command == X87Command::StoreRegPop || command == X87Command::ExchangeTwice;
    if (!indexed && index) return static_cast<u64>(X87Result::Unsupported);
    return static_cast<u64>(Execute(*static_cast<X87State*>(state), command, index, raw));
}

namespace swift::runtime::x86 {
std::uint8_t X87AbridgedTag(const X87State& state) noexcept {
    std::uint8_t result{};
    for (unsigned i = 0; i < 8; ++i)
        if (detail::Tag(state, i) != 3) result |= std::uint8_t(1u << i);
    return result;
}

void X87RestoreAbridgedTags(X87State& state, std::uint8_t present) noexcept {
    for (unsigned i = 0; i < 8; ++i) {
        const auto& value = state.registers[i];
        detail::SetTag(
                state,
                i,
                (present & (1u << i)) ? detail::Classify({value.significand, value.sign_exp}) : 3);
    }
}
}  // namespace swift::runtime::x86
