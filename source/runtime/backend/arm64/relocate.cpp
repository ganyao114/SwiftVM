#include "relocate.h"
#include "compile.h"
#include "encoding.h"
#include "vector_helper.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/arch/arm64/numeric.h"

#include <cstring>

namespace swift::runtime::backend::arm64 {
std::uintptr_t DivisionHelper(unsigned, bool) noexcept;
std::uintptr_t CpuidHelper() noexcept;
std::uintptr_t X87Helper() noexcept;
std::uintptr_t LambdaHelper() noexcept;

HostSymbol CallbackSymbol(std::uintptr_t address) noexcept {
    if (address == reinterpret_cast<std::uintptr_t>(&::swift::runtime::arm64::ExecuteNumeric))
        return HostSymbol::ArmGuestFP;
    if (address == reinterpret_cast<std::uintptr_t>(&x86::QueryGuestCpu))
        return HostSymbol::GuestCpuQuery;
    if (address == reinterpret_cast<std::uintptr_t>(&x86::QueryGuestSystem))
        return HostSymbol::GuestSystemQuery;
    return HostSymbol::None;
}

std::uintptr_t ResolveSymbol(HostSymbol symbol, const CompileOptions& options) noexcept {
    switch (symbol) {
        case HostSymbol::ArmGuestFP:
            return reinterpret_cast<std::uintptr_t>(&::swift::runtime::arm64::ExecuteNumeric);
        case HostSymbol::Dispatcher:
            return options.dispatcher;
        case HostSymbol::Exit:
            return options.exit_handler;
        case HostSymbol::GuestCpuQuery:
            return reinterpret_cast<std::uintptr_t>(&x86::QueryGuestCpu);
        case HostSymbol::GuestSystemQuery:
            return reinterpret_cast<std::uintptr_t>(&x86::QueryGuestSystem);
        case HostSymbol::ArmPairBridge:
            return CpuidHelper();
        case HostSymbol::ArmX87Bridge:
            return X87Helper();
        case HostSymbol::ArmLambdaBridge:
            return LambdaHelper();
        case HostSymbol::ArmVectorBridge:
            return VectorHelperBridge();
        case HostSymbol::ArmDivideU8:
            return DivisionHelper(8, false);
        case HostSymbol::ArmDivideU16:
            return DivisionHelper(16, false);
        case HostSymbol::ArmDivideU32:
            return DivisionHelper(32, false);
        case HostSymbol::ArmDivideU64:
            return DivisionHelper(64, false);
        case HostSymbol::ArmDivideS8:
            return DivisionHelper(8, true);
        case HostSymbol::ArmDivideS16:
            return DivisionHelper(16, true);
        case HostSymbol::ArmDivideS32:
            return DivisionHelper(32, true);
        case HostSymbol::ArmDivideS64:
            return DivisionHelper(64, true);
        case HostSymbol::ArmStringImplicit:
            return detail::ImplicitStringAddress();
        case HostSymbol::ArmStringExplicit:
            return detail::ExplicitStringAddress();
        case HostSymbol::ArmCrypto:
            return detail::CryptoAddress();
        case HostSymbol::ArmDepositBits:
            return detail::DepositBitsAddress();
        case HostSymbol::ArmExtractBits:
            return detail::ExtractBitsAddress();
        default:
            return 0;
    }
}

namespace {
namespace v = vixl::aarch64;

std::uint32_t Word(std::span<std::uint8_t> code, std::size_t offset) noexcept {
    std::uint32_t word;
    std::memcpy(&word, code.data() + offset, sizeof(word));
    return word;
}

bool MoveWide(std::span<std::uint8_t> code, std::size_t offset, std::uintptr_t value) noexcept {
    const auto reg = Word(code, offset) & v::Rd_mask;
    if (reg == 31) return false;
    for (unsigned part = 0; part < 4; ++part) {
        const auto word = Word(code, offset + part * 4);
        const auto expected =
                (part ? v::MOVK_x : v::MOVZ_x) | v::Assembler::ShiftMoveWide(part) | reg;
        if ((word & ~v::ImmMoveWide_mask) != expected) return false;
    }
    for (unsigned part = 0; part < 4; ++part) {
        const auto word = (Word(code, offset + part * 4) & ~v::ImmMoveWide_mask) |
                          v::Assembler::ImmMoveWide((value >> (part * 16)) & 0xffffu);
        std::memcpy(code.data() + offset + part * 4, &word, sizeof(word));
    }
    return true;
}

bool TailLiteral(std::span<std::uint8_t> code, std::size_t offset) noexcept {
    const auto branch = v::BR | v::Assembler::Rn(v::x16);
    if (offset >= 8 && Word(code, offset - 8) == encoding::LoadLiteral16(2) &&
        Word(code, offset - 4) == branch)
        return true;
    return offset >= 12 && Word(code, offset - 12) == encoding::LoadLiteral16(3) &&
           Word(code, offset - 8) == branch &&
           Word(code, offset - 4) == (v::HINT | v::Assembler::ImmHint(v::NOP));
}
}  // namespace

bool Relocate(std::span<std::uint8_t> code,
              std::span<const Relocation> relocations,
              const CompileOptions& options) noexcept {
    std::size_t previous_end{};
    for (const auto& relocation : relocations) {
        const auto offset = static_cast<std::size_t>(relocation.offset);
        const auto bytes = relocation.kind == RelocationKind::Arm64MoveWide64 ? 16u : 8u;
        if (offset < previous_end || offset > code.size() || bytes > code.size() - offset)
            return false;
        std::uintptr_t address;
        if (!backend::detail::RelocationAddress(
                    ResolveSymbol(relocation.symbol, options), relocation.addend, address))
            return false;
        switch (relocation.kind) {
            case RelocationKind::Arm64MoveWide64:
                if ((offset & 3) || !MoveWide(code, offset, address)) return false;
                break;
            case RelocationKind::Absolute64: {
                if ((offset & 7) || !TailLiteral(code, offset)) return false;
                const std::uint64_t value = address;
                std::memcpy(code.data() + offset, &value, sizeof(value));
                break;
            }
            default:
                return false;
        }
        previous_end = offset + bytes;
    }
    return true;
}
}  // namespace swift::runtime::backend::arm64
