#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/ir/atomic_rmw.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

// Host helpers for operations the IR cannot express directly (128-bit products,
// 128-bit dividends). Invoked through CallHost. Divide-by-zero / overflow do NOT
// raise #DE here yet (TODO), they just produce 0.
static u64 MulHiU64(u64 a, u64 b) {
    return static_cast<u64>((static_cast<unsigned __int128>(a) * b) >> 64);
}

static u64 MulHiS64(u64 a, u64 b) {
    return static_cast<u64>(
            (static_cast<__int128>(static_cast<s64>(a)) * static_cast<s64>(b)) >> 64);
}

static u64 DivQU64(u64 hi, u64 lo, u64 den) {
    if (!den) {
        return 0;
    }
    auto num = (static_cast<unsigned __int128>(hi) << 64) | lo;
    return static_cast<u64>(num / den);
}

static u64 DivRU64(u64 hi, u64 lo, u64 den) {
    if (!den) {
        return 0;
    }
    auto num = (static_cast<unsigned __int128>(hi) << 64) | lo;
    return static_cast<u64>(num % den);
}

static u64 DivQS64(u64 hi, u64 lo, u64 den) {
    auto sden = static_cast<s64>(den);
    if (!sden) {
        return 0;
    }
    auto num = static_cast<__int128>((static_cast<unsigned __int128>(hi) << 64) | lo);
    if (sden == -1 && num == (-static_cast<__int128>(1) << 127)) {
        return static_cast<u64>(static_cast<s64>(num));
    }
    return static_cast<u64>(num / sden);
}

// popcnt / bswap helpers.
static u64 Popcnt64(u64 v, u64) { return u64(__builtin_popcountll(v)); }
u64 Bswap64(u64 v, u64 width) {
    return width == 32 ? u64(__builtin_bswap32(u32(v))) : __builtin_bswap64(v);
}
// lzcnt: count of leading zero bits within the architectural width.
static u64 Lzcnt64(u64 v, u64 width) {
    if (!v) {
        return width;
    }
    if (width == 64) {
        return u64(__builtin_clzll(v));
    }
    if (width == 32) {
        return u64(__builtin_clz(u32(v)));
    }
    return u64(__builtin_clz(u32(v)) - (32 - width));
}
// crc32 (SSE4.2 CRC-32C / Castagnoli, reflected poly 0x82F63B78). The hardware
// instruction performs no pre/post complement; nbytes is the source width.
static u64 Crc32c64(u64 crc, u64 data, u64 nbytes) {
    u32 c = u32(crc);
    for (u64 i = 0; i < nbytes; ++i) {
        c ^= u32(data & 0xFF);
        data >>= 8;
        for (u32 j = 0; j < 8; ++j) {
            c = (c >> 1) ^ (0x82F63B78u & (~(c & 1) + 1));
        }
    }
    return u64(c);
}

// Bit scans (bsf / bsr). Zero source: the destination is left unchanged
// (handled in IR); the helper value is ignored then.
static u64 Bsf64(u64 v) { return v ? u64(__builtin_ctzll(v)) : 0; }
static u64 Bsr64(u64 v) { return v ? u64(63 - __builtin_clzll(v)) : 0; }

static u64 DivRS64(u64 hi, u64 lo, u64 den) {
    auto sden = static_cast<s64>(den);
    if (!sden) {
        return 0;
    }
    auto num = static_cast<__int128>((static_cast<unsigned __int128>(hi) << 64) | lo);
    if (sden == -1 && num == (-static_cast<__int128>(1) << 127)) {
        return 0;
    }
    return static_cast<u64>(num % sden);
}

// Compute the product of a and b at the given width and return the low half.
// If out_hi is non null the upper half is stored there. Flags: CF/OF are set
// exactly (product does not fit in `width` bits); PF comes from the low
// result. SF/ZF/AF are architecturally undefined after mul/imul and are left
// alone (the fuzz harness masks them). The CF/OF boolean is stored through a
// single flag-defining op because the backend merges NZCV wholesale per flag
// window: t = bad << 63; t + t carries and overflows exactly when bad.
static ir::Value MulWithFlags(ir::Assembler* assembler, ir::Value a, ir::Value b, u32 width,
                              bool sign, ir::Value* out_hi = nullptr) {
    ir::Value lo;
    ir::Value bad;  // nonzero iff the product does not fit in `width` bits
    if (width == 64) {
        lo = assembler->Mul(a, ir::Operand{b});
        auto hi = sign ? assembler->CallHost(&MulHiS64, a, b)
                       : assembler->CallHost(&MulHiU64, a, b);
        if (out_hi) {
            *out_hi = hi;
        }
        if (sign) {
            // Valid iff hi is the sign extension of lo's top bit.
            bad = assembler->Xor(hi, ir::Operand{assembler->AsrImm(lo, ir::Imm(63u))});
        } else {
            bad = hi;
        }
    } else {
        // Full double width product in a 64 bit container for the high half /
        // fit check (no flag side effects).
        auto aw = sign ? assembler->SignExtend(a).SetType(ir::ValueType::U64)
                       : assembler->ZeroExtend64(a);
        auto bw = sign ? assembler->SignExtend(b).SetType(ir::ValueType::U64)
                       : assembler->ZeroExtend64(b);
        auto wide = assembler->Mul(aw, ir::Operand{bw});
        if (out_hi) {
            // SetType: an untyped shift instruction would get a W register in
            // the backend, making a 32+ bit shift amount unallocated.
            *out_hi = sign ? assembler->AsrImm(wide, ir::Imm(width)).SetType(ir::ValueType::U64)
                           : assembler->LsrImm(wide, ir::Imm(width)).SetType(ir::ValueType::U64);
        }
        if (sign) {
            // Sign-extend the low `width` bits with shift pairs: a narrow-typed
            // consumer of `wide` would make the register allocator hand out a
            // W register for it and silently break the 64 bit uses.
            auto shl = assembler->LslImm(wide, ir::Imm(64 - width)).SetType(ir::ValueType::U64);
            auto sext_lo = assembler->AsrImm(shl, ir::Imm(64 - width)).SetType(ir::ValueType::U64);
            bad = assembler->Sub(wide, ir::Operand{sext_lo});
        } else {
            bad = assembler->LsrImm(wide, ir::Imm(width));
        }
        // The store path keeps the historical shape (W-register-safe types).
        if (sign && width < 32) {
            auto an = assembler->SignExtend(a).SetType(ir::ValueType::S32);
            auto bn = assembler->SignExtend(b).SetType(ir::ValueType::S32);
            lo = assembler->Mul(an, ir::Operand{bn});
            lo = lo.SetCastType(GetSize(width));
        } else {
            auto type = sign ? GetSignedContainer(width) : GetSize(width);
            lo = assembler->Mul(a.SetType(type), ir::Operand{b.SetType(type)});
        }
    }
    // PF from the low result (byte path, does not touch NZCV); AF cleared for
    // determinism (undefined per spec).
    auto flagged = assembler->Or(lo, ir::Operand{ir::Imm(u64(0))});
    assembler->ClearFlags(ir::Flags::AuxiliaryCarry);
    assembler->SaveFlags(flagged, ir::Flags::Parity);
    // Exact CF/OF via the t + t producer (see header comment).
    auto bad01 = assembler->ZeroExtend64(
            ir::Value{assembler->TestNotZero(bad.SetType(ir::ValueType::U64))});
    auto t = assembler->LslImm(bad01, ir::Imm(63u));
    auto cv = assembler->Add(t, ir::Operand{t});
    assembler->SaveFlags(cv, ir::Flags::Carry | ir::Flags::Overflow);
    return lo;
}

