#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace swift::x86 {
namespace detail { struct DecoderAccess; }

enum class Op : std::uint16_t {
    Unsupported,
#define X86_OP(name) name,
#define X86_EXTRA_OP(name) name,
#include "opcodes.inc"
#undef X86_OP
#undef X86_EXTRA_OP
};
// Architectural register identifiers owned here, not a parser's register bank.
// Consecutive GPR widths permit the same guest slot arithmetic in both sinks.
enum Register : std::uint8_t {
    None,
    RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15,
    EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI, R8D, R9D, R10D, R11D, R12D, R13D, R14D, R15D,
    AX, CX, DX, BX, SP, BP, SI, DI, R8W, R9W, R10W, R11W, R12W, R13W, R14W, R15W,
    AL, CL, DL, BL, AH, CH, DH, BH, R8B, R9B, R10B, R11B, R12B, R13B, R14B, R15B, SPL, BPL, SIL, DIL,
    ST0, ST1, ST2, ST3, ST4, ST5, ST6, ST7,
    XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7, XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15,
    RIP, ES, CS, SS, DS, FS, GS,
    MM0, MM1, MM2, MM3, MM4, MM5, MM6, MM7,
    Other = 255,
};
enum class RegisterClass : std::uint8_t { None, Gpr, Vector, X87, Mmx, Other };
struct RegisterView {
    RegisterClass kind{RegisterClass::None};
    std::uint8_t index{}, first_bit{};
    std::uint16_t bits{};
    bool zero_upper{};
};
enum class GuestOperandKind : std::uint8_t {
    None, Register, Immediate, Memory, Displacement, Relative, IndexedMemory,
    ImmediateSecond, ImmediateThird, FarPointer, Other,
};
struct DecodedOperand {
    GuestOperandKind type{};
    std::uint8_t index{Register::None};
    std::uint16_t size{};
};
static_assert(sizeof(DecodedOperand) == 4);
struct OperandView { GuestOperandKind kind{}; RegisterView reg; std::uint16_t bits{}; };
enum class FlowKind : std::uint8_t { Linear, Conditional, Jump, Call, Return, Service };
enum InstructionFlag : std::uint16_t {
    Locked = 1, Repeat = 2, RepeatNotEqual = 4, ImmediateSigned = 8,
    Invalid = 16, Privileged = 32,
};
// These are guest RFLAGS bit positions, independent of both parser and backend.
enum GuestFlag : std::uint16_t {
    Carry = 1, Parity = 4, Auxiliary = 16, Zero = 64, Sign = 128,
    Interrupt = 512, Direction = 1024, Overflow = 2048,
};
struct PrefixView {
    bool locked{}, repeat{}, repeat_not_zero{};
    std::uint8_t address_bits{}, operand_bits{};
};

// This is the sole owned decoded semantic record. DecodeOne translates its
// parser temporary once into this format; no normalized[N] mirror is retained.
struct InstructionData {
    std::uint64_t immediate{}, disp{}, addr{};
    std::array<DecodedOperand, 4> ops{};
    Op opcode{};
    std::uint16_t flags{}, flags_read{}, flags_written{}, flags_undefined{};
    std::uint8_t size{}, segment{Register::None}, base{Register::None}, scale{};
    std::uint8_t address_bits{}, operand_bits{};
    FlowKind flow{};
    bool unit_boundary{};
    std::uint8_t vector_bytes{}; // 0=legacy, 16/32=effective VEX vector length.
    std::array<std::uint8_t, 5> reserved{};
};
static_assert(sizeof(InstructionData) == 64);

class InstructionView;
class Instruction {
public:
    InstructionView View() const noexcept;
private:
    InstructionData data;
    friend struct detail::DecoderAccess;
    friend class InstructionView;
};
static_assert(sizeof(Instruction) == 64);

