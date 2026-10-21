#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
std::string mode;
unsigned probes{};
bool valid_arguments{true};
unsigned long MockAuxval(unsigned long) { return 0; }
long MockSyscall(long number, ...) {
    ++probes;
    va_list args; va_start(args, number);
    auto* probe = va_arg(args, unsigned char*);
    std::int64_t key; std::uint64_t value;
    std::memcpy(&key, probe, 8); std::memcpy(&value, probe + 8, 8);
    valid_arguments &= number == 258 && key == 4 && value == 0;
    valid_arguments &= va_arg(args, int) == 1;
    valid_arguments &= va_arg(args, int) == 0;
    valid_arguments &= va_arg(args, void*) == nullptr;
    valid_arguments &= va_arg(args, int) == 0;
    va_end(args);
    if (mode == "unsupported") { errno = ENOSYS; return -1; }
    if (mode == "unknown") {
        key = -1; value = UINT64_MAX;
        std::memcpy(probe, &key, 8); std::memcpy(probe + 8, &value, 8); return 0;
    }
    value = (1ULL << 4) | (1ULL << 7); // Zbb and Zbc, without V.
    std::memcpy(probe + 8, &value, 8);
    return 0;
}
}  // namespace

// Run deterministic kernel responses in separate processes so the real
// process-wide capability cache and its failure fallback are exercised.
#define syscall MockSyscall
#define getauxval MockAuxval
#include "runtime/backend/riscv64/host_features.h"
#undef getauxval
#undef syscall

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    mode = argv[1];
    if (mode != "supported" && mode != "unsupported" && mode != "unknown") return 2;
    using Features = swift::runtime::backend::riscv64::HostFeatures;
    const auto first = Features::Detect(), second = Features::Detect();
    const bool supported = mode == "supported";
    if (!valid_arguments || probes != 1 || first.zbb != supported || first.zbc != supported ||
        second.zbb != first.zbb || first.zbs || first.vector || first.zkne || first.zknd ||
        first.zknh || first.zacas || first.vector_crypto_aes || first.vector_crypto_sha256 ||
        first.vector_crypto_clmul) {
        std::cerr << "FAIL hwprobe capability cache/fallback: " << mode << '\n'; return 1;
    }
    std::cout << "PASS hwprobe " << mode << " and cached CPU intersection\n";
}
