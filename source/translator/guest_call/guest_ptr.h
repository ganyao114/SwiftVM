//
// GuestPtr<T> — a guest virtual address that is *typed*, and that is a
// different C++ type from any host pointer.
//
// WHY THIS EXISTS (docs/aot-design.md §6, "指针参数的所有权是设计里最尖锐的一处").
// A host `const char*` names host memory; guest code can only dereference guest
// addresses. The two are indistinguishable at runtime (both are 64-bit
// integers), so a call layer that accepted `const char*` would compile happily
// and hand the guest a pointer into the host heap. There are exactly two ways
// out:
//
//   1. only accept addresses that are already in the guest window, and make
//      passing a host pointer a *compile* error;
//   2. have the call layer copy the pointee into guest memory and copy back.
//
// This file implements (1). (2) exists too, but as an explicit, visibly-scoped
// helper — ScopedGuestBuffer — layered on top, never as the default. Under (2)
// as a default, "who owns this block" and "when is it copied back" become
// runtime conventions that no compiler checks; under (1) the mistake is a
// compile error and the copy, when you want one, is a named object with a
// visible lifetime.
//
// GuestPtr is deliberately *not* dereferenceable. Reading guest memory needs
// the address space that owns it (the bias, the window mask, the mapping
// oracle), so it goes through GuestCallEnv::Host<T>() — not through this type.
//

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace swift::guest_call {

// A guest virtual address pointing at a T.  `T` is documentation plus type
// safety between different guest pointer types; it is never dereferenced here.
template <typename T>
class GuestPtr {
public:
    using element_type = T;

    constexpr GuestPtr() = default;
    // Explicit: a guest address only ever comes from something that knows it
    // is a guest address (a symbol lookup, an allocation in the guest window,
    // a value the guest itself produced).
    constexpr explicit GuestPtr(std::uint64_t addr) : addr_(addr) {}

    // Deliberately absent: construction from ANY host pointer. A host address
    // is not a guest address, and there is no conversion that could make it
    // one -- so the mistake is caught at the call site, not at the far end of
    // a marshalled argument.
    template <typename U>
    GuestPtr(U*) = delete;
    GuestPtr(std::nullptr_t) = delete;

    // Implicit widening to a more-const T or to void, mirroring the C pointer
    // conversions, so GuestPtr<char> can be passed where GuestPtr<const char>
    // or GuestPtr<const void> is expected. The reverse never converts.
    template <typename U>
        requires(!std::is_same_v<U, T> &&
                 (std::is_same_v<std::remove_const_t<U>, std::remove_const_t<T>> ||
                  std::is_void_v<std::remove_const_t<T>>) &&
                 (std::is_const_v<T> || !std::is_const_v<U>))
    constexpr GuestPtr(GuestPtr<U> other) : addr_(other.address()) {}

    [[nodiscard]] constexpr std::uint64_t address() const { return addr_; }
    [[nodiscard]] constexpr explicit operator bool() const { return addr_ != 0; }

    // Pointer arithmetic in guest address space (elements, like C).
    constexpr GuestPtr operator+(std::int64_t n) const
        requires(!std::is_void_v<T>)
    {
        return GuestPtr{addr_ + static_cast<std::uint64_t>(n) * sizeof(T)};
    }
    constexpr GuestPtr operator-(std::int64_t n) const
        requires(!std::is_void_v<T>)
    {
        return GuestPtr{addr_ - static_cast<std::uint64_t>(n) * sizeof(T)};
    }

    friend constexpr bool operator==(GuestPtr a, GuestPtr b) { return a.addr_ == b.addr_; }
    friend constexpr auto operator<=>(GuestPtr a, GuestPtr b) { return a.addr_ <=> b.addr_; }

private:
    std::uint64_t addr_{};
};

template <typename T>
struct IsGuestPtr : std::false_type {};
template <typename T>
struct IsGuestPtr<GuestPtr<T>> : std::true_type {};
template <typename T>
inline constexpr bool kIsGuestPtr = IsGuestPtr<T>::value;

static_assert(sizeof(GuestPtr<char>) == 8);
static_assert(std::is_trivially_copyable_v<GuestPtr<char>>);

}  // namespace swift::guest_call
