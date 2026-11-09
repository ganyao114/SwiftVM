#include "decode.h"
#include "runtime/frontend/timing.h"

namespace swift::arm32 {
namespace {
enum class Group : u8 { Arm, Thumb16, Thumb32, VFP, NEON };
enum class FlowEncoding : u8 {
    None,
    ArmB,
    ArmBL,
    ArmBLX,
    Indirect,
    IndirectCall,
    ThumbB,
    ThumbBCond,
    ThumbCBZ,
    ThumbWideB,
    ThumbWideBCond,
    ThumbBL,
    ThumbBLX
};

struct Pattern {
    u32 mask, expected;
    Opcode opcode;
    Group group;
    bool conditional_vfp;
};

struct Bucket {
    u16 offset, count;
};

struct OpcodeInfo {
    const char* name;
    Group group;
    FlowEncoding flow;
    u32 destination_mask;
    unsigned destination_shift;
    u32 pc_list_mask;
    bool conditional, boundary;
};

#include "decode/generated_metadata.inc"
#include "decode/generated_tables.inc"

const OpcodeInfo& Info(Opcode opcode) noexcept { return opcode_info[static_cast<u16>(opcode)]; }

u32 NormalizeNEON(u32 word) noexcept {
    if ((word & 0xef000000) == 0xef000000)
        return 0xf2000000 | ((word >> 4) & 0x01000000) | (word & 0x00ffffff);
    if ((word & 0xff000000) == 0xf9000000) return 0xf4000000 | (word & 0x00ffffff);
    return 0xf7f0a000;
}

unsigned ArmBucket(u32 word) noexcept { return ((word >> 4) & 15) | ((word >> 16) & 0xff0); }

Opcode DecodeArm(u32 word, bool vector_only = false, bool neon_only = false) noexcept {
    const auto bucket = arm_buckets[ArmBucket(word)];
    for (unsigned i = 0; i < bucket.count; ++i) {
        const auto& p = patterns[arm_candidates[bucket.offset + i]];
        const auto group = p.group;
        if (vector_only && group != Group::VFP) continue;
        if (neon_only && group != Group::NEON) continue;
        if (p.conditional_vfp && (word >> 28) == 15) continue;
        if ((word & p.mask) == p.expected) return p.opcode;
    }
    return Opcode::Undefined;
}

bool WritesPC(const Instruction& instruction) noexcept {
    const auto& info = Info(instruction.Code());
    const auto word = instruction.SemanticWord();
    if (info.destination_mask && ((word & info.destination_mask) >> info.destination_shift) == 15)
        return true;
    if (info.pc_list_mask && (word & info.pc_list_mask)) return true;
    if (instruction.Code() == Opcode::thumb16_ADD_reg_t2 ||
        instruction.Code() == Opcode::thumb16_MOV_reg)
        return (word & 0x87) == 0x87;
    return false;
}

s32 SignExtend(u32 bits, unsigned width) noexcept {
    const auto sign = u32{1} << (width - 1);
    return static_cast<s32>((bits ^ sign) - sign);
}

bool IsService(Opcode code) noexcept {
    // The generated boundary flag additionally covers state/context changes.
    return code == Opcode::Undefined || code == Opcode::arm_UDF || code == Opcode::thumb16_UDF ||
           code == Opcode::thumb32_UDF || code == Opcode::arm_SVC || code == Opcode::thumb16_SVC ||
           code == Opcode::arm_BKPT || code == Opcode::thumb16_BKPT;
}
}  // namespace

bool IsThumb32(u16 first_halfword) noexcept { return first_halfword >= 0xe800; }

DecodeOneResult DecodeOne(std::span<const u8> bytes, u64 pc, DecodeTiming* timing) {
    runtime::frontend::ScopedBuildTimer timer{timing ? &timing->nanoseconds : nullptr};
    if (timing) ++timing->calls;
    DecodeOneResult result;
    const auto location = Location{pc};
    if ((location.Address() & (location.Thumb() ? 1u : 3u)) || bytes.empty()) {
        result.error = DecodeError::InvalidInput;
        return result;
    }
    auto& out = result.instruction;
    out.pc = pc;
    if (bytes.size() < 2) {
        result.error = DecodeError::Truncated;
        return result;
    }
    const auto first = u16(bytes[0]) | (u16(bytes[1]) << 8);
    out.length = location.Thumb() && !IsThumb32(first) ? 2 : 4;
    if (bytes.size() < out.length) {
        result.error = DecodeError::Truncated;
        return result;
    }
    if (out.length == 2) {
        out.word = first;
        out.opcode = static_cast<Opcode>(thumb16_opcodes[first]);
        return result;
    }
    const auto second = u16(bytes[2]) | (u16(bytes[3]) << 8);
    out.word = location.Thumb() ? (u32(first) << 16) | second : first | (u32(second) << 16);
    if (!location.Thumb()) {
        out.opcode = DecodeArm(out.word);
        return result;
    }
    if ((out.word & 0xec000000) == 0xec000000 || (out.word & 0xff100000) == 0xf9000000) {
        out.opcode = DecodeArm(out.word, true);
        if (out.opcode == Opcode::Undefined)
            out.opcode = DecodeArm(NormalizeNEON(out.word), false, true);
        if (out.opcode != Opcode::Undefined) return result;
    }
    const auto bucket = thumb32_buckets[out.word >> 24];
    for (unsigned i = 0; i < bucket.count; ++i) {
        const auto& p = patterns[thumb32_candidates[bucket.offset + i]];
        if ((out.word & p.mask) == p.expected) {
            out.opcode = p.opcode;
            break;
        }
    }
    return result;
}

u32 Instruction::SemanticWord() const noexcept {
    return Thumb() && Info(opcode).group == Group::NEON ? NormalizeNEON(word) : word;
}

Cond Instruction::Condition() const noexcept {
    if (Thumb()) {
        if (opcode == Opcode::thumb16_BKPT || (length == 2 && (word & 0xffc0) == 0xba80))
            return Cond::AL;
        return Location{pc}.IT().Cond();
    }
    if ((Info(opcode).conditional || opcode == Opcode::Undefined) && (word >> 28) != 15)
        return static_cast<Cond>(word >> 28);
    return Cond::AL;
}

const char* Instruction::Name() const noexcept { return Info(opcode).name; }

u64 Instruction::NextPC() const noexcept {
    auto next = Location{pc}.Advance(length);
    if (opcode == Opcode::thumb16_IT) return next.SetIT(ITState{static_cast<u8>(word)}).Key();
    return next.AdvanceIT().Key();
}

FlowKind Instruction::Flow() const noexcept {
    const auto flow = Info(opcode).flow;
    switch (flow) {
        case FlowEncoding::ArmBL:
        case FlowEncoding::ArmBLX:
        case FlowEncoding::IndirectCall:
        case FlowEncoding::ThumbBL:
        case FlowEncoding::ThumbBLX:
            return FlowKind::Call;
        case FlowEncoding::ThumbBCond:
        case FlowEncoding::ThumbWideBCond:
        case FlowEncoding::ThumbCBZ:
            return FlowKind::Conditional;
        case FlowEncoding::ArmB:
        case FlowEncoding::ThumbB:
        case FlowEncoding::ThumbWideB:
        case FlowEncoding::Indirect:
            return Condition() != Cond::AL ? FlowKind::Conditional : FlowKind::Jump;
        case FlowEncoding::None:
            break;
    }
    if (WritesPC(*this)) return Condition() != Cond::AL ? FlowKind::Conditional : FlowKind::Jump;
    if (IsService(opcode)) return FlowKind::Service;
    return FlowKind::Linear;
}

bool Instruction::Boundary() const noexcept {
    return Info(opcode).boundary || Info(opcode).flow != FlowEncoding::None || WritesPC(*this);
}

bool Instruction::RelativeTarget(u64& target) const noexcept {
    auto location = Location{pc}.AdvanceIT();
    s32 offset{};
    switch (Info(opcode).flow) {
        case FlowEncoding::ArmB:
        case FlowEncoding::ArmBL:
            offset = SignExtend((word & 0xffffff) << 2, 26) + 8;
            break;
        case FlowEncoding::ArmBLX:
            offset = SignExtend(((word & 0xffffff) << 2) | ((word >> 23) & 2), 26) + 8;
            location = location.SetTFlag(true);
            break;
        case FlowEncoding::ThumbB:
            offset = SignExtend((word & 0x7ff) << 1, 12) + 4;
            break;
        case FlowEncoding::ThumbBCond:
            offset = SignExtend((word & 0xff) << 1, 9) + 4;
            break;
        case FlowEncoding::ThumbCBZ:
            offset = ((word >> 3) & 0x3e) | ((word >> 4) & 0x40);
            offset += 4;
            break;
        case FlowEncoding::ThumbWideBCond: {
            const u32 immediate = ((word >> 6) & 0x100000) | ((word & 0x800) << 8) |
                                  ((word & 0x2000) << 5) | ((word >> 4) & 0x3f000) |
                                  ((word & 0x7ff) << 1);
            offset = SignExtend(immediate, 21) + 4;
            break;
        }
        case FlowEncoding::ThumbWideB:
        case FlowEncoding::ThumbBL:
        case FlowEncoding::ThumbBLX: {
            const u32 sign = (word >> 26) & 1;
            const u32 i1 = 1 ^ ((word >> 13) & 1) ^ sign;
            const u32 i2 = 1 ^ ((word >> 11) & 1) ^ sign;
            const u32 immediate = (sign << 24) | (i1 << 23) | (i2 << 22) |
                                  ((word >> 4) & 0x3ff000) | ((word & 0x7ff) << 1);
            offset = SignExtend(immediate, 25) + 4;
            if (Info(opcode).flow == FlowEncoding::ThumbBLX) {
                location = location.WithPC(Address() & ~3u).SetTFlag(false);
            }
            break;
        }
        default:
            return false;
    }
    target = location.AdvancePC(offset).Key();
    return true;
}
}  // namespace swift::arm32
