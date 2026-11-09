#include "atomic_helpers.h"
#include "call_helpers.h"
#include "compile.h"
#include "relocate.h"
#include "runtime/arch/arm32/numeric_registry.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/arch/x86/bit_operations.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/arch/x86/sse42_string.h"
#include "runtime/arch/x86/vector_crypto.h"
#include "runtime/arch/x86/x87.h"

#include <array>
#include <cstring>
#include "assembler.h"

namespace swift::runtime::backend::loongarch64 {
HostSymbol CallbackSymbol(std::uintptr_t address) noexcept {
    if (address == reinterpret_cast<std::uintptr_t>(&x86::QueryGuestCpu))
        return HostSymbol::GuestCpuQuery;
    if (address == reinterpret_cast<std::uintptr_t>(&x86::QueryGuestSystem))
        return HostSymbol::GuestSystemQuery;
    if (address == reinterpret_cast<std::uintptr_t>(&::swift::runtime::arm64::ExecuteNumeric))
        return HostSymbol::ArmGuestFP;
    if (const auto identity = runtime::arm32::NumericCallbackIdentity(address))
        return static_cast<HostSymbol>(255 + identity);
    return HostSymbol::None;
}

std::uintptr_t ResolveSymbol(HostSymbol symbol, const CompileOptions& options) noexcept {
    if (static_cast<std::uint16_t>(symbol) >= 256)
        return runtime::arm32::ResolveNumericCallbackIdentity(static_cast<std::uint16_t>(symbol) -
                                                              255);
    switch (symbol) {
        case HostSymbol::Dispatcher:
            return options.dispatcher;
        case HostSymbol::Exit:
            return options.exit_handler;
        case HostSymbol::GuestCpuQuery:
            return reinterpret_cast<std::uintptr_t>(&x86::QueryGuestCpu);
        case HostSymbol::GuestSystemQuery:
            return reinterpret_cast<std::uintptr_t>(&x86::QueryGuestSystem);
        case HostSymbol::ArmGuestFP:
            return reinterpret_cast<std::uintptr_t>(&::swift::runtime::arm64::ExecuteNumeric);
        case HostSymbol::LoongWideDivide:
            return reinterpret_cast<std::uintptr_t>(&WideDivide);
        case HostSymbol::LoongCheckedX87:
            return reinterpret_cast<std::uintptr_t>(&CheckedX87);
        case HostSymbol::X87Execute:
            return reinterpret_cast<std::uintptr_t>(&SvmX87Execute);
        case HostSymbol::StringImplicit:
            return reinterpret_cast<std::uintptr_t>(&x86::sse42::CompareImplicit);
        case HostSymbol::StringExplicit:
            return reinterpret_cast<std::uintptr_t>(&x86::sse42::CompareExplicit);
        case HostSymbol::Crypto:
            return reinterpret_cast<std::uintptr_t>(&x86::crypto::Execute);
        case HostSymbol::LoongCompareExchange128:
            return reinterpret_cast<std::uintptr_t>(&CompareExchange128);
        case HostSymbol::DepositBits:
            return reinterpret_cast<std::uintptr_t>(&x86::ParallelBitDeposit);
        case HostSymbol::ExtractBits:
            return reinterpret_cast<std::uintptr_t>(&x86::ParallelBitExtract);
        default:
            return 0;
    }
}

namespace {
bool AddressLiteral(std::span<std::uint8_t> code, std::size_t offset) noexcept {
    if (offset < 16) return false;
    std::uint32_t first;
    std::memcpy(&first, code.data() + offset - 16, sizeof(first));
    const auto reg = GPR{first & 31};
    if (reg == x0) return false;
    // The external-buffer assembler owns no memory, literals or labels.
    // Check only this recorded address's four instructions using the same
    // encoder as HostAddress; do not decode or scan the rest of the unit.
    std::array<std::uint32_t, 4> expected{};
    Assembler sink(reinterpret_cast<std::uint8_t*>(expected.data()), sizeof(expected));
    sink.AUIPC(reg, 0);
    sink.LD(reg, 16, reg);
    sink.J(16);
    sink.NOP();
    return std::memcmp(code.data() + offset - 16, expected.data(), sizeof(expected)) == 0;
}
}  // namespace

bool Relocate(std::span<std::uint8_t> code,
              std::span<const Relocation> relocations,
              const CompileOptions& options) noexcept {
    std::size_t previous_end{};
    for (const auto& relocation : relocations) {
        const auto offset = static_cast<std::size_t>(relocation.offset);
        if (relocation.kind != RelocationKind::Absolute64 || (offset & 7) ||
            offset < previous_end || offset > code.size() || 8u > code.size() - offset ||
            !AddressLiteral(code, offset))
            return false;
        std::uintptr_t address;
        if (!backend::detail::RelocationAddress(
                    ResolveSymbol(relocation.symbol, options), relocation.addend, address))
            return false;
        const std::uint64_t value = address;
        std::memcpy(code.data() + offset, &value, sizeof(value));
        previous_end = offset + 8u;
    }
    return true;
}
}  // namespace swift::runtime::backend::loongarch64
