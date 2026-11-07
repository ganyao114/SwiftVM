#include "mem_map.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace swift::runtime::execution {
namespace {
[[noreturn]] void Failed(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}
}  // namespace

void MemMap::MapNative(std::size_t bytes) {
    if (!bytes || bytes > SIZE_MAX / 2) throw std::length_error("native mapping size overflow");
    if (write) throw std::logic_error("native mapping already owns a view");
    size = bytes;
    void* write = MAP_FAILED;
    void* execute = MAP_FAILED;
#if defined(SYS_memfd_create)
    const auto fd = static_cast<int>(syscall(SYS_memfd_create, "swiftvm-code", 1u));
    if (fd >= 0) {
        if (ftruncate(fd, static_cast<off_t>(bytes)) == 0) {
            // Both views form one owned extent; the upper view is untouched
            // until its RX file mapping replaces the temporary reservation.
            write = mmap(nullptr, bytes * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (write != MAP_FAILED) {
                this->write = static_cast<std::uint8_t*>(write);
                this->execute = this->write;
                dual = true;
                execute = mmap(static_cast<std::uint8_t*>(write) + bytes,
                               bytes,
                               PROT_READ | PROT_EXEC,
                               MAP_SHARED | MAP_FIXED,
                               fd,
                               0);
            }
        }
        // close consumes this descriptor even when it reports an error. No
        // release retry may close the same integer after somebody reuses it.
        if (close(fd)) close_error = errno;
        if (write != MAP_FAILED && execute != MAP_FAILED) {
            this->execute = static_cast<std::uint8_t*>(execute);
            return;
        }
        if (write != MAP_FAILED) {
            if (const auto released = Release(); released.remaining_views)
                Failed("rollback native mapping");
        }
    }
#endif
    write = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (write == MAP_FAILED) Failed("native mapping mmap");
    this->write = this->execute = static_cast<std::uint8_t*>(write);
    size = bytes;
    dual = false;
}

MemMap::MemMap(MemMap&& other) noexcept
        : write(std::exchange(other.write, nullptr))
        , execute(std::exchange(other.execute, nullptr))
        , size(std::exchange(other.size, 0))
        , dual(other.dual)
        , close_error(std::exchange(other.close_error, 0)) {}

MemMap::~MemMap() { (void)Release(); }

bool MemMap::SetWritable() noexcept {
    return dual || !write || mprotect(write, size, PROT_READ | PROT_WRITE) == 0;
}

bool MemMap::SetExecutable() noexcept {
    return dual || !execute || mprotect(execute, size, PROT_READ | PROT_EXEC) == 0;
}

MappingRelease MemMap::Release() noexcept {
    if (write && munmap(write, size * (dual ? 2 : 1)))
        return {dual ? std::size_t{2} : std::size_t{1}, errno, close_error};
    write = execute = nullptr;
    size = 0;
    return {0, 0, close_error};
}
}  // namespace swift::runtime::execution
