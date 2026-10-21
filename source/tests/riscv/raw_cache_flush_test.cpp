#include "test_support.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <fcntl.h>
#include <link.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace swift;
using namespace swift::runtime;
using namespace swift::tests::riscv;
namespace be = swift::runtime::backend;

// The post-operation marker is synthetic, not a JIT publication oracle.
struct Markers {
    volatile sig_atomic_t preflush{}, entered{}, returned{}, signal_entered{}, signal_returned{}, published{};
};
static Markers* markers;
static u8* start;
static size_t length = 8;
static void Handler(int) {
    markers->signal_entered = 1;
    be::ClearICache(start, length);
    markers->signal_returned = 1;
}

static int Relocations(dl_phdr_info* info, size_t, void* result) {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    const auto contains = [&](uintptr_t p, size_t size) {
        for (size_t i = 0; i < info->dlpi_phnum; ++i) {
            const auto& h = info->dlpi_phdr[i];
            if (h.p_type != PT_LOAD) continue;
            const auto begin = info->dlpi_addr + h.p_vaddr;
            if (p >= begin && p - begin <= h.p_memsz && size <= h.p_memsz - (p - begin)) return true;
        }
        return false;
    };
    const auto pointer = [&](uintptr_t p) {
        if (contains(p, 1)) return p;
        Check(p <= UINTPTR_MAX - info->dlpi_addr && contains(p + info->dlpi_addr, 1), "dynamic pointer in PT_LOAD");
        return p + info->dlpi_addr;
    };
    const ElfW(Dyn)* dynamic{};
    for (size_t i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
            dynamic = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    Check(dynamic != nullptr, "main ELF dynamic table");
    const ElfW(Sym)* symbols{};
    const char* strings{};
    const ElfW(Rela)* relocations{};
    size_t bytes{}, string_bytes{}; long relation{};
    for (auto d = dynamic;; ++d) {
        Check(contains(reinterpret_cast<uintptr_t>(d), sizeof(*d)), "dynamic entry bounds");
        if (d->d_tag == DT_NULL) break;
        if (d->d_tag == DT_SYMTAB) symbols = reinterpret_cast<const ElfW(Sym)*>(pointer(d->d_un.d_ptr));
        if (d->d_tag == DT_STRTAB) strings = reinterpret_cast<const char*>(pointer(d->d_un.d_ptr));
        if (d->d_tag == DT_JMPREL) relocations = reinterpret_cast<const ElfW(Rela)*>(pointer(d->d_un.d_ptr));
        if (d->d_tag == DT_PLTRELSZ) bytes = d->d_un.d_val;
        if (d->d_tag == DT_STRSZ) string_bytes = d->d_un.d_val;
        if (d->d_tag == DT_PLTREL) relation = d->d_un.d_val;
    }
    Check(symbols && strings && (!bytes || relation == DT_RELA), "dynamic symbol and RELA tables");
    Check(contains(reinterpret_cast<uintptr_t>(strings), string_bytes) &&
          (!bytes || contains(reinterpret_cast<uintptr_t>(relocations), bytes)), "dynamic table bounds");
    auto count = static_cast<size_t*>(result);
    for (size_t i = 0; i < bytes / sizeof(ElfW(Rela)); ++i) {
        const auto symbol = symbols + ELF64_R_SYM(relocations[i].r_info);
        Check(contains(reinterpret_cast<uintptr_t>(symbol), sizeof(*symbol)), "relocation symbol bounds");
        Check(symbol->st_name < string_bytes, "relocation name offset");
        const auto name = strings + symbol->st_name;
        Check(std::memchr(name, 0, string_bytes - symbol->st_name) != nullptr, "relocation name termination");
        if (std::strcmp(name, "__riscv_flush_icache") == 0) ++*count;
    }
    return 1;
}

static void GotState(const char* state) {
    size_t count{};
    Check(dl_iterate_phdr(Relocations, &count) == 1, "main ELF relocation scan completed");
    Check(count == 0, "raw executable has no flush PLT/GOT relocation");
    std::cout << "GOT " << state << " flush_jump_slots=" << count << " preflush=" << markers->preflush << std::endl;
}

static void DenyFlush() {
    static_assert(SYS_riscv_flush_icache == 259);
    sock_filter instructions[]{
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_riscv_flush_icache, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog program{u16(std::size(instructions)), instructions};
    Check(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0, "no-new-privileges");
    Check(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0, "syscall-259 EPERM filter");
    errno = 0;
    Check(syscall(SYS_riscv_flush_icache, start, start + length, 0) == -1 && errno == EPERM,
          "independent libc syscall confirms seccomp EPERM");
}

static void Pin(unsigned cpu) {
    cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpu, &mask);
    Check(sched_setaffinity(0, sizeof(mask), &mask) == 0, "native hart affinity");
}

static void CrossHart() {
    cpu_set_t allowed; Check(sched_getaffinity(0, sizeof(allowed), &allowed) == 0, "allowed harts");
    std::vector<unsigned> cpus;
    for (unsigned i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &allowed)) cpus.push_back(i);
    Check(cpus.size() > 1, "two native harts");
    be::CodeCache cache{TestConfig(), 1u << 20, FeatureSet{}, false};
    QuiescentRawCache release_cache{cache};
    auto buffer = cache.AllocCode(8); Check(buffer.has_value(), "dual aliases");
    using Fn = u64 (*)(); const auto fn = reinterpret_cast<Fn>(buffer->exec_data);
    const auto emit = [&](u32 value) {
        const std::array<u32, 2> code{(value << 20) | (10u << 7) | 0x13u, 0x00008067};
        std::memcpy(buffer->rw_data, code.data(), 8); buffer->Flush();
        Check(std::memcmp(buffer->exec_data, code.data(), 8) == 0, "RW/RX bytes match");
    };
    Pin(cpus[0]); emit(101);
    std::atomic<u32> generation{}, observed{}; std::atomic<bool> bad{};
    std::thread reader([&] {
        Pin(cpus[1]);
        for (u32 round = 1; round <= 128; ++round) {
            while (generation.load(std::memory_order_acquire) != round) std::this_thread::yield();
            for (unsigned i = 0; i < 16; ++i) if (fn() != (round % 2 ? 202u : 101u)) bad = true;
            observed.store(round, std::memory_order_release);
        }
    });
    for (u32 round = 1; round <= 128; ++round) {
        emit(round % 2 ? 202 : 101); generation.store(round, std::memory_order_release);
        while (observed.load(std::memory_order_acquire) != round) std::this_thread::yield();
    }
    reader.join(); Check(!bad, "2048 remote instruction fetches");
    const auto old = buffer->exec_data; Check(cache.FreeCode(old), "release RX allocation");
    buffer = cache.AllocCode(8); Check(buffer && buffer->exec_data == old, "same-address reuse");
    emit(303); Pin(cpus[1]); Check(fn() == 303, "reused code visible remotely");
    std::cout << "PASS cross-hart rounds=128 fetches=2048 same-address-reuse harts=" << cpus[0] << ',' << cpus[1] << '\n';
}