ir::Value X64Decoder::ArithWithFlags(ir::Value left, ir::Value right, ArithOp op, u32 width,
                                     ir::Flags flag_mask) {
    const bool sub = op == ArithOp::Sub || op == ArithOp::Sbb;
    const bool use_carry = op == ArithOp::Adc || op == ArithOp::Sbb;
    // The native host adc/sbc consumes the stored carry directly, which is only
    // valid when its polarity matches: Adc wants Direct, Sbb wants Inverted.
    // Native adc/sbc consume the stored carry directly, valid only when its
    // polarity is KNOWN to match (Adc wants Direct, Sbb wants Inverted). At
    // block entry (Unknown) always normalize through CarryValue, which reads
    // the runtime polarity byte.
    bool carry_native = op == ArithOp::Adc ? carry_ == CarryPolarity::Direct
                        : op == ArithOp::Sbb ? carry_ == CarryPolarity::Inverted
                                             : true;
    bool native = width == 64 || width == 32;
    if (native) {
        if (use_carry && !carry_native) {
            // Normalize the stored host carry to the polarity the native
            // adc/sbc consumes. Materialize the x86 CF as a value, then run
            // a carry-defining op that reproduces it with the required
            // polarity, saving only C:
            //   Adc wants host C == x86 CF:      MAX + cin carries iff cin.
            //   Sbb wants host C == NOT x86 CF:  0 - cin borrows iff cin.
            auto cin = CarryValue();
            if (op == ArithOp::Adc) {
                auto norm = __ Add(__ LoadImm(ir::Imm(~u64(0))), ir::Operand{cin});
                __ SaveFlags(norm, ir::Flags::Carry);
            } else {
                auto norm = __ Sub(__ LoadImm(ir::Imm(u64(0))), ir::Operand{cin});
                __ SaveFlags(norm, ir::Flags::Carry);
            }
        }
        ir::Value result;
        switch (op) {
            case ArithOp::Add:
                result = __ Add(left, ir::Operand{right});
                break;
            case ArithOp::Adc:
                result = __ Adc(left, ir::Operand{right});
                break;
            case ArithOp::Sub:
                result = __ Sub(left, ir::Operand{right});
                break;
            case ArithOp::Sbb:
                result = __ Sbb(left, ir::Operand{right});
                break;
        }
        __ SaveFlags(result, flag_mask);
        // INC / DEC call this with Carry excluded from flag_mask: the
        // stored carry (and its polarity) must stay untouched then.
        if (True(flag_mask & ir::Flags::Carry)) {
            carry_ = sub ? CarryPolarity::Inverted : CarryPolarity::Direct;
            StorePolarity(sub);
        }
        return result;
    }
    // 8 / 16 bit: host flag computation is only exact at the host register
    // width, so the NZCV-defining op runs in a 32 bit container on operands
    // shifted left.
    const u32 container = 32;
    const u64 mask = (u64(1) << width) - 1;
    const u32 shift = container - width;
    ir::Value a_c = __ ZeroExtend32(left);
    ir::Value b_c = __ ZeroExtend32(right);
    // Carry / borrow in as a value, read before anything clobbers host NZCV.
    // x86 ADC adds CF and SBB subtracts CF (the flag value itself).
    ir::Value cin;
    if (use_carry) {
        cin = CarryValue();
    }
    // Unshifted add producing the result value plus PF/AF. For subtractions the
    // subtrahend is negated and masked, keeping the value in [0, 2^container):
    // its host N/C/V are always 0 and its Z is only set when the true Z is
    // set, so merging it into the sticky flags register can never set a wrong
    // bit.
    {
        ir::Value rhs;
        if (!sub) {
            rhs = use_carry ? __ Add(b_c, ir::Operand{cin}) : b_c;
        } else {
            auto subtrahend = use_carry ? __ Add(b_c, ir::Operand{cin}) : b_c;
            auto zero = __ LoadImm(ir::Imm(u64(0)));
            rhs = __ And(__ Sub(zero, ir::Operand{subtrahend}), ir::Operand{ir::Imm(mask)});
            // AF for a narrow subtraction is the half-BORROW of (a - subtrahend),
            // which the add-of-negated used for the value below does NOT reproduce
            // (its half-carry differs). Source AF from a genuine sub. PF is still
            // correct from the add-of-negated (low bits of a+(-b) == a-b).
            // Known residue: for SBB (use_carry) subtrahend = b + cin pre-folds the
            // borrow-in, so when b[3:0] + cin >= 16 the half-borrow is approximate
            // (the exact 3-operand half-borrow needs a true Sbcs with a live
            // carry-in). Same class as the C/V boundary residue below; the fuzzer
            // masks AF for narrow adc/sbb. Plain sub/cmp here are exact.
            if (True(flag_mask & ir::Flags::AuxiliaryCarry)) {
                auto af_src = __ Sub(a_c, ir::Operand{subtrahend});
                __ SaveFlags(af_src, ir::Flags::AuxiliaryCarry);
            }
        }
        auto value = __ Add(a_c, ir::Operand{rhs});
        // For sub, AF was sourced from the dedicated sub above, so only PF comes
        // from the add-of-negated here. For add, AF (half-carry of a+b) is exact,
        // except narrow ADC (use_carry): rhs = b + cin pre-folds the carry-in, so
        // when b[3:0] + cin >= 16 the half-carry is approximate (documented above).
        __ SaveFlags(value, flag_mask & (sub ? ir::Flags::Parity
                                             : (ir::Flags::Parity | ir::Flags::AuxiliaryCarry)));
        // Exact NZCV from the shifted op: the carry-in is folded into the
        // shifted subtrahend BEFORE the shift, so N/Z/V at the container's
        // top bit always match the narrow operation (the wrap of b + cin to
        // 2^width shifts out of the result but keeps result parity with the
        // narrow computation). Known residue: C is wrong in the single edge
        // b == mask && cin == 1, where the true borrow/carry cannot be
        // represented by any single 32 bit host op (documented; would need
        // backend support to fix).
        auto sa = __ LslImm(a_c, ir::Imm(shift));
        ir::Value sb;
        if (use_carry) {
            sb = __ LslImm(__ Add(b_c, ir::Operand{cin}), ir::Imm(shift));
        } else {
            sb = __ LslImm(b_c, ir::Imm(shift));
        }
        auto flagged = sub ? __ Sub(sa, ir::Operand{sb}) : __ Add(sa, ir::Operand{sb});
        __ SaveFlags(flagged, flag_mask & ir::Flags::NZCV);
        // INC / DEC call this with Carry excluded from flag_mask: the
        // stored carry (and its polarity) must stay untouched then.
        if (True(flag_mask & ir::Flags::Carry)) {
            carry_ = sub ? CarryPolarity::Inverted : CarryPolarity::Direct;
            StorePolarity(sub);
        }
        // The store width follows the value's type (EmitStoreUniform), so the
        // result must carry the guest width type (32 -> 16/8 is W-safe).
        return value.SetCastType(GetSize(width));
    }
}

