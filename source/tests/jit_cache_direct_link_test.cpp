#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/code_serial.h"
#include "runtime/backend/jit_cache.h"
#include "runtime/backend/link_manager.h"
#include "runtime/backend/runtime.h"
#include "runtime/ir/block.h"

namespace {

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

constexpr const char* kPhaseEnv = "DIRECT_LINK_CACHE_PHASE";
constexpr const char* kDirEnv = "DIRECT_LINK_CACHE_DIR";
constexpr const char* kRoundTripTest =
        "disk cache v4 round trips direct-link sites across processes";

IntrusivePtr<Block> BuildTarget(VAddr guest, u64 fingerprint) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    const auto value = block->LoadImm(Imm{fingerprint}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, value);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    return block;
}

IntrusivePtr<Block> BuildConditionalSource(VAddr guest,
                                           VAddr then_target,
                                           VAddr else_target) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    const auto selector =
            block->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    const auto condition = block->TestNotZero(selector);
    block->SetTerminal(terminal::If{
            condition,
            terminal::LinkBlock{Location{then_target}},
            terminal::LinkBlock{Location{else_target}},
    });
    block->ReIdInstr();
    return block;
}

struct ProductionSite {
    u8* rx{};
    LinkSiteKey key{};
    LinkSiteRecord record{};
};

std::vector<ProductionSite> FindProductionSites(AddressSpace& space,
                                                const CodeRegion& region,
                                                u8* allocation,
                                                size_t scan_bytes = 256) {
    std::vector<ProductionSite> result;
    for (size_t offset = 0; offset < scan_bytes; offset += sizeof(u32)) {
        auto* site = allocation + offset;
        const LinkSiteKey key{region.id,
                              static_cast<u32>(site - region.rx_base)};
        if (const auto record = space.GetLinkManager().QuerySite(key)) {
            result.push_back({site, key, *record});
        }
    }
    return result;
}

u32 LoadInsn(const void* address) {
    u32 instruction{};
    std::memcpy(&instruction, address, sizeof(instruction));
    return instruction;
}

void SetUniform(Runtime& runtime, size_t offset, u64 value) {
    auto uniform = runtime.GetUniformBuffer();
    REQUIRE(offset + sizeof(value) <= uniform.size());
    std::memcpy(uniform.data() + offset, &value, sizeof(value));
}

u64 GetUniform(Runtime& runtime, size_t offset) {
    auto uniform = runtime.GetUniformBuffer();
    REQUIRE(offset + sizeof(u64) <= uniform.size());
    u64 value{};
    std::memcpy(&value, uniform.data() + offset, sizeof(value));
    return value;
}

std::string CurrentTestExecutable() {
#if defined(__APPLE__)
    u32 size{};
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    REQUIRE(_NSGetExecutablePath(path.data(), &size) == 0);
    path.resize(std::strlen(path.c_str()));
    return path;
#elif defined(__linux__)
    std::array<char, 4096> path{};
    const auto size = readlink("/proc/self/exe", path.data(), path.size() - 1);
    REQUIRE(size > 0);
    return std::string(path.data(), static_cast<size_t>(size));
#else
    return {};
#endif
}

std::filesystem::path FindCacheFile(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().starts_with("svmjit-") &&
            entry.path().extension() == ".bin") {
            files.push_back(entry.path());
        }
    }
    REQUIRE(files.size() == 1);
    return files.front();
}

void RewriteCacheVersion(const std::filesystem::path& file, u64 version) {
    const int fd = open(file.c_str(), O_RDWR);
    REQUIRE(fd >= 0);
    constexpr off_t kVersionOffset = 8;  // char magic[8], then little-endian u64.
    REQUIRE(pwrite(fd, &version, sizeof(version), kVersionOffset) ==
            static_cast<ssize_t>(sizeof(version)));
    REQUIRE(close(fd) == 0);
}

Config MakeConfig(void* guest_memory, size_t guest_size) {
    return Config{
            .loc_start = 0,
            .loc_end = guest_size,
            .enable_jit = true,
            .enable_asm_interp = false,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = 64,
            .global_opts = Optimizations::BlockLink,
            .memory_base = guest_memory,
            .guest_addr_mask = guest_size - 1,
    };
}

