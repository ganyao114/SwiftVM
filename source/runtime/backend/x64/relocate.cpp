#include <cstring>
#include "helpers.h"
#include "relocate.h"
#include "runtime/arch/arm32/numeric_registry.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/arch/x86/floating.h"
#include "runtime/arch/x86/sse42_string.h"
#include "runtime/arch/x86/vector_crypto.h"
#include "runtime/arch/x86/x87.h"

namespace swift::runtime::backend::x64 {
HostSymbol CallbackSymbol(std::uintptr_t p) noexcept {
    if (p == reinterpret_cast<std::uintptr_t>(&runtime::x86::QueryGuestCpu))
        return HostSymbol::GuestCpuQuery;
    if (p == reinterpret_cast<std::uintptr_t>(&runtime::x86::QueryGuestSystem))
        return HostSymbol::GuestSystemQuery;
    if (p == reinterpret_cast<std::uintptr_t>(&runtime::arm64::ExecuteNumeric))
        return HostSymbol::ArmGuestFP;
    if (const auto identity = runtime::arm32::NumericCallbackIdentity(p))
        return static_cast<HostSymbol>(255 + identity);
    return HostSymbol::None;
}

std::uintptr_t ResolveSymbol(HostSymbol s, const CompileOptions& o) noexcept {
    if (static_cast<std::uint16_t>(s) >= 256)
        return runtime::arm32::ResolveNumericCallbackIdentity(static_cast<std::uint16_t>(s) - 255);
    switch (s) {
        case HostSymbol::Dispatcher:
            return o.dispatcher;
        case HostSymbol::DispatcherL2:
            return o.l2_dispatcher;
        case HostSymbol::Exit:
            return o.exit_handler;
        case HostSymbol::GuestCpuQuery:
            return reinterpret_cast<std::uintptr_t>(&runtime::x86::QueryGuestCpu);
        case HostSymbol::GuestSystemQuery:
            return reinterpret_cast<std::uintptr_t>(&runtime::x86::QueryGuestSystem);
        case HostSymbol::ArmGuestFP:
            return reinterpret_cast<std::uintptr_t>(&runtime::arm64::ExecuteNumeric);
        case HostSymbol::ArmFPBinary:
            return reinterpret_cast<std::uintptr_t>(&runtime::arm64::EvaluateVectorBinaryFP);
        case HostSymbol::ArmFPValues:
            return reinterpret_cast<std::uintptr_t>(&runtime::arm64::EvaluateFPValues);
        case HostSymbol::X64WideDivide:
            return reinterpret_cast<std::uintptr_t>(&WideDivide);
        case HostSymbol::X64CheckedX87:
            return reinterpret_cast<std::uintptr_t>(&CheckedX87);
        case HostSymbol::X64CompareExchange128:
            return reinterpret_cast<std::uintptr_t>(&CompareExchange128);
        case HostSymbol::X64RoundIntegral:
            return reinterpret_cast<std::uintptr_t>(&RoundIntegralStatus);
        case HostSymbol::X64FusedMultiplyAdd:
            return reinterpret_cast<std::uintptr_t>(&FusedMultiplyAddStatus);
        case HostSymbol::X64FloatingCompare:
            return reinterpret_cast<std::uintptr_t>(&runtime::x86::ExecuteFloating);
        case HostSymbol::X87Execute:
            return reinterpret_cast<std::uintptr_t>(&SvmX87Execute);
        case HostSymbol::StringImplicit:
            return reinterpret_cast<std::uintptr_t>(&runtime::x86::sse42::CompareImplicit);
        case HostSymbol::StringExplicit:
            return reinterpret_cast<std::uintptr_t>(&runtime::x86::sse42::CompareExplicit);
        case HostSymbol::Crypto:
            return reinterpret_cast<std::uintptr_t>(&runtime::x86::crypto::Execute);
        default:
            return 0;
    }
}

bool Relocate(std::span<std::uint8_t> w,
              std::span<const Relocation> rs,
              const CompileOptions& o) noexcept {
    std::size_t end{};
    for (const auto& r : rs) {
        if (r.kind != RelocationKind::X64Immediate64 || r.offset < 2 || r.offset < end ||
            r.offset > w.size() || w.size() - r.offset < 8)
            return false;
        const auto rex = w[r.offset - 2], op = w[r.offset - 1];
        if ((rex & 0xfe) != 0x48 || (op & 0xf8) != 0xb8) return false;
        std::uintptr_t address;
        if (!backend::detail::RelocationAddress(ResolveSymbol(r.symbol, o), r.addend, address))
            return false;
        std::memcpy(w.data() + r.offset, &address, 8);
        end = r.offset + 8;
    }
    return true;
}
}  // namespace swift::runtime::backend::x64