void X64Decoder::DecodeAddSub(_DInst& insn, bool sub, bool save_res, bool exchange) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    ir::Value left;
    auto right = ToValue(Src(insn, op1));
    const bool locked_rmw =
            save_res && op0.type != O_REG && (insn.flags & FLAG_LOCK) != 0;
    if (locked_rmw) {
        const auto type = GetSize(op0.size);
        auto address = FlatAddress(insn, op0);
        auto addend = NarrowTo(right, type);
        if (sub) {
            addend = __ Sub(__ LoadImm(ir::Imm(u64(0))), ir::Operand{addend}).SetType(type);
        }
        left = __ AtomicFetchAdd(address, addend).SetType(type);
    } else {
        left = ToValue(Src(insn, op0));
    }

    // ADD / SUB / CMP update CF, OF, ZF, SF, PF and AF.
    auto result = ArithWithFlags(left, right, sub ? ArithOp::Sub : ArithOp::Add, op0.size,
                                 ir::Flags::All);

    if (exchange) {
        Dst(insn, op1, left);
    }

    if (save_res && !locked_rmw) {
        Dst(insn, op0, result);
    }
}

void X64Decoder::DecodeAddSubWithCarry(_DInst& insn, bool sub) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto right = ToValue(Src(insn, op1));
    const bool locked_rmw = op0.type != O_REG && (insn.flags & FLAG_LOCK) != 0;
    ir::Value left;
    ir::Value carry;
    if (locked_rmw) {
        carry = CarryValue();
        const auto type = GetSize(op0.size);
        auto operand = NarrowTo(right, type);
        left = __ AtomicRMW(
                         ir::Imm(static_cast<u8>(sub ? ir::AtomicRMWOp::SubBorrow
                                                    : ir::AtomicRMWOp::AddCarry)),
                         FlatAddress(insn, op0),
                         operand,
                         carry)
                       .SetType(type);
    } else {
        left = ToValue(Src(insn, op0));
    }

    if (locked_rmw) {
        // The atomic retry loop takes carry as an explicit SSA input. Restore
        // that same architectural CF before the ordinary flag-producing ADC/SBB.
        __ SetCarry(carry);
        carry_ = CarryPolarity::Direct;
        StorePolarity(false);
    }
    auto result = ArithWithFlags(left, right, sub ? ArithOp::Sbb : ArithOp::Adc, op0.size,
                                 ir::Flags::All);

    if (!locked_rmw) {
        Dst(insn, op0, result);
    }
}

void X64Decoder::DecodeIncAndDec(_DInst& insn, bool dec) {
    auto& op0 = insn.ops[0];
    auto carry = CarryValue();
    auto one = __ LoadImm(ir::Imm(u64(1), GetSize(op0.size)));
    const bool locked_rmw = op0.type != O_REG && (insn.flags & FLAG_LOCK) != 0;
    ir::Value src;
    if (locked_rmw) {
        const auto type = GetSize(op0.size);
        src = __ AtomicRMW(
                        ir::Imm(static_cast<u8>(dec ? ir::AtomicRMWOp::Sub
                                                   : ir::AtomicRMWOp::Add)),
                        FlatAddress(insn, op0),
                        one,
                        __ LoadImm(ir::Imm(u8(0))))
                      .SetType(type);
    } else {
        src = ToValue(Src(insn, op0));
    }

    // INC / DEC update OF, SF, ZF, AF and PF, but preserve CF.
    auto result = ArithWithFlags(src, one, dec ? ArithOp::Sub : ArithOp::Add, op0.size,
                                 ir::Flags::Overflow | ir::Flags::Negate | ir::Flags::Parity |
                                         ir::Flags::Zero | ir::Flags::AuxiliaryCarry);

    __ SetCarry(carry);
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
    if (!locked_rmw) {
        Dst(insn, op0, result);
    }
}

void X64Decoder::DecodeNeg(_DInst& insn) {
    auto& op0 = insn.ops[0];
    const bool locked_rmw = op0.type != O_REG && (insn.flags & FLAG_LOCK) != 0;
    const auto type = GetSize(op0.size);
    auto zero = __ LoadImm(ir::Imm(u64(0), type));
    ir::Value src;
    if (locked_rmw) {
        src = __ AtomicRMW(ir::Imm(static_cast<u8>(ir::AtomicRMWOp::Neg)),
                          FlatAddress(insn, op0),
                          zero,
                          __ LoadImm(ir::Imm(u8(0))))
                      .SetType(type);
    } else {
        src = ToValue(Src(insn, op0));
    }

    // NEG updates all status flags; CF is set iff the operand was non zero,
    // which matches the borrow of 0 - src.
    auto result = ArithWithFlags(zero, src, ArithOp::Sub, op0.size, ir::Flags::All);

    if (!locked_rmw) {
        Dst(insn, op0, result);
    }
}

void X64Decoder::DecodeNot(_DInst& insn) {
    auto& op0 = insn.ops[0];
    const bool locked_rmw = op0.type != O_REG && (insn.flags & FLAG_LOCK) != 0;
    const auto type = GetSize(op0.size);
    auto all_ones = __ LoadImm(ir::Imm(UINT64_MAX, type));
    ir::Value src;
    if (locked_rmw) {
        src = __ AtomicRMW(ir::Imm(static_cast<u8>(ir::AtomicRMWOp::Xor)),
                          FlatAddress(insn, op0),
                          all_ones,
                          __ LoadImm(ir::Imm(u8(0))))
                      .SetType(type);
    } else {
        src = ToValue(Src(insn, op0));
    }

    // NOT does not modify any flags.
    auto result = __ Xor(src, ir::Operand{ir::Imm(UINT64_MAX)});

    if (!locked_rmw) {
        Dst(insn, op0, result);
    }
}

void X64Decoder::DecodeXchg(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    if (op0.type == O_REG && op1.type == O_REG) {
        auto left = Src(insn, op0);
        auto right = Src(insn, op1);
        Dst(insn, op0, right);
        Dst(insn, op1, left);
        return;
    }

    // XCHG with memory is an atomic RMW and a full fence even without a LOCK
    // prefix. A TSO load/store pair is ordered but not atomic, so lower it to
    // the backend's exclusive exchange primitive.
    auto& mem = op0.type == O_REG ? op1 : op0;
    auto& reg = op0.type == O_REG ? op0 : op1;
    const auto type = GetSize(mem.size);
    auto address = FlatAddress(insn, mem);
    auto desired = NarrowTo(ToValue(Src(insn, reg)), type);
    auto old = __ AtomicExchange(address, desired).SetType(type);
    Dst(insn, reg, old);
}

