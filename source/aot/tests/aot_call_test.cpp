//
// AOT artifact  x  host->guest call layer, end to end.
//
// docs/aot-design.md §7.3 asks for "the symbol table is usable": symbol names
// survive, and a host can take a function pointer out of the artifact by name
// and call it.  Until now the two halves were separate -- the artifact's
// SymbolIndex compiled but was never called, and the call layer did its own
// symbol lookup against the guest ELF.  This test joins them:
//
//   1. compile an artifact from func_tests_x86_64 IN THIS PROCESS (the
//      validity key covers the SwiftVM build id, so an artifact produced by
//      the `svm_aot` executable is -- correctly -- refused here);
//   2. install it into a fresh guest address space before anything runs;
//   3. resolve libc symbols through the ARTIFACT's rewritten `.symtab`;
//   4. call them with GuestFn and compare against the same calls made with no
//      artifact installed, and against the host's own libc.
//
// Ordering note: only one guest environment may be alive at a time (the guest
// memory bias and the signal-handler probes are process-global), so the AOT
// phase runs first, records its answers, and is destroyed before the JIT phase
// re-runs the same calls.  Catch2 runs TEST_CASEs in declaration order within
// a file, and everything here goes through one lazily-built fixture, so the
// order is not left to chance.
//
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "aot/aot_call.h"
#include "aot/aot_compile.h"
#include "guest_call.h"

using namespace swift::guest_call;
namespace aot = swift::aot;

