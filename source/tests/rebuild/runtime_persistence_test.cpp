#include "runtime/execution/disk_cache.h"
#include "runtime/execution/offline_compile.h"
#include "runtime/execution/vm.h"
#include "runtime/platform/linux/artifact_file.h"
#include "runtime/platform/linux/host_runtime.h"
#include "tests/support/test_mode.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <catch2/catch_test_macros.hpp>
#include <elf.h>
#include <unistd.h>

namespace {
namespace execution = swift::runtime::execution;
namespace platform = swift::runtime::platform;
namespace ir = swift::runtime::ir;
constexpr std::uint64_t image_address = 0x400000;
constexpr std::uint64_t entry_pc = image_address + sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);

class Directory {
public:
    Directory() {
#if defined(__ANDROID__)
        constexpr const char* fallback = "/data/local/tmp";
#else
        constexpr const char* fallback = ".";
#endif
        const auto* configured = std::getenv("SVM_TEST_SCRATCH");
        path = std::string{configured ? configured : fallback} + "/swiftvm-persistence-XXXXXX";
        if (!mkdtemp(path.data()))
            swift::ThrowSystemError("create persistence-test directory", errno);
    }

    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path.data(), error);
    }

    std::string Path() const { return path.data(); }

    std::string GuestPath() const { return Path() + "/guest.elf"; }

private:
    std::string path;
};

std::vector<std::uint8_t> Guest(std::span<const std::uint8_t> instructions, bool writable = false) {
    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_EXEC;
    header.e_machine = EM_X86_64;
    header.e_version = EV_CURRENT;
    header.e_entry = entry_pc;
    header.e_phoff = sizeof(header);
    header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(Elf64_Phdr);
    header.e_phnum = 1;
    Elf64_Phdr segment{};
    segment.p_type = PT_LOAD;
    segment.p_flags = PF_R | PF_X | (writable ? PF_W : 0);
    segment.p_vaddr = segment.p_paddr = image_address;
    segment.p_filesz = segment.p_memsz = sizeof(header) + sizeof(segment) + instructions.size();
    segment.p_align = 4096;
    std::vector<std::uint8_t> bytes(segment.p_filesz);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), &segment, sizeof(segment));
    std::memcpy(bytes.data() + sizeof(header) + sizeof(segment),
                instructions.data(),
                instructions.size());
    return bytes;
}

constexpr std::uint8_t exit_program[]{0xbf, 42, 0, 0, 0, 0xb8, 60, 0, 0, 0, 0x0f, 0x05};

std::filesystem::path CacheFile(const Directory& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory.Path()))
        if (entry.path().extension() == ".svmcache") return entry.path();
    return {};
}

void RequireNative() { REQUIRE(swift::tests::NativeMode()); }
}  // namespace

TEST_CASE("fresh VMs revive canonical code before attempting compilation",
          "[native][persistence]") {
    RequireNative();
    Directory directory;
    platform::WriteArtifactFile(directory.GuestPath(), Guest(exit_program));
    execution::RuntimeOptions options;
    options.cache_directory = directory.Path();
    const std::array arguments{directory.GuestPath()};
    const auto cold = execution::RunGuest(directory.GuestPath(), arguments, {}, options);
    INFO(cold.error);
    REQUIRE(cold);
    REQUIRE(cold.exit_code == 42);
    REQUIRE(cold.stats.compile_attempts == 1);
    REQUIRE(cold.stats.disk_cache_writes == 1);
    REQUIRE_FALSE(CacheFile(directory).empty());
    const auto warm = execution::RunGuest(directory.GuestPath(), arguments, {}, options);
    INFO(warm.error);
    REQUIRE(warm);
    CHECK(warm.exit_code == 42);
    CHECK(warm.stats.compile_attempts == 0);
    CHECK(warm.stats.disk_cache_hits == 1);
    CHECK(warm.stats.disk_cache_writes == 0);
    CHECK(warm.stats.native_bytes > 0);
    CHECK(warm.stats.interpreted_steps == 0);
}

