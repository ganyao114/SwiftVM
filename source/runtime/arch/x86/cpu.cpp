#include "cpu.h"

namespace swift::runtime::x86 {

namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
constexpr CpuQueryResult Pack(u32 a, u32 b, u32 c, u32 d) {
    return {u64{a} | (u64{b} << 32), u64{c} | (u64{d} << 32)};
}
constexpr char brand[48] = "SwiftVM x86-64 baseline CPU";
constexpr u32 BrandWord(unsigned offset) {
    u32 word{};
    for (unsigned i = 0; i < 4; ++i)
        word |= static_cast<u32>(static_cast<unsigned char>(brand[offset + i])) << (i * 8);
    return word;
}
}  // namespace

CpuQueryResult QueryGuestCpu(u64 leaf, u64 subleaf) noexcept {
    // A conservative fixed user-mode model. Do not advertise unimplemented
    // extensions (including XSAVE/OSXSAVE/AVX, CX8/CX16 and BMI), or
    // derive guest capabilities from the ARM host's HWCAP bits.
    switch (static_cast<u32>(leaf)) {
        case 0:
            return Pack(7, 0x756e6547u, 0x6c65746eu, 0x49656e69u);  // GenuineIntel.
        case 1:
            return Pack(0x600,
                        (1u << 16) | (8u << 8),
                        1u << 23,
                        (1u << 15) | (1u << 25) | (1u << 26));  // POPCNT, CMOV, SSE, SSE2.
        case 7:
            return {};  // No structured extensions or additional subleaves.
        case 0x80000000u:
            return Pack(0x80000008u, 0, 0, 0);
        case 0x80000001u:
            return Pack(0, 0, 0, (1u << 11) | (1u << 20) | (1u << 29));  // SYSCALL, NX, long mode.
        case 0x80000002u:
        case 0x80000003u:
        case 0x80000004u: {
            const auto offset = (static_cast<u32>(leaf) - 0x80000002u) * 16;
            return Pack(BrandWord(offset),
                        BrandWord(offset + 4),
                        BrandWord(offset + 8),
                        BrandWord(offset + 12));
        }
        case 0x80000008u:
            return Pack(48u | (48u << 8), 0, 0, 0);
        default:
            (void)subleaf;
            return {};
    }
}

}  // namespace swift::runtime::x86
