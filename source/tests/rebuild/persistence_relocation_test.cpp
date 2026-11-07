#include "runtime/arch/x86/cpu.h"
#include "runtime/backend/arm64/compile.h"
#include "runtime/backend/arm64/relocate.h"
#include "runtime/backend/arm64/patch.h"
#include "runtime/backend/riscv64/compile.h"
#include "runtime/backend/riscv64/relocate.h"
#include "runtime/backend/riscv64/patch.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/relocate.h"
#include "runtime/backend/loongarch64/compile.h"
#include "runtime/backend/loongarch64/relocate.h"
#include "tests/support/check.h"
#include "tests/support/ir_builder.h"
#include "tests/support/native_code.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {
using namespace swift::runtime::ir;
using swift::tests::Check;
using swift::tests::ir_builder::Block;
using swift::tests::ir_builder::Constant;
using swift::tests::ir_builder::Need;
using swift::tests::ir_builder::Submit;
namespace backend = swift::runtime::backend;
namespace arm = backend::arm64;
namespace rv = backend::riscv64;
namespace x86 = swift::runtime::x86;
using backend::CompileResult;
using backend::HostSymbol;
using backend::Relocation;
using backend::RelocationKind;
using u64 = std::uint64_t;

struct State {
    u64 flags{}, pc{};
};

struct Arm64 {
    using Options = arm::CompileOptions;
    static constexpr auto isa = swift::tests::NativeISA::Arm64;
    static constexpr auto divide = HostSymbol::ArmDivideU64;

    static CompileResult Compile(const Program& program, Options options) {
        return arm::Compile(program, options);
    }

    static void Compile(const Program& program, Options options, CompileResult& result) {
        arm::Compile(program, options, result);
    }

    static bool Relocate(std::span<std::uint8_t> code,
                         std::span<const Relocation> rows,
                         const Options& options) {
        return arm::Relocate(code, rows, options);
    }

    static std::uintptr_t Resolve(HostSymbol symbol, const Options& options) {
        return arm::ResolveSymbol(symbol, options);
    }

    static bool InitializeLink(std::span<std::uint8_t> code, const backend::LinkSite& site) {
        return arm::InitializeNativeLink(code, 0x100000, site, 0, 0);
    }
};

struct Riscv64 {
    using Options = rv::CompileOptions;
    static constexpr auto isa = swift::tests::NativeISA::Riscv64;
    static constexpr auto divide = HostSymbol::RiscvWideDivide;

    static CompileResult Compile(const Program& program, Options options) {
        return rv::Compile(program, options);
    }

    static void Compile(const Program& program, Options options, CompileResult& result) {
        rv::Compile(program, options, result);
    }

    static bool Relocate(std::span<std::uint8_t> code,
                         std::span<const Relocation> rows,
                         const Options& options) {
        return rv::Relocate(code, rows, options);
    }

    static std::uintptr_t Resolve(HostSymbol symbol, const Options& options) {
        return rv::ResolveSymbol(symbol, options);
    }

    static bool InitializeLink(std::span<std::uint8_t> code, const backend::LinkSite& site) {
        return rv::InitializeNativeLink(code, 0x100000, site, 0, false);
    }
};

struct X64 {
    using Options = backend::x64::CompileOptions;
    static constexpr auto isa = swift::tests::NativeISA::X64;

    static CompileResult Compile(const Program& program, Options options) {
        return backend::x64::Compile(program, options);
    }

    static bool Relocate(std::span<std::uint8_t> bytes,
                         std::span<const Relocation> rows,
                         const Options& options) {
        return backend::x64::Relocate(bytes, rows, options);
    }
};

struct Loongarch64 {
    using Options = backend::loongarch64::CompileOptions;
    static constexpr auto isa = swift::tests::NativeISA::LoongArch64;

    static CompileResult Compile(const Program& program, Options options) {
        return backend::loongarch64::Compile(program, options);
    }

    static bool Relocate(std::span<std::uint8_t> bytes,
                         std::span<const Relocation> rows,
                         const Options& options) {
        return backend::loongarch64::Relocate(bytes, rows, options);
    }
};

