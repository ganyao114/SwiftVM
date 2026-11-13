#include <cstring>
#include "abi.h"
#include "recovery.h"
#include "runtime/execution/fault.h"

namespace swift::runtime::backend::arm64 {
namespace {
struct Registers {
    std::span<const std::byte> bytes;

    std::uint64_t operator[](unsigned index) const noexcept {
        std::uint64_t bits;
        std::memcpy(&bits, bytes.data() + index * 8, 8);
        return bits;
    }
};
}  // namespace

bool RestoreGuestState(execution::FaultContext& context,
                       const backend::FaultSite& site,
                       std::span<const std::byte> gprs,
                       std::uint64_t native_flags,
                       std::span<const std::byte> vectors) noexcept {
    const Registers registers{gprs};
    const auto recipe = DecodeFaultRecovery(site);
    if (context.pinned_vector_offset != UINT32_MAX) {
        if (vectors.empty()) return false;
        auto* slots = reinterpret_cast<std::uint8_t*>(context.state) + context.pinned_vector_offset;
        std::memcpy(slots, vectors.data() + 16 * 16, 16 * 16);
    }
    if (context.pinned_uniform_offset != UINT32_MAX) {
        auto* slots =
                reinterpret_cast<std::uint8_t*>(context.state) + context.pinned_uniform_offset;
        for (unsigned i = 0; i < context.pinned_uniform_slots; ++i) {
            // Low eight are callee-saved; pin16 maps high slots
            // directly to x8..x15. Preserve the legacy pin12 map.
            const auto reg =
                    backend::arm64::abi::PinnedUniformRegister(i, context.pinned_uniform_slots);
            const auto bits = registers[reg];
            std::memcpy(slots + i * 8, &bits, 8);
        }
    }
    if (context.flags_offset != UINT32_MAX) {
        auto packed = registers[1];
        if (context.native_nzcv) {
            std::memcpy(&packed,
                        reinterpret_cast<const std::uint8_t*>(context.state) + context.flags_offset,
                        8);
            packed &= ~backend::arm64::abi::PackedDynamicFlags;
            if (recipe.test_bits && (registers[29] & backend::arm64::abi::DeferredTestTag)) {
                // The compiler records the actual still-valid
                // entry snapshot, which can differ from the pin
                // already overwritten by a successful load.
                const auto raw = registers[recipe.test_register];
                const auto value = recipe.test_bits == 32
                                           ? std::uint64_t{static_cast<std::uint32_t>(raw)}
                                           : raw;
                packed |= (value & UINT64_C(0xff)) | (std::uint64_t{value == 0} << 30) |
                          (((value >> (recipe.test_bits - 1)) & 1) << 31);
            } else if (recipe.logical_bits) {
                if ((recipe.logical_register + 1u) * 8u > gprs.size()) return false;
                const auto raw = registers[recipe.logical_register];
                const auto mask = recipe.logical_bits == 64
                                          ? ~UINT64_C(0)
                                          : (UINT64_C(1) << recipe.logical_bits) - 1;
                const auto value = raw & mask;
                packed |= (value & UINT64_C(0xff)) | (std::uint64_t{value == 0} << 30) |
                          (((value >> (recipe.logical_bits - 1)) & 1) << 31);
            } else {
                const auto pf = registers[recipe.parity_register ? recipe.parity_register : 1];
                const auto carry_flip = recipe.carry_direct ? UINT64_C(0) : UINT64_C(0x20000000);
                packed |= (pf & UINT64_C(0xff)) |
                          ((native_flags ^ carry_flip) & UINT64_C(0xf0000000)) |
                          ((((registers[29] ^ pf) >> 4) & 1) << 26);
            }
        }
        std::memcpy(
                reinterpret_cast<std::uint8_t*>(context.state) + context.flags_offset, &packed, 8);
    }
    return true;
}

}  // namespace swift::runtime::backend::arm64