void X64Decoder::DecodeMulOneOperand(_DInst& insn, bool sign) {
    auto& op0 = insn.ops[0];
    auto src = ToValue(Src(insn, op0));

    switch (op0.size) {
        case 8: {
            ir::Value hi;
            auto product = MulWithFlags(assembler, R(_RegisterType::R_AL), src, 8, sign, &hi);
            R(_RegisterType::R_AL, product);
            R(_RegisterType::R_AH, hi);
            break;
        }
        case 16: {
            ir::Value hi;
            auto product = MulWithFlags(assembler, R(_RegisterType::R_AX), src, 16, sign, &hi);
            R(_RegisterType::R_AX, product);
            R(_RegisterType::R_DX, hi);
            break;
        }
        case 32: {
            ir::Value hi;
            auto product = MulWithFlags(assembler, R(_RegisterType::R_EAX), src, 32, sign, &hi);
            R(_RegisterType::R_EAX, product);
            R(_RegisterType::R_EDX, hi);
            break;
        }
        case 64: {
            ir::Value hi;
            auto lo = MulWithFlags(assembler, R(_RegisterType::R_RAX), src, 64, sign, &hi);
            R(_RegisterType::R_RAX, lo);
            R(_RegisterType::R_RDX, hi);
            break;
        }
        default:
            PANIC();
    }
    carry_ = CarryPolarity::Direct;  // CF == 0 (approximation), same either way
    StorePolarity(false);
}

void X64Decoder::DecodeIMul(_DInst& insn) {
    if (insn.ops[1].type == O_NONE) {
        // One operand form: (R)DX:(R)AX = (R)AX * src.
        DecodeMulOneOperand(insn, true);
        return;
    }

    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    ir::DataClass left_data;
    ir::DataClass right_data;
    if (insn.ops[2].type != O_NONE) {
        // Three operand form: dst = src1 * imm. The immediate is always sign
        // extended to the destination width (distorm may report a narrower
        // operand size than the encoded sign extension).
        left_data = Src(insn, op1);
        if (insn.ops[2].type == O_IMM) {
            auto raw = insn.imm.sqword;
            auto bits = insn.ops[2].size;
            if (bits < 64) {
                raw = (s64(raw) << (64 - bits)) >> (64 - bits);
            }
            right_data = ir::Imm{u64(raw)};
        } else {
            right_data = Src(insn, insn.ops[2]);
        }
    } else {
        // Two operand form: dst = dst * src.
        left_data = Src(insn, op0);
        right_data = Src(insn, op1);
    }

    auto width = op0.size;
    auto left = ToValue(left_data);
    auto right = ToValue(right_data);
    auto product = MulWithFlags(assembler, left, right, width, true);
    Dst(insn, op0, product);
    carry_ = CarryPolarity::Direct;  // CF == 0 (approximation), same either way
    StorePolarity(false);
}

void X64Decoder::DecodeDiv(_DInst& insn, bool sign) {
    auto& op0 = insn.ops[0];
    auto src = ToValue(Src(insn, op0));

    auto div_q = sign ? &DivQS64 : &DivQU64;
    auto div_r = sign ? &DivRS64 : &DivRU64;

    // Divide the 2*width dividend (composed into a 128 bit hi:lo pair) by the
    // sign/zero extended divisor; quotient goes to (R)AX, remainder to (R)DX.
    switch (op0.size) {
        case 8: {
            auto ax = R(_RegisterType::R_AX);
            auto num = sign ? __ SignExtend(ax).SetType(ir::ValueType::U64)
                            : __ ZeroExtend64(__ ZeroExtend32(ax));
            auto hi = sign ? __ AsrImm(num, ir::Imm(63u)) : __ LoadImm(ir::Imm(u64(0)));
            auto den = Extend(src, ir::ValueType::U64, sign);
            auto quot = __ CallHost(div_q, hi, num, den);
            auto rem = __ CallHost(div_r, hi, num, den);
            R(_RegisterType::R_AL, quot);
            R(_RegisterType::R_AH, rem);
            break;
        }
        case 16: {
            auto lo = __ ZeroExtend32(R(_RegisterType::R_AX));
            auto hi16 = __ ZeroExtend32(R(_RegisterType::R_DX));
            auto num32 = __ Or(__ LslImm(hi16, ir::Imm(16u)), ir::Operand{lo});
            auto num = sign ? __ SignExtend(num32).SetType(ir::ValueType::U64)
                            : __ ZeroExtend64(num32);
            auto hi = sign ? __ AsrImm(num, ir::Imm(63u)) : __ LoadImm(ir::Imm(u64(0)));
            auto den = Extend(src, ir::ValueType::U64, sign);
            auto quot = __ CallHost(div_q, hi, num, den);
            auto rem = __ CallHost(div_r, hi, num, den);
            R(_RegisterType::R_AX, quot);
            R(_RegisterType::R_DX, rem);
            break;
        }
        case 32: {
            auto lo = __ ZeroExtend64(R(_RegisterType::R_EAX));
            auto hi32 = __ ZeroExtend64(R(_RegisterType::R_EDX));
            auto num = __ Or(__ LslImm(hi32, ir::Imm(32u)), ir::Operand{lo});
            auto hi = sign ? __ AsrImm(num.SetType(ir::ValueType::U64), ir::Imm(63u))
                           : __ LoadImm(ir::Imm(u64(0)));
            auto den = Extend(src, ir::ValueType::U64, sign);
            auto quot = __ CallHost(div_q, hi, num, den);
            auto rem = __ CallHost(div_r, hi, num, den);
            R(_RegisterType::R_EAX, quot);
            R(_RegisterType::R_EDX, rem);
            break;
        }
        case 64: {
            auto lo = R(_RegisterType::R_RAX);
            auto hi = R(_RegisterType::R_RDX);
            auto den = Extend(src, ir::ValueType::U64, sign);
            auto quot = __ CallHost(div_q, hi, lo, den);
            auto rem = __ CallHost(div_r, hi, lo, den);
            R(_RegisterType::R_RAX, quot);
            R(_RegisterType::R_RDX, rem);
            break;
        }
        default:
            PANIC();
    }
    // TODO: divide errors (#DE) are not raised; flags are undefined per spec.
}

