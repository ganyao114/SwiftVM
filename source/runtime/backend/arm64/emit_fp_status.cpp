#include "emit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64::detail {

void Writer::HandleReadFPStatusFamily(OpContext& ctx) {
    Move(0, Read(ctx.cursor.Take(), 0));
    __ mrs(v::x30, v::FPSR);
    Extract(ctx.destination, 30, 0, 1);
    Extract(17, 30, 7, 1);
    __ orr(W(ctx.destination), W(ctx.destination), Operand(v::w17, v::LSL, 1));
    Extract(17, 30, 1, 4);
    __ orr(W(ctx.destination), W(ctx.destination), Operand(v::w17, v::LSL, 2));
    __ and_(v::w0, v::w0, Operand(u64{63}));
    __ orr(W(ctx.destination), W(ctx.destination), Operand(v::w0));
}

void Writer::LoadMxcsrFlushControl() {
    if (options.mxcsr_flush_mode != UINT8_MAX || options.mxcsr_flush_offset == UINT32_MAX) return;
    const auto offset = options.mxcsr_flush_offset;
    if (!(offset & 3) && offset / 4 <= 4095)
        Memory(true, 32, 0, 19, offset / 4);
    else {
        Immediate(17, offset);
        __ add(v::x17, v::x19, Operand(v::x17));
        Memory(true, 32, 0, 17);
    }
}

u32 Writer::DenormalControlGuard() {
    if (options.mxcsr_flush_mode != UINT8_MAX) return UINT32_MAX;
    if (options.mxcsr_flush_offset != UINT32_MAX) {
        const auto disabled = masm.Here();
        __ tbnz(v::w0, 6, std::int64_t{0});
        return disabled;
    }
    if (options.native_fp_environment) {
        // AFP keeps DAZ in FPCR.FIZ. Read once for the whole consumer.
        __ mrs(v::x17, v::FPCR);
        const auto disabled = masm.Here();
        __ tbnz(v::x17, 0, std::int64_t{0});
        return disabled;
    }
    return UINT32_MAX;
}