void RunCacheChildPhase(std::string_view phase) {
#if defined(__aarch64__)
    const char* dir = std::getenv(kDirEnv);
    REQUIRE(dir != nullptr);
    REQUIRE(setenv("SVM_JIT_CACHE", dir, 1) == 0);
    REQUIRE(setenv("SVM_BACKEDGE_FLAGS", "0", 1) == 0);

    const size_t page_size = static_cast<size_t>(getpagesize());
    const size_t guest_size = 16 * page_size;
    void* guest_memory = mmap(nullptr,
                              guest_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON,
                              -1,
                              0);
    REQUIRE(guest_memory != MAP_FAILED);
    std::memset(guest_memory, 0xa5, guest_size);

    constexpr u64 kThenFingerprint = 0x1122334455667788ull;
    constexpr u64 kElseFingerprint = 0x8877665544332211ull;
    const VAddr source_guest = page_size + 0x100;
    const VAddr then_guest = 5 * page_size + 0x100;
    const VAddr else_guest = 9 * page_size + 0x100;

    {
        AddressSpace space{MakeConfig(guest_memory, guest_size)};
        auto* disk = space.GetJitDiskCache();
        REQUIRE(disk != nullptr);
        REQUIRE(disk->Enabled());
        auto module = space.GetDefaultModule();

        if (phase == "write") {
            auto then_block = BuildTarget(then_guest, kThenFingerprint);
            auto else_block = BuildTarget(else_guest, kElseFingerprint);
            auto* then_code = TranslateIR(module, then_block);
            auto* else_code = TranslateIR(module, else_block);
            REQUIRE(then_code != nullptr);
            REQUIRE(else_code != nullptr);
            space.PushCodeCache(Location{then_guest}, then_code);
            space.PushCodeCache(Location{else_guest}, else_code);

            auto source = BuildConditionalSource(source_guest, then_guest, else_guest);
            auto* source_code = static_cast<u8*>(TranslateIR(module, source));
            REQUIRE(source_code != nullptr);
            space.PushCodeCache(Location{source_guest}, source_code);
            const auto region = module->GetCodeRegion(source_code);
            REQUIRE(region);
            const auto sites = FindProductionSites(space, *region, source_code);
            REQUIRE(sites.size() == 2);

            Runtime runtime{&space};
            SetUniform(runtime, 8, 1);
            runtime.SetLocation(source_guest);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            REQUIRE(GetUniform(runtime, 0) == kThenFingerprint);
            SetUniform(runtime, 8, 0);
            runtime.SetLocation(source_guest);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            REQUIRE(GetUniform(runtime, 0) == kElseFingerprint);
            for (const auto& site : sites) {
                REQUIRE(space.GetLinkManager().QuerySite(site.key)->state ==
                        LinkSiteState::Linked);
            }

            // Save and target invalidation deliberately race. RecordUnit has
            // already captured immutable spans and metadata; Save must never
            // observe a transient live B/BL word.
            std::atomic_bool start{};
            std::thread saver([&] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                disk->Save();
            });
            std::thread invalidator([&] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                space.InvalidateCodeRange(then_guest, then_guest + 1);
            });
            start.store(true, std::memory_order_release);
            saver.join();
            invalidator.join();
            const auto then_site = std::find_if(
                    sites.begin(), sites.end(), [&](const auto& site) {
                        return site.record.guest_target == then_guest;
                    });
            REQUIRE(then_site != sites.end());
            REQUIRE(space.GetLinkManager().QuerySite(then_site->key)->state ==
                    LinkSiteState::Unlinked);
            REQUIRE(disk->Stats().units_stored.load() == 3);
        } else if (phase == "read") {
            REQUIRE(disk->Stats().units_loaded.load() == 3);
            REQUIRE(disk->Stats().units_compiled.load() == 0);
            auto* source_code = static_cast<u8*>(space.GetCodeCache(Location{source_guest}));
            REQUIRE(source_code != nullptr);
            const auto region = module->GetCodeRegion(source_code);
            REQUIRE(region);
            const auto sites = FindProductionSites(space, *region, source_code);
            REQUIRE(sites.size() == 2);
            auto* trampoline = region->rx_base + region->trampoline_offset;
            for (const auto& site : sites) {
                const auto record = space.GetLinkManager().QuerySite(site.key);
                REQUIRE(record);
                REQUIRE(record->state == LinkSiteState::Unlinked);
                REQUIRE(DecodeBranchTarget(site.rx, LoadInsn(site.rx)) ==
                        reinterpret_cast<uintptr_t>(trampoline));
            }
            REQUIRE(std::count_if(sites.begin(), sites.end(), [](const auto& site) {
                        return site.record.kind == LinkSiteKind::ConditionalThen;
                    }) == 1);
            REQUIRE(std::count_if(sites.begin(), sites.end(), [](const auto& site) {
                        return site.record.kind == LinkSiteKind::ConditionalElse;
                    }) == 1);

            Runtime runtime{&space};
            SetUniform(runtime, 8, 1);
            runtime.SetLocation(source_guest);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            REQUIRE(GetUniform(runtime, 0) == kThenFingerprint);
            SetUniform(runtime, 8, 0);
            runtime.SetLocation(source_guest);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            REQUIRE(GetUniform(runtime, 0) == kElseFingerprint);
            for (const auto& site : sites) {
                const auto record = space.GetLinkManager().QuerySite(site.key);
                REQUIRE(record);
                REQUIRE(record->state == LinkSiteState::Linked);
                REQUIRE(DecodeBranchTarget(site.rx, LoadInsn(site.rx)) ==
                        reinterpret_cast<uintptr_t>(
                                space.GetCodeCache(Location{record->guest_target})));
            }

            const auto then_site = std::find_if(
                    sites.begin(), sites.end(), [&](const auto& site) {
                        return site.record.guest_target == then_guest;
                    });
            REQUIRE(then_site != sites.end());
            space.InvalidateCodeRange(then_guest, then_guest + 1);
            REQUIRE(space.GetLinkManager().QuerySite(then_site->key)->state ==
                    LinkSiteState::Unlinked);
            REQUIRE(DecodeBranchTarget(then_site->rx, LoadInsn(then_site->rx)) ==
                    reinterpret_cast<uintptr_t>(trampoline));
        } else if (phase == "reject") {
            REQUIRE(disk->Stats().reject_header.load() == 1);
            REQUIRE(disk->Stats().units_loaded.load() == 0);
            REQUIRE(space.GetCodeCache(Location{source_guest}) == nullptr);
        } else {
            FAIL("unknown direct-link cache child phase");
        }
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    (void)phase;
#endif
}

