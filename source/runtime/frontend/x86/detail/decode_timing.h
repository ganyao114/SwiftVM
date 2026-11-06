#pragma once
#include "../decode.h"
#include <chrono>

namespace swift::x86::detail {
// Construct only after selecting the actual parser. A failed extension probe
// must not add an unaccounted clock read to every legacy instruction.
class DecodeSample {
    DecodeTiming* timing;
    std::chrono::steady_clock::time_point begin{};
public:
    explicit DecodeSample(DecodeTiming* timing) : timing(timing) {
        if (timing) begin=std::chrono::steady_clock::now();
    }
    ~DecodeSample() {
        if (timing) {
            timing->nanoseconds+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()-begin).count());
            ++timing->calls;
        }
    }
};
} // namespace swift::x86::detail