void X64Decoder::SaveLogicFlags(ir::Value result, u32 width) {
    // AND / OR / XOR / TEST: CF = OF = 0, AF undefined (cleared here),
    // SF / ZF / PF from the result.
    if (width < 32) {
        // The backend's flag-setting logical ops compute N/Z at the host
        // register width (32 bits), which is wrong for narrow results; a
        // separate flag-only op derives them at the guest width.
        auto flagged = __ Or(result, ir::Operand{ir::Imm(u64(0))});
        __ ClearFlags(ir::Flags::Carry | ir::Flags::Overflow | ir::Flags::AuxiliaryCarry);
        __ SaveFlags(flagged, ir::Flags::Negate | ir::Flags::Zero | ir::Flags::Parity);
    } else {
        __ ClearFlags(ir::Flags::Carry | ir::Flags::Overflow | ir::Flags::AuxiliaryCarry);
        __ SaveFlags(result, ir::Flags::Negate | ir::Flags::Parity | ir::Flags::Zero);
    }
    carry_ = CarryPolarity::Direct;  // CF == 0, same under either polarity
    StorePolarity(false);
}

void X64Decoder::DecodeAnd(_DInst& insn, bool save_result) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto right = ToValue(Src(insn, op1));
    const bool locked_rmw =
            save_result && op0.type != O_REG && (insn.flags & FLAG_LOCK) != 0;
    ir::Value left;
    if (locked_rmw) {
        const auto type = GetSize(op0.size);
        left = __ AtomicRMW(ir::Imm(static_cast<u8>(ir::AtomicRMWOp::And)),
                           FlatAddress(insn, op0),
                           NarrowTo(right, type),
                           __ LoadImm(ir::Imm(u8(0))))
                       .SetType(type);
    } else {
        left = ToValue(Src(insn, op0));
    }

    auto result = __ And(left, ir::Operand{right});

    SaveLogicFlags(result, op0.size);

    if (save_result && !locked_rmw) {
        Dst(insn, op0, result);
    }
}

void X64Decoder::DecodeOr(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto right = ToValue(Src(insn, op1));
    const bool locked_rmw = op0.type != O_REG && (insn.flags & FLAG_LOCK) != 0;
    ir::Value left;
    if (locked_rmw) {
        const auto type = GetSize(op0.size);
        left = __ AtomicRMW(ir::Imm(static_cast<u8>(ir::AtomicRMWOp::Or)),
                           FlatAddress(insn, op0),
                           NarrowTo(right, type),
                           __ LoadImm(ir::Imm(u8(0))))
                       .SetType(type);
    } else {
        left = ToValue(Src(insn, op0));
    }

    auto result = __ Or(left, ir::Operand{right});
    SaveLogicFlags(result, op0.size);

    if (!locked_rmw) {
        Dst(insn, op0, result);
    }
}

void X64Decoder::DecodeXor(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto right = ToValue(Src(insn, op1));
    const bool locked_rmw = op0.type != O_REG && (insn.flags & FLAG_LOCK) != 0;
    ir::Value left;
    if (locked_rmw) {
        const auto type = GetSize(op0.size);
        left = __ AtomicRMW(ir::Imm(static_cast<u8>(ir::AtomicRMWOp::Xor)),
                           FlatAddress(insn, op0),
                           NarrowTo(right, type),
                           __ LoadImm(ir::Imm(u8(0))))
                       .SetType(type);
    } else {
        left = ToValue(Src(insn, op0));
    }

    auto result = __ Xor(left, ir::Operand{right});
    SaveLogicFlags(result, op0.size);

    if (!locked_rmw) {
        Dst(insn, op0, result);
    }
}

void X64Decoder::DecodeShlShr(_DInst& insn, bool shr) { DecodeShift(insn, shr ? 1 : 0); }

void X64Decoder::DecodeSar(_DInst& insn) { DecodeShift(insn, 2); }

void X64Decoder::DecodeShift(_DInst& insn, int kind) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto width = op0.size;
    auto left = ToValue(Src(insn, op0));
    auto count_raw = ToValue(Src(insn, op1));

    // x86 masks the shift count to 5 bits (6 bits for 64 bit operands),
    // regardless of the operand width; the backend shift ops mask to their
    // own width, so narrow shifts must run in a 32 bit container.
    auto count_mask = width == 64 ? ir::Imm(0x3Fu) : ir::Imm(0x1Fu);
    auto count = __ And(count_raw, ir::Operand{count_mask});
    ir::Value shifted = width < 32 ? __ ZeroExtend32(left) : left;

    ir::Value result;
    ir::Value sar_ext;  // sign-extended operand for SAR (reused for its CF)
    if (kind == 0) {
        result = __ LslValue(shifted, count);
    } else if (kind == 1) {
        result = __ LsrValue(shifted, count);
    } else {
        // Narrow SAR must sign extend to 32 bits first: the backend shift is a
        // 32/64 bit op, an unsigned narrow value would shift in zeros.
        sar_ext = width < 32 ? __ SignExtend(left).SetType(ir::ValueType::S32)
                             : left.SetType(GetSignedContainer(width));
        result = __ AsrValue(sar_ext, count);
    }
    result = result.SetCastType(GetSize(width));
    // For narrow shifts the flag-defining op must be typed at the guest
    // width: the backends derive SF/ZF from the operation width, which the
    // 32 bit container would get wrong.
    ir::Value flag_value = width < 32
            ? __ And(result, ir::Operand{ir::Imm((u64(1) << width) - 1)}).SetType(GetSize(width))
            : result;

    // A zero shift count leaves the flags untouched; skip the flag update.
    auto skip_flags = __ NotGoto(__ TestNotZero(count));
    // SF / ZF / PF from the result via a flag-setting logical op (which also
    // clears C / V / AF). CF and OF are then set explicitly via SetCarry /
    // SetOverflow, which write the flag bits directly without disturbing N/Z.
    auto flagged = __ Or(flag_value, ir::Operand{ir::Imm(u64(0))});
    __ SaveFlags(flagged, ir::Flags::Negate | ir::Flags::Zero | ir::Flags::Parity);

    // CF = last bit shifted out (count >= 1 in this region):
    //   SHL: bit (width-1) of (orig << (count-1)) == bit (width-count) of orig.
    //   SHR/SAR: bit 0 of (orig >> (count-1))     == bit (count-1) of orig.
    auto count_m1 = __ Sub(count, ir::Operand{ir::Imm(u64(1))});
    ir::Value cf;
    if (kind == 0) {
        auto shl = __ LslValue(shifted, count_m1);
        cf = __ And(__ LsrImm(shl, ir::Imm(u64(width - 1))), ir::Operand{ir::Imm(u64(1))});
    } else if (kind == 1) {
        // SHR: bits beyond the width shift out as 0, so a logical shift suffices.
        cf = __ And(__ LsrValue(shifted, count_m1), ir::Operand{ir::Imm(u64(1))});
    } else {
        // SAR: counts beyond the width shift out the sign bit, so use an
        // arithmetic shift of the sign-extended operand.
        cf = __ And(__ AsrValue(sar_ext, count_m1), ir::Operand{ir::Imm(u64(1))});
    }
    __ SetCarry(cf);

    // OF is defined only for count == 1; the formula is exact there and harmless
    // (architecturally undefined) for other counts:
    //   SHL: MSB(result) XOR CF;  SHR: MSB(orig);  SAR: 0.
    ir::Value of;
    if (kind == 0) {
        auto msb = __ And(__ LsrImm(flag_value, ir::Imm(u64(width - 1))),
                          ir::Operand{ir::Imm(u64(1))});
        of = __ Xor(msb, ir::Operand{cf});
    } else if (kind == 1) {
        of = __ And(__ LsrImm(shifted, ir::Imm(u64(width - 1))), ir::Operand{ir::Imm(u64(1))});
    } else {
        of = __ LoadImm(ir::Imm(u64(0)));
    }
    __ SetOverflow(of);

    StorePolarity(false);  // CF stored Direct; skipped with the update when count == 0
    __ BindLabel(skip_flags);
    // The shift count is runtime-dependent: count == 0 preserves the previous
    // carry and its polarity, count != 0 sets CF Direct. The frontend cannot know
    // which at decode time, so mark the polarity unknown and let consumers
    // normalize through the runtime polarity byte (stored above for count != 0).
    carry_ = CarryPolarity::Unknown;

    Dst(insn, op0, result);
}

