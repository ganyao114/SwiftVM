#include <catch2/catch_test_macros.hpp>
#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include "base/logging.h"
#include "runtime/backend/arm64/jit/placement_experiment.h"
#include "runtime/common/logging.h"
#include "runtime/common/signal_diagnostic.h"
#include "translator/linux/syscall_diagnostics.h"

TEST_CASE("register dump syntax requires two complete unsigned numbers", "[diagnostic-helpers]") {
    using swift::linux::RegisterDumpSpec;
    for (const auto* invalid : {"", "1", "1:", ":1", "1:0", "-1:1", "1:-1",
                               "1:2tail", "1:2:3", " 1:2", "1: 2",
                               "18446744073709551616:1", "1:18446744073709551616"}) {
        INFO(invalid);
        CHECK_FALSE(RegisterDumpSpec::Parse(invalid));
    }
    errno = EDOM;
    const auto spec = RegisterDumpSpec::Parse("0x20:010");
    REQUIRE(spec);
    CHECK(spec->syscall == 32);
    CHECK(spec->occurrence == 8);
    CHECK(errno == EDOM);
}

TEST_CASE("register dump counts are process local and fire once", "[diagnostic-helpers]") {
    swift::linux::RegisterDumpTrigger first{"7:16"};
    swift::linux::RegisterDumpTrigger second{"7:1"};
    std::atomic<unsigned> fired{0};
    {
        std::array<std::jthread, 4> workers;
        for (auto& worker : workers) {
            worker = std::jthread([&] {
                for (unsigned i = 0; i < 16; ++i) {
                    if (first.Take(8)) ++fired;
                    if (first.Take(7)) ++fired;
                }
            });
        }
    }
    CHECK(fired == 1);
    CHECK(second.Take(7));
    CHECK_FALSE(second.Take(7));
    CHECK_FALSE(first.Take(7));
}

TEST_CASE("logging uses inclusive thresholds and skips disabled arguments", "[diagnostic-helpers]") {
    using namespace swift::log;
    struct Restore {
        Level saved{GetLogLevel()};
        ~Restore() { SetLogLevel(saved); }
    } restore;
    const std::array levels{Level::Debug, Level::Info, Level::Warning, Level::Error};
    for (auto threshold : levels) {
        swift::runtime::log::SetLogLevel(threshold);
        for (auto level : levels) CHECK(Enabled(level) == (level >= threshold));
    }
    SetLogLevel(Level::Error);
    int evaluated = 0;
    LOG_DEBUG("{}", ++evaluated);
    CHECK(evaluated == 0);
    // Invalid formatting would throw if disabled direct calls formatted first.
    CHECK_NOTHROW(LogMessage(Level::Debug, __FILE__, __LINE__, __func__, "{"));
    SetLogLevel(Level::Max);
    CHECK_FALSE(Enabled(Level::Error));
    CHECK_FALSE(Enabled(Level::Max));
}