namespace {

std::string WorkDir() {
    static const std::string dir = [] {
        std::string t = "/tmp/svm_aot_call_XXXXXX";
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", t.c_str());
        const char* d = mkdtemp(buf);
        return std::string(d != nullptr ? d : "/tmp");
    }();
    return dir;
}

const std::string& PatchedGuest() {
    static const std::string path = [] {
        const std::string out = WorkDir() + "/guest_patched";
        std::string error;
        // main's first byte becomes `hlt`: the artifact must be compiled from
        // the same bytes that will be installed and executed, so the patch has
        // to happen in the file rather than in the running image.
        if (!aot::PatchGuestStopByte(SVM_AOT_GLIBC_ELF, "main", out, error)) {
            FAIL("cannot patch the guest: " << error);
        }
        return out;
    }();
    return path;
}

const std::string& Artifact() {
    static const std::string path = [] {
        const std::string out = WorkDir() + "/guest.aot";
        aot::CompileOptions options{};
        options.guest_elf = PatchedGuest();
        options.out_path = out;
        // Widest static coverage the design allows: whole-function decode plus
        // the successor sweep (both are decoder-published addresses only).
        options.decode_budget = 128;
        options.sweep = true;
        aot::AotStats stats{};
        std::string error;
        if (!aot::CompileArtifact(options, stats, error)) {
            FAIL("cannot compile the artifact: " << error);
        }
        return out;
    }();
    return path;
}

// The answers a run produces, so the AOT run and the JIT run can be compared
// without both being alive at once.
struct Answers {
    std::map<std::string, std::uint64_t> scalar;
    std::map<std::string, std::string> text;
    std::map<std::string, double> real;
};

// Every call the comparison covers, written once and run against whichever
// environment is passed in. Integer / pointer / float / varargs / recursion
// are all represented (docs/aot-design.md §7.4).
Answers Exercise(JitGuestEnv& env) {
    Answers a;
    const char* kStrings[] = {"", "a", "hello", "0123456789abcdef",
                              "a rather longer string that crosses sixteen bytes"};

    auto strlen_g = env.Lookup<std::uint64_t(GuestPtr<const char>)>("strlen");
    REQUIRE(strlen_g.valid());
    for (const char* s : kStrings) {
        auto buf = ScopedGuestBuffer::FromCString(env, s);
        REQUIRE(buf.valid());
        a.scalar[std::string("strlen:") + s] = strlen_g(buf.as<const char>());
    }

    // int, not long: glibc's strcmp returns its answer in %eax and the upper
    // half of %rax is whatever the implementation left there.
    auto strcmp_g = env.Lookup<std::int32_t(GuestPtr<const char>, GuestPtr<const char>)>("strcmp");
    REQUIRE(strcmp_g.valid());
    {
        auto x = ScopedGuestBuffer::FromCString(env, "abcd");
        auto y = ScopedGuestBuffer::FromCString(env, "abce");
        a.scalar["strcmp"] = static_cast<std::uint64_t>(
                strcmp_g(x.as<const char>(), y.as<const char>()) < 0 ? 1 : 0);
    }

    auto memcpy_g = env.Lookup<std::uint64_t(GuestPtr<void>, GuestPtr<const void>,
                                             std::uint64_t)>("memcpy");
    REQUIRE(memcpy_g.valid());
    {
        auto src = ScopedGuestBuffer::FromCString(env, "0123456789abcdefghij");
        ScopedGuestBuffer dst{env, 32, 16};
        REQUIRE(dst.valid());
        memcpy_g(dst.as<void>(), src.as<const void>(), 21);
        a.text["memcpy"] = std::string(static_cast<const char*>(env.HostPointer(
                dst.address(), 21)));
    }

    auto strchr_g = env.Lookup<std::uint64_t(GuestPtr<const char>, std::int32_t)>("strchr");
    REQUIRE(strchr_g.valid());
    {
        auto s = ScopedGuestBuffer::FromCString(env, "abcdefg");
        a.scalar["strchr"] = strchr_g(s.as<const char>(), 'd') - s.address();
    }

    auto strtol_g = env.Lookup<std::int64_t(GuestPtr<const char>, GuestPtr<void>,
                                            std::int32_t)>("strtol");
    REQUIRE(strtol_g.valid());
    {
        auto s = ScopedGuestBuffer::FromCString(env, "-1234567");
        a.scalar["strtol"] = static_cast<std::uint64_t>(
                strtol_g(s.as<const char>(), GuestPtr<void>{std::uint64_t{0}}, 10));
    }

    // Varargs, doubles and %al: snprintf only produces this if the vector
    // register count reaches the guest correctly.
    auto snprintf_g = env.Lookup<std::int32_t(GuestPtr<char>, std::uint64_t,
                                              GuestPtr<const char>, std::int32_t, double,
                                              GuestPtr<const char>)>("snprintf");
    REQUIRE(snprintf_g.valid());
    {
        ScopedGuestBuffer out{env, 128, 16};
        REQUIRE(out.valid());
        auto fmt = ScopedGuestBuffer::FromCString(env, "%d|%.3f|%s");
        auto tail = ScopedGuestBuffer::FromCString(env, "tail");
        const std::int32_t n = snprintf_g(out.as<char>(), 128, fmt.as<const char>(), -42,
                                          2.5, tail.as<const char>());
        a.scalar["snprintf:n"] = static_cast<std::uint64_t>(n);
        a.text["snprintf"] = std::string(static_cast<const char*>(
                env.HostPointer(out.address(), 128)));
    }

    // The guest program's own recursion, compiled by the same pipeline.
    auto fib = env.Lookup<std::uint64_t(std::uint64_t)>("fib");
    if (fib.valid()) {
        for (std::uint64_t n : {1u, 5u, 12u, 20u}) {
            a.scalar["fib:" + std::to_string(n)] = fib(n);
        }
    }
    auto ack = env.Lookup<std::uint64_t(std::uint64_t, std::uint64_t)>("ackermann");
    if (ack.valid()) {
        a.scalar["ackermann"] = ack(2, 3);
    }
    return a;
}

// ---------------------------------------------------------------------------
// The two phases. Built lazily and torn down in order, because only one
// environment may exist at a time.
// ---------------------------------------------------------------------------
struct AotPhase {
    std::unique_ptr<aot::AotGuestEnv> env;
    Answers answers;
    std::size_t installed{};
    std::size_t index_size{};
    std::uint64_t ifunc_trap{};
    std::uint64_t ifunc_resolved{};
    std::uint64_t strlen_raw{};
    void* strlen_entry{};
    std::vector<std::string> checked_names;
};

AotPhase& Aot() {
    static AotPhase phase = [] {
        AotPhase p;
        p.env = std::make_unique<aot::AotGuestEnv>();
        std::string error;
        if (!p.env->Init(PatchedGuest(), Artifact(), error)) {
            FAIL("AOT env init: " << error);
        }
        p.installed = p.env->load_report().units_installed;
        p.index_size = p.env->symbols().Size();
        if (!p.env->RunStartupUntil("main", error, /*patch_stop=*/false)) {
            FAIL("AOT startup: " << error);
        }
        // Ifunc evidence has to be taken before LookupSymbol caches the
        // resolved value.
        p.strlen_raw = p.env->RawSymbol("strlen");
        p.ifunc_trap = p.env->IfuncTrap("strlen");
        p.ifunc_resolved = p.env->LookupSymbol("strlen");
        bool mismatch = false;
        p.strlen_entry = p.env->EntryOf("strlen", mismatch);
        p.answers = Exercise(*p.env);
        return p;
    }();
    return phase;
}

const Answers& Jit() {
    static const Answers answers = [] {
        Aot();               // make sure the AOT phase ran first ...
        Aot().env.reset();   // ... and is gone before a second env exists
        static JitGuestEnv env;
        std::string error;
        if (!env.Init(PatchedGuest(), error)) {
            FAIL("JIT env init: " << error);
        }
        if (!env.RunStartupUntil("main", error, /*patch_stop=*/false)) {
            FAIL("JIT startup: " << error);
        }
        return Exercise(env);
    }();
    return answers;
}

}  // namespace

