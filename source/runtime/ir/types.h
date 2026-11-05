#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <iterator>
#include <span>

namespace swift::runtime::ir {

struct NodeId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value{invalid};
    constexpr explicit operator bool() const noexcept { return value != invalid; }
    friend constexpr bool operator==(NodeId, NodeId) = default;
};

struct BlockId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value{invalid};
    constexpr explicit operator bool() const noexcept { return value != invalid; }
    friend constexpr bool operator==(BlockId, BlockId) = default;
};

// Handles identify the real request, including across Builder/Program moves.
// They neither own storage nor carry a claimed result type.
struct Value {
    std::uint64_t unit{};
    NodeId node{};
    constexpr explicit operator bool() const noexcept { return unit && bool(node); }
    friend constexpr bool operator==(Value, Value) = default;
};
struct BlockRef {
    std::uint64_t unit{};
    BlockId id{};
    constexpr explicit operator bool() const noexcept { return unit && bool(id); }
    friend constexpr bool operator==(BlockRef, BlockRef) = default;
};

enum class Type : std::uint8_t {
    Void, Bool, U8, U16, U32, U64, S8, S16, S32, S64,
    U128, F32, F64, V8, V16, V32, V64, V128, V256,
};
constexpr bool ValidType(Type type) noexcept {
    return static_cast<unsigned>(type) <= static_cast<unsigned>(Type::V256);
}
constexpr bool IntegerType(Type type) noexcept {
    return type >= Type::Bool && type <= Type::S64;
}
constexpr unsigned TypeBits(Type type) noexcept {
    switch (type) {
        case Type::Bool: return 1;
        case Type::U8: case Type::S8: case Type::V8: return 8;
        case Type::U16: case Type::S16: case Type::V16: return 16;
        case Type::U32: case Type::S32: case Type::F32: case Type::V32: return 32;
        case Type::U64: case Type::S64: case Type::F64: case Type::V64: return 64;
        case Type::U128: case Type::V128: return 128;
        case Type::V256: return 256;
        default: return 0;
    }
}

enum class Scope : std::uint8_t { Block, Function };
enum class TargetMode : std::uint8_t { Known, EarlyOff, Unknown };
enum class TargetFeature : std::uint64_t {
    ShiftedAddress = UINT64_C(1) << 0,
    // The execution service recovers the guest PC from native fault sites.
    // Unknown/EarlyOff frontends keep their explicit guest-state PC stores.
    FaultLocations = UINT64_C(1) << 1,
    // A logical condition selects two literal U64 values for one full uniform
    // store. This capability describes IR input, not physical host registers.
    ConditionalUniformStore = UINT64_C(1) << 2,
    // Runtime owns private GPR state: guest memory and implicit flag fields
    // cannot modify these slots. This is an execution property, not an ISA.
    PrivateGPRState = UINT64_C(1) << 3,
    // Guest memory and implicit flag fields cannot modify these complete
    // private vector slots. This is an execution property, not a host ISA.
    PrivateVectorState = UINT64_C(1) << 4,
    // Signed widening products of consecutive low vector lanes are accepted
    // directly. This describes neutral arithmetic, not physical registers.
    VectorSignedMulWidenLow = UINT64_C(1) << 5,
    // The execution owner accepts complete external entries into nonempty
    // source blocks, sharing one native unit and its lifetime.
    ExternalEntries = UINT64_C(1) << 6,
};
// Immutable target capabilities, consumed by the frontend without a host-ISA
// dependency. Unknown/EarlyOff preserve the bits but do not enable a feature.
struct TargetInfo {
    TargetMode mode{TargetMode::Unknown};
    std::uint32_t target{};
    std::uint64_t features{};
    constexpr bool Has(TargetFeature feature) const noexcept {
        return mode == TargetMode::Known && (features & static_cast<std::uint64_t>(feature));
    }
    friend constexpr bool operator==(TargetInfo, TargetInfo) = default;
};

enum class Error : std::uint8_t {
    None, Consumed, ScopeMismatch, InvalidBlock, ForeignUnit,
    BlockAlreadyOpen, BlockAlreadyEnded, NoActiveBlock,
    InvalidOpcode, InvalidType, InvalidResultType, InvalidOperandKind,
    MissingOperand, ExtraOperand, InvalidValue, InvalidRange,
    IncompleteTerminal, IncompleteScope, CapacityOverflow, EditOnly,
    EditBudgetExceeded, ObservableOperation,
};

enum class OperandKind : std::uint8_t {
    Invalid, Value, Immediate, Uniform, Local, Condition, Flags,
    Expression, Arguments, Lambda,
};
enum class OperandOp : std::uint16_t { Identity, Add, Sub, Lsl, Lsr, Asr, AddExtend, AddShift };