void Writer::RecordDenormalLanes(std::span<const unsigned> inputs,
                                 unsigned bits,
                                 unsigned count,
                                 std::initializer_list<unsigned> peers,
                                 unsigned denominator,
                                 unsigned fma_flags) {
    const unsigned fraction = bits == 32 ? 23 : 52, exponent = bits == 32 ? 8 : 11;
    const u64 exponent_mask = ((u64{1} << exponent) - 1) << fraction;
    const bool fused = fma_flags != UINT32_MAX;
    for (unsigned lane = 0; lane < count; ++lane) {
        std::array<u32, 16> done{};
        unsigned done_count{};
        const auto skip_zero = [&](const v::Register& reg) {
            done[done_count++] = masm.Here();
            __ cbz(reg, std::int64_t{0});
        };
        const auto skip_nonzero = [&](const v::Register& reg) {
            done[done_count++] = masm.Here();
            __ cbnz(reg, std::int64_t{0});
        };
        const auto read = [&](unsigned reg) { __ umov(R(16, bits), Lanes(reg, bits), lane); };
        const auto zero = [&](unsigned reg) {
            read(reg);
            __ lsl(R(17, bits), R(16, bits), 1);
            skip_zero(R(17, bits));
        };
        const auto nan = [&](unsigned reg) {
            read(reg);
            Extract(17, 16, fraction, exponent);
            __ eor(v::x17, v::x17, Operand((u64{1} << exponent) - 1));
            const auto finite = masm.Here();
            __ cbnz(v::x17, std::int64_t{0});
            __ lsl(R(17, bits), R(16, bits), bits - fraction);
            skip_nonzero(R(17, bits));
            masm.Patch(finite, masm.Here());
        };
        if (inputs.size() > 1) {
            std::array<u32, 3> denormal{};
            unsigned denormal_count{};
            for (const auto input : inputs) {
                read(input);
                Extract(17, 16, fraction, exponent);
                const auto normal_input = masm.Here();
                __ cbnz(v::x17, std::int64_t{0});
                __ lsl(R(17, bits), R(16, bits), 1);
                denormal[denormal_count++] = masm.Here();
                __ cbnz(R(17, bits), std::int64_t{0});
                masm.Patch(normal_input, masm.Here());
            }
            done[done_count++] = masm.Here();
            __ b(std::int64_t{0});
            for (unsigned i = 0; i < denormal_count; ++i) masm.Patch(denormal[i], masm.Here());
        } else {
            read(inputs.front());
            Extract(17, 16, fraction, exponent);
            skip_nonzero(v::x17);
            __ lsl(R(17, bits), R(16, bits), 1);
            skip_zero(R(17, bits));
        }
        if (source.Operations()[node.value].opcode == OpCode::VecFUnary) {
            done[done_count++] = masm.Here();
            __ tbnz(R(16, bits), bits - 1, std::int64_t{0});  // Negative sqrt: IE takes priority.
        }
        for (const auto peer : peers) nan(peer);
        if (denominator != UINT32_MAX) zero(denominator);  // ZE takes priority over same-lane DE.
        if (fused) {
            const auto a = peers.begin()[0], b = peers.begin()[1], c = peers.begin()[2];
            const auto infinity = [&](unsigned reg) {
                read(reg);
                __ and_(R(17, bits), R(16, bits), Operand((u64{1} << (bits - 1)) - 1));
                __ eor(R(17, bits), R(17, bits), Operand(exponent_mask));
                const auto site = masm.Here();
                __ cbz(R(17, bits), std::int64_t{0});
                return site;
            };
            const auto a_infinite = infinity(a), b_infinite = infinity(b);
            const auto finite_product = masm.Here();
            __ b(std::int64_t{0});
            masm.Patch(a_infinite, masm.Here());
            zero(b);
            const auto product_infinite = masm.Here();
            __ b(std::int64_t{0});
            masm.Patch(b_infinite, masm.Here());
            zero(a);
            masm.Patch(product_infinite, masm.Here());
            read(c);
            __ and_(R(17, bits), R(16, bits), Operand((u64{1} << (bits - 1)) - 1));
            __ eor(R(17, bits), R(17, bits), Operand(exponent_mask));
            const auto finite_addend = masm.Here();
            __ cbnz(R(17, bits), std::int64_t{0});
            read(a);
            __ umov(R(17, bits), Lanes(b, bits), lane);
            __ eor(R(16, bits), R(16, bits), Operand(R(17, bits)));
            __ umov(R(17, bits), Lanes(c, bits), lane);
            __ eor(R(16, bits), R(16, bits), Operand(R(17, bits)));
            const bool negate_c = bool(fma_flags & 2) ^
                                  (bool(fma_flags & 8) && ((lane & 1) == ((fma_flags >> 4) & 1)));
            if (bool(fma_flags & 1) != negate_c)
                __ eor(R(16, bits), R(16, bits), Operand(u64{1} << (bits - 1)));
            done[done_count++] = masm.Here();
            __ tbnz(R(16, bits), bits - 1, std::int64_t{0});
            masm.Patch(finite_addend, masm.Here());
            masm.Patch(finite_product, masm.Here());
        }
        __ mrs(v::x17, v::FPSR);
        __ orr(v::x17, v::x17, Operand(u64{1} << 7));
        __ msr(v::FPSR, v::x17);
        for (unsigned i = 0; i < done_count; ++i) masm.Patch(done[i], masm.Here());
    }
}

void Writer::RecordDenormalInput(unsigned value,
                                 unsigned bits,
                                 unsigned count,
                                 std::initializer_list<unsigned> peers,
                                 unsigned denominator,
                                 unsigned fma_flags) {
    if (options.mxcsr_flush_mode != UINT8_MAX && (options.mxcsr_flush_mode & 1)) return;
    const bool fused = fma_flags != UINT32_MAX;
    const std::span<const unsigned> inputs =
            fused ? std::span<const unsigned>{peers.begin(), peers.size()}
                  : std::span<const unsigned>{&value, 1};
    const auto disabled = DenormalControlGuard();
    u32 normal = UINT32_MAX;
    if (bits == 32 && count == 4) {
        unsigned scratch{};
        for (const unsigned candidate : {VectorTemporary(), VectorTemporary(true), 0u, 1u}) {
            bool input = candidate == value;
            for (const auto peer : peers) input |= candidate == peer;
            if (!input) {
                scratch = candidate;
                break;
            }
        }
        std::array<u32, 3> rare{};
        unsigned rare_count{};
        for (const auto input : inputs) {
            __ shl(Lanes(scratch, 32), Lanes(input, 32), 1);
            __ ushr(Lanes(scratch, 32), Lanes(scratch, 32), 24);
            __ uminv(S(scratch), Lanes(scratch, 32));
            __ fmov(v::w16, S(scratch));
            rare[rare_count++] = masm.Here();
            __ cbz(v::w16, std::int64_t{0});
        }
        normal = masm.Here();
        __ b(std::int64_t{0});
        for (unsigned i = 0; i < rare_count; ++i) masm.Patch(rare[i], masm.Here());
    }
    RecordDenormalLanes(inputs, bits, count, peers, denominator, fma_flags);
    if (normal != UINT32_MAX) masm.Patch(normal, masm.Here());
    if (disabled != UINT32_MAX) masm.Patch(disabled, masm.Here());
}