TEST_CASE("SMC prevents revival or recording of runtime-modified image code",
          "[native][persistence]") {
    RequireNative();
    // Change the target's MOV EDI immediate before entering it. Both units
    // exist in the disk snapshot, so reading the old target would exit 42.
    constexpr std::uint8_t instructions[]{
            0xc6, 0x05, 0x03, 0, 0, 0,    43,  // MOV BYTE [RIP+3],43; target immediate.
            0xeb, 0x00,                        // JMP target.
            0xbf, 42,   0,    0, 0,            // target: MOV EDI,42.
            0xb8, 60,   0,    0, 0, 0x0f, 0x05};
    Directory directory;
    const auto bytes = Guest(instructions, true);
    platform::WriteArtifactFile(directory.GuestPath(), bytes);
    execution::RuntimeOptions options;
    execution::RuntimeStats compile_stats;
    const std::array entries{entry_pc, entry_pc + 9};
    const auto image = execution::CompileGuestImage(
            bytes, directory.GuestPath(), entries, false, options, compile_stats);
    REQUIRE(image.units.size() >= 2);
    execution::DiskCache cache{directory.Path(), image.key};
    for (const auto& unit : image.units) cache.Record(unit);
    REQUIRE(cache.Flush());
    const auto cache_path = CacheFile(directory);
    REQUIRE_FALSE(cache_path.empty());
    const auto before = swift::HashBytes(platform::ReadArtifactFile(cache_path.string()));
    options.cache_directory = directory.Path();
    const std::array arguments{directory.GuestPath()};
    const auto result = execution::RunGuest(directory.GuestPath(), arguments, {}, options);
    INFO(result.error);
    REQUIRE(result);
    CHECK(result.exit_code == 43);
    CHECK(result.stats.disk_cache_hits == 1);
    CHECK(result.stats.compile_attempts >= 1);
    CHECK(result.stats.cache_invalidations >= 1);
    CHECK(result.stats.smc_write_faults >= 1);
    CHECK(result.stats.disk_cache_writes == 0);
    CHECK(result.stats.interpreted_steps == 0);
    CHECK(swift::HashBytes(platform::ReadArtifactFile(cache_path.string())) == before);
}

TEST_CASE("AOT source policy mismatch is rejected before publishing native code",
          "[native][persistence]") {
    RequireNative();
    const auto bytes = Guest(exit_program);
    execution::RuntimeOptions options;
    execution::RuntimeStats compile_stats;
    auto image = execution::CompileGuestImage(
            bytes, "policy-fixture.elf", {}, false, options, compile_stats);
    REQUIRE_FALSE(image.units.empty());
    REQUIRE_FALSE(image.units.front().sources.empty());
    image.units.front().sources.front().mode =
            static_cast<std::uint8_t>(execution::SourceMode::WatchedBlock);
    platform::HostRuntime host{false};
    execution::RuntimeStats stats;
    execution::VM vm{options, stats, host.Bind()};
    CHECK_THROWS_AS(vm.PrepareGuest(bytes, "policy-fixture.elf", {}, {}, &image),
                    swift::RuntimeError);
    CHECK(stats.aot_units_loaded == 0);
    CHECK(stats.native_bytes == 0);
    CHECK(vm.Close().Complete());
}

