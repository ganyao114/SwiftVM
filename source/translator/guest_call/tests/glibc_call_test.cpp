//
// Cross-check against REAL glibc code.
//
// func_tests_x86_64 is a statically linked glibc binary with a full symbol
// table (docs/aot-design.md §9: 1317 STT_FUNC entries).  The guest is booted
// through its own _start -- so the ifunc IRELATIVE relocations are applied, TLS
// is installed and the locale tables are up -- and stopped on the first byte of
// main.  From there the call layer calls libc functions by symbol name and the
// results are compared with the host's own libc.
//
// This is the strongest evidence available for the marshalling being right:
// the code on the other side was compiled by GCC from glibc sources against
// the same psABI, and every answer is independently checkable.
//
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "guest_call.h"
#include "test_vm.h"

using namespace swift::guest_call;
using svmtest::Vm;
using svmtest::VmKind;
using Catch::Approx;

namespace {

// Copies a host string into the guest window and keeps it alive for the scope.
struct GuestString {
    explicit GuestString(GuestCallEnv& env, const char* s)
            : buf(ScopedGuestBuffer::FromCString(env, s)) {}
    [[nodiscard]] GuestPtr<const char> ptr() const { return buf.as<const char>(); }
    ScopedGuestBuffer buf;
};

}  // namespace

TEST_CASE("glibc guest: startup reaches main with a usable symbol table") {
    auto& env = Vm(VmKind::Glibc);
    CHECK(env.image().functions.size() > 1000);
    CHECK(env.LookupSymbol("strlen") != 0);
    CHECK(env.LookupSymbol("memcmp") != 0);
    CHECK(env.LookupSymbol("snprintf") != 0);
    CHECK(env.last_unknown_syscall() == 0);
}

TEST_CASE("glibc guest: STT_GNU_IFUNC symbols are resolved, not called directly") {
    auto& env = Vm(VmKind::Glibc);
    // In a static glibc, strlen/memcpy/strchr are ifuncs: st_value is the
    // RESOLVER.  Calling that address as if it were strlen returns a code
    // address -- a plausible-looking number that is not a length.  The lookup
    // must run the resolver instead.
    for (const char* name : {"strlen", "memcpy", "strchr", "memset", "memcmp"}) {
        INFO("symbol " << name);
        REQUIRE(env.IsIfunc(name));
        const std::uint64_t raw = env.RawSymbol(name);
        const std::uint64_t resolved = env.LookupSymbol(name);
        CHECK(raw != 0);
        CHECK(resolved != 0);
        CHECK(resolved != raw);  // the resolver picked a different address
    }
    // And a plain STT_FUNC symbol is passed through untouched.
    CHECK_FALSE(env.IsIfunc("snprintf"));
    CHECK(env.LookupSymbol("snprintf") == env.RawSymbol("snprintf"));
}

TEST_CASE("glibc guest: strlen matches the host") {
    auto& env = Vm(VmKind::Glibc);
    auto strlen_g = env.Lookup<std::uint64_t(GuestPtr<const char>)>("strlen");
    REQUIRE(strlen_g.valid());
    for (const char* s : {"", "a", "hello", "0123456789abcdef",
                          "a rather longer string that crosses the sixteen byte boundary"}) {
        GuestString gs{env, s};
        REQUIRE(gs.buf.valid());
        CHECK(strlen_g(gs.ptr()) == std::strlen(s));
    }
}

TEST_CASE("glibc guest: strcmp / strncmp match the host in sign") {
    auto& env = Vm(VmKind::Glibc);
    auto strcmp_g = env.Lookup<int(GuestPtr<const char>, GuestPtr<const char>)>("strcmp");
    auto strncmp_g =
            env.Lookup<int(GuestPtr<const char>, GuestPtr<const char>, std::uint64_t)>("strncmp");
    REQUIRE(strcmp_g.valid());
    REQUIRE(strncmp_g.valid());
    const char* pairs[][2] = {
            {"abc", "abc"}, {"abc", "abd"}, {"abd", "abc"}, {"", "a"}, {"a", ""},
            {"zzzzzzzzzzzzzzzzzzz", "zzzzzzzzzzzzzzzzzzy"},
    };
    const auto sign = [](int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); };
    for (auto& p : pairs) {
        GuestString a{env, p[0]};
        GuestString b{env, p[1]};
        CHECK(sign(strcmp_g(a.ptr(), b.ptr())) == sign(std::strcmp(p[0], p[1])));
        CHECK(sign(strncmp_g(a.ptr(), b.ptr(), 3)) == sign(std::strncmp(p[0], p[1], 3)));
    }
}

