#include "safe_access.h"

#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

namespace swift::runtime::platform {

bool ReadGuestBytes(void* destination, std::uint64_t source, std::size_t size) noexcept {
    if (!size) return true;
    iovec local{destination, size};
    iovec remote{reinterpret_cast<void*>(source), size};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

bool WriteGuestBytes(std::uint64_t destination, const void* source, std::size_t size) noexcept {
    if (!size) return true;
    iovec local{const_cast<void*>(source), size};
    iovec remote{reinterpret_cast<void*>(destination), size};
    return syscall(SYS_process_vm_writev, getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

} // namespace swift::runtime::platform
