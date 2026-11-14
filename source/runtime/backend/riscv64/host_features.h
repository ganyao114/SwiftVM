#pragma once

#include <cstdint>
#include <cstdlib>
#include <string_view>

#if defined(__linux__) && defined(__riscv) && __riscv_xlen == 64
#include <unistd.h>
#endif

namespace swift::runtime::backend::riscv64 {

struct HostFeatures {
    bool zba{};
    bool zbb{};
    bool zbs{};
    bool zicond{};
    bool zbc{};
    bool zbkb{};
    bool zbkc{};
    bool ual{};
    bool ztso{};

    friend constexpr bool operator==(HostFeatures, HostFeatures) = default;

    // Linux RISC-V UAPI hwprobe keys and extension bits. Kept available on all
    // hosts so cross-host unit tests can verify policy without CPU probes.
    static constexpr std::int64_t kHwprobeKeyImaExt0 = 4;
    static constexpr std::int64_t kHwprobeKeyCpuPerf0 = 5;
    static constexpr std::int64_t kHwprobeKeyMisalignedScalarPerf = 9;

    static constexpr std::uint64_t kHwprobeMisalignedMask = 7ULL;
    static constexpr std::uint64_t kHwprobeMisalignedFast = 3ULL;

    static constexpr std::uint64_t kHwprobeExtZba = 1ULL << 3;
    static constexpr std::uint64_t kHwprobeExtZbb = 1ULL << 4;
    static constexpr std::uint64_t kHwprobeExtZbs = 1ULL << 5;
    static constexpr std::uint64_t kHwprobeExtZbc = 1ULL << 7;
    static constexpr std::uint64_t kHwprobeExtZbkb = 1ULL << 8;
    static constexpr std::uint64_t kHwprobeExtZbkc = 1ULL << 9;
    static constexpr std::uint64_t kHwprobeExtZtso = 1ULL << 33;
    static constexpr std::uint64_t kHwprobeExtZicond = 1ULL << 35;

    [[nodiscard]] constexpr std::uint64_t IsaFeatureMask() const noexcept {
        return (zba ? (1ULL << 0) : 0) | (zbb ? (1ULL << 1) : 0) | (zbs ? (1ULL << 2) : 0) |
               (zicond ? (1ULL << 3) : 0) | (zbc ? (1ULL << 4) : 0) | (zbkb ? (1ULL << 5) : 0) |
               (zbkc ? (1ULL << 6) : 0) | (ual ? (1ULL << 7) : 0) | (ztso ? (1ULL << 8) : 0);
    }

    static constexpr HostFeatures FromHwprobe(std::uint64_t ima_ext0,
                                              bool fast_misaligned) noexcept {
        HostFeatures result;
        result.zba = (ima_ext0 & kHwprobeExtZba) != 0;
        result.zbb = (ima_ext0 & kHwprobeExtZbb) != 0;
        result.zbs = (ima_ext0 & kHwprobeExtZbs) != 0;
        result.zbc = (ima_ext0 & kHwprobeExtZbc) != 0;
        result.zbkb = (ima_ext0 & kHwprobeExtZbkb) != 0;
        result.zbkc = (ima_ext0 & kHwprobeExtZbkc) != 0;
        result.ztso = (ima_ext0 & kHwprobeExtZtso) != 0;
        result.zicond = (ima_ext0 & kHwprobeExtZicond) != 0;
        result.ual = fast_misaligned;
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
            if (token == "force_all") {
                current = {true, true, true, true, true, true, true, true, true};
                continue;
            }
            bool enable = true;
            if (token.front() == '-' || token.front() == '!') {
                enable = false;
                token.remove_prefix(1);
            } else if (token.front() == '+') {
                token.remove_prefix(1);
            }
            if (token == "bitmanip") {
                current.zba = enable;
                current.zbb = enable;
                current.zbs = enable;
            } else if (token == "zba") {
                current.zba = enable;
            } else if (token == "zbb") {
                current.zbb = enable;
            } else if (token == "zbs") {
                current.zbs = enable;
            } else if (token == "zicond") {
                current.zicond = enable;
            } else if (token == "zbc") {
                current.zbc = enable;
            } else if (token == "zbkb") {
                current.zbkb = enable;
            } else if (token == "zbkc") {
                current.zbkc = enable;
            } else if (token == "ual") {
                current.ual = enable;
            } else if (token == "ztso") {
                current.ztso = enable;
            }
        }
        return current;
    }

    static HostFeatures DetectHardware() noexcept {
        // Query kernel-supported capabilities via SYS_riscv_hwprobe (258) only.
        // Never use SIGILL probing, which would overwrite guest fault handlers.
#if defined(__linux__) && defined(__riscv) && __riscv_xlen == 64
        static const auto features = [] {
            struct HwprobePair {
                std::int64_t key;
                std::uint64_t value;
            };
            constexpr long kSysRiscvHwprobe = 258;
            HwprobePair pairs[3]{
                    {kHwprobeKeyImaExt0, 0},
                    {kHwprobeKeyCpuPerf0, 0},
                    {kHwprobeKeyMisalignedScalarPerf, 0},
            };
            if (syscall(kSysRiscvHwprobe, pairs, 3, 0, nullptr, 0) != 0) return HostFeatures{};
            const std::uint64_t ima_ext0 = pairs[0].key == kHwprobeKeyImaExt0 ? pairs[0].value : 0;
            bool fast_ual = false;
            if (pairs[2].key == kHwprobeKeyMisalignedScalarPerf) {
                fast_ual = pairs[2].value == kHwprobeMisalignedFast;
            } else if (pairs[1].key == kHwprobeKeyCpuPerf0) {
                fast_ual = (pairs[1].value & kHwprobeMisalignedMask) == kHwprobeMisalignedFast;
            }
            return FromHwprobe(ima_ext0, fast_ual);
        }();
        return features;
#else
        return {};
#endif
    }

    static HostFeatures Detect() noexcept {
        const auto hardware = DetectHardware();
        if (const char* override_spec = std::getenv("SVM_RV64_FEATURES"))
            return ApplyEnvOverride(override_spec, hardware);
        return hardware;
    }
};

}  // namespace swift::runtime::backend::riscv64