class InstructionView {
public:
    InstructionView() = default;
    const InstructionData& Record() const noexcept { return record->data; }
    std::uint64_t PC() const noexcept { return Record().addr; }
    std::uint8_t Size() const noexcept { return Record().size; }
    Op Opcode() const noexcept { return Record().opcode; }
    OperandView Operand(unsigned) const noexcept;
    PrefixView Prefixes() const noexcept;
    FlowKind Flow() const noexcept { return Record().flow; }
    bool UnitBoundary() const noexcept { return Record().unit_boundary; }
    bool RelativeTarget(std::uint64_t& target) const noexcept {
        if (Record().ops[0].type != GuestOperandKind::Relative) return false;
        target = Record().addr + Record().size + Record().immediate;
        return true;
    }
private:
    const Instruction* record{};
    explicit InstructionView(const Instruction* record) : record(record) {}
    friend class Instruction;
    friend class InstructionRange;
};
inline InstructionView Instruction::View() const noexcept { return InstructionView{this}; }

inline OperandView InstructionView::Operand(unsigned index) const noexcept {
    if (index >= Record().ops.size()) return {};
    const auto operand = Record().ops[index];
    OperandView out; out.kind = operand.type; out.bits = operand.size;
    if (operand.type != GuestOperandKind::Register) return out;
    auto& reg = out.reg; reg.bits = operand.size;
    if (operand.index >= Register::XMM0 && operand.index <= Register::XMM15) {
        reg.kind = RegisterClass::Vector; reg.index = operand.index - Register::XMM0;
    } else if (operand.index >= Register::MM0 && operand.index <= Register::MM7) {
        reg.kind = RegisterClass::Mmx; reg.index = operand.index - Register::MM0;
    } else if (operand.index >= Register::ST0 && operand.index <= Register::ST7) {
        reg.kind = RegisterClass::X87; reg.index = operand.index - Register::ST0;
    } else if (operand.index >= Register::RAX && operand.index < Register::AL) {
        reg.kind = RegisterClass::Gpr; reg.index = (operand.index - Register::RAX) % 16;
        reg.zero_upper = operand.size == 32;
    } else if (operand.index >= Register::AL && operand.index <= Register::BL) {
        reg.kind = RegisterClass::Gpr; reg.index = operand.index - Register::AL;
    } else if (operand.index >= Register::AH && operand.index <= Register::BH) {
        reg.kind = RegisterClass::Gpr; reg.index = operand.index - Register::AH; reg.first_bit = 8;
    } else if (operand.index >= Register::R8B && operand.index <= Register::R15B) {
        reg.kind = RegisterClass::Gpr; reg.index = 8 + operand.index - Register::R8B;
    } else if (operand.index >= Register::SPL && operand.index <= Register::DIL) {
        reg.kind = RegisterClass::Gpr; reg.index = 4 + operand.index - Register::SPL;
    } else reg.kind = RegisterClass::Other;
    return out;
}
inline PrefixView InstructionView::Prefixes() const noexcept {
    return {bool(Record().flags & InstructionFlag::Locked), bool(Record().flags & InstructionFlag::Repeat),
            bool(Record().flags & InstructionFlag::RepeatNotEqual), Record().address_bits, Record().operand_bits};
}

// A borrow of actual records, never a parallel normalized instruction array.
class InstructionRange {
public:
    InstructionRange() = default;
    InstructionRange(std::span<const Instruction> records) noexcept : records(records.data()), count(records.size()) {}
    InstructionRange(const std::vector<Instruction>& records) noexcept : InstructionRange(std::span<const Instruction>{records}) {}
    InstructionView operator[](std::size_t i) const noexcept { return records[i].View(); }
    std::size_t size() const noexcept { return count; }
    bool empty() const noexcept { return !count; }
    InstructionRange subspan(std::size_t first, std::size_t size) const noexcept { return {std::span{records + first, size}}; }
private:
    const Instruction* records{};
    std::size_t count{};
};

} // namespace swift::x86