TEST_CASE("glibc guest: memcmp matches the host in sign") {
    auto& env = Vm(VmKind::Glibc);
    auto memcmp_g =
            env.Lookup<int(GuestPtr<const void>, GuestPtr<const void>, std::uint64_t)>("memcmp");
    REQUIRE(memcmp_g.valid());
    unsigned char a[64];
    unsigned char b[64];
    for (int i = 0; i < 64; ++i) {
        a[i] = static_cast<unsigned char>(i * 7);
        b[i] = static_cast<unsigned char>(i * 7);
    }
    ScopedGuestBuffer ga{env, sizeof(a)};
    ScopedGuestBuffer gb{env, sizeof(b)};
    REQUIRE(ga.valid());
    REQUIRE(gb.valid());
    const auto sign = [](int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); };
    for (int flip : {-1, 0, 5, 33, 63}) {
        std::memcpy(b, a, sizeof(a));
        if (flip >= 0) {
            b[flip] ^= 0x80;
        }
        REQUIRE(ga.Write(a, sizeof(a)));
        REQUIRE(gb.Write(b, sizeof(b)));
        CHECK(sign(memcmp_g(ga.as<const void>(), gb.as<const void>(), sizeof(a))) ==
              sign(std::memcmp(a, b, sizeof(a))));
    }
}

TEST_CASE("glibc guest: memcpy and memset move the bytes they should") {
    auto& env = Vm(VmKind::Glibc);
    auto memcpy_g = env.Lookup<GuestPtr<void>(GuestPtr<void>, GuestPtr<const void>,
                                              std::uint64_t)>("memcpy");
    auto memset_g = env.Lookup<GuestPtr<void>(GuestPtr<void>, int, std::uint64_t)>("memset");
    REQUIRE(memcpy_g.valid());
    REQUIRE(memset_g.valid());

    unsigned char src[100];
    for (int i = 0; i < 100; ++i) {
        src[i] = static_cast<unsigned char>(i + 1);
    }
    ScopedGuestBuffer gs{env, sizeof(src)};
    ScopedGuestBuffer gd{env, sizeof(src)};
    REQUIRE(gs.Write(src, sizeof(src)));
    // memset returns its first argument; that also checks an INTEGER return.
    CHECK(memset_g(gd.as<void>(), 0xAB, sizeof(src)).address() == gd.address());
    unsigned char out[100];
    REQUIRE(gd.Read(out, sizeof(out)));
    for (unsigned char c : out) {
        CHECK(c == 0xAB);
    }
    CHECK(memcpy_g(gd.as<void>(), gs.as<const void>(), sizeof(src)).address() == gd.address());
    REQUIRE(gd.Read(out, sizeof(out)));
    CHECK(std::memcmp(out, src, sizeof(src)) == 0);
}

TEST_CASE("glibc guest: strchr returns a guest pointer into the same string") {
    auto& env = Vm(VmKind::Glibc);
    auto strchr_g = env.Lookup<GuestPtr<char>(GuestPtr<const char>, int)>("strchr");
    REQUIRE(strchr_g.valid());
    const char* s = "the quick brown fox";
    GuestString gs{env, s};
    for (int ch : {'t', 'q', 'x', 'z', ' '}) {
        const GuestPtr<char> got = strchr_g(gs.ptr(), ch);
        const char* want = std::strchr(s, ch);
        if (want == nullptr) {
            CHECK(got.address() == 0);
        } else {
            CHECK(got.address() == gs.ptr().address() + (want - s));
        }
    }
}

TEST_CASE("glibc guest: strtol parses like the host") {
    auto& env = Vm(VmKind::Glibc);
    auto strtol_g = env.Lookup<long(GuestPtr<const char>, GuestPtr<void>, int)>("strtol");
    REQUIRE(strtol_g.valid());
    for (const char* s : {"42", "  -17", "0x1f", "9999999999", "abc"}) {
        GuestString gs{env, s};
        for (int base : {0, 10, 16}) {
            CHECK(strtol_g(gs.ptr(), GuestPtr<void>{std::uint64_t{0}}, base) ==
                  std::strtol(s, nullptr, base));
        }
    }
}

