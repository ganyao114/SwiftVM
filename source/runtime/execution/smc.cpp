#include "smc.h"
#include "timing.h"
#include "vm.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "base/error.h"

namespace swift::runtime::execution {
namespace {
using u64 = std::uint64_t;

[[noreturn]] void SystemError(const char* op) { swift::ThrowSystemError(op, errno); }

u64 Add(u64 a, u64 b) {
    if (b > UINT64_MAX - a) swift::ThrowError("source range overflow", ErrorKind::InvalidArgument);
    return a + b;
}

bool IsFile(const MemoryRegion& r) noexcept {
    return r.backing == BackingKind::FilePrivate || r.backing == BackingKind::FileShared;
}

bool FileOverlap(const MemoryRegion& a, const MemoryRegion& b, u64 first, u64 last) noexcept {
    return IsFile(a) && IsFile(b) && a.device == b.device && a.inode == b.inode &&
           first < b.file_offset + b.size && b.file_offset < last;
}

template <typename Visit>
void CodeWriteRanges(std::span<const MemoryRegion> regions, u64 address, u64 end, Visit visit) {
    for (const auto& source : regions) {
        const auto begin = std::max(address, source.address),
                   tail = std::min(end, source.address + source.size);
        if (begin >= tail || !(source.protection & PROT_EXEC)) continue;
        if (source.protection & PROT_WRITE) visit(begin, tail);
        if (!IsFile(source)) continue;
        const auto first = source.file_offset + (begin - source.address),
                   last = source.file_offset + (tail - source.address);
        for (const auto& peer : regions) {
            if (&source == &peer || peer.backing != BackingKind::FileShared ||
                !(peer.protection & PROT_WRITE) || !FileOverlap(source, peer, first, last))
                continue;
            visit(peer.address + (std::max(first, peer.file_offset) - peer.file_offset),
                  peer.address + (std::min(last, peer.file_offset + peer.size) - peer.file_offset));
        }
    }
}

}  // namespace

void Smc::Arm(u64 address, std::size_t length) {
    const SourceSpan source{address, length};
    Arm(std::span{&source, 1});
}

void Smc::Arm(std::span<const SourceSpan> sources) {
    const auto page_size = vm.memory.PageSize();
    const auto visit_pages = [&](auto&& visit) {
        for (const auto& source : sources) {
            if (!source.size) continue;
            const auto end = Add(source.pc, source.size);
            CodeWriteRanges(vm.memory.Regions(), source.pc, end, [&](u64 begin, u64 tail) {
                const auto last = (tail - 1) & ~(u64{page_size} - 1);
                for (auto page = begin & ~(u64{page_size} - 1);; page += page_size) {
                    visit(page);
                    if (page == last) break;
                }
            });
        }
    };
    // The existing sorted records already answer repeated blocks on an armed
    // page. Do not append duplicates, sort or walk the whole page owner again.
    const auto known_pages = pages.size();
    const auto armed = [&](u64 address) {
        const auto end = pages.begin() + known_pages;
        const auto found =
                std::lower_bound(pages.begin(), end, address, [](const Page& row, u64 value) {
                    return row.address < value;
                });
        return found != end && found->address == address && found->armed;
    };
    std::size_t candidate_pages{};
    bool has_candidates{};
    visit_pages([&](u64 page) {
        has_candidates = true;
        if (armed(page)) return;
        if (candidate_pages == pages.max_size() - known_pages)
            swift::ThrowError("guest code-write page capacity overflow", ErrorKind::ResourceLimit);
        ++candidate_pages;
    });
    if (!has_candidates || (!candidate_pages && fully_armed)) return;
    // Recover can only observe an armed page. Establish its miss view here,
    // before protection can admit a signal reader, and never in the handler.
    if (vm.backend.thread_dispatch_cache) vm.empty_l1.Ensure();
    pages.reserve(pages.size() + candidate_pages);
    fully_armed = false;
    visit_pages([&](u64 page) {
        if (!armed(page)) pages.push_back({page, false});
    });
    std::sort(pages.begin(), pages.end(), [](const Page& left, const Page& right) {
        return left.address < right.address;
    });
    std::size_t kept{};
    for (const auto page : pages) {
        if (kept && pages[kept - 1].address == page.address)
            pages[kept - 1].armed |= page.armed;
        else
            pages[kept++] = page;
    }
    pages.resize(kept);
    // All records and allocations precede protection, including non-EXEC
    // MAP_SHARED writers of these source pages. Keep existing armed records.
    bool complete = true;
    for (auto& page : pages) {
        if (page.armed) continue;
        const auto protection = vm.memory.PageProtection(page.address);
        if (protection < 0 || !(protection & PROT_WRITE)) {
            complete = false;
            continue;
        }
        if (mprotect(reinterpret_cast<void*>(page.address), page_size, protection & ~PROT_WRITE))
            SystemError("protect guest code source/alias");
        page.armed = true;
    }
    fully_armed = complete;
}

bool Smc::Recover(std::uintptr_t address) noexcept {
    const auto page_size = vm.memory.PageSize();
    const auto page = address & ~(u64{page_size} - 1);
    const auto found =
            std::lower_bound(pages.begin(), pages.end(), page, [](const Page& row, u64 value) {
                return row.address < value;
            });
    if (found == pages.end() || found->address != page || !found->armed) return false;
    const auto protection = vm.memory.PageProtection(page);
    if (protection < 0 || !(protection & PROT_WRITE)) return false;
    if (syscall(SYS_mprotect, reinterpret_cast<void*>(page), page_size, protection)) {
        vm.thread.bridge_failure.Set(ErrorKind::System, "remove native code-write watch", errno);
        return false;
    }
    found->armed = false;
    fully_armed = false;
    vm.thread.faults.smc_pending = 1;
    __atomic_store_n(&vm.thread.native.mask, u64{0}, __ATOMIC_RELAXED);
    __atomic_store_n(&vm.thread.native.slots, vm.Cache().EmptyData(), __ATOMIC_RELEASE);
    vm.thread.faults.dispatch_resume_l1.store(reinterpret_cast<std::uintptr_t>(vm.empty_l1.Data()),
                                              std::memory_order_release);
    vm.RefreshDispatchView();
    for (auto& cache : vm.caches) cache.ResetLinks();
    return true;
}

bool Smc::PrepareWriteRange(u64 address, std::size_t size) noexcept {
    if (!size) return true;
    const auto page = address & ~(u64{vm.memory.PageSize()} - 1);
    const auto end = address + size;
    auto at = std::lower_bound(pages.begin(), pages.end(), page, [](const Page& row, u64 value) {
        return row.address < value;
    });
    for (; at != pages.end() && at->address < end; ++at)
        if (at->armed && !Recover(at->address)) return false;
    return true;
}

void Smc::Clear() {
    const auto page_size = vm.memory.PageSize();
    fully_armed = false;
    for (auto& page : pages)
        if (page.armed) {
            const auto protection = vm.memory.PageProtection(page.address);
            // Mapping/protection syscalls may already have replaced or unmapped
            // this page. Never restore the earlier mapping's permissions.
            if (protection >= 0 &&
                mprotect(reinterpret_cast<void*>(page.address), page_size, protection))
                SystemError("restore guest code source");
            page.armed = false;
        }
    pages.clear();  // Retain actual page-record capacity.
    fully_armed = true;
}

void Smc::Invalidate() {
    vm.DisablePersistence();
    RuntimeTimer total{vm.stats.compile_request_ns}, release{vm.stats.code_release_ns};
    Clear();
    vm.thread.l1.Reset();
    vm.thread.faults.smc_pending = 0;
    // The signal path already restored links while old native code could run.
    // A service-triggered invalidation is itself cold, so removing all domains
    // also makes every old range unreachable without rewriting it a second time.
    for (auto& cache : vm.caches) cache.Clear();
    vm.thread.anchor = {};
    vm.RefreshNative(vm.thread.native);
    const auto released = vm.heap.Release();
    if (!released.Complete()) swift::ThrowError("cannot retire native code mapping");
    ++vm.stats.cache_invalidations;
}
}  // namespace swift::runtime::execution