TEST_CASE("AOT publication retains the actual native guest fault contract",
          "[native][persistence]") {
    RequireNative();
    constexpr std::uint8_t fault_program[]{0x8b,
                                           0x04,
                                           0x25,
                                           1,
                                           0,
                                           0,
                                           0,  // MOV EAX,DWORD [1].
                                           0xbf,
                                           0,
                                           0,
                                           0,
                                           0,
                                           0xb8,
                                           60,
                                           0,
                                           0,
                                           0,
                                           0x0f,
                                           0x05};
    const auto bytes = Guest(fault_program);
    execution::RuntimeOptions options;
    execution::RuntimeStats compile_stats;
    const auto image = execution::CompileGuestImage(
            bytes, "fault-fixture.elf", {}, false, options, compile_stats);
    REQUIRE_FALSE(image.units.empty());
    REQUIRE_FALSE(image.units.front().code.faults.empty());
    platform::HostRuntime host{false};
    execution::RuntimeStats stats;
    execution::VM vm{options, stats, host.Bind()};
    vm.PrepareGuest(bytes, "fault-fixture.elf", {}, {}, &image);
    (void)vm.RunPrepared();
    CHECK(vm.StopReason() == swift::runtime::GuestStopReason::GuestSignalTermination);
    const auto snapshot = vm.CaptureException();
    REQUIRE(snapshot);
    CHECK(snapshot->info.instruction_pc == 0x400078);
    CHECK(snapshot->info.address_valid);
    CHECK(snapshot->info.address == 1);
    CHECK(stats.compile_attempts == 0);
    CHECK(stats.aot_units_loaded == image.units.size());
    CHECK(stats.native_bytes > 0);
    CHECK(stats.interpreted_steps == 0);
    CHECK(vm.Close().Complete());
}

TEST_CASE("ARM64 FPCR semantic domains survive disk and AOT restoration", "[native][persistence]") {
    RequireNative();
    constexpr std::array<std::uint32_t, 8> instructions{
            0xd2a00800, 0xd51b4400, 0x1e622820, 0xd51b441f,
            0x1e622820, 0xd2800540, 0xd2800ba8, 0xd4000001};
    auto bytes = Guest({reinterpret_cast<const std::uint8_t*>(instructions.data()), sizeof(instructions)});
    Elf64_Ehdr header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.e_machine = EM_AARCH64;
    std::memcpy(bytes.data(), &header, sizeof(header));
    Directory directory;
    platform::WriteArtifactFile(directory.GuestPath(), bytes);
    execution::RuntimeOptions options;
    options.scope = ir::Scope::Function;
    options.cache_directory = directory.Path();
    const std::array arguments{directory.GuestPath()};
    const auto cold = execution::RunGuest(directory.GuestPath(), arguments, {}, options);
    INFO(cold.error);
    REQUIRE(cold);
    REQUIRE(cold.exit_code == 42);
    REQUIRE(cold.stats.compile_attempts > 0);
    const auto warm = execution::RunGuest(directory.GuestPath(), arguments, {}, options);
    INFO(warm.error);
    REQUIRE(warm);
    CHECK(warm.exit_code == 42);
    CHECK(warm.stats.compile_attempts == 0);
    CHECK(warm.stats.disk_cache_hits > 0);
    CHECK(warm.stats.interpreted_steps == 0);
    execution::CodeImage image;
    std::string error;
    REQUIRE(execution::DecodeCodeImage(platform::ReadArtifactFile(CacheFile(directory).string()), image, error));
    platform::HostRuntime host{false};
    const auto backend = host.Bind();
    if (backend.specializes_arm_fpcr) {
        CHECK(cold.stats.fp_domains_created == 2);
        CHECK(cold.stats.fp_domain_switches == 2);
        CHECK(std::any_of(image.units.begin(), image.units.end(), [](const auto& unit) { return unit.fp_mode == 8; }));
    }
    options.cache_directory.clear();
    execution::RuntimeStats stats;
    execution::VM vm{options, stats, backend};
    vm.PrepareGuest(bytes, directory.GuestPath(), {}, {}, &image);
    CHECK(vm.RunPrepared() == 42);
    CHECK(stats.compile_attempts == 0);
    CHECK(stats.aot_units_loaded == image.units.size());
    CHECK(stats.interpreted_steps == 0);
    CHECK(vm.Close().Complete());
}