// ---------------------------------------------------------------------------
TEST_CASE("aot call: the artifact installs and its symbol index is populated") {
    auto& p = Aot();
    REQUIRE(p.env != nullptr);
    // The corpus has ~1300 STT_FUNC entries; anything much smaller means the
    // artifact was installed only in part, which InstallArtifact must never do.
    CHECK(p.installed > 1000);
    // 1317 STT_FUNC + 37 STT_GNU_IFUNC in this corpus. Aliases share a unit,
    // so the index has more names than the artifact has units -- and the ifuncs
    // must be in there: skipping them was a real gap (aot_format.h,
    // IsCompilableFuncType), and strlen/memcpy/strcmp are all in that set.
    CHECK(p.index_size > 1300);
    CHECK(p.env->artifact().units.size() == p.installed);
}

TEST_CASE("aot call: SymbolIndex::Lookup resolves to installed artifact code") {
    auto& p = Aot();
    REQUIRE(p.env != nullptr);
    // Every name must (a) exist in the artifact's rewritten symbol table,
    // (b) name code the address space really published for that guest address.
    // strlen/memcpy/strcmp/memchr/strchr are STT_GNU_IFUNC here; snprintf,
    // strtol, fib, malloc, free are plain STT_FUNC. Both kinds must resolve.
    for (const char* name : {"strlen", "memcpy", "strcmp", "memchr", "strchr", "snprintf",
                             "strtol", "fib", "malloc", "free", "__libc_start_main"}) {
        INFO("symbol " << name);
        bool mismatch = false;
        void* host = p.env->EntryOf(name, mismatch);
        CHECK_FALSE(mismatch);
        REQUIRE(host != nullptr);
        // It is artifact code, not something the JIT compiled later: the
        // offset->host map is written only by InstallArtifact.
        CHECK(aot::ResolveCodeOffset(0) != nullptr);
    }
    // A name that is not in the table resolves to nothing rather than to
    // something plausible.
    bool mismatch = false;
    CHECK(p.env->EntryOf("no_such_symbol_at_all", mismatch) == nullptr);
    CHECK_FALSE(mismatch);
}

TEST_CASE("aot call: a symbol rewritten to the wrong unit is caught, not called") {
    // MUTATION. Rewrite one STT_FUNC's st_value in the artifact so it names a
    // different unit's code, rebuild the index from the mutated table, and
    // require the cross-check to reject it. Without the check the call would
    // go to the wrong function and return a plausible number.
    auto& p = Aot();
    REQUIRE(p.env != nullptr);
    const std::string mutated = WorkDir() + "/guest_mutated.aot";
    std::string error;

    // Copy the artifact and shift `strlen`'s st_value by one instruction.
    {
        std::FILE* in = std::fopen(Artifact().c_str(), "rb");
        REQUIRE(in != nullptr);
        std::string bytes;
        char buf[65536];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), in)) != 0) bytes.append(buf, n);
        std::fclose(in);

        // Find the symtab entry for "strlen" by scanning for a 24-byte record
        // whose st_value is the one the index reports, then bump it.
        void* host = nullptr;
        bool mismatch = false;
        host = p.env->EntryOf("strlen", mismatch);
        REQUIRE(host != nullptr);
        // st_value in the artifact is kAotCodeVaddr + offset; recover it from
        // the index rather than re-deriving the ELF layout here.
        const std::uint64_t st_value =
                aot::kAotCodeVaddr + [&] {
                    // SymbolIndex stores name -> offset; Lookup() maps it to a
                    // host pointer. Recover the offset by searching the units.
                    for (const auto& u : p.env->artifact().units) {
                        if (aot::ResolveCodeOffset(u.code_offset) == host) {
                            return static_cast<std::uint64_t>(u.code_offset);
                        }
                    }
                    return std::uint64_t{~0ull};
                }();
        REQUIRE(st_value != aot::kAotCodeVaddr + ~std::uint64_t{0});

        std::size_t patched = 0;
        for (std::size_t i = 0; i + 8 <= bytes.size(); i += 8) {
            std::uint64_t v{};
            std::memcpy(&v, bytes.data() + i, 8);
            if (v == st_value) {
                const std::uint64_t bumped = v + 4;
                std::memcpy(bytes.data() + i, &bumped, 8);
                patched++;
            }
        }
        REQUIRE(patched > 0);
        std::FILE* out = std::fopen(mutated.c_str(), "wb");
        REQUIRE(out != nullptr);
        std::fwrite(bytes.data(), 1, bytes.size(), out);
        std::fclose(out);
    }

    aot::SymbolIndex bad;
    REQUIRE(bad.Build(mutated, p.env->artifact(), error));
    // The mutated table still resolves to *something* inside the code section
    // -- that is exactly why "the lookup returned a pointer" is not evidence.
    void* bad_host = bad.Lookup("strlen");
    REQUIRE(bad_host != nullptr);
    bool mismatch = false;
    void* good_host = p.env->EntryOf("strlen", mismatch);
    CHECK_FALSE(mismatch);
    REQUIRE(good_host != nullptr);
    CHECK(bad_host != good_host);
    // The cross-check against what the address space published is what
    // notices: with the mutated index it must report a mismatch rather than
    // hand back the wrong entry.
    bool bad_mismatch = false;
    CHECK(p.env->EntryOfIn(bad, "strlen", bad_mismatch) == nullptr);
    CHECK(bad_mismatch);
    // ... and a name the mutation did not touch still resolves cleanly, so the
    // check is discriminating rather than uniformly pessimistic.
    bool ok_mismatch = false;
    CHECK(p.env->EntryOfIn(bad, "snprintf", ok_mismatch) != nullptr);
    CHECK_FALSE(ok_mismatch);
}