TEST_CASE("glibc guest: strstr / memchr / strspn match the host") {
    auto& env = Vm(VmKind::Glibc);
    auto strstr_g = env.Lookup<GuestPtr<char>(GuestPtr<const char>, GuestPtr<const char>)>("strstr");
    auto memchr_g = env.Lookup<GuestPtr<void>(GuestPtr<const void>, int, std::uint64_t)>("memchr");
    auto strspn_g = env.Lookup<std::uint64_t(GuestPtr<const char>, GuestPtr<const char>)>("strspn");
    REQUIRE(strstr_g.valid());
    REQUIRE(memchr_g.valid());
    REQUIRE(strspn_g.valid());

    const char* hay = "the quick brown fox jumps over the lazy dog";
    GuestString gh{env, hay};
    for (const char* needle : {"quick", "fox", "dog", "cat", "the"}) {
        GuestString gn{env, needle};
        const GuestPtr<char> got = strstr_g(gh.ptr(), gn.ptr());
        const char* want = std::strstr(hay, needle);
        if (want == nullptr) {
            CHECK(got.address() == 0);
        } else {
            CHECK(got.address() == gh.ptr().address() + (want - hay));
        }
    }
    for (int ch : {'q', 'z', 'x', '!'}) {
        const GuestPtr<void> got = memchr_g(gh.ptr(), ch, std::strlen(hay));
        const void* want = std::memchr(hay, ch, std::strlen(hay));
        if (want == nullptr) {
            CHECK(got.address() == 0);
        } else {
            CHECK(got.address() ==
                  gh.ptr().address() + (static_cast<const char*>(want) - hay));
        }
    }
    GuestString accept{env, "the "};
    CHECK(strspn_g(gh.ptr(), accept.ptr()) == std::strspn(hay, "the "));
}

TEST_CASE("glibc guest: the program\'s own recursive functions match a host model") {
    auto& env = Vm(VmKind::Glibc);
    // func_tests.c compiles fib/factorial/scramble/helper_leaf as NOINLINE
    // statics, so they are ordinary STT_FUNC symbols. Recursion also means the
    // call runs on a deep guest stack laid out entirely by this layer.
    // (factorial() is not in the binary -- gcc const-folded its only call --
    // so ackermann, which is far more deeply recursive, stands in for it.)
    auto fib = env.Lookup<std::uint64_t(unsigned)>("fib");
    auto ackermann = env.Lookup<std::uint64_t(unsigned, unsigned)>("ackermann");
    auto scramble = env.Lookup<std::uint64_t(std::uint64_t)>("scramble");
    auto helper_leaf = env.Lookup<std::uint64_t(std::uint64_t, unsigned)>("helper_leaf");
    REQUIRE(fib.valid());
    REQUIRE(ackermann.valid());
    REQUIRE(scramble.valid());
    REQUIRE(helper_leaf.valid());

    const auto host_fib = [](unsigned n) {
        std::uint64_t a = 0;
        std::uint64_t b = 1;
        for (unsigned i = 0; i < n; ++i) {
            const std::uint64_t t = a + b;
            a = b;
            b = t;
        }
        return a;
    };
    // n >= 1 only: gcc turned fib into an accumulate-and-halve loop whose
    // n == 0 path calls fib(0xFFFFFFFF), so THIS BINARY's fib(0) recurses
    // until the stack runs out. That is a property of the guest code, not of
    // the call layer -- and the layer reports it as a fault (see the
    // stack-exhaustion case in stub_call_test.cpp) rather than as a value.
    for (unsigned n : {1u, 2u, 5u, 12u, 20u}) {
        CHECK(fib(n) == host_fib(n));
    }
    struct Ack {
        static std::uint64_t Of(unsigned m, unsigned n) {
            if (m == 0) {
                return n + 1;
            }
            if (n == 0) {
                return Of(m - 1, 1);
            }
            return Of(m - 1, static_cast<unsigned>(Of(m, n - 1)));
        }
    };
    for (auto mn : {std::pair<unsigned, unsigned>{0, 5}, {1, 4}, {2, 3}, {3, 3}}) {
        CHECK(ackermann(mn.first, mn.second) == Ack::Of(mn.first, mn.second));
    }
    const auto host_scramble = [](std::uint64_t x) {
        x ^= x >> 29;
        x *= UINT64_C(0x9e3779b185ebca87);
        x ^= x >> 31;
        return x;
    };
    for (std::uint64_t x : {UINT64_C(0), UINT64_C(1), UINT64_C(0xdeadbeefcafe),
                            ~UINT64_C(0)}) {
        CHECK(scramble(x) == host_scramble(x));
    }
    const auto host_leaf = [](std::uint64_t x, unsigned shift) {
        x ^= UINT64_C(0xd6e8feb86659fd93) + shift;
        return (x << shift) | (x >> (64u - shift));
    };
    for (unsigned shift : {1u, 7u, 31u, 63u}) {
        CHECK(helper_leaf(0x0123456789abcdefULL, shift) ==
              host_leaf(0x0123456789abcdefULL, shift));
    }
}

