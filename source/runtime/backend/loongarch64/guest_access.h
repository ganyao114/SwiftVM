#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include "runtime/common/types.h"

namespace swift::runtime::backend::loongarch64 {

enum class GuestAccessKind : u8 { Read, Write, ReadWrite };
enum class GuestAccessCoverage : u8 { Precise, Unsupported };

// Exactly one four-byte native guest access, including each byte access on
// the unaligned path and both instructions of an LL/SC pair. The interrupted
// address is gprs[address_gpr] + displacement, not merely the validated range.
struct GuestAccessRange {
    u32 begin_offset{};
    u32 end_offset{};
    u32 size{};
    GuestAccessKind kind{};
    u8 address_gpr{};
    s32 displacement{};
};

// Validate metadata against the instruction itself before making code RX.
// Only the native scalar accesses currently emitted for guest memory qualify;
// stack/State/SSA accesses and arbitrary instructions cannot acquire recovery
// permission by supplying a wider metadata interval.
inline bool GuestAccessEncodingValid(std::span<const u8> code,
                                     const GuestAccessRange& site) noexcept {
    if ((site.begin_offset & 3) || site.begin_offset > code.size() ||
        code.size() - site.begin_offset < 4 ||
        site.end_offset != u64(site.begin_offset) + 4 ||
        (site.address_gpr != 16 && site.address_gpr != 18)) return false;
    u32 word{};
    std::memcpy(&word, code.data() + site.begin_offset, sizeof(word));
    if (((word >> 5) & 31) != site.address_gpr) return false;
    const auto signed_field = [](u32 value, u32 bits) -> s32 {
        const auto sign = u32{1} << (bits - 1);
        return static_cast<s32>((value ^ sign) - sign);
    };
    const u32 offset = word >> 22;
    u32 size{};
    GuestAccessKind kind{};
    switch (offset) {
        case 0xa0: case 0xa8: size = 1; kind = GuestAccessKind::Read; break;
        case 0xa1: case 0xa9: size = 2; kind = GuestAccessKind::Read; break;
        case 0xa2: case 0xaa: size = 4; kind = GuestAccessKind::Read; break;
        case 0xa3: size = 8; kind = GuestAccessKind::Read; break;
        case 0xa4: size = 1; kind = GuestAccessKind::Write; break;
        case 0xa5: size = 2; kind = GuestAccessKind::Write; break;
        case 0xa6: size = 4; kind = GuestAccessKind::Write; break;
        case 0xa7: size = 8; kind = GuestAccessKind::Write; break;
        default: break;
    }
    if (size) return site.size == size && site.kind == kind &&
            site.displacement == signed_field((word >> 10) & 4095, 12);
    const u32 llsc = word >> 24;
    if (llsc >= 0x20 && llsc <= 0x23) {
        size = llsc < 0x22 ? 4 : 8;
        kind = (llsc & 1) ? GuestAccessKind::Write : GuestAccessKind::Read;
        return site.size == size && site.kind == kind &&
                site.displacement == signed_field((word >> 10) & 16383, 14) * 4;
    }
    const u32 atomic = word >> 15;
    constexpr u32 first = 0b00111000011010010; // AMSWAP.DB.W
    constexpr u32 last = 0b00111000011100011;  // AMMIN.DB.DU
    if (atomic < first || atomic > last) return false;
    const u32 destination = word & 31, operand = (word >> 10) & 31;
    return destination != site.address_gpr && destination != operand &&
            site.size == ((atomic & 1) ? 8u : 4u) && site.displacement == 0 &&
            site.kind == GuestAccessKind::ReadWrite;
}

} // namespace swift::runtime::backend::loongarch64