std::span<std::uint8_t> Bytes(std::vector<std::uint32_t>& words) {
    return {reinterpret_cast<std::uint8_t*>(words.data()), words.size() * sizeof(words[0])};
}

bool HasSymbol(const CompileResult& code, HostSymbol symbol) {
    return std::any_of(code.relocations.begin(), code.relocations.end(), [symbol](const auto& row) {
        return row.symbol == symbol;
    });
}

template <typename Arch> typename Arch::Options Options() {
    typename Arch::Options options;
    options.flags_offset = offsetof(State, flags);
    options.pc_offset = offsetof(State, pc);
    // Equal initial addresses must still retain distinct semantic symbols.
    options.dispatcher = options.exit_handler = UINT64_C(0x1111222233334444);
    return options;
}

Program Helpers(Scope scope, std::uintptr_t callback) {
    Builder builder{scope};
    const auto entry = Block(builder);
    const auto returned = scope == Scope::Function ? Block(builder) : BlockRef{};
    const auto dispatched = scope == Scope::Function ? Block(builder) : BlockRef{};
    Need(builder.BeginBlock(entry));
    const auto zero = Constant(builder, 0);
    const auto query = Need(builder.Emit<OpCode::Cpuid>(Type::U64,
                                                        {Operand::ValueOf(zero),
                                                         Operand::ValueOf(zero),
                                                         Operand::Immediate(callback, Type::U64)}));
    Need(builder.Emit<OpCode::ReadSystem>(
            Type::U64,
            {Operand::Immediate(0, Type::U64),
             Operand::Immediate(reinterpret_cast<std::uintptr_t>(&x86::QueryGuestSystem),
                                Type::U64)}));
    const auto low = Constant(builder, 42), divisor = Constant(builder, 7);
    const auto quotient = Need(builder.Emit<OpCode::Div128>(Type::U64,
                                                            {Operand::ValueOf(zero),
                                                             Operand::ValueOf(low),
                                                             Operand::ValueOf(divisor),
                                                             Operand::Immediate(0, Type::U8)}));
    const auto answer = Need(builder.Emit<OpCode::Add>(
            Type::U64, {Operand::ValueOf(query), Operand::ValueOf(quotient)}));
    if (scope == Scope::Function) {
        const auto condition =
                Need(builder.Emit<OpCode::TestZero>(Type::Bool, {Operand::ValueOf(quotient)}));
        Need(builder.EndBlock(Terminal::Branch(condition, dispatched, returned)));
        Need(builder.BeginBlock(returned));
        Need(builder.EndBlock(Terminal::Return(answer)));
        Need(builder.BeginBlock(dispatched));
        const auto pc = Constant(builder, 0x400080);
        Need(builder.Emit<OpCode::StoreUniform>(
                Type::Void,
                {Operand::Uniform(offsetof(State, pc), Type::U64), Operand::ValueOf(pc)}));
        Need(builder.EndBlock(Terminal::Dispatch()));
    } else
        Need(builder.EndBlock(Terminal::Return(answer)));
    return Submit(builder);
}

void UnchangedOutsideSlots(const std::vector<std::uint32_t>& before,
                           const std::vector<std::uint32_t>& after,
                           std::span<const Relocation> rows) {
    Check(before.size() == after.size(), "rebinding preserves the native unit size");
    const auto old_bytes = std::as_bytes(std::span{before});
    const auto new_bytes = std::as_bytes(std::span{after});
    for (std::size_t i = 0; i < old_bytes.size(); ++i) {
        const bool in_slot = std::any_of(rows.begin(), rows.end(), [i](const auto& row) {
            const auto bytes = row.kind == RelocationKind::Arm64MoveWide64 ? 16u : 8u;
            return i >= row.offset && i - row.offset < bytes;
        });
        if (!in_slot)
            Check(old_bytes[i] == new_bytes[i], "rebinding preserves every surrounding byte");
    }
}

