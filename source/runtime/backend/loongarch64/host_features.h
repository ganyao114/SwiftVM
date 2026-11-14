#pragma once

#include <cstdint>
#include <cstdlib>
#include <string_view>

#if defined(__linux__) &&                                                                          \
        (defined(__loongarch64) || (defined(__loongarch__) && __loongarch_grlen == 64))
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

    friend constexpr bool operator==(HostFeatures, HostFeatures) = default;

    [[nodiscard]] constexpr HostFeatures WithPolicy(bool allow_lsx,
                                                    bool allow_lasx,
                                                    bool allow_crc32) const {
        auto result = *this;
        result.lsx &= allow_lsx;
        result.lasx &= allow_lasx;
        result.crc32 &= allow_crc32;
        return result;
    }

    [[nodiscard]] constexpr std::uint64_t IsaFeatureMask() const noexcept {
        return (lbt_x86 ? (1ULL << 0) : 0) | (ual ? (1ULL << 1) : 0) | (crc32 ? (1ULL << 2) : 0) |
               (lsx ? (1ULL << 3) : 0) | (lasx ? (1ULL << 4) : 0) | (lbt_arm ? (1ULL << 5) : 0) |
               (lbt_mips ? (1ULL << 6) : 0);
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

    // Dual-gate hardware + kernel context-switch capability check:
    // LBT requires both kernel AT_HWCAP (context save/restore) and CPUCFG2
    // bits 18..20 (LBT_X86, LBT_ARM, LBT_MIPS).
    static constexpr HostFeatures FromHWCAPAndCPUCFG2(std::uint64_t hwcap,
                                                      std::uint32_t cpucfg2) {
        auto result = FromHWCAP(hwcap);
        result.lbt_x86 &= (cpucfg2 & (1u << 18)) != 0;
        result.lbt_arm &= (cpucfg2 & (1u << 19)) != 0;
        result.lbt_mips &= (cpucfg2 & (1u << 20)) != 0;
        return result;
    }

    [[nodiscard]] static constexpr HostFeatures ApplyEnvOverride(std::string_view spec,
                                                                 HostFeatures hardware) noexcept {
        auto current = hardware;
        while (!spec.empty()) {
            const auto comma = spec.find(',');
            auto token = comma == std::string_view::npos ? spec : spec.substr(0, comma);
            spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
                token.remove_prefix(1);
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
                token.remove_suffix(1);
            if (token.empty()) continue;
            if (token == "off" || token == "none" || token == "0") {
                current = {};
                continue;
            }
            if (token == "all" || token == "default" || token == "on" || token == "1") {
                current = hardware;
                continue;
            }
            bool enable = true;
            if (token.front() == '-') {
                enable = false;
                token.remove_prefix(1);
            } else if (token.front() == '+') {
                token.remove_prefix(1);
            }
            if (token == "lbt") {
                current.lbt_x86 = enable && hardware.lbt_x86;
                current.lbt_arm = enable && hardware.lbt_arm;
                current.lbt_mips = enable && hardware.lbt_mips;
            } else if (token == "lbt_x86") {
                current.lbt_x86 = enable && hardware.lbt_x86;
            } else if (token == "lbt_arm") {
                current.lbt_arm = enable && hardware.lbt_arm;
            } else if (token == "lbt_mips") {
                current.lbt_mips = enable && hardware.lbt_mips;
            } else if (token == "ual") {
                current.ual = enable && hardware.ual;
            } else if (token == "crc32") {
                current.crc32 = enable && hardware.crc32;
            } else if (token == "lsx") {
                current.lsx = enable && hardware.lsx;
                if (!enable) current.lasx = false;
            } else if (token == "lasx") {
                current.lasx = enable && hardware.lasx;
                if (current.lasx) current.lsx = hardware.lsx;
            }
        }
        return current;
    }

    static HostFeatures DetectHardware() noexcept {
        // Kernel-advertised capabilities include context-switch support.
        // SIGILL probing would interfere with guest fault handling. QEMU TCG
        // without LBT must use the software flags path.
#if defined(__linux__) &&                                                                          \
        (defined(__loongarch64) || (defined(__loongarch__) && __loongarch_grlen == 64))
        static const auto features = [] {
            const std::uint64_t hwcap = getauxval(AT_HWCAP);
            std::uint32_t cpucfg2 = 0;
            asm volatile("cpucfg %0, %1" : "=r"(cpucfg2) : "r"(2u));
            return FromHWCAPAndCPUCFG2(hwcap, cpucfg2);
        }();
        return features;
#else
        return {};
#endif
    }

    static HostFeatures Detect() noexcept {
        const auto hardware = DetectHardware();
        if (const char* override_spec = std::getenv("SVM_LA64_FEATURES"))
            return ApplyEnvOverride(override_spec, hardware);
        return hardware;
    }
};

}  // namespace swift::runtime::backend::loongarch64