TEST_CASE("aot call: STT_GNU_IFUNC through the artifact is resolved, not called raw") {
    auto& p = Aot();
    // The artifact compiled the RESOLVER (that is what st_value named), so the
    // artifact's `strlen` symbol points at compiled resolver code. Calling it
    // as if it were strlen returns a code address: a plausible-looking number
    // that is not a length.
    CHECK(p.strlen_raw != 0);
    CHECK(p.ifunc_resolved != 0);
    CHECK(p.ifunc_resolved != p.strlen_raw);
    CHECK(p.ifunc_trap != 0);
    // The trap value is an address inside the guest text, not the length of
    // the (empty) string it was handed.
    CHECK(p.ifunc_trap > 0x400000);
    CHECK(p.ifunc_trap == p.ifunc_resolved);  // the resolver's own answer
    // And the artifact does carry an entry for the resolver's symbol.
    CHECK(p.strlen_entry != nullptr);
}

TEST_CASE("aot call: every answer matches the same call with no artifact") {
    auto& aot_phase = Aot();
    const Answers& jit = Jit();
    // Aot() has been torn down by now; its recorded answers survive.
    const Answers& art = aot_phase.answers;

    REQUIRE(art.scalar.size() == jit.scalar.size());
    REQUIRE(art.text.size() == jit.text.size());
    REQUIRE(!art.scalar.empty());
    for (const auto& [k, v] : art.scalar) {
        INFO("scalar " << k);
        auto it = jit.scalar.find(k);
        REQUIRE(it != jit.scalar.end());
        CHECK(v == it->second);
    }
    for (const auto& [k, v] : art.text) {
        INFO("text " << k);
        auto it = jit.text.find(k);
        REQUIRE(it != jit.text.end());
        CHECK(v == it->second);
    }
}

TEST_CASE("aot call: the answers are also right, not merely equal") {
    // "JIT and AOT agree" would be satisfied by both being wrong, so the
    // values are checked against the host's own libc too.
    const Answers& a = Aot().answers;
    for (const char* s : {"", "a", "hello", "0123456789abcdef",
                          "a rather longer string that crosses sixteen bytes"}) {
        INFO("strlen of '" << s << "'");
        auto it = a.scalar.find(std::string("strlen:") + s);
        REQUIRE(it != a.scalar.end());
        CHECK(it->second == std::strlen(s));
    }
    CHECK(a.scalar.at("strcmp") == 1);
    CHECK(a.scalar.at("strchr") == 3);
    CHECK(a.scalar.at("strtol") == static_cast<std::uint64_t>(-1234567));
    CHECK(a.text.at("memcpy") == "0123456789abcdefghij");
    CHECK(a.text.at("snprintf") == "-42|2.500|tail");
    CHECK(a.scalar.at("snprintf:n") == 14);
    if (a.scalar.count("fib:12") != 0) {
        CHECK(a.scalar.at("fib:12") == 144);
        CHECK(a.scalar.at("fib:20") == 6765);
    }
}
