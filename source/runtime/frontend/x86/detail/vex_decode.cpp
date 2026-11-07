#include "modrm.h"
#include "decode_timing.h"

namespace swift::x86::detail {
namespace {
enum Form : std::uint8_t {
    Binary,
    Unary,
    BinaryImm,
    UnaryImm,
    Scalar,
    ScalarImm,
    ScalarRead,
    Load,
    Store,
    ScalarLoad,
    ScalarStore,
    LowHighLoad,
    LowHighStore,
    DuplicateDouble,
    FromInteger,
    ToInteger,
    ConvertScalar,
    Widen32,
    Narrow64,
    IntegerLoad,
    IntegerStore,
    QwordLoad,
    QwordStore,
    Mask,
    BinaryCount,
    Insert,
    InsertWide,
    Extract,
    ExtractWide,
    ExtractWord,
    ShiftImmediate,
    Widen,
    Read,
    ReadImm,
    Broadcast,
    MaskLoad,
    MaskStore,
    Gather32,
    Gather64,
    Fma,
    Insert128,
    Extract128,
    VariableBlend,
    Mxcsr,
};

enum Policy : unsigned {
    Any = 0,
    ReservedV = 1,
    L0 = 2,
    L1 = 4,
    W0 = 8,
    W1 = 16,
    Effective128 = 32,
    MemOnly = 64,
    RegOnly = 128
};

struct Descriptor {
    Op narrow{}, wide{};
    Form form{};
    unsigned bits{}, policy{};
};

Descriptor Describe(unsigned map, unsigned pp, unsigned opcode) noexcept {
    switch ((map << 10) | (pp << 8) | opcode) {
#define VEX(map, pp, code, narrow, wide, form, bits, policy)                                       \
    case ((map << 10) | (pp << 8) | code):                                                         \
        return {Op::narrow, Op::wide, form, bits, policy};
#include "vex_opcodes.inc"
#undef VEX
        default:
            return {};
    }
}

bool Immediate(Form form) noexcept {
    switch (form) {
        case BinaryImm:
        case UnaryImm:
        case ScalarImm:
        case Insert:
        case InsertWide:
        case Extract:
        case ExtractWide:
        case ExtractWord:
        case ShiftImmediate:
        case Insert128:
        case Extract128:
        case VariableBlend:
        case ReadImm:
            return true;
        default:
            return false;
    }
}

// The BMI opcodes share the encoding reader, not vector register semantics.
bool Bmi(InstructionData& out,
         const ModRM& mod,
         unsigned map,
         unsigned pp,
         unsigned opcode,
         unsigned vvvv,
         bool w,
         bool l,
         ByteCursor& cursor) noexcept {
    const auto width = w ? 64u : 32u;
    const auto reg = RegisterOperand(mod.reg, width), v = RegisterOperand(vvvv, width),
               rm = RmOperand(mod, width);
    if (map == 2) {
        if (opcode == 0xf2 && pp == 0) {
            out.opcode = Op::ANDN;
            out.ops = {reg, v, rm, {}};
        } else if (opcode == 0xf3 && pp == 0 && mod.reg >= 1 && mod.reg <= 3) {
            out.opcode = mod.reg == 1 ? Op::BLSR : mod.reg == 2 ? Op::BLSMSK : Op::BLSI;
            out.ops = {v, rm, {}, {}};
        } else if (opcode == 0xf5 && (pp == 0 || pp == 2 || pp == 3)) {
            out.opcode = pp == 0 ? Op::BZHI : pp == 2 ? Op::PEXT : Op::PDEP;
            out.ops = pp == 0 ? std::array<DecodedOperand, 4>{reg, rm, v, {}}
                              : std::array<DecodedOperand, 4>{reg, v, rm, {}};
        } else if (opcode == 0xf6 && pp == 3) {
            out.opcode = Op::MULX;
            out.ops = {reg, v, rm, {}};
        } else if (opcode == 0xf7) {
            out.opcode = pp == 0 ? Op::BEXTR : pp == 1 ? Op::SHLX : pp == 2 ? Op::SARX : Op::SHRX;
            out.ops = {reg, rm, v, {}};
        } else
            return false;
    } else if (map == 3 && pp == 3 && opcode == 0xf0) {
        out.opcode = Op::RORX;
        out.immediate = cursor.Take();
        out.ops = {reg, rm, {GuestOperandKind::Immediate, Register::None, 8}, {}};
        if (vvvv) cursor.error = DecodeError::InvalidEncoding;
    } else
        return false;
    out.operand_bits = static_cast<std::uint8_t>(width);
    if (l) cursor.error = DecodeError::InvalidEncoding;
    switch (out.opcode) {
        case Op::ANDN:
        case Op::BLSR:
        case Op::BLSMSK:
        case Op::BLSI:
        case Op::BZHI:
            out.flags_written =
                    GuestFlag::Carry | GuestFlag::Overflow | GuestFlag::Zero | GuestFlag::Sign;
            out.flags_undefined = GuestFlag::Auxiliary | GuestFlag::Parity;
            break;
        case Op::BEXTR:
            out.flags_written = GuestFlag::Carry | GuestFlag::Overflow | GuestFlag::Zero;
            out.flags_undefined = GuestFlag::Auxiliary | GuestFlag::Parity | GuestFlag::Sign;
            break;
        default:
            break;
    }
    return true;
}
}  // namespace

ExtendedDecodeResult DecodeVex(ByteCursor& cursor,
                               EncodingPrefix prefix,
                               std::uint64_t pc,
                               DecodeTiming* timing) noexcept {
    DecodeSample sample(timing);
    ExtendedDecodeResult result;
    result.handled = true;
    auto& out = result.instruction;
    out.addr = pc;
    out.address_bits = prefix.address32 ? 32 : 64;
    out.segment = prefix.segment;
    const auto fail = [&](DecodeError error) {
        result.error = error;
        return result;
    };
    if (prefix.rex || prefix.word || prefix.repeat || prefix.repeat_not_equal || prefix.locked)
        return fail(DecodeError::InvalidEncoding);
    const auto lead = cursor.Take(), first = cursor.Take();
    unsigned map = 1, pp = first & 3, vvvv = (~first >> 3) & 15, r = (~first >> 7) & 1, x = 0,
             b = 0;
    bool w = false, l = (first & 4) != 0;
    if (lead == 0xc4) {
        const auto second = cursor.Take();
        map = first & 31;
        pp = second & 3;
        vvvv = (~second >> 3) & 15;
        x = (~first >> 6) & 1;
        b = (~first >> 5) & 1;
        w = (second & 128) != 0;
        l = (second & 4) != 0;
        if (map < 1 || map > 3)
            return fail(cursor.error == DecodeError::None ? DecodeError::InvalidEncoding
                                                          : cursor.error);
    }
    const auto opcode = cursor.Take();
    if (cursor.error != DecodeError::None) return fail(cursor.error);
    if (map == 1 && opcode == 0x77) {
        if (pp || vvvv) return fail(DecodeError::InvalidEncoding);
        out.opcode = l ? Op::VZEROALL : Op::VZEROUPPER;
        out.vector_bytes = l ? 32 : 16;
        out.size = static_cast<std::uint8_t>(cursor.at);
        return result;
    }
    const auto descriptor = Describe(map, pp, opcode);
    const bool bmi = (map == 2 && (opcode == 0xf2 || opcode == 0xf3 || opcode == 0xf5 ||
                                   opcode == 0xf6 || opcode == 0xf7)) ||
                     (map == 3 && pp == 3 && opcode == 0xf0);
    if (!bmi && descriptor.narrow == Op::Unsupported)
        return fail(DecodeError::UnsupportedGuestFeature);
    const bool gather = descriptor.form == Gather32 || descriptor.form == Gather64;
    const auto mod = ReadModRM(cursor, out, r, x, b, gather);
    if (cursor.error != DecodeError::None) return fail(cursor.error);
    if (bmi) {
        if (!Bmi(out, mod, map, pp, opcode, vvvv, w, l, cursor))
            return fail(DecodeError::InvalidEncoding);
        out.size = static_cast<std::uint8_t>(cursor.at);
        result.error = cursor.error;
        return result;
    }
    const auto policy = descriptor.policy;
    if (((policy & ReservedV) && vvvv) || ((policy & L0) && l) || ((policy & L1) && !l) ||
        ((policy & W0) && w) || ((policy & W1) && !w) || ((policy & MemOnly) && mod.mod == 3) ||
        ((policy & RegOnly) && mod.mod != 3))
        return fail(DecodeError::InvalidEncoding);
    out.opcode = w ? descriptor.wide : descriptor.narrow;
    const auto width = (policy & Effective128) ? 128u : l ? 256u : 128u;
    out.vector_bytes = static_cast<std::uint8_t>(width / 8);
    out.operand_bits = static_cast<std::uint8_t>(w ? 64 : 32);
    const auto reg = RegisterOperand(mod.reg, width, true), v = RegisterOperand(vvvv, width, true),
               rm = RmOperand(mod, width, true, width);
    const auto fp = RmOperand(mod, descriptor.bits, true, 128);
    const auto control = DecodedOperand{GuestOperandKind::Immediate, Register::None, 8};
    switch (descriptor.form) {
        case Binary:
        case BinaryImm:
            out.ops = {reg, v, rm, {}};
            break;
        case Fma:
            out.ops = {reg,
                       v,
                       (policy & Effective128) ? RmOperand(mod, w ? 64 : 32, true, 128) : rm,
                       {}};
            break;
        case Unary:
        case UnaryImm:
        case Read:
        case ReadImm:
        case Load:
            out.ops = {reg, rm, {}, {}};
            break;
        case Store:
            out.ops = {rm, reg, {}, {}};
            break;
        case Scalar:
        case ScalarImm:
        case ConvertScalar:
            out.ops = {reg, v, fp, {}};
            break;
        case ScalarRead:
            out.ops = {RegisterOperand(mod.reg, 128, true), fp, {}, {}};
            break;
        case ScalarLoad:
        case ScalarStore:
            if (mod.mod == 3)
                out.ops = descriptor.form == ScalarLoad
                                  ? std::array<DecodedOperand, 4>{reg, v, fp, {}}
                                  : std::array<DecodedOperand, 4>{fp, v, reg, {}};
            else {
                if (vvvv) return fail(DecodeError::InvalidEncoding);
                out.ops = descriptor.form == ScalarLoad
                                  ? std::array<DecodedOperand, 4>{reg, fp, {}, {}}
                                  : std::array<DecodedOperand, 4>{fp, reg, {}, {}};
            }
            break;
        case LowHighLoad:
            if (mod.mod == 3 && pp == 1) return fail(DecodeError::InvalidEncoding);
            if (mod.mod == 3) out.opcode = opcode == 0x12 ? Op::VMOVHLPS : Op::VMOVLHPS;
            out.ops = {reg, v, fp, {}};
            break;
        case LowHighStore:
            out.ops = {fp, reg, {}, {}};
            break;
        case DuplicateDouble:
            out.ops = {reg, RmOperand(mod, width == 128 ? 64 : 256, true, width), {}, {}};
            break;
        case FromInteger:
            out.ops = {reg, v, RmOperand(mod, w ? 64 : 32), {}};
            break;
        case ToInteger:
            out.ops = {RegisterOperand(mod.reg, w ? 64 : 32), fp, {}, {}};
            break;
        case Widen32:
            out.ops = {reg, RmOperand(mod, width / 2, true, 128), {}, {}};
            break;
        case Narrow64:
            out.ops = {RegisterOperand(mod.reg, 128, true), rm, {}, {}};
            break;
        case IntegerLoad:
            out.ops = {reg, RmOperand(mod, w ? 64 : 32), {}, {}};
            break;
        case IntegerStore:
            out.ops = {RmOperand(mod, w ? 64 : 32), reg, {}, {}};
            break;
        case QwordLoad:
            out.ops = {reg, RmOperand(mod, 64, true, 128), {}, {}};
            break;
        case QwordStore:
            out.ops = {RmOperand(mod, 64, true, 128), reg, {}, {}};
            break;
        case Mask:
            out.ops = {RegisterOperand(mod.reg, 32), rm, {}, {}};
            break;
        case BinaryCount:
            out.ops = {reg, v, RmOperand(mod, 128, true, 128), {}};
            break;
        case Insert:
        case InsertWide:
            out.ops = {reg,
                       v,
                       RmOperand(mod,
                                 descriptor.form == InsertWide ? (w ? 64 : 32) : descriptor.bits,
                                 false,
                                 descriptor.form == InsertWide ? (w ? 64 : 32) : 32),
                       {}};
            break;
        case Extract:
        case ExtractWide:
            out.ops = {RmOperand(mod,
                                 descriptor.form == ExtractWide ? (w ? 64 : 32) : descriptor.bits,
                                 false,
                                 descriptor.form == ExtractWide ? (w ? 64 : 32) : 32),
                       reg,
                       {},
                       {}};
            break;
        case ExtractWord:
            out.ops = {RegisterOperand(mod.reg, 32), RmOperand(mod, 128, true, 128), {}, {}};
            break;
        case ShiftImmediate: {
            if (mod.mod != 3 || mod.reg > 7) return fail(DecodeError::InvalidEncoding);
            const auto group = mod.extension;
            if (group == 2)
                out.opcode = opcode == 0x71 ? Op::VPSRLW : opcode == 0x72 ? Op::VPSRLD : Op::VPSRLQ;
            else if (group == 4 && opcode != 0x73)
                out.opcode = opcode == 0x71 ? Op::VPSRAW : Op::VPSRAD;
            else if (group == 6)
                out.opcode = opcode == 0x71 ? Op::VPSLLW : opcode == 0x72 ? Op::VPSLLD : Op::VPSLLQ;
            else if (group == 3 && opcode == 0x73)
                out.opcode = Op::VPSRLDQ;
            else if (group == 7 && opcode == 0x73)
                out.opcode = Op::VPSLLDQ;
            else
                return fail(DecodeError::InvalidEncoding);
            out.ops = {v, rm, {}, {}};
            break;
        }
        case Widen:
            out.ops = {reg, RmOperand(mod, descriptor.bits * (width / 128), true, 128), {}, {}};
            break;
        case Broadcast:
            out.ops = {reg, RmOperand(mod, descriptor.bits, true, 128), {}, {}};
            break;
        case MaskLoad:
            out.ops = {reg, rm, v, {}};
            break;
        case MaskStore:
            out.ops = {rm, reg, v, {}};
            break;
        case Gather32:
        case Gather64: {
            if (mod.mod == 3 || !mod.sib || mod.reg == vvvv ||
                mod.reg == mod.memory.index - Register::XMM0 ||
                vvvv == mod.memory.index - Register::XMM0)
                return fail(DecodeError::InvalidEncoding);
            const auto destination_bits = descriptor.form == Gather64 && !w ? 128u : width;
            out.ops = {RegisterOperand(mod.reg, destination_bits, true),
                       RmOperand(mod, w ? 64 : 32, true),
                       RegisterOperand(vvvv, destination_bits, true),
                       {}};
            break;
        }
        case Insert128:
            out.ops = {reg, v, RmOperand(mod, 128, true, 128), {}};
            break;
        case Extract128:
            out.ops = {RmOperand(mod, 128, true, 128), reg, {}, {}};
            break;
        case VariableBlend:
            out.ops = {reg, v, rm, {}};
            break;
        case Mxcsr:
            if (mod.reg != 2 && mod.reg != 3) return fail(DecodeError::InvalidEncoding);
            out.opcode = mod.reg == 2 ? Op::LDMXCSR : Op::STMXCSR;
            out.ops = {RmOperand(mod, 32), {}, {}, {}};
            out.unit_boundary = mod.reg == 2;
            break;
    }
    if (Immediate(descriptor.form)) {
        out.immediate = cursor.Take();
        if (descriptor.form == VariableBlend)
            out.ops[3] = RegisterOperand((out.immediate >> 4) & 15, width, true);
        else {
            unsigned position = 0;
            while (position < out.ops.size() && out.ops[position].type != GuestOperandKind::None)
                ++position;
            if (position < out.ops.size()) out.ops[position] = control;
        }
    }
    if (descriptor.form == ScalarRead || descriptor.form == Read || descriptor.form == ReadImm)
        out.flags_written = GuestFlag::Carry | GuestFlag::Parity | GuestFlag::Auxiliary |
                            GuestFlag::Zero | GuestFlag::Sign | GuestFlag::Overflow;
    out.size = static_cast<std::uint8_t>(cursor.at);
    result.error = cursor.error;
    return result;
}
}  // namespace swift::x86::detail
