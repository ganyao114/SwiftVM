#pragma once
#include "case_support.h"
namespace {

using swift::runtime::FeatureSet;

swift::u64 ReadFPCRFromHostHelper() {
    return swift::runtime::backend::arm64::ReadNativeFPCR();
}

swift::u64 WriteMXCSRFromHostHelper(swift::u64 context, swift::u64 mxcsr) {
    auto* thread_context =
            reinterpret_cast<swift::x86::ThreadContext64*>(context);
    thread_context->mxcsr = static_cast<swift::u32>(mxcsr);
    return 0;
}

swift::u64 g_fpcr_transparent_helper_fpcr{};
swift::u64 g_fpcr_transparent_helper_fpsr{};

__attribute__((noinline)) swift::u64 ObserveFPEnvironmentFromTransparentHelper() {
#if defined(__aarch64__)
    asm volatile("mrs %0, fpcr" : "=r"(g_fpcr_transparent_helper_fpcr));
    asm volatile("mrs %0, fpsr" : "=r"(g_fpcr_transparent_helper_fpsr));
#else
    g_fpcr_transparent_helper_fpcr = 0;
    g_fpcr_transparent_helper_fpsr = 0;
#endif
    return swift::u64{0x5a5a5a5a5a5a5a5a};
}

swift::u64 ReadNativeFPSR() {
#if defined(__aarch64__)
    swift::u64 value{};
    asm volatile("mrs %0, fpsr" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

void WriteNativeFPSR(swift::u64 value) {
#if defined(__aarch64__)
    asm volatile("msr fpsr, %0" : : "r"(value) : "memory");
#else
    (void)value;
#endif
}

class ScopedNativeFPCR {
public:
    explicit ScopedNativeFPCR(swift::u64 value)
            : saved(swift::runtime::backend::arm64::ReadNativeFPCR()) {
        swift::runtime::backend::arm64::WriteNativeFPCR(value);
        installed = swift::runtime::backend::arm64::ReadNativeFPCR();
    }

    ~ScopedNativeFPCR() {
        swift::runtime::backend::arm64::WriteNativeFPCR(saved);
    }

    [[nodiscard]] swift::u64 Installed() const { return installed; }

private:
    swift::u64 saved{};
    swift::u64 installed{};
};

class ScopedNativeFPSR {
public:
    explicit ScopedNativeFPSR(swift::u64 value) : saved(ReadNativeFPSR()) {
        WriteNativeFPSR(value);
        installed = ReadNativeFPSR();
    }

    ~ScopedNativeFPSR() { WriteNativeFPSR(saved); }

    [[nodiscard]] swift::u64 Installed() const { return installed; }

private:
    swift::u64 saved{};
    swift::u64 installed{};
};

}  // namespace
