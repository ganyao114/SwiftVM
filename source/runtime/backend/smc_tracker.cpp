//
// Self-modifying code (SMC) tracking — see smc_tracker.h.
//

#include "smc_tracker.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/module.h"
#include "runtime/common/logging.h"

namespace swift::runtime::backend {

namespace {
std::atomic_bool g_smc_enabled{true};
}

void SmcTracker::SetEnabled(bool enabled) {
    g_smc_enabled.store(enabled, std::memory_order_relaxed);
}

bool SmcTracker::IsEnabled() {
    return g_smc_enabled.load(std::memory_order_relaxed);
}

SmcTracker::SmcTracker(u64 guest_bias, u64 guest_addr_mask)
        : bias_(guest_bias)
        , mask_(guest_addr_mask ? guest_addr_mask : UINT64_MAX)
        , page_size_(static_cast<u64>(getpagesize()))
        , page_mask_(page_size_ - 1) {
    ASSERT((page_size_ & page_mask_) == 0);
}

void SmcTracker::LockMetadata() const {
    while (metadata_lock_.test_and_set(std::memory_order_acquire)) {
        // Signal-handler compatible spin: never call into the scheduler here.
    }
}

void SmcTracker::UnlockMetadata() const {
    metadata_lock_.clear(std::memory_order_release);
}

bool SmcTracker::SetPageProtected(VAddr page, bool prot_read_only) {
    // Truncate to the guest window first: `page` is derived from a
    // guest-controlled block location, and an unmasked page + bias_ would
    // mprotect an arbitrary *host* mapping (observed: the translator's own
    // __TEXT, which then loses execute permission under its own feet).
    const auto host_addr = (page & mask_) + bias_;
    const auto host = reinterpret_cast<void*>(host_addr);
    const int prot = prot_read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
    if (mprotect(host, page_size_, prot) != 0) {
        LOG_ERROR("SMC: mprotect({:#x}, {}) failed: {}",
                  host_addr,
                  prot_read_only ? "R" : "RW",
                  std::strerror(errno));
        return false;
    }
    return true;
}

SmcTracker::RuntimeToken SmcTracker::RegisterRuntime(TranslateTable& l1) {
    auto token = std::make_shared<RuntimeEpoch>(&l1);
    MetadataGuard guard(*this);
    runtimes_.push_back(token);
    return token;
}

void SmcTracker::UnregisterRuntime(const RuntimeToken& token) {
    if (!token) {
        return;
    }
    token->active_epoch.store(kInactiveEpoch, std::memory_order_release);
    {
        MetadataGuard guard(*this);
        std::erase(runtimes_, token);
    }
    if (pending_count_.load(std::memory_order_relaxed) != 0) {
        ReclaimRetired();
    }
}

void SmcTracker::BeginJit(const RuntimeToken& token) {
    if (!token) {
        return;
    }
    if (!multithreaded_.load(std::memory_order_acquire)) {
        return;
    }
    for (;;) {
        const auto epoch = global_epoch_.load(std::memory_order_seq_cst);
        token->active_epoch.store(epoch, std::memory_order_seq_cst);
        // Closing the entry race requires validation after publication. If
        // invalidation advanced the epoch between the two loads, retry before
        // looking up a JIT pointer. With the same seq_cst order used by the
        // reclaimer's scan, either that scan observes this runtime in the old
        // epoch or this runtime observes the bump and republishes.
        if (global_epoch_.load(std::memory_order_seq_cst) == epoch) {
            return;
        }
    }
}

void SmcTracker::EndJit(const RuntimeToken& token) {
    if (!token || !multithreaded_.load(std::memory_order_acquire)) {
        return;
    }
    token->active_epoch.store(kInactiveEpoch, std::memory_order_seq_cst);
    if (pending_count_.load(std::memory_order_relaxed) != 0) {
        ReclaimRetired();
    }
}

void SmcTracker::EnableMultithreading() {
    multithreaded_.store(true, std::memory_order_release);
}

void SmcTracker::RegisterNode(const std::shared_ptr<Module>& module,
                              ir::AddressNode* node,
                              VAddr guest_start,
                              VAddr guest_end) {
    if (!IsEnabled()) {
        return;
    }
    MetadataGuard guard(*this);
    if (!locally_enabled_) {
        return;
    }
    if (guest_end <= guest_start) {
        // A block whose range wraps the address space has no pages to track.
        // Block locations are guest-controlled -- `jmp rax` with rax = -1
        // produces start = 2^64-1, end = 0 -- and the walk below would then
        // step past the top page, wrap to 0 and insert a PageRecord for every
        // page in the address space until the host ran out of memory.
        if (guest_start == std::numeric_limits<VAddr>::max()) {
            return;
        }
        guest_end = guest_start + 1;
    }
    // Page-walk bounds are taken in *window* coordinates so that the pages_
    // keys match what HandleWriteFault derives from a fault address and what
    // SetPageProtected actually mprotects. A range that leaves the window is
    // not a real translation (legitimate blocks were fetched through the same
    // truncation), so there is nothing to protect.
    VAddr page_lo = guest_start;
    VAddr page_hi = guest_end - 1;
    if (mask_ != UINT64_MAX) {
        const u64 len = guest_end - guest_start;
        const VAddr lo = guest_start & mask_;
        if (len > mask_ + 1 - lo) {
            return;
        }
        page_lo = lo;
        page_hi = lo + len - 1;
    }
    const VAddr first = PageKey(page_lo);
    const VAddr last = PageKey(page_hi);
    if (last < first) {
        return;
    }
    for (VAddr page = first; page <= last; page += page_size_) {
        auto& rec = pages_[page];
        // O(1) duplicate check: node pointer is the unique key (a single
        // compiled unit has exactly one guest_start/guest_end pair).
        if (rec.node_ptrs.insert(node).second) {
            rec.nodes.push_back(
                    TrackedNode{module, ir::NodeRef{node}, guest_start, guest_end});
        }
        // A translation published during another thread's open write window
        // is collected by CloseWriteWindow's retry loop. Do not protect the
        // page until that batch has detached every such translation.
        if (!rec.dirty && !rec.write_protected) {
            if (SetPageProtected(page, true)) {
                rec.write_protected = true;
                rec.claim_stale_fault = false;
            }
        }
        // page += page_size_ would wrap past the top of the address space and
        // restart the walk at 0; `last` is inclusive, so stop here instead.
        if (last - page < page_size_) {
            break;
        }
    }
}

void SmcTracker::ClearDispatchSlots(AddressSpace& space,
                                    TranslateTable* extra_l1,
                                    const TrackedNode& tracked) {
    auto clear_location = [&](VAddr location) {
        space.GetCodeCacheTable().Zero(location);
        if (extra_l1) {
            extra_l1->Zero(location);
        }
        for (const auto& runtime : runtimes_) {
            if (runtime && runtime->l1 && runtime->l1 != extra_l1) {
                runtime->l1->Zero(location);
            }
        }
    };

    auto* node = tracked.node.Get();
    clear_location(node->GetStartLocation().Value());
    if (node->node_type == ir::AddressNode::Function) {
        auto* function = static_cast<ir::Function*>(node);
        for (auto& block : function->GetBlocks()) {
            clear_location(block.GetStartLocation().Value());
        }
    }
}

void SmcTracker::RemoveTrackedNode(ir::AddressNode* node) {
    for (auto& [page, record] : pages_) {
        // Erase from the O(1) lookup set first; then sweep the vector.
        // node_ptrs.erase is a no-op if this page never held the node.
        record.node_ptrs.erase(node);
        std::erase_if(record.nodes,
                      [&](const TrackedNode& tracked) { return tracked.node.Get() == node; });
    }
}

std::vector<SmcTracker::TrackedNode> SmcTracker::TakeDirtyNodes(
        AddressSpace& space, TranslateTable* extra_l1) {
    std::vector<TrackedNode> nodes;
    // O(1) cross-page dedup: a function can span many pages; avoid adding the
    // same node to the result more than once without an any_of scan per entry.
    std::unordered_set<ir::AddressNode*> seen;
    for (auto& [page, record] : pages_) {
        if (!record.dirty) {
            continue;
        }
        for (const auto& tracked : record.nodes) {
            if (seen.insert(tracked.node.Get()).second) {
                ClearDispatchSlots(space, extra_l1, tracked);
                nodes.push_back(tracked);
            }
        }
    }
    for (const auto& tracked : nodes) {
        RemoveTrackedNode(tracked.node.Get());
    }
    return nodes;
}

std::vector<SmcTracker::TrackedNode> SmcTracker::TakeRangeNodes(
        AddressSpace& space, TranslateTable* extra_l1, VAddr first, VAddr last) {
    std::vector<TrackedNode> nodes;
    std::unordered_set<ir::AddressNode*> seen;
    for (VAddr page = first; page <= last; page += page_size_) {
        auto it = pages_.find(page);
        if (it == pages_.end()) {
            continue;
        }
        for (const auto& tracked : it->second.nodes) {
            if (seen.insert(tracked.node.Get()).second) {
                ClearDispatchSlots(space, extra_l1, tracked);
                nodes.push_back(tracked);
            }
        }
    }
    for (const auto& tracked : nodes) {
        RemoveTrackedNode(tracked.node.Get());
    }
    return nodes;
}

bool SmcTracker::HandleWriteFault(AddressSpace& space,
                                  TranslateTable& current_l1,
                                  std::uintptr_t fault_host_addr) {
    const VAddr guest = static_cast<VAddr>(fault_host_addr) - bias_;  // already in-window
    MetadataGuard guard(*this);
    const auto it = pages_.find(PageKey(guest));
    if (it == pages_.end()) {
        return false;
    }
    auto& rec = it->second;
    // A second CPU may have taken the old protection fault but reached this
    // handler only after the first CPU closed the window and detached the
    // page's last node. The page is still mapped RW; claim the delayed
    // synchronous fault so sigreturn retries the store instead of treating it
    // as an unrelated guest-memory failure.
    if (rec.claim_stale_fault && !rec.write_protected) {
        return true;
    }
    // Another thread may have taken the same protection fault before the
    // first handler completed mprotect. The page is now writable and the
    // pending fault can safely resume.
    if (rec.dirty && !rec.write_protected) {
        return true;
    }
    if (!rec.write_protected) {
        return false;
    }
    if (!SetPageProtected(it->first, false)) {
        return false;
    }
    rec.write_protected = false;
    rec.dirty = true;
    for (const auto& tracked : rec.nodes) {
        ClearDispatchSlots(space, &current_l1, tracked);
    }
    return true;
}

bool SmcTracker::CanReclaim(u64 retire_epoch) const {
    for (const auto& runtime : runtimes_) {
        if (!runtime) {
            continue;
        }
        const auto active = runtime->active_epoch.load(std::memory_order_seq_cst);
        if (active != kInactiveEpoch && active < retire_epoch) {
            return false;
        }
    }
    return true;
}

void SmcTracker::Retire(std::vector<ReclaimCandidate>& candidates) {
    if (candidates.empty()) {
        return;
    }
    // Dispatch visibility was removed before this release operation. A
    // Runtime that publishes the resulting epoch must therefore observe the
    // cleared tables and cannot newly enter one of these allocations.
    const auto retire_epoch = global_epoch_.fetch_add(1, std::memory_order_seq_cst) + 1;
    MetadataGuard guard(*this);
    for (auto& candidate : candidates) {
        retired_.push_back(
                RetiredCode{std::move(candidate.module), candidate.exec_ptr, retire_epoch});
    }
    candidates.clear();
    pending_count_.store(retired_.size(), std::memory_order_relaxed);
}

void SmcTracker::ReclaimRetiredLocked() {
    std::vector<RetiredCode> ready;
    {
        MetadataGuard guard(*this);
        for (auto it = retired_.begin(); it != retired_.end();) {
            if (CanReclaim(it->retire_epoch)) {
                ready.push_back(std::move(*it));
                it = retired_.erase(it);
            } else {
                ++it;
            }
        }
        pending_count_.store(retired_.size(), std::memory_order_relaxed);
    }
    for (auto& retired : ready) {
        retired.module->ReclaimCode(retired.exec_ptr);
    }
}

void SmcTracker::ReclaimRetired() {
    std::lock_guard invalidation_guard(invalidation_mutex_);
    ReclaimRetiredLocked();
}

void SmcTracker::CloseWriteWindow(AddressSpace& space, TranslateTable& current_l1) {
    std::lock_guard invalidation_guard(invalidation_mutex_);
    std::vector<ReclaimCandidate> candidates;

    for (;;) {
        std::vector<TrackedNode> nodes;
        {
            MetadataGuard guard(*this);
            nodes = TakeDirtyNodes(space, &current_l1);
            if (nodes.empty()) {
                // An empty batch IMPLIES every dirty page now has an empty
                // node list: TakeDirtyNodes visits every dirty page, collects
                // its nodes, and only then erases them from the records, so
                // "it returned nothing" and "no dirty page still tracks a
                // node" are the same statement. There is therefore nothing
                // left to detach and nothing to re-protect here -- pages are
                // re-protected by RegisterNode when a new translation is
                // published on them, which is the only moment protecting them
                // again is useful.
                for (auto& [page, rec] : pages_) {
                    if (!rec.dirty) {
                        continue;
                    }
                    rec.dirty = false;
                    // Keep an RW tombstone until the page is either
                    // re-protected by RegisterNode or synchronously unmapped.
                    // It closes the delayed-second-fault race described in
                    // HandleWriteFault.
                    rec.claim_stale_fault = true;
                }
                break;
            }
        }

        // Do not hold the signal-handler metadata spinlock while waiting on a
        // module/node lock. A publisher holds those locks while RegisterNode
        // takes metadata_lock_; the retry loop collects anything it published
        // during this detach phase.
        for (const auto& tracked : nodes) {
            if (auto* exec_ptr = tracked.module->DetachNode(tracked.node.Get()); exec_ptr) {
                candidates.push_back(ReclaimCandidate{tracked.module, exec_ptr});
            }
        }
    }

    Retire(candidates);
    ReclaimRetiredLocked();
}

void SmcTracker::InvalidateRange(AddressSpace& space,
                                 TranslateTable* current_l1,
                                 VAddr guest_start,
                                 VAddr guest_end) {
    if (guest_end <= guest_start) {
        return;
    }
    // Window coordinates, matching RegisterNode's page keys.
    if (mask_ != UINT64_MAX) {
        const u64 len = guest_end - guest_start;
        const VAddr lo = guest_start & mask_;
        if (len > mask_ + 1 - lo) {
            return;
        }
        guest_start = lo;
        guest_end = lo + len;
    }
    const VAddr first = PageKey(guest_start);
    const VAddr last = PageKey(guest_end - 1);
    std::lock_guard invalidation_guard(invalidation_mutex_);
    std::vector<ReclaimCandidate> candidates;

    for (;;) {
        std::vector<TrackedNode> nodes;
        {
            MetadataGuard guard(*this);
            nodes = TakeRangeNodes(space, current_l1, first, last);
            if (nodes.empty()) {
                for (VAddr page = first; page <= last; page += page_size_) {
                    auto it = pages_.find(page);
                    if (it == pages_.end()) {
                        continue;
                    }
                    if (it->second.write_protected) {
                        SetPageProtected(page, false);
                    }
                    pages_.erase(it);
                }
                break;
            }
        }
        for (const auto& tracked : nodes) {
            if (auto* exec_ptr = tracked.module->DetachNode(tracked.node.Get()); exec_ptr) {
                candidates.push_back(ReclaimCandidate{tracked.module, exec_ptr});
            }
        }
    }

    Retire(candidates);
    ReclaimRetiredLocked();
}

void SmcTracker::DisableAndUnprotectAll() {
    std::lock_guard invalidation_guard(invalidation_mutex_);
    // Dropped after metadata_lock_ is released: the records own node
    // references, and releasing the last one runs ~Block/~Function (which
    // frees the whole instruction list) -- work that must not happen under a
    // spinlock the signal handler also takes.
    std::map<VAddr, PageRecord> dead_pages;
    {
        MetadataGuard guard(*this);
        locally_enabled_ = false;
        multithreaded_.store(false, std::memory_order_release);
        for (const auto& runtime : runtimes_) {
            if (runtime) {
                runtime->active_epoch.store(kInactiveEpoch, std::memory_order_seq_cst);
            }
        }
        for (auto& [page, record] : pages_) {
            if (record.write_protected) {
                SetPageProtected(page, false);
            }
        }
        dead_pages.swap(pages_);
    }
    dead_pages.clear();
    ReclaimRetiredLocked();
}

bool SmcTracker::HasProtectedPages() const {
    MetadataGuard guard(*this);
    return std::any_of(pages_.begin(), pages_.end(), [](const auto& entry) {
        return entry.second.write_protected;
    });
}

}  // namespace swift::runtime::backend
