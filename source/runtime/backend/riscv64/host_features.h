#pragma once

#include <cstdint>

#if defined(__linux__) && defined(__riscv) && __riscv_xlen == 64
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace swift::runtime::backend::riscv64 {

struct HostFeatures {
    bool zbb{};
    bool zbc{};
    bool zbs{};
    bool zkne{};
    bool zknd{};
    bool zknh{};
    bool zacas{};
    bool vector{};
    bool vector_crypto_aes{};
    bool vector_crypto_sha256{};
    bool vector_crypto_clmul{};

    static HostFeatures Detect() {
        // No instruction probes: installing process-wide SIGILL handlers
        // races with guest fault recovery and concurrent host threads.
        static const auto features = [] {
            HostFeatures result;
#if defined(__linux__) && defined(__riscv) && __riscv_xlen == 64
            result.vector = (getauxval(AT_HWCAP) & (1UL << ('V' - 'A'))) != 0;
#ifdef SYS_riscv_hwprobe
            // Linux UAPI key 4, IMA_EXT_0. NULL CPU set intersects all online
            // CPUs, so emitted code remains valid after thread migration.
            struct Probe { std::int64_t key; std::uint64_t value; } probe{4, 0};
            if (syscall(SYS_riscv_hwprobe, &probe, 1, 0, nullptr, 0) == 0 && probe.key == 4) {
                result.vector = (probe.value & (1ULL << 2)) != 0;
                result.zbb = (probe.value & (1ULL << 4)) != 0;
                result.zbs = (probe.value & (1ULL << 5)) != 0;
                result.zbc = (probe.value & ((1ULL << 7) | (1ULL << 9))) != 0;
                result.zknd = (probe.value & (1ULL << 11)) != 0;
                result.zkne = (probe.value & (1ULL << 12)) != 0;
                result.zknh = (probe.value & (1ULL << 13)) != 0;
                result.zacas = (probe.value & (1ULL << 34)) != 0;
                result.vector_crypto_aes = result.vector && (probe.value & (1ULL << 21));
                result.vector_crypto_sha256 = result.vector && (probe.value & ((1ULL << 22) | (1ULL << 23)));
                result.vector_crypto_clmul = result.vector && (probe.value & (1ULL << 18));
            }
#endif
#endif
            return result;
        }();
        auto result = features;
#if defined(__linux__) && defined(__riscv) && __riscv_xlen == 64 && defined(PR_RISCV_V_GET_CONTROL)
        // Thread policy is separate from CPU capability. Respect an explicit
        // disable; never change the parent's configured vector policy.
        if (result.vector) {
            const auto policy = prctl(PR_RISCV_V_GET_CONTROL);
            if (policy >= 0 && (policy & PR_RISCV_V_VSTATE_CTRL_CUR_MASK) == PR_RISCV_V_VSTATE_CTRL_OFF)
                result.vector = result.vector_crypto_aes = result.vector_crypto_sha256 = result.vector_crypto_clmul = false;
        }
#endif
        return result;
    }
};

}  // namespace swift::runtime::backend::riscv64
