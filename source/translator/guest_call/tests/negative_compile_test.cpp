//
// The negative half of the GuestPtr decision: misuse must fail to COMPILE.
//
// A test that only checks the positive cases would pass just as happily if the
// layer silently accepted a host `const char*` and handed guest code a host
// address.  These checks assert that the ill-formed cases really are
// ill-formed, using detection idioms so the file itself still compiles.
//
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>
#include <vector>

#include "abi_sysv.h"
#include "guest_call.h"

using namespace swift::guest_call;

namespace {

// --- 1. raw host pointers are not marshalable -------------------------------
static_assert(!kIsMarshalable<const char*>);
static_assert(!kIsMarshalable<char*>);
static_assert(!kIsMarshalable<void*>);
static_assert(!kIsMarshalable<int*>);
static_assert(kIsMarshalable<GuestPtr<const char>>);
static_assert(kIsMarshalable<GuestPtr<char>>);

// --- 2. GuestPtr cannot be built from a host pointer ------------------------
// The deleted constructors are what make the mistake visible at the call site
// rather than at the far end of a marshalled argument.
template <typename T, typename Arg>
concept ConstructibleFrom = requires(Arg a) { GuestPtr<T>{a}; };

static_assert(!ConstructibleFrom<char, char*>);
static_assert(!ConstructibleFrom<char, const char*>);
static_assert(!ConstructibleFrom<char, std::nullptr_t>);
static_assert(ConstructibleFrom<char, std::uint64_t>);  // the only way in

// --- 3. GuestPtr does not implicitly convert to or from an integer ----------
static_assert(!std::is_convertible_v<std::uint64_t, GuestPtr<char>>);  // explicit ctor
static_assert(!std::is_convertible_v<GuestPtr<char>, std::uint64_t>);

// --- 4. guest pointers of unrelated types do not interconvert ---------------
static_assert(!std::is_convertible_v<GuestPtr<char>, GuestPtr<double>>);
static_assert(std::is_convertible_v<GuestPtr<char>, GuestPtr<const char>>);
static_assert(!std::is_convertible_v<GuestPtr<const char>, GuestPtr<char>>);
static_assert(std::is_convertible_v<GuestPtr<char>, GuestPtr<void>>);

// --- 5. a host pointer cannot be passed to a GuestFn ------------------------
// The call expression is checked without instantiating the class body, so the
// negative case is detectable rather than a hard error.
template <typename Fn, typename... A>
concept Callable = requires(const Fn& f, A... a) { f(a...); };

using StrlenFn = GuestFn<std::uint64_t(GuestPtr<const char>)>;
static_assert(Callable<StrlenFn, GuestPtr<const char>>);
static_assert(Callable<StrlenFn, GuestPtr<char>>);   // adds const, allowed
static_assert(!Callable<StrlenFn, const char*>);     // <- the whole point
static_assert(!Callable<StrlenFn, char*>);
static_assert(!Callable<StrlenFn, std::uint64_t>);   // no implicit int->guest ptr
static_assert(!Callable<StrlenFn, std::nullptr_t>);

// --- 6. non-trivially-copyable types are refused ----------------------------
static_assert(!kIsMarshalable<std::string>);
static_assert(!kIsMarshalable<std::vector<int>>);
struct NonTrivial {
    NonTrivial() = default;
    NonTrivial(const NonTrivial&) {}
    int x;
};
static_assert(!kIsMarshalable<NonTrivial>);

// --- 7. long double has no faithful host representation here ----------------
static_assert(!kIsMarshalable<long double>);
static_assert(!kIsMarshalable<int&>);

}  // namespace

TEST_CASE("host pointers cannot cross the guest boundary") {
    // All the substance is above; keep one runtime check so the intent is
    // visible in the test list rather than only in the build log.
    STATIC_REQUIRE(!kIsMarshalable<const char*>);
    STATIC_REQUIRE(!Callable<StrlenFn, const char*>);
    STATIC_REQUIRE(Callable<StrlenFn, GuestPtr<const char>>);
}
