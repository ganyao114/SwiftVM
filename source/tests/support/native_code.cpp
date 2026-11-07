#include "native_code.h"

#include <cstring>
#include <cerrno>
#include "base/error.h"

#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__riscv)
#include <asm/unistd.h>
#endif
#endif

namespace swift::tests {
NativeCode::NativeCode(std::span<const std::uint32_t> words, NativeISA isa) {
    if (!NativeMode() || !CanExecute(isa))
        swift::ThrowError("native publication requires a supported --native test entry",
                          swift::ErrorKind::InvalidState);
#if defined(__linux__)
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) swift::ThrowSystemError("native test page size", errno);
    const auto page = static_cast<std::size_t>(page_size);
    size = (words.size_bytes() + page - 1) / page * page;
    fd = static_cast<int>(syscall(SYS_memfd_create, "svm-test-code", MFD_CLOEXEC));
    if (fd < 0) swift::ThrowSystemError("native test memfd_create", errno);
    const auto failed = [&](const char* operation) {
        const int saved = errno;
        Release();
        swift::ThrowSystemError(operation, saved);
    };
    if (ftruncate(fd, static_cast<off_t>(size))) failed("native test backing size");
    auto* writable = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (writable == MAP_FAILED) failed("native test writable mapping");
    write = static_cast<std::uint8_t*>(writable);
    auto* executable = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    if (executable == MAP_FAILED) failed("native test executable mapping");
    execute = executable;
    std::memcpy(write, words.data(), words.size_bytes());
#if defined(__riscv) && __riscv_xlen == 64
    const auto synchronize = [&](void* view) {
        return syscall(__NR_riscv_flush_icache, view, static_cast<std::uint8_t*>(view) + size, 0UL);
    };
    if (synchronize(write) || synchronize(execute)) {
        failed("native test process-wide I-cache synchronization");
    }
#else
    __builtin___clear_cache(reinterpret_cast<char*>(write), reinterpret_cast<char*>(write) + size);
    __builtin___clear_cache(static_cast<char*>(execute), static_cast<char*>(execute) + size);
#endif
#else
    (void)words;
    swift::ThrowError("native code publication requires Linux/Android",
                      swift::ErrorKind::Unsupported);
#endif
}

NativeCode::~NativeCode() { Release(); }

void NativeCode::Release() noexcept {
#if defined(__linux__)
    if (write) munmap(write, size);
    if (execute) munmap(execute, size);
    if (fd >= 0) close(fd);
#endif
    write = nullptr;
    execute = nullptr;
    fd = -1;
}

std::uint64_t NativeCode::Run(std::uint8_t* state) const {
    using Function = std::uint64_t (*)(std::uint8_t*);
    return reinterpret_cast<Function>(execute)(state);
}
}  // namespace swift::tests
