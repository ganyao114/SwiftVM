#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::backend {

// Facts describe the physical view, not a promise that every use of an SSA
// integer may read the entire X register holding that view.
struct GPRWidthFacts {
    u8 known_zero_above{};
    u8 sign_extended_from{};
    u8 sign_extended_to{};

    [[nodiscard]] bool KnownZeroAbove(u32 bits) const {
        return known_zero_above != 0 && known_zero_above <= bits;
    }
    [[nodiscard]] bool KnownSignExtended(u32 from, u32 to) const {
        return (sign_extended_from != 0 && sign_extended_from <= from &&
                sign_extended_to >= to) ||
               (known_zero_above != 0 && known_zero_above < from);
    }
    bool operator==(const GPRWidthFacts&) const = default;
};

struct FixedGPRValue {
    u16 home{};
    u8 width{};
    GPRWidthFacts extension{};
    bool operator==(const FixedGPRValue&) const = default;
};

}  // namespace swift::runtime::backend