template <typename Arch>
void InitializeLinks(std::vector<std::uint32_t>& words, std::span<const backend::LinkSite> links) {
    // Mutable link payloads belong to the normal publisher, not host-symbol
    // relocation. Bind both references to the same synthetic code base.
    for (const auto& site : links)
        Check(Arch::InitializeLink(Bytes(words), site),
              "the normal link publisher accepts the unit");
}

template <typename Arch> void ColdRebinding() {
    for (const auto scope : {Scope::Block, Scope::Function}) {
        auto program = Helpers(scope, reinterpret_cast<std::uintptr_t>(&x86::QueryGuestCpu));
        auto options = Options<Arch>();
        options.relocatable = true;
        const auto recorded = Arch::Compile(program, options);
        Check(bool(recorded) && recorded.relocatable, "supported helpers have complete recording");
        Check(HasSymbol(recorded, HostSymbol::GuestCpuQuery) &&
                      HasSymbol(recorded, HostSymbol::GuestSystemQuery) &&
                      HasSymbol(recorded, Arch::divide) && HasSymbol(recorded, HostSymbol::Exit),
              "real callback, numerical helper and exit births are recorded");
        if (scope == Scope::Function)
            Check(HasSymbol(recorded, HostSymbol::Dispatcher) && !recorded.links.empty(),
                  "static link retains a separately rebindable cold dispatcher");
        Check(Arch::Resolve(HostSymbol::GuestCpuQuery, options) ==
                              reinterpret_cast<std::uintptr_t>(&x86::QueryGuestCpu) &&
                      Arch::Resolve(HostSymbol::GuestSystemQuery, options) ==
                              reinterpret_cast<std::uintptr_t>(&x86::QueryGuestSystem),
              "callback symbols resolve by semantic identity");

        options.dispatcher = UINT64_C(0x123456789abcdef0);
        options.exit_handler = UINT64_C(0x76543210fedcba98);
        auto rebound = recorded.words;
        Check(Arch::Relocate(Bytes(rebound), recorded.relocations, options),
              "cold binding accepts current process symbols");
        auto fresh = Arch::Compile(program, options);
        Check(bool(fresh), "fresh reference compiles under the new bindings");
        Check(rebound != recorded.words, "changed bindings replace the encoded old addresses");
        UnchangedOutsideSlots(recorded.words, rebound, recorded.relocations);
        InitializeLinks<Arch>(rebound, recorded.links);
        InitializeLinks<Arch>(fresh.words, fresh.links);
        Check(fresh.words == rebound,
              "cold dispatcher/exit binding and link initialization match a fresh unit");

        // Change the callback's semantic binding without encoding instructions
        // in the test. A freshly compiled alternate callback is the reference.
        auto alternate = Helpers(scope, reinterpret_cast<std::uintptr_t>(&x86::QueryGuestSystem));
        auto rows = recorded.relocations;
        for (auto& row : rows)
            if (row.symbol == HostSymbol::GuestCpuQuery) row.symbol = HostSymbol::GuestSystemQuery;
        rebound = recorded.words;
        Check(Arch::Relocate(Bytes(rebound), rows, options), "callback can be rebound by symbol");
        UnchangedOutsideSlots(recorded.words, rebound, rows);
        auto alternate_code = Arch::Compile(alternate, options);
        Check(bool(alternate_code), "replacement callback reference compiles");
        InitializeLinks<Arch>(rebound, recorded.links);
        InitializeLinks<Arch>(alternate_code.words, alternate_code.links);
        Check(rebound == alternate_code.words,
              "callback rebinding matches a fresh compile of its replacement");
    }
}

template <typename Arch> void InvalidRelocations() {
    auto program = Helpers(Scope::Block, reinterpret_cast<std::uintptr_t>(&x86::QueryGuestCpu));
    auto options = Options<Arch>();
    options.relocatable = true;
    const auto recorded = Arch::Compile(program, options);
    Check(bool(recorded) && !recorded.relocations.empty(),
          "rejection fixture has real relocations");
    for (unsigned failure = 0; failure < 4; ++failure) {
        auto row = recorded.relocations.front();
        if (failure == 0) row.symbol = static_cast<HostSymbol>(0xffff);
        if (failure == 1) row.offset = 0;  // Prologue text is not an address slot.
        if (failure == 2) row.offset = backend::NoNativeOffset;
        if (failure == 3) row.kind = static_cast<RelocationKind>(0xffff);
        auto words = recorded.words;
        Check(!Arch::Relocate(Bytes(words), std::span{&row, 1}, options),
              "unknown symbols, invalid slots/ranges and unknown kinds reject installation");
        Check(words == recorded.words, "a rejected first relocation does not alter code");
    }
}

