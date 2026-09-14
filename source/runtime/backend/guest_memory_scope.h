#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::backend {

// Host helpers share the address mapping of the Runtime that called them.
// A scope is entered for both JIT and interpreted execution. Restoring the
// previous scope also supports a host callback entering another Runtime.
class GuestMemoryScope final {
public:
    struct Mapping {
        u64 bias{};
        u64 mask{UINT64_MAX};
    };

    GuestMemoryScope(void* memory_base, u64 address_mask)
            : mapping{reinterpret_cast<u64>(memory_base),
                      address_mask ? address_mask : UINT64_MAX},
              previous(current) {
        current = &mapping;
    }

    ~GuestMemoryScope() { current = previous; }
    GuestMemoryScope(const GuestMemoryScope&) = delete;
    GuestMemoryScope& operator=(const GuestMemoryScope&) = delete;

    [[nodiscard]] static const Mapping& Current() {
        // Standalone helpers used without a Runtime retain direct addressing.
        static constexpr Mapping direct{};
        return current ? *current : direct;
    }

private:
    const Mapping mapping;
    const Mapping* const previous;
    inline static thread_local const Mapping* current{};
};

}  // namespace swift::runtime::backend