static int ChildMain(const std::string& mode, const char* marker_path) {
    const int fd = open(marker_path, O_RDWR); Check(fd >= 0, "existing marker descriptor");
    auto mapping = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd);
    Check(mapping != MAP_FAILED, "shared markers"); markers = new (mapping) Markers{};
    start = static_cast<u8*>(mapping) + 128;
    GotState("before");
    const bool warmed = mode == "warm-signal" || mode == "warm-fail-signal";
    if (warmed) { be::ClearICache(start, length); markers->preflush = 1; GotState("warmed"); }
    const bool failed = mode == "fail-ordinary" || mode == "fail-data" || mode == "fail-signal" || mode == "warm-fail-signal";
    if (failed) DenyFlush();
    markers->entered = 1;
    if (mode == "first-signal" || mode == "warm-signal" || mode == "fail-signal" || mode == "warm-fail-signal") {
        struct sigaction action{}; action.sa_handler = Handler; sigemptyset(&action.sa_mask);
        Check(sigaction(SIGUSR1, &action, nullptr) == 0, "install simple signal handler");
        Check(kill(getpid(), SIGUSR1) == 0, "deliver signal");
        Check(markers->signal_entered && markers->signal_returned, "signal flush returned");
    } else if (mode == "first-ordinary" || mode == "fail-ordinary") be::ClearICache(start, length);
    else if (mode == "fail-data") be::ClearDCache(start, length);
    else if (mode == "empty-range") {
        be::ClearICache(nullptr, 0); be::ClearDCache(nullptr, 0);
        be::ClearICache(start, 0); be::ClearDCache(start, 0);
        be::ClearICache(start, 128); be::ClearDCache(start, 128);
    } else if (mode == "cross-hart") CrossHart();
    else Check(false, "known mode");
    markers->returned = 1; markers->published = 1;
    GotState("after");
    std::cout << "PASS " << mode << " returned=1\n";
    Check(munmap(mapping, 4096) == 0, "unmap child markers"); return 0;
}

static int Supervise(const char* mode_arg) {
    const std::string mode{mode_arg};
    char name[] = "/tmp/swiftvm-raw-flush-XXXXXX";
    const int fd = mkstemp(name); Check(fd >= 0, "temporary marker descriptor");
    Check(unlink(name) == 0 && ftruncate(fd, 4096) == 0, "anonymous marker backing");
    auto mapping = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    Check(mapping != MAP_FAILED, "parent shared markers");
    auto* state = new (mapping) Markers{};
    const std::string descriptor = "/proc/self/fd/" + std::to_string(fd);
    const pid_t pid = fork(); Check(pid >= 0, "child fork");
    if (!pid) {
        // Exec discards parent loader state; the cold child never preflushes.
        const char* args[]{"swift_riscv_raw_cache_flush_test", "--child", mode_arg, descriptor.c_str(), nullptr};
        execv("/proc/self/exe", const_cast<char* const*>(args));
        _exit(126);
    }
    close(fd);
    int status{}; Check(waitpid(pid, &status, 0) == pid, "child reaped");
    const bool failed = mode == "fail-ordinary" || mode == "fail-data" || mode == "fail-signal" || mode == "warm-fail-signal";
    const bool signal = mode == "first-signal" || mode == "warm-signal" || mode == "fail-signal" || mode == "warm-fail-signal";
    const bool warmed = mode == "warm-signal" || mode == "warm-fail-signal";
    Check(WIFEXITED(status) && WEXITSTATUS(status) == (failed ? 127 : 0), "expected child exit");
    Check(state->preflush == int(warmed) && state->entered == 1 && state->returned == int(!failed) &&
          state->published == int(!failed) && state->signal_entered == int(signal) &&
          state->signal_returned == int(signal && !failed), "child cold/warm and failure-stop markers");
    std::cout << "PASS raw-cache " << mode << " child_rc=" << WEXITSTATUS(status)
              << " preflush=" << state->preflush << " returned=" << state->returned
              << " synthetic_post_operation=" << state->published << '\n';
    Check(munmap(mapping, 4096) == 0, "unmap parent markers"); return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string_view{argv[1]} == "--child") return ChildMain(argv[2], argv[3]);
        Check(argc == 2, "mode required");
        return Supervise(argv[1]);
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
