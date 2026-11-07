#include "aot/call.h"
#include "aot/compile.h"
#include "runtime/platform/linux/artifact_file.h"
#include "tests/support/aot_fixture.h"
#include "tests/support/check.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unistd.h>

namespace {
namespace aot = swift::aot;
namespace execution = swift::runtime::execution;
using U64 = std::uint64_t;
using namespace swift::guest_call;

struct Mixed {
    U64 integer;
    double real;
};

struct Big {
    U64 a, b, c;
};

class Directory {
public:
    Directory() {
        const auto* configured = std::getenv("SVM_TEST_SCRATCH");
#if defined(__ANDROID__)
        const std::string base = configured ? configured : "/data/local/tmp";
#else
        const std::string base = configured ? configured : ".";
#endif
        auto pattern = base + "/swiftvm-aot-call-XXXXXX";
        const auto* created = mkdtemp(pattern.data());
        REQUIRE(created);
        path = created;
    }

    ~Directory() {
        std::error_code ignored;
        if (!path.empty()) std::filesystem::remove_all(path, ignored);
    }

    std::string path;
};

class Fixture {
public:
    explicit Fixture(swift::runtime::ir::Scope scope, bool startup = false) {
        const auto guest = directory.path + "/guest.elf";
        const auto patched = directory.path + "/patched.elf";
        artifact = directory.path + "/guest.aot";
        auto bytes = startup ? swift::tests::aot_fixture::StartupGuest()
                             : swift::tests::aot_fixture::Guest();
        swift::runtime::platform::WriteArtifactFile(guest, bytes);
        std::string error;
        if (startup) {
            const auto succeeded = aot::PatchGuestStopByte(guest, "main", patched, error);
            INFO(error);
            REQUIRE(succeeded);
        }
        aot::CompileOptions options;
        options.guest_elf = startup ? patched : guest;
        options.output = artifact;
        options.runtime.scope = scope;
        aot::ArtifactStats stats;
        const auto compiled = aot::CompileArtifact(options, stats, error);
        INFO(error);
        REQUIRE(compiled);
        REQUIRE(stats.compiled_units > 0);
        REQUIRE(unlink(guest.c_str()) == 0);
        if (startup) REQUIRE(unlink(patched.c_str()) == 0);
    }

    std::string artifact;

private:
    Directory directory;
};

void Calls(swift::runtime::ir::Scope scope) {
    const Fixture fixture{scope};
    aot::AotGuestEnv env;
    std::string error;
    const auto initialized = env.Init(fixture.artifact, error);
    INFO(error);
    REQUIRE(initialized);
    CHECK(env.Stats().aot_units_loaded > 0);
    const auto sum = env.Lookup<U64(U64, U64, U64, U64, U64, U64, U64)>("sum7");
    REQUIRE(sum.valid());
    CHECK(sum(1, 2, 3, 4, 5, 6, 7) == 28);
    const auto alias = env.Lookup<U64(U64, U64, U64, U64, U64, U64, U64)>("sum_alias");
    CHECK(alias(7, 6, 5, 4, 3, 2, 1) == 28);
    CHECK(env.Lookup<double(double, double)>("fp_add")(1.25, 2.5) == 3.75);
    const auto mixed = env.Lookup<Mixed(Mixed)>("mixed")({17, 1.5});
    CHECK(mixed.integer == 18);
    CHECK(mixed.real == 3.0);
    const auto big = env.Lookup<Big(U64)>("big")(12);
    CHECK(big.a == 12 && big.b == 13 && big.c == 14);
    CHECK(env.Lookup<U64(U64, ...)>("variadic")(42, 1.5f, 2.5) == 2);
    CHECK(env.Lookup<U64(U64)>("increment")(2) == 42);
    CHECK(env.Lookup<U64(U64)>("increment")(5) == 47);
    U64 global{};
    const auto* pointer = env.HostPointer(env.LookupSymbol("global"), sizeof(global));
    REQUIRE(pointer);
    std::memcpy(&global, pointer, sizeof(global));
    CHECK(global == 47);
    const auto implementation = env.LookupSymbol("choose");
    CHECK(implementation == swift::tests::aot_fixture::Nine);
    CHECK(env.Lookup<U64()>("choose")() == 9);
    // CPUID calls the relocatable host bridge, then resumes a separate guest
    // continuation. The expected maximum basic leaf is independently fixed.
    CHECK(env.Lookup<U64()>("cpu")() == 7);
    {
        ScopedGuestBuffer memory{env, sizeof(U64)};
        const U64 expected = UINT64_C(0x123456789abcdef0);
        REQUIRE(memory.Write(&expected, sizeof(expected)));
        const auto read = env.Lookup<U64(GuestPtr<const U64>)>("read");
        CHECK(read(memory.as<const U64>()) == expected);
        // No original file exists, no interpreter is allowed, and no fallback
        // compiler should be needed for any of these covered entries.
        CHECK(env.Stats().compile_attempts == 0);
        CHECK(env.Stats().interpreted_steps == 0);
        CHECK(env.Stats().native_bytes > 0);
        const auto fault = read.Try(GuestPtr<const U64>{U64{0}});
        CHECK(!fault);
        CHECK(fault.status() == GuestCallStatus::GuestFault);
        CHECK(read(memory.as<const U64>()) == expected);
    }
    CHECK(env.Close().Complete());
}
}  // namespace

TEST_CASE("AOT Block entries support typed native guest calls after source removal",
          "[native][aot]") {
    Calls(swift::runtime::ir::Scope::Block);
}

TEST_CASE("AOT Function entries support typed native guest calls after source removal",
          "[native][aot]") {
    Calls(swift::runtime::ir::Scope::Function);
}

TEST_CASE("AOT startup reaches a precompiled stop snapshot and retains initialized globals",
          "[native][aot]") {
    const Fixture fixture{swift::runtime::ir::Scope::Function, true};
    aot::AotGuestEnv env;
    std::string error;
    const auto initialized = env.Init(fixture.artifact, error);
    INFO(error);
    REQUIRE(initialized);
    CHECK(!env.RunStartupUntil("main", error, true));
    CHECK(error.find("before compilation") != std::string::npos);
    const auto started = env.RunStartupUntil("main", error);
    INFO(error);
    REQUIRE(started);
    CHECK(env.Context().pc == swift::tests::aot_fixture::Main);
    CHECK(env.Lookup<U64(U64)>("increment")(3) == 0x1237);
    CHECK(env.Lookup<U64(U64)>("increment")(1) == 0x1238);
    CHECK(env.Stats().compile_attempts == 0);
    CHECK(env.Stats().interpreted_steps == 0);
    CHECK(env.Close().Complete());
}
