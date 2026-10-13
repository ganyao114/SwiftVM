#include "runtime/backend/loongarch64/assembler.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace swift::runtime::backend::loongarch64 {
namespace {

class OfflineAllocator final : public Xbyak_loongarch64::Allocator {
public:
    bool useProtect() const override { return false; }
};

OfflineAllocator& GetOfflineAllocator() {
    static OfflineAllocator allocator;
    return allocator;
}

void CheckRegister(GPR reg) {
    if (reg.getIdx() > 31) throw std::invalid_argument("LoongArch register index exceeds 31");
}

Label& CheckedLabel(Label* label) {
    if (!label) throw std::invalid_argument("LoongArch branch has a null label");
    return *label;
}

constexpr std::int32_t SignExtend(std::uint32_t value, unsigned bits) {
    const auto sign = std::uint32_t{1} << (bits - 1);
    return std::bit_cast<std::int32_t>((value ^ sign) - sign);
}

}  // namespace

Assembler::Assembler(std::size_t initial_capacity, HostFeatures features)
        : CodeGenerator(std::max<std::size_t>(initial_capacity, 4),
                  Xbyak_loongarch64::AutoGrow, &GetOfflineAllocator()), features_(features) {}

void Assembler::LI(GPR destination, std::uint64_t value) {
    CheckRegister(destination);
    const auto signed_value = std::bit_cast<std::int64_t>(value);
    if (signed_value >= -2048 && signed_value <= 2047) {
        addi_d(destination, zero, static_cast<std::int32_t>(signed_value));
        return;
    }
    if (value <= 4095) {
        ori(destination, zero, static_cast<std::int32_t>(value));
        return;
    }
    lu12i_w(destination, SignExtend(static_cast<std::uint32_t>((value >> 12) & 0xfffff), 20));
    if (value & 0xfff) ori(destination, destination, static_cast<std::int32_t>(value & 0xfff));
    // lu12i.w sign-extends bit 31, while lu32i.d sign-extends bit 51.
    // Compare the exact upper bits instead of testing whether they are zero.
    const auto upper32 = value >> 32;
    const auto extended32 = (value & (1ULL << 31)) ? 0xffffffffULL : 0ULL;
    if (upper32 != extended32) {
        lu32i_d(destination, SignExtend(static_cast<std::uint32_t>((value >> 32) & 0xfffff), 20));
        const auto upper12 = value >> 52;
        const auto extended52 = (value & (1ULL << 51)) ? 0xfffULL : 0ULL;
        if (upper12 != extended52)
            lu52i_d(destination, destination, SignExtend(static_cast<std::uint32_t>(upper12), 12));
    }
}

void Assembler::MV(GPR destination, GPR source) {
    CheckRegister(destination);
    CheckRegister(source);
    if (destination != source) or_(destination, source, zero);
}

void Assembler::Bind(Label* label) { L(CheckedLabel(label)); }
void Assembler::J(Label* label) { b(CheckedLabel(label)); }

// Only the fixed local skip uses the short conditional branch. The long
// unconditional branch covers the complete code allocation without a scratch.
#define SWIFT_LONG_BRANCH(name, inverse) \
    void Assembler::name(GPR lhs, GPR rhs, Label* label) { \
        auto& target = CheckedLabel(label); \
        CheckRegister(lhs); \
        CheckRegister(rhs); \
        inverse(lhs, rhs, 8); \
        b(target); \
    }
SWIFT_LONG_BRANCH(BEQ, bne)
SWIFT_LONG_BRANCH(BNE, beq)
SWIFT_LONG_BRANCH(BLT, bge)
SWIFT_LONG_BRANCH(BGE, blt)
SWIFT_LONG_BRANCH(BLTU, bgeu)
SWIFT_LONG_BRANCH(BGEU, bltu)
#undef SWIFT_LONG_BRANCH

void Assembler::LILabel(GPR destination, Label* label) {
    CheckRegister(destination);
    pcaddi(destination, CheckedLabel(label));
}

void Assembler::Finalize() {
    // Recompute all label fixups when callers append another compilation
    // unit after publishing earlier units. No executable protection is applied.
    isCalledCalcJmpAddress_ = false;
    ready(PROTECT_RW);
}

void Assembler::RequireLBT(bool arm) const {
    if (!(arm ? features_.lbt_arm : features_.lbt_x86))
        throw std::logic_error(arm ? "ARM LBT lowering is disabled" : "x86 LBT lowering is disabled");
}