// AtomicRMW returns the observed old value and updates memory modulo the
// result width. AddCarry/SubBorrow consume an explicit zero-or-one carry;
// other operations ignore it, and Neg also ignores the ordinary operand.
enum class AtomicRMWOp : std::uint8_t {
    Add, Sub, And, Or, Xor, Neg, AddCarry, SubBorrow,
};

// Neutral flags-word ABI. Raw u64 operands/metadata can retain additional
// bits; All names the currently defined architectural subset, not a filter.
enum class Flag : std::uint64_t {
    None = 0, Carry = 1, Overflow = 2, Zero = 4, Negate = 8,
    Parity = 16, AuxiliaryCarry = 64, NZCV = 15, All = 95,
};
constexpr std::uint64_t FlagBits(Flag flags) noexcept {
    return static_cast<std::uint64_t>(flags);
}
constexpr Flag operator|(Flag first, Flag second) noexcept {
    return static_cast<Flag>(FlagBits(first) | FlagBits(second));
}
constexpr Flag operator&(Flag first, Flag second) noexcept {
    return static_cast<Flag>(FlagBits(first) & FlagBits(second));
}

enum class Condition : std::uint8_t {
    EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL, NV,
};

enum class FlagProfile : std::uint8_t {
    None, ArithmeticRaw, ArithmeticCanonicalCarry, LogicalZeroCVA,
};
struct OperationMetadata {
    std::uint64_t flag_outputs{};
    FlagProfile profile{FlagProfile::None};
    bool branch_only{};
    // After this operation, omitted Flag::All outputs may have any value.
    // This is ordinary unspecified state, not UB/poison or a cached proof
    // about future readers. Reserved bits stay defined; retaining the old
    // omitted flags is also a valid implementation.
    bool discard_omitted_flags{};
    friend constexpr bool operator==(OperationMetadata, OperationMetadata) = default;
};

// One stored word. Wide payloads occupy the following raw word; only the
// header carries tags. Value views remain consumer views (Void = natural).
struct Cell {
    static constexpr std::uint64_t wide_bit = std::uint64_t{1} << 55;
    std::uint64_t bits{};
};
static_assert(sizeof(Cell) == 8);

class CellRange;
class CellView {
public:
    constexpr CellView() noexcept = default;
    constexpr OperandKind Kind() const noexcept {
        return static_cast<OperandKind>((header >> 48) & 0x7f);
    }
    constexpr Type TypeView() const noexcept { return static_cast<Type>(header >> 56); }
    constexpr std::uint64_t Payload() const noexcept {
        return Kind() == OperandKind::Expression || Kind() == OperandKind::Arguments ? 0 : payload;
    }
    constexpr std::uint32_t Count() const noexcept {
        return Kind() == OperandKind::Expression || Kind() == OperandKind::Arguments
                ? static_cast<std::uint32_t>(header) : 0;
    }
    constexpr std::uint16_t Code() const noexcept {
        return Kind() == OperandKind::Expression ? static_cast<std::uint16_t>(header >> 32) : 0;
    }
    constexpr OperandOp ExpressionOp() const noexcept {
        return static_cast<OperandOp>(Code() & 0xff);
    }
    constexpr std::uint8_t ShiftAmount() const noexcept { return Code() >> 8; }

private:
    constexpr CellView(std::uint64_t header, std::uint64_t payload) noexcept
            : header(header), payload(payload) {}
    std::uint64_t header{}, payload{};
    friend class CellRange;
};

// A borrowed range over the one actual word store, without a decoded table.
// Iteration returns logical cells and skips their wide payload words.
class CellRange {
public:
    class Iterator {
    public:
        using value_type = CellView;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;
        Iterator() noexcept = default;
        CellView operator*() const noexcept { return Read(current, end); }
        Iterator& operator++() noexcept {
            if (current != end)
                current += (current->bits & Cell::wide_bit) && end - current >= 2 ? 2 : 1;
            return *this;
        }
        Iterator operator++(int) noexcept { auto old = *this; ++*this; return old; }
        friend bool operator==(Iterator, Iterator) = default;

    private:
        Iterator(const Cell* current, const Cell* end) noexcept : current(current), end(end) {}
        const Cell* current{};
        const Cell* end{};
        friend class CellRange;
        friend class Builder;
    };

