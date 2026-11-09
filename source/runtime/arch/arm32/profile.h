#pragma once
#include <cstdint>

namespace swift::runtime::arm32 {
enum class ArchVersion : std::uint8_t { v3 = 3, v4 = 4, v4T, v5TE, v6K, v6T2, v7, v8 };

struct Profile {
    ArchVersion version{ArchVersion::v7};
    bool vfp{true}, neon{true}, divide{true}, crc32{};
    bool fp16{true}, vfpv4{true}, crypto{};

    constexpr unsigned Version() const noexcept {
        switch (version) {
            case ArchVersion::v3:
                return 3;
            case ArchVersion::v4:
            case ArchVersion::v4T:
                return 4;
            case ArchVersion::v5TE:
                return 5;
            case ArchVersion::v6K:
            case ArchVersion::v6T2:
                return 6;
            case ArchVersion::v7:
                return 7;
            case ArchVersion::v8:
                return 8;
        }
        return 7;
    }

    constexpr std::uint64_t Identity() const noexcept {
        return unsigned(version) | (std::uint64_t(vfp) << 8) | (std::uint64_t(neon) << 9) |
               (std::uint64_t(divide) << 10) | (std::uint64_t(crc32) << 11) |
               (std::uint64_t(fp16) << 12) | (std::uint64_t(vfpv4) << 13) |
               (std::uint64_t(crypto) << 14);
    }
};
}  // namespace swift::runtime::arm32