TEST_CASE("glibc guest: snprintf is a real variadic call that depends on %al") {
    auto& env = Vm(VmKind::Glibc);
    auto snprintf_g =
            env.Lookup<int(GuestPtr<char>, std::uint64_t, GuestPtr<const char>, ...)>("snprintf");
    REQUIRE(snprintf_g.valid());

    ScopedGuestBuffer out{env, 128};
    REQUIRE(out.valid());
    char host[128];

    {
        GuestString fmt{env, "%d/%d"};
        const int n = snprintf_g(out.as<char>(), 128, fmt.ptr(), 7, 9);
        const int hn = std::snprintf(host, sizeof(host), "%d/%d", 7, 9);
        char back[128] = {};
        REQUIRE(out.Read(back, sizeof(back)));
        CHECK(n == hn);
        CHECK(std::string(back) == host);
    }
    {
        // Doubles in the variadic tail: the callee only fills its vector
        // register save area when %al is non-zero, so a caller that forgets it
        // prints garbage here.
        GuestString fmt{env, "%.3f %.3f %.3f"};
        const int n = snprintf_g(out.as<char>(), 128, fmt.ptr(), 1.5, 2.25, 3.125);
        const int hn = std::snprintf(host, sizeof(host), "%.3f %.3f %.3f", 1.5, 2.25, 3.125);
        char back[128] = {};
        REQUIRE(out.Read(back, sizeof(back)));
        CHECK(n == hn);
        CHECK(std::string(back) == host);
    }
    {
        // A float is promoted to double at the call site, exactly as C would.
        GuestString fmt{env, "%.2f"};
        const int n = snprintf_g(out.as<char>(), 128, fmt.ptr(), 2.5f);
        char back[128] = {};
        REQUIRE(out.Read(back, sizeof(back)));
        CHECK(n == 4);
        CHECK(std::string(back) == "2.50");
    }
    {
        // Mixed integers and doubles: two independent counters, and %al = 2.
        GuestString fmt{env, "%d %.1f %d %.1f"};
        const int n = snprintf_g(out.as<char>(), 128, fmt.ptr(), 3, 1.5, 4, 2.5);
        const int hn = std::snprintf(host, sizeof(host), "%d %.1f %d %.1f", 3, 1.5, 4, 2.5);
        char back[128] = {};
        REQUIRE(out.Read(back, sizeof(back)));
        CHECK(n == hn);
        CHECK(std::string(back) == host);
    }
    {
        // A string argument is a guest pointer; passing a host char* here would
        // not compile (see negative_compile_test.cpp).
        GuestString fmt{env, "[%s]"};
        GuestString arg{env, "text"};
        const int n = snprintf_g(out.as<char>(), 128, fmt.ptr(), arg.ptr());
        char back[128] = {};
        REQUIRE(out.Read(back, sizeof(back)));
        CHECK(n == 6);
        CHECK(std::string(back) == "[text]");
    }
}

TEST_CASE("glibc guest: many calls in a row leave the environment intact") {
    auto& env = Vm(VmKind::Glibc);
    auto strlen_g = env.Lookup<std::uint64_t(GuestPtr<const char>)>("strlen");
    GuestString gs{env, "abcdefghij"};
    for (int i = 0; i < 200; ++i) {
        REQUIRE(strlen_g(gs.ptr()) == 10);
    }
}
