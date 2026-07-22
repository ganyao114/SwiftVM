//
// Guest (ARM64 Linux) memory management — see guest_memory.h.
//

#include <algorithm>
#include <cstring>
#include <string>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
// Linux: MAP_FIXED_NOREPLACE (4.17+); never clobbers existing mappings.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif
#include "base/logging.h"
#include "guest_memory.h"

namespace swift::linux {

bool GuestMemory::MapFixed(VAddr addr, u64 size) {
    ASSERT(addr % kHostPageSize == 0);
    auto map_size = RoundHostPage(size);
    const VAddr host_addr = addr + bias_;
#if defined(__APPLE__)
    // mach_vm_allocate(VM_FLAGS_FIXED) fails with KERN_NO_SPACE when any
    // part of the range is already occupied — unlike MAP_FIXED it never
    // silently replaces existing host mappings. That matters because the
    // JIT code cache and other host allocations are placed freely by the
    // host VM, and a clobbering guest map could wipe them out.
    mach_vm_address_t target = host_addr;
    const kern_return_t kr = mach_vm_allocate(mach_task_self(), &target, map_size, VM_FLAGS_FIXED);
    if (kr != KERN_SUCCESS || target != host_addr) {
        if (kr == KERN_SUCCESS) {
            mach_vm_deallocate(mach_task_self(), target, map_size);
        }
        LOG_ERROR("GuestMemory: fixed map failed at guest {:#x} (host {:#x}) size {:#x} kr {}",
                  addr,
                  host_addr,
                  map_size,
                  kr);
        return false;
    }
#else
    auto* res = mmap(reinterpret_cast<void*>(host_addr),
                     map_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                     -1,
                     0);
    if (res == MAP_FAILED) {
        LOG_ERROR("GuestMemory: fixed map failed at guest {:#x} (host {:#x}) size {:#x} errno {}",
                  addr,
                  host_addr,
                  map_size,
                  errno);
        return false;
    }
    ASSERT(reinterpret_cast<VAddr>(res) == host_addr);
#endif
    TrackMap(addr, map_size);
    return true;
}

VAddr GuestMemory::MapAnywhere(u64 size) {
    auto map_size = RoundHostPage(size);
    auto* res = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) {
        LOG_ERROR("GuestMemory: map failed size {:#x} errno {}", map_size, errno);
        return 0;
    }
    // Report the guest address (host - bias).
    auto addr = reinterpret_cast<VAddr>(res) - bias_;
    TrackMap(addr, map_size);
    return addr;
}

bool GuestMemory::MapImageAnywhere(VAddr guest_start, u64 size) {
    ASSERT(guest_start % kHostPageSize == 0);
    ASSERT(bias_ == 0);  // the image reservation installs the bias; call once
    auto map_size = RoundHostPage(size);
    auto* res = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) {
        LOG_ERROR("GuestMemory: image reserve failed size {:#x} errno {}", map_size, errno);
        return false;
    }
    SetBias(reinterpret_cast<VAddr>(res) - guest_start);
    TrackMap(guest_start, map_size);
    LOG_INFO("GuestMemory: image span guest {:#x} -> host {} (bias {:#x})",
             guest_start,
             res,
             bias_);
    return true;
}

void GuestMemory::Unmap(VAddr addr, u64 size) {
    auto base = RoundDownHostPage(addr);
    auto end = RoundHostPage(addr + size);
    if (end > base) {
        munmap(ToHost(base), end - base);
        TrackUnmap(base, end - base);
    }
}

bool GuestMemory::Protect(VAddr addr, u64 size, bool read, bool write, bool exec) {
    auto base = RoundDownHostPage(addr);
    auto end = RoundHostPage(addr + size);
    int prot = PROT_NONE;
    if (read) prot |= PROT_READ;
    if (write) prot |= PROT_WRITE;
    if (exec) prot |= PROT_EXEC;
    return mprotect(ToHost(base), end - base, prot) == 0;
}

bool GuestMemory::Read(void* dest, size_t addr, size_t size) {
    std::memcpy(dest, ToHostConst(addr), size);
    return true;
}

bool GuestMemory::Write(void* src, size_t addr, size_t size) {
    std::memcpy(ToHost(addr), src, size);
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

void GuestMemory::TrackMap(VAddr addr, u64 size) {
    if (size == 0) return;
    const VAddr end = addr + size;
    // Replace any overlaps, then coalesce touching intervals.
    TrackUnmap(addr, size);
    mapped_regions.emplace_back(addr, end);
    std::sort(mapped_regions.begin(), mapped_regions.end());
    std::vector<std::pair<VAddr, VAddr>> merged;
    merged.reserve(mapped_regions.size());
    for (const auto& r : mapped_regions) {
        if (!merged.empty() && r.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, r.second);
        } else {
            merged.push_back(r);
        }
    }
    mapped_regions = std::move(merged);
}

void GuestMemory::TrackUnmap(VAddr addr, u64 size) {
    if (size == 0) return;
    const VAddr end = addr + size;
    std::vector<std::pair<VAddr, VAddr>> out;
    out.reserve(mapped_regions.size());
    for (const auto& r : mapped_regions) {
        if (r.second <= addr || r.first >= end) {
            out.push_back(r);
            continue;
        }
        if (r.first < addr) out.emplace_back(r.first, addr);
        if (r.second > end) out.emplace_back(end, r.second);
    }
    mapped_regions = std::move(out);
}

bool GuestMemory::RangeIsMapped(VAddr addr, u64 size) const {
    if (size == 0) return true;
    if (addr + size < addr) return false;  // overflow
    // Find the last region with start <= addr.
    auto it = std::upper_bound(mapped_regions.begin(),
                               mapped_regions.end(),
                               addr,
                               [](VAddr a, const auto& p) { return a < p.first; });
    if (it == mapped_regions.begin()) return false;
    --it;
    return it->first <= addr && addr + size <= it->second;
}

bool GuestMemory::TryReadBytes(VAddr addr, std::span<u8> out) {
    if (!RangeIsMapped(addr, out.size())) return false;
    std::memcpy(out.data(), ToHostConst(addr), out.size());
    return true;
}

bool GuestMemory::TryWriteBytes(VAddr addr, std::span<const u8> data) {
    if (!RangeIsMapped(addr, data.size())) return false;
    std::memcpy(ToHost(addr), data.data(), data.size());
    return true;
}

bool GuestMemory::TryReadCString(VAddr addr, std::string& out, size_t max_len) {
    out.clear();
    for (size_t i = 0; i < max_len; ++i) {
        if (!RangeIsMapped(addr + i, 1)) return false;
        char c;
        std::memcpy(&c, ToHostConst(addr + i), 1);
        if (c == '\0') return true;
        out.push_back(c);
    }
    return false;  // no terminator within max_len
}

}  // namespace swift::linux