// Encodings independently checked against LLVM LoongArchLBTInstrInfo.td and
// LoongArchLBTInstrFormats.td at d6675b6a61ad94124c6fba13dea7ddf9a6374e8f.
void Assembler::ARMFlags(std::uint32_t opcode, GPR lhs, GPR rhs, std::uint32_t condition) {
    RequireLBT(true);
    CheckRegister(lhs);
    CheckRegister(rhs);
    if (condition > 15) throw std::invalid_argument("ARM LBT condition exceeds four bits");
    dd(opcode | (rhs.getIdx() << 10) | (lhs.getIdx() << 5) | condition);
}

#define SWIFT_LBT_ARM_DEF(name, opcode) \
    void Assembler::name(GPR lhs, GPR rhs, std::uint32_t condition) { \
        ARMFlags(opcode, lhs, rhs, condition); \
    }
SWIFT_LBT_ARM_DEF(ARMADD_W, 0x00370010)
SWIFT_LBT_ARM_DEF(ARMSUB_W, 0x00378010)
SWIFT_LBT_ARM_DEF(ARMADC_W, 0x00380010)
SWIFT_LBT_ARM_DEF(ARMSBC_W, 0x00388010)
SWIFT_LBT_ARM_DEF(ARMAND_W, 0x00390010)
SWIFT_LBT_ARM_DEF(ARMOR_W, 0x00398010)
SWIFT_LBT_ARM_DEF(ARMXOR_W, 0x003a0010)
#undef SWIFT_LBT_ARM_DEF

void Assembler::X86Flags(std::uint32_t opcode, GPR lhs, GPR rhs) {
    RequireLBT(false);
    CheckRegister(lhs);
    CheckRegister(rhs);
    dd(opcode | (rhs.getIdx() << 10) | (lhs.getIdx() << 5));
}

#define SWIFT_LBT_X86_DEF(name, opcode) \
    void Assembler::name##_B(GPR lhs, GPR rhs) { X86Flags(opcode, lhs, rhs); } \
    void Assembler::name##_H(GPR lhs, GPR rhs) { X86Flags(opcode + 1, lhs, rhs); } \
    void Assembler::name##_W(GPR lhs, GPR rhs) { X86Flags(opcode + 2, lhs, rhs); } \
    void Assembler::name##_D(GPR lhs, GPR rhs) { X86Flags(opcode + 3, lhs, rhs); }
SWIFT_LBT_X86_DEF(X86ADD, 0x003f0004)
SWIFT_LBT_X86_DEF(X86SUB, 0x003f0008)
SWIFT_LBT_X86_DEF(X86ADC, 0x003f000c)
SWIFT_LBT_X86_DEF(X86SBC, 0x003f0010)
SWIFT_LBT_X86_DEF(X86AND, 0x003f8010)
SWIFT_LBT_X86_DEF(X86OR, 0x003f8014)
SWIFT_LBT_X86_DEF(X86XOR, 0x003f8018)
#undef SWIFT_LBT_X86_DEF

void Assembler::FlagTransfer(bool arm, std::uint32_t opcode, GPR reg, std::uint32_t mask) {
    RequireLBT(arm);
    CheckRegister(reg);
    if (mask > 255) throw std::invalid_argument("LBT flag mask exceeds eight bits");
    dd(opcode | (mask << 10) | reg.getIdx());
}

void Assembler::SetCondition(bool arm, std::uint32_t opcode, GPR reg, std::uint32_t condition) {
    RequireLBT(arm);
    CheckRegister(reg);
    if (condition > 15) throw std::invalid_argument("LBT condition exceeds four bits");
    dd(opcode | (condition << 10) | reg.getIdx());
}

void Assembler::ARMMFFLAG(GPR destination, std::uint32_t mask) { FlagTransfer(true, 0x005c0040, destination, mask); }
void Assembler::ARMMTFLAG(GPR source, std::uint32_t mask) { FlagTransfer(true, 0x005c0060, source, mask); }
void Assembler::SETARMJ(GPR destination, std::uint32_t condition) { SetCondition(true, 0x0036c000, destination, condition); }
void Assembler::X86MFFLAG(GPR destination, std::uint32_t mask) { FlagTransfer(false, 0x005c0000, destination, mask); }
void Assembler::X86MTFLAG(GPR source, std::uint32_t mask) { FlagTransfer(false, 0x005c0020, source, mask); }
void Assembler::SETX86J(GPR destination, std::uint32_t condition) { SetCondition(false, 0x00368000, destination, condition); }

}  // namespace swift::runtime::backend::loongarch64
