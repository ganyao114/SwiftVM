//
// Guest (ARM64 Linux) memory management — see guest_memory.h.
//

#include <cstring>
#include <string>
#include <sys/mman.h>
#include "base/logging.h"
#include "guest_memory.h"

namespace swift::linux {

bool GuestMemory::MapFixed(VAddr addr, u64 size) {
    ASSERT(addr % kHostPageSize == 0);
    auto map_size = RoundHostPage(size);
    auto* res = mmap(reinterpret_cast<void*>(addr),
                     map_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                     -1,
                     0);
    if (res == MAP_FAILED) {
        LOG_ERROR("GuestMemory: fixed map failed at {:#x} size {:#x} errno {}", addr, map_size, errno);
        return false;
    }
    ASSERT(reinterpret_cast<VAddr>(res) == addr);
    return true;
}

VAddr GuestMemory::MapAnywhere(u64 size) {
    auto map_size = RoundHostPage(size);
    auto* res = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) {
        LOG_ERROR("GuestMemory: map failed size {:#x} errno {}", map_size, errno);
        return 0;
    }
    return reinterpret_cast<VAddr>(res);
}

void GuestMemory::Unmap(VAddr addr, u64 size) {
    auto base = RoundDownHostPage(addr);
    auto end = RoundHostPage(addr + size);
    if (end > base) {
        munmap(reinterpret_cast<void*>(base), end - base);
    }
}

bool GuestMemory::Protect(VAddr addr, u64 size, bool read, bool write, bool exec) {
    auto base = RoundDownHostPage(addr);
    auto end = RoundHostPage(addr + size);
    int prot = PROT_NONE;
    if (read) prot |= PROT_READ;
    if (write) prot |= PROT_WRITE;
    if (exec) prot |= PROT_EXEC;
    return mprotect(reinterpret_cast<void*>(base), end - base, prot) == 0;
}

bool GuestMemory::Read(void* dest, size_t addr, size_t size) {
    std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
    return true;
}

bool GuestMemory::Write(void* src, size_t addr, size_t size) {
    std::memcpy(reinterpret_cast<void*>(addr), src, size);
    return true;
}

std::string GuestMemory::ReadCString(VAddr addr, size_t max_len) {
    std::string result;
    result.reserve(64);
    for (size_t i = 0; i < max_len; ++i) {
        char c = Read<char>(addr + i);
        if (c == '\0') break;
        result.push_back(c);
    }
    return result;
}

}  // namespace swift::linux
