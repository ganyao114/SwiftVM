#pragma once

#include <cstdint>

#if defined(__linux__) && (defined(__loongarch64) || \
        (defined(__loongarch__) && __loongarch_grlen == 64))
#include <sys/auxv.h>
#endif

namespace swift::runtime::backend::loongarch64 {

struct HostFeatures {
    bool lsx{};
    bool lasx{};
    bool ual{};
    bool lbt_arm{};
    bool lbt_x86{};
    bool lbt_mips{};
    bool crc32{};

    [[nodiscard]] constexpr HostFeatures WithPolicy(bool allow_lsx, bool allow_lasx,
                                                   bool allow_crc32) const {
        auto result = *this;
        result.lsx &= allow_lsx;
        result.lasx &= allow_lasx;
        result.crc32 &= allow_crc32;
        return result;
    }

    // Linux LoongArch UAPI HWCAP values. Keep this available on other hosts
    // so cross-host encoding and feature-policy tests need no CPU probes.
    static constexpr HostFeatures FromHWCAP(std::uint64_t hwcap) {
        HostFeatures result;
        result.ual = (hwcap & (1ULL << 2)) != 0;
        result.lsx = (hwcap & (1ULL << 4)) != 0;
        result.lasx = (hwcap & (1ULL << 5)) != 0;
        result.crc32 = (hwcap & (1ULL << 6)) != 0;
        result.lbt_x86 = (hwcap & (1ULL << 10)) != 0;
        result.lbt_arm = (hwcap & (1ULL << 11)) != 0;
        result.lbt_mips = (hwcap & (1ULL << 12)) != 0;
        return result;
    }

    static HostFeatures Detect() {
        // Kernel-advertised capabilities include context-switch support.
        // SIGILL probing would interfere with guest fault handling. QEMU TCG
        // does not implement LBT and must use the software flags path.
#if defined(__linux__) && (defined(__loongarch64) || \
        (defined(__loongarch__) && __loongarch_grlen == 64))
        static const auto features = FromHWCAP(getauxval(AT_HWCAP));
        return features;
#else
        return {};
#endif
    }
};

}  // namespace swift::runtime::backend::loongarch64