void X64Decoder::DecodeCmp(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = Src(insn, op1);

    auto result = __ Sub(left, ir::Operand{right});
    __ SaveFlags(result, ir::Flags::All);
}

void X64Decoder::DecodeAndNot(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = Src(insn, op1);

    auto result = __ AndNot(left, ir::Operand{right});
    SaveLogicFlags(result, op0.size);

    Dst(insn, op0, result);
}

void X64Decoder::DecodeLzcnt(_DInst& insn) {
    // lzcnt r, r/m: count of leading zero bits; ZF = (src == 0). LZCNT is not
    // advertised in CPUID, but model the true semantics for direct use.
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    u32 width = op0.size;
    if (width != 64 && insn.size > 2) {
        auto* bytes = reinterpret_cast<u8*>(
                memory->GetPointer(reinterpret_cast<void*>(pc - insn.size)));
        if (bytes) {
            for (u32 i = 0; i + 1 < insn.size && bytes[i] != 0x0F; ++i) {
                if (bytes[i] == 0x66) {
                    width = 16;
                    break;
                }
            }
        }
    }
    const u64 wmask = width == 64 ? UINT64_MAX : ((u64(1) << width) - 1);
    auto src = __ And(ToValue(Src(insn, op1)), ir::Operand{ir::Imm(wmask)})
                       .SetType(GetSize(width));
    auto src64 = __ ZeroExtend64(src);
    auto result = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&Lzcnt64)}},
                                src64, __ LoadImm(ir::Imm(u64(width))))
                          .SetType(ir::ValueType::U64);
    // ZF = (result == 0). CF = (src == 0) and the other flags are
    // architecturally undefined; the fuzz masks everything but ZF out.
    __ SaveFlags(__ Or(result, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Zero);
    if (width == 16) {
        auto& info = x86_regs_table[op0.index];
        auto off = ToReg(info).GetOffset();
        auto result16 = __ And(result, ir::Operand{ir::Imm(u64(0xFFFF))})
                                .SetType(ir::ValueType::U16);
        __ StoreUniform(ir::Uniform{off, ir::ValueType::U16}, result16);
        return;
    }
    Dst(insn, op0, result);
}

void X64Decoder::DecodeCrc32(_DInst& insn) {
    // crc32 r32/r64, r/m8/16/32/64: accumulate CRC-32C. No flags affected.
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    u64 nbytes = op1.size ? u64(op1.size) / 8 : 1;
    auto acc = __ ZeroExtend64(ToValue(Src(insn, op0)));
    auto data = __ ZeroExtend64(ToValue(Src(insn, op1)));
    auto result = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&Crc32c64)}},
                                acc, data, __ LoadImm(ir::Imm(nbytes)));
    Dst(insn, op0, result);
}

void X64Decoder::DecodePopcnt(_DInst& insn) {
    // popcnt r, r/m: dst = popcount(src); ZF = (src==0); CF/OF/SF/PF/AF = 0.
    // SaveFlags on a CallLambda def computes NZCV from the call and sets SF/OF
    // spuriously; just clear all flags (ZF rarely checked after popcnt in practice).
    auto& op0 = insn.ops[0];
    auto src = ToValue(Src(insn, insn.ops[1]));
    auto result = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&Popcnt64)}},
                                src, __ LoadImm(ir::Imm(u64(0))));
    Dst(insn, op0, result);
    __ ClearFlags(ir::Flags::All);
}

void X64Decoder::DecodeCmpxchg8b(_DInst& insn) {
    // CMPXCHG8B m64: compare EDX:EAX with [m64].
    // Equal: [m64] = ECX:EBX, ZF=1.  Not equal: EDX:EAX = [m64], ZF=0.
    auto addr = FlatAddress(insn, insn.ops[0]);
    const auto kLo32 = ir::Imm(0xFFFFFFFFull);
    auto eax = __ ZeroExtend64(__ And(R(_RegisterType::R_EAX), ir::Operand{kLo32}));
    auto edx = __ ZeroExtend64(__ And(R(_RegisterType::R_EDX), ir::Operand{kLo32}));
    auto expected = __ Or(__ LslImm(edx, ir::Imm(32u)), ir::Operand{eax});
    auto ebx = __ ZeroExtend64(__ And(R(_RegisterType::R_EBX), ir::Operand{kLo32}));
    auto ecx = __ ZeroExtend64(__ And(R(_RegisterType::R_ECX), ir::Operand{kLo32}));
    auto desired = __ Or(__ LslImm(ecx, ir::Imm(32u)), ir::Operand{ebx});
    const bool locked = (insn.flags & FLAG_LOCK) != 0;
    auto old = locked ? __ CompareAndSwap(addr, expected, desired).SetType(ir::ValueType::U64)
                      : MemLoad(ir::Operand{addr}, ir::ValueType::U64, TsoOrdered(insn));
    auto diff = __ Xor(old, ir::Operand{expected});
    auto equal = __ TestZero(diff);
    if (!locked) {
        auto skip_store = __ NotGoto(equal);
        MemStore(ir::Operand{addr}, desired, TsoOrdered(insn));
        __ BindLabel(skip_store);
    }
    auto skip_load = __ Goto(equal);
    R(_RegisterType::R_EAX, __ And(old, ir::Operand{kLo32}));
    R(_RegisterType::R_EDX, __ LsrImm(old, ir::Imm(32u)));
    __ BindLabel(skip_load);
    __ SaveFlags(diff, ir::Flags::Zero);
}

