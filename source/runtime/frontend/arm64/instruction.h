#pragma once
#include "decode.h"

namespace swift::arm64::detail {
enum class Family : std::uint16_t {
#define ARM_FAMILY(name) name,
#include "instruction_families.inc"
#undef ARM_FAMILY
    Count
};
}  // namespace swift::arm64::detail
