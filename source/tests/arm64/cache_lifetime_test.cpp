#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/scope_exit.h"
#include "runtime/backend/code_cache.h"

using namespace swift;
using namespace swift::runtime;
namespace be = swift::runtime::backend;

namespace {
struct Resources {
    unsigned fds{}, host_fds{}, maps{}, host_maps{};
};

Resources Snapshot() {
    Resources result;
    auto* directory = opendir("/proc/self/fd");
    if (!directory) throw std::runtime_error("cannot inspect process descriptors");
    const auto close_directory = ScopeExit([&] { closedir(directory); });
    while (auto* entry = readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        ++result.fds; // Includes this probe's directory fd in every snapshot.
        const auto path = std::string{"/proc/self/fd/"} + entry->d_name;
        char target[256]{};
        const auto length = readlink(path.c_str(), target, sizeof(target) - 1);
        if (length >= 0 && std::strstr(target, "memfd:HostMemory")) ++result.host_fds;
    }
    std::ifstream maps{"/proc/self/maps"};
    if (!maps) throw std::runtime_error("cannot inspect process mappings");
    for (std::string line; std::getline(maps, line);) {
        ++result.maps;
        if (line.find("memfd:HostMemory") != std::string::npos) ++result.host_maps;
    }
    return result;
}

void Check(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

void CheckResources(const Resources& before, const Resources& after, const char* mode) {
    std::cout << mode << " fds=" << before.fds << "->" << after.fds
              << " HostMemory_fds=" << before.host_fds << "->" << after.host_fds
              << " maps=" << before.maps << "->" << after.maps
              << " HostMemory_maps=" << before.host_maps << "->" << after.host_maps << '\n';
    Check(before.fds == after.fds && before.host_fds == after.host_fds &&
          before.maps == after.maps && before.host_maps == after.host_maps,
          std::string{mode} + " resources grow after owners are destroyed");
}

void MappingCycle(unsigned iteration) {
    const auto page = static_cast<u32>(getpagesize());
    be::MemMap memory{page, false};
    auto* view = static_cast<u8*>(memory.Map(page, 0, be::MemMap::Read));
    const auto release_view = ScopeExit([&] { memory.Unmap(view, page); });
    memory.GetMemory()[0] = static_cast<u8>(iteration);
    memory.GetMemory()[page - 1] = static_cast<u8>(~iteration);
    Check(view[0] == static_cast<u8>(iteration) && view[page - 1] == static_cast<u8>(~iteration),
          "caller-owned read alias must share the backing pages");
}

void CacheCycle(unsigned iteration) {
    const Config config{.loc_start = 0, .loc_end = u64{1} << 32, .enable_jit = true,
                        .backend_isa = kArm64, .stack_alignment = 16};
    const bool read_only = (iteration & 1) != 0;
    be::CodeCache cache{config, 64 * 1024, FeatureSet{}, read_only};
    const auto release = ScopeExit([&] {
        if (!cache.ReleaseAfterQuiescence().Complete()) {
            std::fputs("cache release after synchronous calls failed\n", stderr);
            std::terminate();
        }
    });
    auto code = cache.AllocCode(8);
    Check(code.has_value(), "allocate code in a fresh cache");
    const u32 value = iteration & 0x7ff;
    const u32 instructions[]{0x52800000u | (value << 5), 0xd65f03c0u}; // mov w0,value; ret
    std::memcpy(code->rw_data, instructions, sizeof(instructions));
    Check(std::memcmp(code->exec_data, instructions, sizeof(instructions)) == 0,
          "RX and RW code aliases share bytes");
    code->Flush();
    const auto function = reinterpret_cast<unsigned (*)()>(code->exec_data);
    Check(function() == value, "fresh/reused cache executes the published bytes");
    if (!read_only) Check(cache.FreeCode(code->exec_data), "free before cache teardown");
}

void FailedMappingCycle(unsigned) {
    bool rejected{};
    try { be::MemMap invalid{0, false}; }
    catch (const std::bad_alloc&) { rejected = true; }
    Check(rejected, "zero-length mmap construction must fail");
}

void Pressure(const char* mode, void (*cycle)(unsigned)) {
    // Warm lazy library state before taking the strict resource baseline.
    for (unsigned i = 0; i < 32; ++i) {
        cycle(i);
        // Android's allocator also initializes arenas used by the resource
        // probe and its reporting. Warm that exact path before the baseline.
        const auto probe = Snapshot();
        CheckResources(probe, probe, mode);
    }
    const auto before = Snapshot();
    for (unsigned i = 0; i < 1500; ++i) {
        cycle(i);
        if ((i + 1) % 128 == 0 || i == 1499) CheckResources(before, Snapshot(), mode);
    }
}

void DescriptorZero() {
    const int saved_stdin = dup(STDIN_FILENO);
    Check(saved_stdin >= 0, "save original stdin");
    const auto restore_stdin = ScopeExit([&] {
        dup2(saved_stdin, STDIN_FILENO); close(saved_stdin);
    });
    close(STDIN_FILENO);
    {
        const auto page = static_cast<u32>(getpagesize());
        be::MemMap memory{page, false};
        struct stat info{};
        Check(fstat(STDIN_FILENO, &info) == 0 && info.st_size == page,
              "fd zero is a valid backing descriptor and must be truncated");
        memory.GetMemory()[page - 1] = 91;
        Check(memory.GetMemory()[page - 1] == 91, "fd-zero mapping is backed by storage");
    }
    errno = 0;
    Check(fcntl(STDIN_FILENO, F_GETFD) == -1 && errno == EBADF,
          "owned backing fd zero must close at destruction");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        rlimit limit{}; getrlimit(RLIMIT_NOFILE, &limit);
        std::cout << "RLIMIT_NOFILE=" << limit.rlim_cur << '\n';
        if (argc != 2) return 2;
        const std::string mode = argv[1];
        if (mode == "mapping") Pressure("mapping", MappingCycle);
        else if (mode == "cache") Pressure("cache", CacheCycle);
        else if (mode == "exceptions") Pressure("exceptions", FailedMappingCycle);
        else if (mode == "fd-zero") DescriptorZero();
        else return 2;
        std::cout << "PASS cache lifetime " << mode << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}

