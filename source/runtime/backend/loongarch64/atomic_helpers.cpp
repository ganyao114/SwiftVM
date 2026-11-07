#include "atomic_helpers.h"

namespace swift::runtime::backend::loongarch64 {

AtomicPair CompareExchange128(std::uint64_t address,
                              std::uint64_t expected_low,
                              std::uint64_t expected_high,
                              std::uint64_t desired_low,
                              std::uint64_t desired_high) noexcept {
    using Wide = unsigned __int128;
    auto* pointer = reinterpret_cast<Wide*>(address);
    const auto expected = Wide{expected_low} | (Wide{expected_high} << 64);
    const auto desired = Wide{desired_low} | (Wide{desired_high} << 64);
    auto observed = expected;
    for (;;) {
        const auto before = observed;
        const auto replacement = before == expected ? desired : before;
        // A mismatch must also complete a real locked write cycle. Retry
        // with old->old rather than treating failed CAS as a read-only load;
        // this retains write permission and SMC observation on that path.
        if (__atomic_compare_exchange_n(
                    pointer, &observed, replacement, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return {static_cast<std::uint64_t>(before), static_cast<std::uint64_t>(before >> 64)};
    }
}

}  // namespace swift::runtime::backend::loongarch64
