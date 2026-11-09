#include "numeric.h"
#include "numeric_registry.h"

#include <algorithm>
#include <iterator>

namespace swift::runtime::arm32 {
namespace {
struct Callback {
    std::uint16_t identity;
    NumericCallback function;
};

template <Numeric operation, unsigned bits, bool standard, unsigned widths, bool allow_standard>
constexpr NumericCallback Lane() {
    constexpr unsigned width_bit = bits == 8 ? 1 : bits == 16 ? 2 : bits == 32 ? 4 : 8;
    if constexpr ((widths & width_bit) && (!standard || allow_standard))
        return &NumericLane<operation, bits, standard>;
    else
        return nullptr;
}

constexpr Callback callbacks[]{
#define ARM32_NUMERIC_CALLBACK(name, identity, widths, standard)                                   \
    {1 + identity * 8, Lane<Numeric::name, 8, false, widths, standard>()},                         \
            {2 + identity * 8, Lane<Numeric::name, 8, true, widths, standard>()},                  \
            {3 + identity * 8, Lane<Numeric::name, 16, false, widths, standard>()},                \
            {4 + identity * 8, Lane<Numeric::name, 16, true, widths, standard>()},                 \
            {5 + identity * 8, Lane<Numeric::name, 32, false, widths, standard>()},                \
            {6 + identity * 8, Lane<Numeric::name, 32, true, widths, standard>()},                 \
            {7 + identity * 8, Lane<Numeric::name, 64, false, widths, standard>()},                \
            {8 + identity * 8, Lane<Numeric::name, 64, true, widths, standard>()},
#include "numeric_callbacks.inc"
#undef ARM32_NUMERIC_CALLBACK
};
}  // namespace

std::uint16_t NumericCallbackIdentity(std::uintptr_t address) noexcept {
    if (!address) return 0;
    const auto found = std::find_if(
            std::begin(callbacks), std::end(callbacks), [address](const auto& callback) {
                return reinterpret_cast<std::uintptr_t>(callback.function) == address;
            });
    return found == std::end(callbacks) ? 0 : found->identity;
}

std::uintptr_t ResolveNumericCallbackIdentity(std::uint16_t identity) noexcept {
    const auto found =
            std::lower_bound(std::begin(callbacks),
                             std::end(callbacks),
                             identity,
                             [](const auto& callback, auto id) { return callback.identity < id; });
    return found != std::end(callbacks) && found->identity == identity
                   ? reinterpret_cast<std::uintptr_t>(found->function)
                   : 0;
}
}  // namespace swift::runtime::arm32
