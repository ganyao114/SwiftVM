#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#include "base/scope_exit.h"
#include "runtime/backend/module.h"

using namespace swift;
using namespace swift::runtime;
namespace be = swift::runtime::backend;

namespace {
struct Probe {
    void* rw{};
    void* rx{};
    void* fail_unmap{};
    int fd{-1};
    unsigned rw_unmaps{}, rx_unmaps{}, closes{}, maps{}, remaps{}, unmaps{}, destroys{};
    bool fail_close{}, fail_rx_map{}, fail_arena{};
    bool fail_view_growth{}, fail_next_unmap{};
};
Probe probe;
int last_fd{-1};
unsigned checks{};

void Check(bool condition, const char* detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}

struct Resources {
    unsigned fds{}, host_maps{};
    bool operator==(const Resources&) const = default;
};

Resources Snapshot() {
    Resources result;
    auto* directory = opendir("/proc/self/fd");
    Check(directory != nullptr, "open descriptor inventory");
    const auto close_directory = ScopeExit([&] { closedir(directory); });
    while (auto* entry = readdir(directory)) {
        if (entry->d_name[0] != '.') ++result.fds;
    }
    std::ifstream maps{"/proc/self/maps"};
    Check(static_cast<bool>(maps), "open mapping inventory");
    for (std::string line; std::getline(maps, line);) {
        if (line.find("memfd:HostMemory") != std::string::npos) ++result.host_maps;
    }
    return result;
}

bool IsMapped(void* address) {
    unsigned char resident{};
    errno = 0;
    const auto result = mincore(address, getpagesize(), &resident);
    Check(result == 0 || errno == ENOMEM, "inspect actual kernel mapping");
    return result == 0;
}

Config TestConfig() {
    return {.loc_start = 0, .loc_end = u64{1} << 32, .enable_jit = true,
            .backend_isa = kArm64, .stack_alignment = 16};
}

void Watch(void* rw, void* rx = nullptr) {
    probe = {.rw = rw, .rx = rx, .fd = last_fd};
}

void Mapping(const std::string& mode) {
    const auto page = static_cast<u32>(getpagesize());
    be::MemMap memory{page, true};
    auto* rw = memory.GetMemory();
    auto* rx = static_cast<u8*>(memory.Map(page, 0, be::MemMap::ReadExe));
    rw[0] = 73;
    Check(rx[0] == 73, "RX and RW alias the same storage");
    Watch(rw, rx);
    const auto invalid = memory.UnmapChecked(rx, page / 2);
    Check(invalid.error == EINVAL && probe.unmaps == 0, "reject non-owned unmap extent");
    if (mode == "retry-rx") probe.fail_unmap = rx;
    if (mode == "retry-rw") probe.fail_unmap = rw;
    errno = ERANGE;
    const auto first = memory.ReleaseAfterQuiescence();
    Check(errno == ERANGE, "preserve caller errno while reporting immediate syscall errno");
    if (mode != "mapping") {
        Check(!first.Complete() && first.unmap_error == EPERM &&
              first.backing_mapped && first.descriptor_open && probe.closes == 0,
              "failed view and descriptor remain owned");
        Check(first.remaining_views == (mode == "retry-rx" ? 2u : 1u),
              "report exact partially released view count");
        Check(IsMapped(rw) && IsMapped(rx) == (mode == "retry-rx"),
              "kernel views match partial release state");
        Check(rw[0] == 73, "failed RW view still contains its storage");
    } else {
        Check(first.Complete(), "release both views and descriptor");
    }
    bool rejected{};
    try { (void)memory.Map(page, 0, be::MemMap::Read); }
    catch (const std::logic_error&) { rejected = true; }
    Check(rejected, "release seals new aliases");
    const auto second = memory.ReleaseAfterQuiescence();
    Check(second.Complete() && memory.GetMemory() == nullptr, "retry completes release");
    Check(probe.rw_unmaps == (mode == "retry-rw" ? 2u : 1u) &&
          probe.rx_unmaps == (mode == "retry-rx" ? 2u : 1u) && probe.closes == 1,
          "retry touches only views still owned and closes once");
    Check(!IsMapped(rw) && !IsMapped(rx), "both kernel mappings are gone");
    Check(memory.UnmapChecked(rx, page).error == EINVAL,
          "old address cannot unmap a later owner");
}

void CloseError() {
    be::MemMap memory{static_cast<u32>(getpagesize()), true};
    Watch(memory.GetMemory());
    const int descriptor = probe.fd;
    probe.fail_close = true;
    const auto first = memory.ReleaseAfterQuiescence();
    Check(!first.Complete() && first.remaining_views == 0 &&
          !first.descriptor_open && first.close_error == EINTR,
          "report consumed but unconfirmed close");
    const int replacement = open("/dev/null", O_RDONLY | O_CLOEXEC);
    Check(replacement == descriptor, "real close made the descriptor available for reuse");
    const auto close_replacement = ScopeExit([&] { close(replacement); });
    const auto second = memory.ReleaseAfterQuiescence();
    Check(!second.Complete() && second.close_error == EINTR && probe.closes == 1,
          "close error survives retry without another close");
    Check(fcntl(replacement, F_GETFD) >= 0, "retry preserves another owner's reused descriptor");
}

void RegistrationFailure() {
    const auto page = static_cast<u32>(getpagesize());
    be::MemMap memory{page, true};
    auto* rx = memory.Map(page, 0, be::MemMap::ReadExe);
    Watch(memory.GetMemory(), rx);
    probe.fail_view_growth = true;
    probe.fail_next_unmap = true;
    bool rejected{};
    try { (void)memory.Map(page, 0, be::MemMap::Read); }
    catch (const std::bad_alloc&) { rejected = true; }
    Check(rejected && !probe.fail_view_growth, "third-view metadata allocation failure is exercised");
    Check(probe.maps == 0 && probe.unmaps == 0 && probe.fail_next_unmap &&
          memory.GetReleaseState().remaining_views == 2,
          "registration failure acquires no kernel view and needs no compensation");
    const auto failed = memory.ReleaseAfterQuiescence();
    Check(!failed.Complete() && failed.remaining_views == 2 && failed.unmap_error == EPERM,
          "later unmap failure still has a tracked owner");
    Check(memory.ReleaseAfterQuiescence().Complete() && probe.unmaps == 3 && probe.closes == 1,
          "all acquired views release without an untracked mapping");
}

void Cache(bool read_only, bool fail_rw) {
    const auto config = TestConfig();
    be::CodeCache cache{config, static_cast<u32>(17 * getpagesize()), FeatureSet{}, read_only};
    const auto first = cache.AllocCode(8);
    const auto second = cache.AllocCode(8);
    Check(first && second && first->rw_data != second->rw_data,
          "successive code allocations have distinct live storage");
    const u32 instructions[]{0x52800f60u, 0xd65f03c0u}; // mov w0,123; ret
    std::memcpy(first->rw_data, instructions, sizeof(instructions));
    std::memset(second->rw_data, 0xa5, second->size);
    Check(std::memcmp(first->exec_data, instructions, sizeof(instructions)) == 0,
          "later allocation does not overwrite published code");
    first->Flush();
#if defined(__aarch64__)
    Check(reinterpret_cast<unsigned (*)()>(first->exec_data)() == 123,
          "published ARM64 code executes before quiescent release");
#endif
    const auto old_region = cache.GetRegion();
    Watch(old_region.rw_base, old_region.rx_base);
    if (fail_rw) probe.fail_unmap = old_region.rw_base;
    const auto released = cache.ReleaseAfterQuiescence();
    Check(released.Complete() == !fail_rw, "cache reports physical release result");
    Check(probe.destroys == (read_only ? 0u : 1u), "destroy fixed arena exactly once");
    Check(!cache.AllocCode(8) && !cache.FreeCode(first->exec_data) &&
          !cache.Contain(first->exec_data), "partially released cache cannot allocate or free old code");
    Check(cache.GetExePtr(0) == nullptr && cache.GetRWPtr(u32{0}) == nullptr &&
          cache.GetRWPtr(first->exec_data) == nullptr && cache.GetRegion().id == 0 &&
          cache.GetRegionTrampoline() == nullptr && cache.GetRegionLinkContext() == nullptr,
          "release makes region and callable accessors inert");
    Check(cache.ReleaseAfterQuiescence().Complete(), "cache release retry completes");
    Check(probe.destroys == (read_only ? 0u : 1u) && probe.closes == 1 &&
          probe.rx_unmaps == 1 && probe.rw_unmaps == (fail_rw ? 2u : 1u),
          "cache retry never repeats arena destruction or successful release");
}

void Data() {
    be::DataAllocator arena{static_cast<u32>(17 * getpagesize())};
    auto* value = arena.Alloc(32);
    Check(value != nullptr && arena.IsOverlap(static_cast<u8*>(value)), "data allocation is in base");
    arena.Free(value);
    Watch(arena.GetBackend());
    probe.fail_unmap = probe.rw;
    const auto first = arena.ReleaseAfterQuiescence();
    Check(!first.Complete() && first.remaining_views == 1 && first.unmap_error == EPERM,
          "data allocator reports failed base release");
    Check(arena.GetBackend() == nullptr && !arena.Alloc(8) &&
          !arena.IsOverlap(static_cast<u8*>(value)), "data owner is inert during retry");
    Check(arena.ReleaseAfterQuiescence().Complete() && probe.destroys == 1 &&
          probe.rw_unmaps == 2 && probe.closes == 1, "data retry completes without repeating arena destroy");
}

void Bounded() {
    const auto page = static_cast<u32>(getpagesize());
    const std::array<u32, 6> sizes{
            page, 4 * page, 17 * page, 4_MB, 32_MB, static_cast<u32>(32_MB) + page};
    for (const auto size : sizes) {
        be::MemMap memory{size, false};
        auto space = create_mspace_with_base(memory.GetMemory(), size, 0);
        Check(space != nullptr, "create caller-supplied arena");
        const auto cleanup = ScopeExit([&] { if (space) destroy_mspace(space); });
        Check(mspace_disable_growth(space) == 1 && mspace_footprint_limit(space) == size,
              "exact finite limit has no granularity rounding");
        Check(mspace_mallopt(M_GRANULARITY, page) == 1, "lower global granularity after sealing");
        Watch(memory.GetMemory());
        std::array<void*, 128> allocations{};
        unsigned count{};
        const auto chunk = std::max<u32>(page, size / 64);
        while (count < allocations.size()) {
            auto* result = mspace_memalign(space, 64, chunk);
            if (!result) break;
            allocations[count++] = result;
            const auto offset = static_cast<u8*>(result) - memory.GetMemory();
            Check(offset >= 0 && static_cast<size_t>(offset) + chunk <= size,
                  "allocated chunk fits the fixed base");
        }
        Check(count < allocations.size() && !mspace_memalign(space, 65536, size + page),
              "alignment exhaustion cannot grow the arena");
        Check(!mspace_malloc(space, size + 1) &&
              !mspace_malloc(space, std::numeric_limits<size_t>::max()),
              "large and overflowing requests fail without outside chunks");
        if (count) {
            Check(!mspace_realloc(space, allocations[0], static_cast<size_t>(size) * 2),
                  "realloc cannot introduce a hidden mapping");
        }
        Check(mspace_footprint(space) == size && mspace_max_footprint(space) == size &&
              probe.maps == 0 && probe.remaps == 0 && probe.unmaps == 0,
              "failed allocations never reach system mapping operations");
        for (unsigned i = 0; i < count; ++i) mspace_free(space, allocations[i]);
        auto* reused = mspace_malloc(space, 32);
        Check(reused != nullptr, "fixed arena remains usable after exhaustion and free");
        mspace_free(space, reused);
        (void)mspace_trim(space, 0);
        Check(destroy_mspace(space) == 0, "zero returned bytes is normal for external-base destroy");
        space = nullptr;
        Check(probe.maps == 0 && probe.remaps == 0 && probe.unmaps == 0 && IsMapped(memory.GetMemory()),
              "allocator never maps or unmaps the external backing");
        Check(memory.ReleaseAfterQuiescence().Complete(), "owner performs checked physical base release");
        std::cout << "bounded capacity=" << size << " allocations=" << count << '\n';
    }
}

void Construction() {
    const auto config = TestConfig();
    for (unsigned round = 0; round < 16; ++round) {
        for (unsigned stage = 0; stage < 3; ++stage) {
            const auto before = Snapshot();
            probe = {};
            probe.fail_rx_map = stage == 0;
            probe.fail_arena = stage != 0;
            probe.fail_next_unmap = true;
            bool rejected{};
            try {
                if (stage == 2) { be::DataAllocator arena{64 * 1024}; }
                else { be::CodeCache cache{config, 64 * 1024, FeatureSet{}}; }
            } catch (const std::bad_alloc&) { rejected = true; }
            Check(rejected, "injected construction stage rejects unpublished owner");
            Check(Snapshot() == before, "failed construction leaves no backing descriptor or views");
            Check(probe.unmaps == (stage == 1 ? 3u : 2u) && !probe.fail_next_unmap,
                  "destructor retries the single injected rollback-unmap failure");
        }
    }
}

void RetainedDestructor() {
    const auto config = TestConfig();
    be::CodeRegion region;
    {
        be::CodeCache cache{config, 64 * 1024, FeatureSet{}};
        Check(cache.AllocCode(8).has_value(), "ordinary owner has a live code allocation");
        region = cache.GetRegion();
        Watch(region.rw_base, region.rx_base);
    }
    Check(probe.destroys == 1 && probe.unmaps == 0 && probe.closes == 1,
          "ordinary executable destructor retains both views and closes its descriptor");
    Check(IsMapped(region.rw_base) && IsMapped(region.rx_base),
          "published-view retention remains observable in the kernel");
    // This fixture has no other callers, threads, signals, or published aliases.
    Check(munmap(region.rx_base, region.capacity) == 0 && munmap(region.rw_base, region.capacity) == 0,
          "fixture releases its retained views after its synchronous caller has left");
}
}  // namespace

extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" int __real_munmap(void*, size_t);
extern "C" int __real_close(int);
extern "C" void* __real_mremap(void*, size_t, size_t, int, ...);
extern "C" mspace __real_create_mspace_with_base(void*, size_t, int);
extern "C" size_t __real_destroy_mspace(mspace);
extern "C" void* __real__Znwm(size_t);

extern "C" void* __wrap__Znwm(size_t size) {
    if (probe.fail_view_growth) {
        probe.fail_view_growth = false;
        throw std::bad_alloc{};
    }
    return __real__Znwm(size);
}

extern "C" void* __wrap_mmap(void* address, size_t size, int prot, int flags, int fd, off_t offset) {
    ++probe.maps;
    if (probe.fail_rx_map && (prot & PROT_EXEC)) {
        probe.fail_rx_map = false;
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (fd >= 0) last_fd = fd;
    return __real_mmap(address, size, prot, flags, fd, offset);
}

extern "C" int __wrap_munmap(void* address, size_t size) {
    ++probe.unmaps;
    if (address == probe.rw) ++probe.rw_unmaps;
    if (address == probe.rx) ++probe.rx_unmaps;
    if (address == probe.fail_unmap || probe.fail_next_unmap) {
        probe.fail_unmap = nullptr;
        probe.fail_next_unmap = false;
        errno = EPERM;
        return -1;
    }
    return __real_munmap(address, size);
}

extern "C" int __wrap_close(int fd) {
    if (fd == probe.fd) {
        ++probe.closes;
        if (probe.fail_close) {
            probe.fail_close = false;
            const auto result = __real_close(fd);
            if (result != 0) return result;
            errno = EINTR;
            return -1;
        }
    }
    return __real_close(fd);
}

extern "C" void* __wrap_mremap(void* address, size_t old_size, size_t new_size, int flags, ...) {
    ++probe.remaps;
    return __real_mremap(address, old_size, new_size, flags);
}

extern "C" mspace __wrap_create_mspace_with_base(void* base, size_t size, int locked) {
    if (probe.fail_arena) {
        probe.fail_arena = false;
        return nullptr;
    }
    return __real_create_mspace_with_base(base, size, locked);
}

extern "C" size_t __wrap_destroy_mspace(mspace space) {
    ++probe.destroys;
    return __real_destroy_mspace(space);
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const std::string mode = argv[1];
        if (mode == "mapping" || mode == "retry-rx" || mode == "retry-rw") Mapping(mode);
        else if (mode == "close") CloseError();
        else if (mode == "registration") RegistrationFailure();
        else if (mode == "cache") { Cache(false, false); Cache(true, false); }
        else if (mode == "cache-retry") { Cache(false, true); Cache(true, true); }
        else if (mode == "data") Data();
        else if (mode == "bounded") Bounded();
        else if (mode == "construction") Construction();
        else if (mode == "retained") RetainedDestructor();
        else return 2;
        std::cout << "mode=" << mode << " checks=" << checks << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
