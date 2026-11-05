#pragma once

#include <cstddef>
#include <cstdint>

namespace swift::runtime::ir {

// Architectural extended precision: the integer bit is explicit. Padding is
// not part of the 80-bit value and carries no cached numerical facts.
struct X87Value {
    std::uint64_t significand{};
    std::uint16_t sign_exp{};
    std::uint8_t reserved[6]{};
};
static_assert(sizeof(X87Value) == 16 && offsetof(X87Value, sign_exp) == 8);

struct alignas(16) X87State {
    X87Value registers[8]{}; // Physical registers; logical ST(i)=(TOP+i)&7.
    std::uint16_t control{0x037f}, status{}, tags{0xffff}, reserved{};
    // Query output, never an extra x87 register or an arithmetic cache.
    // The helper always returns X87Result; raw64 data uses this separate word.
    std::uint64_t result{};
};
static_assert(sizeof(X87State) == 144 && alignof(X87State) == 16);
static_assert(offsetof(X87State, control) == 128 && offsetof(X87State, result) == 136);

enum class X87Command : std::uint8_t {
    Init = 0, PushOne, PushF64, Exchange, Add, Mul, Div, Sqrt,
    StoreRegPop, Status, ControlWord, ReadF64, Pop, SetControlWord,
    ExchangeTwice, PushOnePop
};
enum class X87Result : std::uint64_t { Ok = 0, Unsupported = 1, UnmaskedPending = 2 };

// Low8=command, bits10:8=logical ST index, all remaining bits zero.
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

} // namespace swift::runtime::ir

// No guest-memory access, allocation, native floating-point or host exception.
// Unmasked exceptions update sticky status and leave stack/results uncommitted.
extern "C" std::uint64_t SvmX87Execute(void* state, std::uint64_t command,
                                      std::uint64_t raw) noexcept;