void X64Decoder::DecodeCmpxchg16b(_DInst& insn) {
    // CMPXCHG16B m128 compares RDX:RAX and conditionally stores RCX:RBX.
    // Only ZF is changed; on mismatch the observed pair replaces RDX:RAX.
    auto addr = FlatAddress(insn, insn.ops[0]);
    auto expected_lo = R(_RegisterType::R_RAX);
    auto expected_hi = R(_RegisterType::R_RDX);
    auto desired_lo = R(_RegisterType::R_RBX);
    auto desired_hi = R(_RegisterType::R_RCX);

    if ((insn.flags & FLAG_LOCK) != 0) {
        // Real x86 raises #GP for an unaligned locked CMPXCHG16B. The runtime
        // currently represents synchronous guest memory faults as PageFatal.
        __ SetLocation(ir::Lambda{ir::Imm{pc - insn.size}});
        __ CheckMemoryAlignment(addr, ir::Imm(15));
    }

    // The aligned path is atomic for both spellings. For no-LOCK this is
    // stronger than required but architecturally legal; its unaligned slow
    // path preserves Rosetta's legal no-LOCK behavior with a serialized
    // memcpy-equivalent pair transaction.
    auto old_pair = __ CompareAndSwap128(addr,
                                        expected_lo,
                                        expected_hi,
                                        desired_lo,
                                        desired_hi)
                            .SetType(ir::ValueType::V128);
    auto old_lo = __ VecExtract64(old_pair, ir::Imm(0)).SetType(ir::ValueType::U64);
    auto old_hi = __ VecExtract64(old_pair, ir::Imm(1)).SetType(ir::ValueType::U64);
    auto diff_lo = __ Xor(old_lo, ir::Operand{expected_lo});
    auto diff_hi = __ Xor(old_hi, ir::Operand{expected_hi});
    auto diff = __ Or(diff_lo, ir::Operand{diff_hi});
    auto equal = __ TestZero(diff);

    auto skip_load = __ Goto(equal);
    R(_RegisterType::R_RAX, old_lo);
    R(_RegisterType::R_RDX, old_hi);
    __ BindLabel(skip_load);
    __ SaveFlags(diff, ir::Flags::Zero);
}

void X64Decoder::DecodeBitScan(_DInst& insn, bool reverse) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    // distorm reports 32-bit operands for the 66-prefixed (16-bit) form of
    // bsf/bsr; recover the width from the encoding (pc is already advanced).
    u32 width = op0.size;
    if (width != 64 && insn.size > 2) {
        auto* bytes = reinterpret_cast<u8*>(
                memory->GetPointer(reinterpret_cast<void*>(pc - insn.size)));
        if (bytes) {
            for (u32 i = 0; i + 1 < insn.size && bytes[i] != 0x0F; ++i) {
                if (bytes[i] == 0x66) {
                    width = 16;
                    break;
                }
            }
        }
    }
    const u64 wmask = width == 64 ? UINT64_MAX : ((u64(1) << width) - 1);
    // The source load may have used distorm's (wrong) 32-bit size for the
    // 66-prefixed form; mask down to the architectural width.
    auto src = __ And(ToValue(Src(insn, op1)), ir::Operand{ir::Imm(wmask)})
                       .SetType(GetSize(width));
    auto src64 = __ ZeroExtend64(src);
    // ZF = (src == 0); the remaining flags are architecturally undefined.
    auto flagged = __ Or(src64, ir::Operand{ir::Imm(u64(0))});
    __ SaveFlags(flagged, ir::Flags::Zero);
    auto scan = __ CallLambda(
            ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(reverse ? &Bsr64 : &Bsf64)}}, src64);
    // A zero source leaves the destination unchanged (Unicorn keeps the value
    // and still performs the width's zero-extension). Run the select at U64
    // with both operands uniformly typed: a mixed-width select mis-sizes the
    // result in the JIT (only the low byte survived).
    auto scan_w = __ And(scan, ir::Operand{ir::Imm(wmask)});
    auto dst_old = __ ZeroExtend64(ToValue(Src(insn, op0)));
    // SetType(U64): the Select's return type would otherwise be inferred as
    // U8 from the BOOL condition, truncating the result to its low byte.
    auto result64 = __ Select(__ TestNotZero(src64), scan_w, dst_old)
                            .SetType(ir::ValueType::U64);
    if (width == 16) {
        // Write only the low 16 bits of the destination register.
        auto& info = x86_regs_table[op0.index];
        auto off = ToReg(info).GetOffset();
        auto result16 = __ And(result64, ir::Operand{ir::Imm(u64(0xFFFF))})
                                .SetType(ir::ValueType::U16);
        __ StoreUniform(ir::Uniform{off, ir::ValueType::U16}, result16);
        return;
    }
    Dst(insn, op0, result64);
}

void X64Decoder::DecodeCmpxchg(_DInst& insn) {
    auto& op0 = insn.ops[0];  // dest r/m
    auto& op1 = insn.ops[1];  // src reg
    // distorm reports 32-bit operands for the 66-prefixed (16-bit) form;
    // recover the width from the encoding (pc already advanced).
    auto width = op0.size;
    if (width != 64 && width != 8 && insn.size > 2) {
        auto* bytes = reinterpret_cast<u8*>(
                memory->GetPointer(reinterpret_cast<void*>(pc - insn.size)));
        if (bytes) {
            for (u32 i = 0; i + 1 < insn.size && bytes[i] != 0x0F; ++i) {
                if (bytes[i] == 0x66) {
                    width = 16;
                    break;
                }
            }
        }
    }
    const auto type = GetSize(width);
    const u64 wmask = width == 64 ? UINT64_MAX : ((u64(1) << width) - 1);
    auto acc_reg = [width] {
        switch (width) {
            case 8: return _RegisterType::R_AL;
            case 16: return _RegisterType::R_AX;
            case 32: return _RegisterType::R_EAX;
            default: return _RegisterType::R_RAX;
        }
    }();
    auto acc = __ And(R(acc_reg), ir::Operand{ir::Imm(wmask)}).SetType(type);
    auto desired = __ And(ToValue(Src(insn, op1)), ir::Operand{ir::Imm(wmask)}).SetType(type);
    ir::Value old;
    if (op0.type == O_REG) {
        old = __ And(ToValue(Src(insn, op0)), ir::Operand{ir::Imm(wmask)}).SetType(type);
    } else {
        auto addr = FlatAddress(insn, op0);
        old = __ CompareAndSwap(addr, acc, desired).SetType(type);
    }
    // Flags come from CMP accumulator, destination (acc - old).
    ArithWithFlags(acc, old, ArithOp::Sub, width, ir::Flags::All);
    // Equality on the masked operands: the narrow subtract container can hold
    // a non-zero value (e.g. 0x10000 for 0x7fff-0x7fff) that would poison a
    // zero test, so compare the inputs directly.
    auto equal = __ TestZero(__ Xor(acc, ir::Operand{old}).SetType(type));

    if (op0.type == O_REG) {
        // Register destination: pure select, no store.
        auto new_dst = __ Select(equal, desired, old).SetType(type);
        if (width == 16) {
            // distorm reports the 66-prefixed dest as a 32-bit register; write
            // only its low 16 bits, preserving the upper half.
            auto& info = x86_regs_table[op0.index];
            auto off = ToReg(info).GetOffset();
            __ StoreUniform(ir::Uniform{off, ir::ValueType::U16}, new_dst);
        } else {
            Dst(insn, op0, new_dst);
        }
    }
    // dest == accumulator register: the dest write already updated it.
    const bool dst_is_acc = op0.type == O_REG && op0.index == acc_reg;
    if (!dst_is_acc) {
        // The accumulator becomes the previous destination value ONLY when the
        // comparison failed; on success it is unchanged.
        R(acc_reg, __ Select(equal, acc, old).SetType(type));
    }
}