    CellRange() noexcept = default;
    Iterator begin() const noexcept { return {words.data(), End()}; }
    Iterator end() const noexcept { return {End(), End()}; }
    const Cell* data() const noexcept { return words.data(); }
    bool empty() const noexcept { return words.empty(); }
    std::size_t WordCount() const noexcept { return words.size(); } // O(1), physical words.
    CellView front() const noexcept { return Read(words.data(), End()); }

private:
    explicit CellRange(std::span<const Cell> words) noexcept : words(words) {}
    const Cell* End() const noexcept { return words.empty() ? words.data() : words.data() + words.size(); }
    static CellView Read(const Cell* at, const Cell* end) noexcept {
        if (at == end) return {};
        const auto header = at->bits;
        if (header & Cell::wide_bit) {
            if (end - at < 2) return {}; // A forged/truncated operation range cannot overread.
            return {header, at[1].bits};
        }
        return {header, static_cast<std::uint32_t>(header)};
    }
    std::span<const Cell> words;
    friend class Program;
};

// Compound logical arguments are a header followed by their actual cells in
// the same flat operand range. No argument or node owns a heap allocation.
struct Operand {
    std::uint64_t payload{};
    std::uint64_t unit{};
    std::uint32_t count{};
    std::uint16_t code{};
    OperandKind kind{OperandKind::Invalid};
    Type type{Type::Void};

    static constexpr Operand ValueOf(Value value, Type view = Type::Void) noexcept {
        Operand out; out.kind = OperandKind::Value;
        out.payload = value.node.value; out.unit = value.unit; out.type = view; return out;
    }
    static constexpr Operand Immediate(std::uint64_t bits, Type type) noexcept {
        Operand out; out.kind = OperandKind::Immediate;
        out.payload = bits; out.type = type; return out;
    }
    static constexpr Operand Uniform(std::uint32_t offset, Type type) noexcept {
        Operand out; out.kind = OperandKind::Uniform;
        out.payload = offset; out.type = type; return out;
    }
    static constexpr Operand Local(std::uint32_t slot, Type type) noexcept {
        Operand out; out.kind = OperandKind::Local;
        out.payload = slot; out.type = type; return out;
    }
    static constexpr Operand Condition(std::uint32_t condition) noexcept {
        Operand out; out.kind = OperandKind::Condition; out.payload = condition; return out;
    }
    static constexpr Operand Condition(::swift::runtime::ir::Condition condition) noexcept {
        return Condition(static_cast<std::uint32_t>(condition));
    }
    static constexpr Operand Flags(std::uint64_t flags) noexcept {
        Operand out; out.kind = OperandKind::Flags; out.payload = flags; return out;
    }
    static constexpr Operand Flags(Flag flags) noexcept { return Flags(FlagBits(flags)); }
    // The existing 16-bit code word carries the operation and its shift.
    // Code() retains both bytes for lossless traces and semantic checksums.
    static constexpr Operand Expression(OperandOp operation, std::uint32_t atoms = 1,
                                        std::uint8_t shift = 0) noexcept {
        Operand out; out.kind = OperandKind::Expression;
        out.code = static_cast<std::uint16_t>(operation) | (std::uint16_t{shift} << 8);
        out.count = atoms; return out;
    }
    static constexpr Operand Arguments(std::uint32_t cells) noexcept {
        Operand out; out.kind = OperandKind::Arguments; out.count = cells; return out;
    }
    static constexpr Operand Lambda(std::uint64_t traits = 0) noexcept {
        Operand out; out.kind = OperandKind::Lambda; out.payload = traits; return out;
    }
};

enum class TerminalKind : std::uint8_t { Incomplete, Return, Jump, Branch, Unreachable, Dispatch };
struct Terminal {
    TerminalKind kind{TerminalKind::Incomplete};
    bool value_present{};
    Type value_type{Type::Void};
    Value value{};
    BlockRef first{}, second{};

    static constexpr Terminal Return() noexcept {
        Terminal out; out.kind = TerminalKind::Return; return out;
    }
    static constexpr Terminal Return(Value value, Type view = Type::Void) noexcept {
        auto out = Return(); out.value_present = true; out.value = value;
        out.value_type = view; return out;
    }
    static constexpr Terminal Jump(BlockRef target) noexcept {
        Terminal out; out.kind = TerminalKind::Jump; out.first = target; return out;
    }
    static constexpr Terminal Branch(Value condition, BlockRef yes, BlockRef no,
                                      Type view = Type::Void) noexcept {
        Terminal out; out.kind = TerminalKind::Branch; out.value_present = true;
        out.value = condition; out.first = yes; out.second = no; out.value_type = view;
        return out;
    }
    static constexpr Terminal Unreachable() noexcept {
        Terminal out; out.kind = TerminalKind::Unreachable; return out;
    }
    static constexpr Terminal Dispatch() noexcept {
        Terminal out; out.kind = TerminalKind::Dispatch; return out;
    }
};

}  // namespace swift::runtime::ir
