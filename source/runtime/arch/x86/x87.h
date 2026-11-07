#pragma once

#include <cstddef>
#include <cstdint>

namespace swift::runtime::x86 {

// Architectural extended precision: the integer bit is explicit. Padding is
// not part of the 80-bit value and carries no cached numerical facts.
struct X87Value {
    std::uint64_t significand{};
    std::uint16_t sign_exp{};
    std::uint8_t reserved[6]{};
};

static_assert(sizeof(X87Value) == 16 && offsetof(X87Value, sign_exp) == 8);

struct alignas(16) X87State {
    X87Value registers[8]{};  // Physical registers; logical ST(i)=(TOP+i)&7.
    std::uint16_t control{0x037f}, status{}, tags{0xffff}, reserved{};
    // Query output, never an extra x87 register or an arithmetic cache.
    // The helper always returns X87Result; raw64 data uses this separate word.
    std::uint64_t result{};
};

static_assert(sizeof(X87State) == 144 && alignof(X87State) == 16);
static_assert(offsetof(X87State, control) == 128 && offsetof(X87State, result) == 136);

enum class X87Command : std::uint8_t {
    Init = 0,
    PushOne,
    PushF64,
    Exchange,
    Add,
    Mul,
    Div,
    Sqrt,
    StoreRegPop,
    Status,
    ControlWord,
    ReadF64,
    Pop,
    SetControlWord,
    ExchangeTwice,
    PushOnePop,
    PushF32,
    PushI16,
    PushI32,
    PushI64,
    PushReg,
    StageExtLow,
    PushExtHigh,
    StoreReg,
    PrepareF32,
    PrepareF64,
    PrepareExt80,
    PrepareI16,
    PrepareI32,
    PrepareI64,
    CommitPrepared,
    CommitExt80,
    Binary,
    Compare,
    Unary,
    PushConstant,
    Free,
    AdjustTop,
    ClearExceptions,
    Wait,
    LoadControlWord,
    RefreshStatus,
    MaskExceptions,
    QueryRegExt,
    RestoreRegExtHigh,
    Remainder,
    Scale,
    Extract,
    Transcendental,
    ConditionalMove,
    PushBcdHigh,
    PrepareBcd,
    CommitBcd
};
enum class X87Result : std::uint64_t {
    Ok = 0,
    Unsupported = 1,
    UnmaskedPending = 2,
    RaisedException = 3
};

enum class X87Format : std::uint8_t { Register, Float32, Float64, Int16, Int32, Int64 };
enum class X87Binary : std::uint8_t { Add, Mul, Sub, Div };
enum class X87Unary : std::uint8_t { ChangeSign, Abs, Test, Examine, Round };
enum class X87Constant : std::uint8_t { One, Log2Ten, Log2E, Pi, Log10Two, LnTwo, Zero };
enum class X87Transcendental : std::uint8_t {
    Sin,
    Cos,
    SinCos,
    Tan,
    Atan,
    YLog2X,
    YLog2XPlusOne,
    TwoToXMinusOne
};

enum X87Option : std::uint64_t {
    X87Pop = UINT64_C(1) << 16,
    X87Reverse = UINT64_C(1) << 17,
    X87DestIndex = UINT64_C(1) << 18,
    X87QuietCompare = UINT64_C(1) << 19,
    X87ToFlags = UINT64_C(1) << 20,
    X87PopTwice = UINT64_C(1) << 21,
};

// Low8=command, bits10:8=logical ST index. Legacy commands require remaining
// bits zero; Binary/Compare use the explicit extended encoding below.
// Add/Mul/Div write ST(0) op ST(index); Exchange swaps ST(0)/ST(index);
// StoreRegPop copies ST(0) to ST(index), then pops. ExchangeTwice also uses
// index; remaining commands use index0.
// PushF64 consumes raw binary64 bits. Status/ControlWord/ReadF64 write result;
// ReadF64 converts ST(0) without popping, SetControlWord consumes raw low16.
// ReadF64 commits its conversion status; it is not a transactional memory
// store. Pop preserves that status for an explicitly successful continuation.
// ExchangeTwice(index) preserves the complete result of two Exchanges.
// PushOnePop(index0) preserves PushOne;Sqrt;StoreRegPop(0), including a
// masked stack-overflow payload and stopping before Pop on unmasked failure.
constexpr std::uint64_t X87Encode(X87Command command, unsigned index = 0) noexcept {
    return static_cast<std::uint64_t>(command) | (std::uint64_t{index} << 8);
}

// Binary/Compare alone use format[13:11], operation[15:14] and X87Option.
// The raw argument is the actual memory value, never a guest pointer.
constexpr std::uint64_t X87EncodeOperation(X87Command command,
                                           unsigned index,
                                           X87Format format,
                                           X87Binary operation = X87Binary::Add,
                                           std::uint64_t options = 0) noexcept {
    return X87Encode(command, index) | (static_cast<std::uint64_t>(format) << 11) |
           (static_cast<std::uint64_t>(operation) << 14) | options;
}

// Relative architectural metadata positions for the checked helper call.
// This is a call immediate, not persistent execution or analysis state.
// Bit60 also commits metadata on success, only for the instruction's final
// helper after any guest memory stores have completed.
constexpr std::uint64_t X87EncodeMetadata(std::int16_t ip_offset,
                                          std::int16_t dp_offset,
                                          std::int16_t opcode_offset,
                                          std::uint16_t opcode,
                                          bool has_dp,
                                          bool commit_success = false) noexcept {
    return std::uint64_t{static_cast<std::uint16_t>(ip_offset)} |
           (std::uint64_t{static_cast<std::uint16_t>(dp_offset)} << 16) |
           (std::uint64_t{static_cast<std::uint16_t>(opcode_offset)} << 32) |
           (std::uint64_t{static_cast<std::uint16_t>(opcode & 0x7ff)} << 48) |
           (std::uint64_t{has_dp} << 59) | (std::uint64_t{commit_success} << 60);
}

constexpr std::int16_t X87MetadataOffset(std::uint64_t encoded, unsigned field) noexcept {
    return static_cast<std::int16_t>((encoded >> (field * 16)) & 0xffff);
}

constexpr std::uint16_t X87MetadataOpcode(std::uint64_t encoded) noexcept {
    return (encoded >> 48) & 0x7ff;
}

constexpr bool X87MetadataHasDP(std::uint64_t encoded) noexcept { return (encoded >> 59) & 1; }

constexpr bool X87MetadataCommitsSuccess(std::uint64_t encoded) noexcept {
    return (encoded >> 60) & 1;
}

// BCD scratch keeps the low sixteen decimal digits in result. reserved holds
// digit16/17 in bits7:0 and the original sign in bit15. Unused bits12:8 hold
// the NEW IE/DE/PE/SF and C1 for the successful store continuation. Existing
// sticky flags remain architectural, so an old IE cannot forge an indefinite.
constexpr std::uint16_t X87BcdHigh(std::uint16_t scratch) noexcept {
    return scratch & 0x0100 ? 0xffff : scratch & 0x80ff;
}

// m80 load: read both guest pieces first, StageExtLow(significand), then
// PushExtHigh(sign_exp). QueryRegExt writes result=significand/reserved=exp;
// RestoreRegExtHigh consumes that staged pair without changing imported tags.
// Prepare* stores only non-architectural query scratch. For binary32/64 and
// integer stores, reserved holds prospective status; CommitPrepared(pop0/1)
// applies it only after the complete guest store succeeded. PrepareExt80 uses
// reserved for the sign/exponent; CommitExt80(pop0/1) finalizes stack status.
// Unmasked Prepare failures update sticky exception status but never pop or
// write guest memory. There is one active instruction, no persistent plan.
// BCD load similarly uses StageExtLow followed by PushBcdHigh. PrepareBcd
// exposes its payload through result/X87BcdHigh(reserved); CommitBcd applies
// its exception/C1 changes and pops only after both guest pieces were stored.
// ConditionalMove(index, rawBool) checks BOTH source/destination stack tags
// even when rawBool is false. It never consumes guest flags itself.

std::uint8_t X87AbridgedTag(const X87State&) noexcept;
void X87RestoreAbridgedTags(X87State&, std::uint8_t) noexcept;

}  // namespace swift::runtime::x86

// No guest-memory access, allocation, native floating-point or host exception.
// Unmasked exceptions update sticky status and leave stack/results uncommitted.
extern "C" std::uint64_t SvmX87Execute(void* state,
                                       std::uint64_t command,
                                       std::uint64_t raw) noexcept;