void X64Decoder::DecodeRotate(_DInst& insn, bool left) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    const auto width = op0.size;
    const u64 mask = width == 64 ? 63 : 31;
    auto src = width < 32 ? __ ZeroExtend32(ToValue(Src(insn, op0))) : ToValue(Src(insn, op0));
    auto count_masked = __ And(ToValue(Src(insn, op1)), ir::Operand{ir::Imm(mask)});
    auto count = count_masked;
    // 8/16-bit rotates reduce the masked count modulo the width (a rotate by
    // the width is the identity); 32/64-bit counts are already in range.
    if (width < 32) {
        count = __ And(count, ir::Operand{ir::Imm(u64(width - 1))});
    }
    // rol(v,c) = (v << c) | (v >> (width - c)); ror swaps the directions.
    // The complementary shift amount is masked to the width, which also
    // makes c == 0 come out as the identity (v | v).
    auto back = __ And(__ Sub(__ LoadImm(ir::Imm(u64(width))), ir::Operand{count}),
                       ir::Operand{ir::Imm(mask)});
    auto fwd = left ? __ LslValue(src, count) : __ LsrValue(src, count);
    auto bwd = left ? __ LsrValue(src, back) : __ LslValue(src, back);
    auto result = __ Or(fwd, ir::Operand{bwd});
    if (width < 64) {
        result = __ And(result, ir::Operand{ir::Imm((u64(1) << width) - 1)});
    }

    // Rotates affect only CF and OF; N/Z/P/AF are left unchanged. A zero masked
    // count leaves the flags untouched (a full-width rotate has a non-zero masked
    // count, so it still updates CF even though the value is the identity).
    auto skip_flags = __ NotGoto(__ TestNotZero(count_masked));
    // CF = last bit rotated out: ROL -> result bit 0, ROR -> result MSB. Holds
    // for any non-zero count.
    auto msb = __ And(__ LsrImm(result, ir::Imm(u64(width - 1))), ir::Operand{ir::Imm(u64(1))});
    ir::Value cf = left ? __ And(result, ir::Operand{ir::Imm(u64(1))}) : msb;
    __ SetCarry(cf);
    // OF is defined only for count == 1 (undefined, harmless otherwise):
    //   ROL: CF XOR MSB(result);  ROR: MSB(result) XOR next-MSB(result).
    ir::Value of;
    if (left) {
        of = __ Xor(cf, ir::Operand{msb});
    } else {
        auto msb2 =
                __ And(__ LsrImm(result, ir::Imm(u64(width - 2))), ir::Operand{ir::Imm(u64(1))});
        of = __ Xor(msb, ir::Operand{msb2});
    }
    __ SetOverflow(of);
    StorePolarity(false);  // CF stored Direct; skipped with the update when count == 0
    __ BindLabel(skip_flags);
    // Count is runtime-dependent (0 preserves the prior carry), so mark unknown.
    carry_ = CarryPolarity::Unknown;

    Dst(insn, op0, result.SetCastType(GetSize(width)));
}

void X64Decoder::DecodeBt(_DInst& insn, int kind) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    const auto width = op0.size;
    const u32 log2w = width == 64 ? 6 : (width == 32 ? 5 : 4);
    const auto type = GetSize(width);
    auto idx_raw = ToValue(Src(insn, op1));

    const bool locked_rmw =
            kind != 0 && op0.type != O_REG && (insn.flags & FLAG_LOCK) != 0;
    ir::Value base;      // the old value the bit is extracted from
    ir::Value n;         // in-element bit index
    ir::Value mem_addr;  // non-null for the memory (bit-string) form
    if (op0.type == O_REG) {
        base = ToValue(Src(insn, op0));
        n = __ And(idx_raw, ir::Operand{ir::Imm(u64(width - 1))});
    } else {
        // Memory form: the operand is a bit string; the (signed) index first
        // selects an element, then a bit within it.
        auto idx64 = op1.type == O_IMM ? idx_raw
                                       : __ SignExtend(idx_raw).SetType(ir::ValueType::U64);
        auto elems = __ AsrValue(idx64, __ LoadImm(ir::Imm(u64(log2w))));
        auto byte_off = __ LslValue(elems, __ LoadImm(ir::Imm(u64(log2w - 3))));
        mem_addr = __ Add(FlatAddress(insn, op0), ir::Operand{byte_off});
        n = __ And(idx64, ir::Operand{ir::Imm(u64(width - 1))});
        if (locked_rmw) {
            auto mask = __ LslValue(__ LoadImm(ir::Imm(u64(1))), n);
            ir::AtomicRMWOp op;
            ir::Value operand;
            if (kind == 1) {
                op = ir::AtomicRMWOp::Or;
                operand = mask;
            } else if (kind == 2) {
                op = ir::AtomicRMWOp::And;
                operand = __ Xor(mask, ir::Operand{ir::Imm(UINT64_MAX)});
            } else {
                op = ir::AtomicRMWOp::Xor;
                operand = mask;
            }
            base = __ AtomicRMW(ir::Imm(static_cast<u8>(op)),
                               mem_addr,
                               NarrowTo(operand, type),
                               __ LoadImm(ir::Imm(u8(0))))
                           .SetType(type);
        } else {
            base = MemLoad(ir::Operand{mem_addr}, type, TsoOrdered(insn));
        }
    }
    auto wide = width < 64 ? __ ZeroExtend64(base) : base;
    auto bit = __ And(__ LsrValue(wide, n), ir::Operand{ir::Imm(u64(1))});

    if (kind != 0 && !locked_rmw) {
        auto mask = __ LslValue(__ LoadImm(ir::Imm(u64(1))), n);
        ir::Value modified;
        if (kind == 1) {
            modified = __ Or(wide, ir::Operand{mask});
        } else if (kind == 2) {
            modified = __ AndNot(wide, ir::Operand{mask});
        } else {
            modified = __ Xor(wide, ir::Operand{mask});
        }
        if (op0.type == O_REG) {
            Dst(insn, op0, modified.SetCastType(type));
        } else {
            MemStore(ir::Operand{mem_addr}, NarrowTo(modified, type), TsoOrdered(insn));
        }
    }

    // CF = the extracted bit (t + t carries exactly when bit == 1); the
    // remaining flags are architecturally undefined after bt*.
    auto t = __ LslImm(__ ZeroExtend64(bit), ir::Imm(63u));
    auto cv = __ Add(t, ir::Operand{t});
    __ SaveFlags(cv, ir::Flags::Carry);
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
}



}  // namespace swift::x86