TEST_CASE("enabled host logs go to stderr", "[diagnostic-helpers]") {
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    REQUIRE(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    const int saved_stderr = dup(STDERR_FILENO);
    REQUIRE(saved_stderr >= 0);
    const auto saved_level = swift::log::GetLogLevel();
    struct Restore {
        int* fds;
        int saved;
        swift::log::Level level;
        ~Restore() {
            fflush(stderr);
            dup2(saved, STDERR_FILENO);
            close(saved);
            close(fds[0]);
            close(fds[1]);
            swift::log::SetLogLevel(level);
        }
    } restore{fds, saved_stderr, saved_level};
    fflush(stderr);
    REQUIRE(dup2(fds[1], STDERR_FILENO) >= 0);
    swift::log::SetLogLevel(swift::log::Level::Error);
    int evaluated = 0;
    LOG_ERROR("host diagnostic {}", ++evaluated);
    fflush(stderr);
    REQUIRE(dup2(saved_stderr, STDERR_FILENO) >= 0);
    char buffer[1024];
    const auto count = read(fds[0], buffer, sizeof(buffer));
    REQUIRE(count > 0);
    CHECK(evaluated == 1);
    CHECK(std::string(buffer, count).find("host diagnostic 1") != std::string::npos);
}

TEST_CASE("diagnostic channels skip disabled arguments independently", "[diagnostic-helpers]") {
    using namespace swift::log;
    const auto channel = DiagnosticChannel::Syscall;
    struct Restore {
        DiagnosticChannel channel;
        bool enabled;
        ~Restore() { SetDiagnosticChannelEnabled(channel, enabled); }
    } restore{channel, DiagnosticEnabled(channel)};
    const bool runtime_enabled = DiagnosticEnabled(DiagnosticChannel::Runtime);
    SetDiagnosticChannelEnabled(channel, false);
    int evaluated = 0;
    SVM_DIAG_FORMAT(Syscall, "{}", ++evaluated);
    SVM_DIAG_PRINT(Syscall, "%d", ++evaluated);
    CHECK(evaluated == 0);
    CHECK(DiagnosticEnabled(DiagnosticChannel::Runtime) == runtime_enabled);
    CHECK_FALSE(DiagnosticEnabled(DiagnosticChannel::Count));
    CHECK_FALSE(DiagnosticEnabled(static_cast<DiagnosticChannel>(999)));
    SetDiagnosticChannelEnabled(channel, true);
    CHECK(DiagnosticEnabled(channel));
}

TEST_CASE("signal diagnostics are bounded and preserve errno", "[diagnostic-helpers]") {
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    struct Close {
        int* fds;
        ~Close() { close(fds[0]); close(fds[1]); }
    } close_fds{fds};
    swift::runtime::SignalDiagnostic line{"[fault]"};
    line.Field("pc", 0x1234);
    errno = EDOM;
    line.Write(fds[1]);
    CHECK(errno == EDOM);
    char buffer[1024];
    const auto count = read(fds[0], buffer, sizeof(buffer));
    REQUIRE(count > 0);
    CHECK(std::string(buffer, count) == "[fault] pc=0x1234\n");
    swift::runtime::SignalDiagnostic long_line{std::string(2048, 'x').c_str()};
    long_line.Write(fds[1]);
    const auto truncated = read(fds[0], buffer, sizeof(buffer));
    REQUIRE(truncated == 768);
    CHECK(buffer[truncated - 1] == '\n');
    std::atomic<uint32_t> samples{0};
    CHECK(swift::runtime::ClaimDiagnosticSample(samples, 1));
    CHECK_FALSE(swift::runtime::ClaimDiagnosticSample(samples, 1));
    CHECK(samples == 1);
}

TEST_CASE("layout input rejects malformed rows without aborting compilation", "[diagnostic-helpers]") {
    char path[] = "/tmp/swiftvm-layout-test-XXXXXX";
    const int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    struct Remove {
        const char* path;
        int fd;
        ~Remove() { close(fd); unlink(path); }
    } remove{path, fd};
    const std::string rows =
            "entry 0x10 0x20 12\n"
            "bad xyz 0x20 12\n"
            "negative 0x10 0x20 -4\n"
            "unaligned 0x10 0x20 3\n"
            "overflow 0x10 0x20 4294967296\n"
            "large 0x10 0x20 4294967292\n"
            "far 0x10 0x20 65540\n"
            "limit 0x10 0x20 1048576\n";
    REQUIRE(write(fd, rows.data(), rows.size()) == static_cast<ssize_t>(rows.size()));
    using swift::runtime::backend::arm64::PlacementExperiment;
    PlacementExperiment enabled{1, path};
    CHECK(enabled.Padding("entry", 0x10, 0x20, 4) == 2);
    CHECK(enabled.Padding("entry", 0x10, 0x20, 16) == 0);
    CHECK(enabled.Padding("negative", 0x10, 0x20, 0) == 0);
    CHECK(enabled.Padding("unaligned", 0x10, 0x20, 0) == 0);
    CHECK(enabled.Padding("overflow", 0x10, 0x20, 0) == 0);
    CHECK(enabled.Padding("large", 0x10, 0x20, 0) == 0);
    CHECK(enabled.Padding("far", 0x10, 0x20, 0) == 0);
    CHECK(enabled.Padding("far", 0x10, 0x20, 4) == 16384);
    CHECK(enabled.Padding("limit", 0x10, 0x20, 1048572) == 1);
    PlacementExperiment disabled{0, path};
    CHECK(disabled.Padding("entry", 0x10, 0x20, 4) == 0);
}