int RunCacheChild(const std::filesystem::path& dir, const char* phase) {
    const auto executable = CurrentTestExecutable();
    REQUIRE_FALSE(executable.empty());
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        setenv(kPhaseEnv, phase, 1);
        setenv(kDirEnv, dir.c_str(), 1);
        execl(executable.c_str(),
              executable.c_str(),
              kRoundTripTest,
              "--reporter",
              "compact",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    int status{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        }
        REQUIRE(waited == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
    return 124;
}

}  // namespace

TEST_CASE("disk cache scanner accepts only metadata-declared external BL sites",
          "[direct-link][jit-cache][serializer]") {
    std::array<u32, 4> words{
            0xd503201fu,
            *EncodeBL(0x1000),
            0xd503201fu,
            *EncodeBL(-0x1000),
    };
    const auto code = std::span<const u8>{reinterpret_cast<const u8*>(words.data()),
                                          sizeof(words)};
    const HostImageInfo image{.base = 0x100000000ull, .size = 0x100000};
    const std::array<u32, 2> sites{4, 12};
    REQUIRE(ScanCodeUnit(code, image, 0x100000, sites).ok);
    REQUIRE_FALSE(ScanCodeUnit(code, image, 0x100000).ok);
    const std::array<u32, 2> duplicate{4, 4};
    REQUIRE_FALSE(ScanCodeUnit(code, image, 0x100000, duplicate).ok);
    words[1] = *EncodeB(0x1000);
    REQUIRE_FALSE(ScanCodeUnit(code, image, 0x100000, sites).ok);
}

TEST_CASE("disk cache v4 serializes arbitrary link-site records and kinds",
          "[direct-link][jit-cache][serializer]") {
    SerialUnit input{};
    input.guest_start = 0x1000;
    input.code.resize(32, 0);
    input.blocks.push_back({0x1000, 0x1004, 0, 0x1234});
    input.link_sites = {
            {4, 0x2000, static_cast<u8>(LinkSiteKind::ConditionalThen)},
            {8, 0x3000, static_cast<u8>(LinkSiteKind::ConditionalElse)},
            {20, 0x4000, static_cast<u8>(LinkSiteKind::SwitchArm)},
    };
    BlobWriter writer;
    WriteUnit(writer, input);
    BlobReader reader{writer.Data().data(), writer.Size()};
    SerialUnit output{};
    REQUIRE(ReadUnit(reader, output));
    REQUIRE(reader.Remaining() == 0);
    REQUIRE(output.guest_start == input.guest_start);
    REQUIRE(output.code == input.code);
    REQUIRE(output.link_sites.size() == input.link_sites.size());
    for (size_t i = 0; i < input.link_sites.size(); ++i) {
        REQUIRE(output.link_sites[i].code_offset == input.link_sites[i].code_offset);
        REQUIRE(output.link_sites[i].guest_target == input.link_sites[i].guest_target);
        REQUIRE(output.link_sites[i].kind == input.link_sites[i].kind);
    }
}

TEST_CASE(kRoundTripTest, "[direct-link][jit-cache][production][smc]") {
#if defined(__aarch64__)
    if (const char* phase = std::getenv(kPhaseEnv)) {
        RunCacheChildPhase(phase);
        return;
    }
    std::array<char, 64> path_template{};
    std::strcpy(path_template.data(), "/tmp/svm-direct-link-p3.XXXXXX");
    char* dir_name = mkdtemp(path_template.data());
    REQUIRE(dir_name != nullptr);
    const std::filesystem::path dir{dir_name};
    REQUIRE(RunCacheChild(dir, "write") == 0);
    const auto cache_file = FindCacheFile(dir);
    REQUIRE(RunCacheChild(dir, "read") == 0);
    RewriteCacheVersion(cache_file, kCacheFormatVersion - 1);
    REQUIRE(RunCacheChild(dir, "reject") == 0);
    REQUIRE(std::filesystem::remove_all(dir) > 0);
#else
    SUCCEED("direct-link code generation requires AArch64");
#endif
}