void Writer::RecordDenormalInputs(
        unsigned left, unsigned right, unsigned bits, unsigned count, unsigned denominator) {
    if (count == 1) {
        RecordDenormalInput(left, bits, count, {right}, denominator);
        RecordDenormalInput(right, bits, count, {left}, denominator);
        return;
    }
    if (options.mxcsr_flush_mode != UINT8_MAX && (options.mxcsr_flush_mode & 1)) return;
    const auto disabled = DenormalControlGuard();
    // All candidates are reserved scratches, never pinned/allocated homes.
    // Spilled inputs can occupy v0/v1, so exclude both actual source registers.
    std::array<unsigned, 2> scratch{};
    unsigned available{};
    for (const auto candidate : {VectorTemporary(), VectorTemporary(true), 0u, 1u}) {
        if (candidate == left || candidate == right) continue;
        scratch[available++] = candidate;
        if (available == scratch.size()) break;
    }
    const auto first = scratch[0], second = scratch[1];
    if (bits == 64) {
        // Pack the four high words; their unsigned minimum after removing
        // sign has a zero exponent iff some input lane needs classification.
        __ uzp2(Lanes(first, 32), Lanes(left, 32), Lanes(right, 32));
        __ shl(Lanes(first, 32), Lanes(first, 32), 1);
    } else {
        __ shl(Lanes(first, 32), Lanes(left, 32), 1);
        __ shl(Lanes(second, 32), Lanes(right, 32), 1);
        __ umin(Lanes(first, 32), Lanes(first, 32), Lanes(second, 32));
    }
    __ uminv(S(first), Lanes(first, 32));
    __ fmov(v::w16, S(first));
    __ lsr(v::w16, v::w16, bits == 32 ? 24 : 21);
    const auto normal = masm.Here();
    __ cbnz(v::x16, std::int64_t{0});
    const std::array inputs{left, right};
    // One lane classification handles both inputs and checks NaN/invalid
    // precedence before setting DE. No source or guest integer flags change.
    RecordDenormalLanes(inputs, bits, count, {left, right}, denominator, UINT32_MAX);
    masm.Patch(normal, masm.Here());
    if (disabled != UINT32_MAX) masm.Patch(disabled, masm.Here());
}

void Writer::RecordNaNInput(unsigned value, unsigned bits, unsigned count, bool signaling) {
    const unsigned fraction = bits == 32 ? 23 : 52, exponent = bits == 32 ? 8 : 11;
    for (unsigned lane = 0; lane < count; ++lane) {
        __ umov(R(16, bits), Lanes(value, bits), lane);
        Extract(17, 16, fraction, exponent);
        __ eor(v::x17, v::x17, Operand((u64{1} << exponent) - 1));
        const auto finite = masm.Here();
        __ cbnz(v::x17, std::int64_t{0});
        __ lsl(R(17, bits), R(16, bits), bits - fraction);
        const auto infinity = masm.Here();
        __ cbz(R(17, bits), std::int64_t{0});
        u32 quiet = UINT32_MAX;
        if (!signaling) {
            quiet = masm.Here();
            __ tbnz(R(16, bits), fraction - 1, std::int64_t{0});
        }
        __ mrs(v::x17, v::FPSR);
        __ orr(v::x17, v::x17, Operand(u64{1}));
        __ msr(v::FPSR, v::x17);
        masm.Patch(finite, masm.Here());
        masm.Patch(infinity, masm.Here());
        if (quiet != UINT32_MAX) masm.Patch(quiet, masm.Here());
    }
}

}  // namespace swift::runtime::backend::arm64::detail

#undef __
