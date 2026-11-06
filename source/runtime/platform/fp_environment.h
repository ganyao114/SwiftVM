#pragma once

#include <cfenv>
#include <cstdint>

namespace swift::runtime::platform {

enum class FloatRounding : std::uint8_t { Nearest, Down, Up, TowardZero };

// A numerical scope for C++ guest operations, separate from native-code FP
// ownership. Callers implement input/output flushing explicitly. The complete
// caller environment, including exception status and masks, is restored.
class HostFloatScope {
public:
    explicit HostFloatScope(FloatRounding rounding) {
        std::feholdexcept(&previous);
#if defined(__aarch64__)
        std::uint64_t fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        fpcr &= ~((std::uint64_t{1} << 24) | (std::uint64_t{1} << 19) |
                  (std::uint64_t{1} << 25) | (std::uint64_t{1} << 26) | std::uint64_t{7});
        asm volatile("msr fpcr, %0" :: "r"(fpcr));
#elif defined(__x86_64__) || defined(__i386__)
        std::uint32_t controls;
        asm volatile("stmxcsr %0" : "=m"(controls));
        controls &= ~((std::uint32_t{1} << 6) | (std::uint32_t{1} << 15));
        asm volatile("ldmxcsr %0" :: "m"(controls));
#endif
        constexpr int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
        std::fesetround(modes[static_cast<unsigned>(rounding)]);
    }
    ~HostFloatScope() { std::fesetenv(&previous); }
    HostFloatScope(const HostFloatScope&) = delete;
    HostFloatScope& operator=(const HostFloatScope&) = delete;
private:
    std::fenv_t previous;
};

} // namespace swift::runtime::platform