template <typename Arch> void DefaultOff() {
    auto program = Helpers(Scope::Function, reinterpret_cast<std::uintptr_t>(&x86::QueryGuestCpu));
    auto options = Options<Arch>();
    const auto ordinary = Arch::Compile(program, options);
    Check(bool(ordinary) && !ordinary.relocatable && ordinary.relocations.empty(),
          "default JIT emits no persistence metadata");
    CompileResult reused;
    options.relocatable = true;
    Arch::Compile(program, options, reused);
    Check(bool(reused) && reused.relocatable && !reused.relocations.empty(),
          "the reused result actually entered recording mode");
    options.relocatable = false;
    Arch::Compile(program, options, reused);
    Check(bool(reused) && !reused.relocatable && reused.relocations.empty() &&
                  reused.words == ordinary.words,
          "turning recording off restores ordinary words and clears old relocation state");
}

x86::CpuQueryResult UnregisteredQuery(u64 leaf, u64 subleaf) noexcept {
    return {leaf + subleaf, leaf ^ subleaf};
}

template <typename Arch> void UnknownCallback() {
    auto program = Helpers(Scope::Block, reinterpret_cast<std::uintptr_t>(&UnregisteredQuery));
    auto options = Options<Arch>();
    for (const bool recording : {false, true}) {
        options.relocatable = recording;
        const auto code = Arch::Compile(program, options);
        Check(bool(code) && !code.words.empty() && !code.relocatable,
              "an unregistered callback remains executable JIT code without cache eligibility");
    }
}

template <typename Arch> void NativeRoundTrip() {
    if (!swift::tests::NativeMode() || !swift::tests::CanExecute(Arch::isa)) return;
    auto program = Helpers(Scope::Block, reinterpret_cast<std::uintptr_t>(&x86::QueryGuestCpu));
    typename Arch::Options options;
    options.flags_offset = offsetof(State, flags);
    options.pc_offset = offsetof(State, pc);
    options.relocatable = true;
    auto code = Arch::Compile(program, options);
    Check(bool(code) && code.relocatable, "standalone helper unit has complete recording");
    Check(Arch::Relocate(Bytes(code.words), code.relocations, options),
          "standalone callbacks and numerical helper bind before native publication");
    swift::tests::NativeCode native{code.words, Arch::isa};
    State state;
    Check(native.Run(reinterpret_cast<std::uint8_t*>(&state)) == x86::QueryGuestCpu(0, 0).lower + 6,
          "rebound CPU query and wide divide execute through their real host ABI");
}
}  // namespace

TEST_CASE("Persistence symbols rebind real emitted addresses",
          "[compile][persistence-relocation]") {
    ColdRebinding<Arm64>();
    ColdRebinding<Riscv64>();
}

TEST_CASE("Persistence rejects unresolved or invalid address slots",
          "[compile][persistence-relocation]") {
    InvalidRelocations<Arm64>();
    InvalidRelocations<Riscv64>();
}

TEST_CASE("Persistence recording is opt in and resets on reuse",
          "[compile][persistence-relocation]") {
    DefaultOff<Arm64>();
    DefaultOff<Riscv64>();
}

TEST_CASE("Unknown callbacks retain JIT execution without persistence",
          "[compile][persistence-relocation]") {
    UnknownCallback<Arm64>();
    UnknownCallback<Riscv64>();
}

TEST_CASE("Rebound helpers execute natively", "[native][persistence-relocation]") {
    NativeRoundTrip<Arm64>();
    NativeRoundTrip<Riscv64>();
    NativeRoundTrip<X64>();
    NativeRoundTrip<Loongarch64>();
}
