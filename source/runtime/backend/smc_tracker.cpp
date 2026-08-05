//
// Self-modifying code (SMC) tracking — see smc_tracker.h.
//

#include "smc_tracker.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/module.h"
#include "runtime/common/backedge_control.h"
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

SmcTracker::SmcTracker(u64 guest_bias,
                       u64 guest_addr_mask,
                       bool exit_latch_enabled)
        : bias_(guest_bias)
        , mask_(guest_addr_mask ? guest_addr_mask : UINT64_MAX)
        , page_size_(static_cast<u64>(getpagesize()))
        , page_mask_(page_size_ - 1)
        , dirty_hint_enabled_(GetSvmConfig().smc_dirty_hint)
        , close_profile_enabled_(GetSvmConfig().exec_prof)
        , exit_latch_enabled_(exit_latch_enabled) {
    ASSERT((page_size_ & page_mask_) == 0);
}

SmcTracker::~SmcTracker() {
    if (!close_profile_enabled_) {
        return;
    }
    const auto calls = close_calls_.load(std::memory_order_relaxed);
    const auto fast = close_fast_returns_.load(std::memory_order_relaxed);
    const auto slow = close_slow_calls_.load(std::memory_order_relaxed);
    const auto total_ns = close_total_ns_.load(std::memory_order_relaxed);
    const auto fast_ns = close_fast_ns_.load(std::memory_order_relaxed);
    const auto slow_ns = close_slow_ns_.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[svm-smc] dirty_hint=%u close_calls=%llu fast=%llu slow=%llu "
                 "fast_pct=%.3f total_ns=%llu fast_ns=%llu slow_ns=%llu\n",
                 dirty_hint_enabled_ ? 1u : 0u,
                 calls,
                 fast,
                 slow,
                 calls ? static_cast<double>(fast) * 100.0 / static_cast<double>(calls) : 0.0,
                 total_ns,
                 fast_ns,
                 slow_ns);
}

void SmcTracker::MarkCloseWorkPending() {
    if (dirty_hint_enabled_) {
        close_work_pending_.store(true, std::memory_order_seq_cst);
    }
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

SmcTracker::RuntimeToken SmcTracker::RegisterRuntime(TranslateTable& l1,
                                                     u64* exit_request) {
    auto token = std::make_shared<RuntimeEpoch>(&l1, exit_request);
    // Registration occurs at a host boundary before this Runtime can execute
    // JIT code, so it is born synchronized with the current patch epoch and
    // pays no first-entry ISB in the no-delink steady state.
    token->synced_patch_epoch.store(code_patch_epoch_.load(std::memory_order_seq_cst),
                                    std::memory_order_relaxed);
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
        // Load the reclaim epoch first. Delink publishes patch_epoch before
        // advancing global_epoch_ under invalidation_mutex_. Therefore either
        // this iteration observes the new patch generation, or the existing
        // global-epoch validation below fails and retries. This ordering keeps
        // the steady-state tax to one patch load+compare.
        const auto epoch = global_epoch_.load(std::memory_order_seq_cst);
        const auto patch_epoch = code_patch_epoch_.load(std::memory_order_seq_cst);
        if (token->synced_patch_epoch.load(std::memory_order_relaxed) < patch_epoch) {
#if defined(__aarch64__)
            // Context synchronization is execution-thread-local. Cache
            // maintenance by the invalidator is not a substitute for ISB on
            // a core that may have already fetched the old branch.
            asm volatile("isb" ::: "memory");
#else
            // Keeps the protocol testable on non-AArch64 hosts; no generated
            // A64 instructions can execute there.
            std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
            token->synced_patch_epoch.store(patch_epoch, std::memory_order_seq_cst);
            token->patch_sync_count.fetch_add(1, std::memory_order_relaxed);
            patch_sync_count_.fetch_add(1, std::memory_order_relaxed);
        }
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

u64 SmcTracker::AdvanceCodePatchEpoch() {
    return code_patch_epoch_.fetch_add(1, std::memory_order_seq_cst) + 1;
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
        // Preserve the lock-free close invariant before publishing another
        // translation into an already-open write window. Normally the fault
        // already left the hint set; this store makes RegisterNode itself
        // conservative if that setup ever changes.
        if (rec.dirty) {
            MarkCloseWorkPending();
        }
        const bool duplicate = std::any_of(
                rec.nodes.begin(), rec.nodes.end(), [&](const TrackedNode& tracked) {
                    return tracked.node.Get() == node &&
                           tracked.guest_start == guest_start &&
                           tracked.guest_end == guest_end;
                });
        if (!duplicate) {
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
    // Publish only after every shared/L1 dispatch slot is clear. The release
    // operation below must order those clears before a generated LDAR that
    // chooses the deopt veneer; publishing first would permit an executing
    // loop to return and re-enter through a stale slot.
    PublishExitRequest();
}

void SmcTracker::PublishExitRequest() {
    if (!exit_latch_enabled_) {
        return;
    }
    static_assert(std::atomic_ref<u64>::required_alignment <= alignof(u64));
    for (const auto& runtime : runtimes_) {
        if (!runtime || !runtime->exit_request) {
            continue;
        }
        // Release publishes every dispatch-slot clear and dirty-page update
        // ordered before this call. The generated LDAR is the matching
        // acquire. A counter, rather than a boolean bit, lets the Runtime CAS
        // away only the exact request its cold veneer observed.
        std::atomic_ref<u64>(*runtime->exit_request)
                .fetch_add(1, std::memory_order_release);
    }
}

void SmcTracker::RemoveTrackedNode(ir::AddressNode* node) {
    for (auto& [page, record] : pages_) {
        std::erase_if(record.nodes,
                      [&](const TrackedNode& tracked) { return tracked.node.Get() == node; });
    }
}

void SmcTracker::DelinkTargets(AddressSpace& space,
                               const std::vector<TrackedNode>& nodes) {
    std::unordered_set<VAddr> targets;
    for (const auto& tracked : nodes) {
        auto* node = tracked.node.Get();
        targets.insert(node->GetStartLocation().Value());
        if (node->node_type == ir::AddressNode::Function) {
            for (auto& block : static_cast<ir::Function*>(node)->GetBlocks()) {
                targets.insert(block.GetStartLocation().Value());
            }
        }
    }

    auto& manager = space.GetLinkManager();
    size_t restored_linked{};
    bool patched_any{};
    for (const auto target : targets) {
        const auto incoming = manager.BeginTargetInvalidation(target);
        for (const auto& record : incoming) {
            auto* source_module =
                    const_cast<Module*>(static_cast<const Module*>(record.source_owner.module));
            const auto region = source_module
                    ? source_module->GetCodeRegion(record.site.region_id)
                    : std::nullopt;
            ASSERT_MSG(region, "direct-link source region {} disappeared before QSBR purge",
                       record.site.region_id);
            ASSERT(record.site.offset + sizeof(u32) <= region->capacity);
            auto* rx_site = region->rx_base + record.site.offset;
            auto* rw_site = region->rw_base + record.site.offset;
            ASSERT(region->trampoline_offset != CodeRegion::kInvalidTrampolineOffset);
            auto* trampoline = region->rx_base + region->trampoline_offset;
            const auto branch = EncodeBL(trampoline - rx_site);
            ASSERT(branch);
            ASSERT_MSG(PatchDirectBranch(*region, rx_site, rw_site, *branch),
                       "failed to restore direct-link site before target retirement");
            patched_any = true;
            restored_linked += record.state == LinkSiteState::Linked;
        }
    }
    if (restored_linked) {
        manager.RecordDelink(restored_linked);
    }
    if (patched_any) {
        // Cache maintenance for every BL above happens-before this patch
        // generation. Retire() advances global_epoch_ later in the same
        // invalidation_mutex_ transaction, which is the BeginJit handshake.
        (void)AdvanceCodePatchEpoch();
    }
}

std::vector<SmcTracker::TrackedNode> SmcTracker::TakeDirtyNodes(
        AddressSpace& space, TranslateTable* extra_l1) {
    std::vector<TrackedNode> nodes;
    for (auto& [page, record] : pages_) {
        if (!record.dirty) {
            continue;
        }
        for (const auto& tracked : record.nodes) {
            const bool duplicate = std::any_of(
                    nodes.begin(), nodes.end(), [&](const TrackedNode& other) {
                        return other.node.Get() == tracked.node.Get();
                    });
            if (!duplicate) {
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
    for (VAddr page = first; page <= last; page += page_size_) {
        auto it = pages_.find(page);
        if (it == pages_.end()) {
            continue;
        }
        for (const auto& tracked : it->second.nodes) {
            const bool duplicate = std::any_of(
                    nodes.begin(), nodes.end(), [&](const TrackedNode& other) {
                        return other.node.Get() == tracked.node.Get();
                    });
            if (!duplicate) {
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
        MarkCloseWorkPending();
        return true;
    }
    if (!rec.write_protected) {
        return false;
    }
    // This is the publication edge used by CloseWriteWindow's lock-free
    // proof. It must precede both opening the host page and rec.dirty=true:
    // seeing false may mean "the fault has not opened its window yet", never
    // "the page is already RW/dirty but the hint has not caught up".
    MarkCloseWorkPending();
    // Direct-link invalidation belongs to the synchronous fault transaction,
    // while this handler still owns metadata_lock_ and before the page becomes
    // writable. A slot clear alone cannot stop a B-linked cross-block cycle
    // from running forever and preventing CloseWriteWindow from being reached.
    //
    // SignalInvalidateTarget is lock-free/allocation-free: it atomically
    // deactivates the generation first, restores every by-value incoming site
    // to this region's BL trampoline form, waits out an older cold-link commit,
    // and restores once more. Advancing patch_epoch only after those cache
    // maintenance operations preserves the P edge consumed by BeginJit.
    auto signal_invalidate = [&](VAddr target) {
        const auto result = space.GetLinkManager().SignalInvalidateTarget(target);
        if (result.found) {
            (void)AdvanceCodePatchEpoch();
        }
    };
    for (const auto& tracked : rec.nodes) {
        auto* node = tracked.node.Get();
        signal_invalidate(node->GetStartLocation().Value());
        if (node->node_type == ir::AddressNode::Function) {
            for (auto& block : static_cast<ir::Function*>(node)->GetBlocks()) {
                signal_invalidate(block.GetStartLocation().Value());
            }
        }
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
    // Cover QSBR work with the same close hint. A false positive is harmless;
    // publishing before the epoch/list mutation prevents a lock-free close
    // from skipping reclaim work that is already pending.
    MarkCloseWorkPending();
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
        // Still under invalidation_mutex_: owner records remain queryable from
        // detach through the QSBR grace period, and are purged immediately
        // before the module/cache lock can free and reuse the allocation.
        auto& manager = retired.module->GetAddressSpace().GetLinkManager();
        (void)manager.PurgeSource({retired.module.get(), retired.exec_ptr});
        retired.module->ReclaimCode(retired.exec_ptr);
    }
    // Keep the hint true until the actual ReclaimCode calls finish, not merely
    // until entries move out of retired_. MaybeClear takes metadata_lock_, so a
    // concurrent fault cannot have its true publication overwritten.
    MaybeClearCloseWorkPending();
}

void SmcTracker::ReclaimRetired() {
    std::lock_guard invalidation_guard(invalidation_mutex_);
    ReclaimRetiredLocked();
}

void SmcTracker::MaybeClearCloseWorkPending() {
    if (!dirty_hint_enabled_) {
        return;
    }
    MetadataGuard guard(*this);
    if (!retired_.empty()) {
        return;
    }
    const bool dirty = std::any_of(pages_.begin(), pages_.end(), [](const auto& entry) {
        return entry.second.dirty;
    });
    if (!dirty) {
        close_work_pending_.store(false, std::memory_order_seq_cst);
    }
}

void SmcTracker::CloseWriteWindow(AddressSpace& space, TranslateTable& current_l1) {
    const auto profile_start =
            close_profile_enabled_ ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
    if (close_profile_enabled_) {
        close_calls_.fetch_add(1, std::memory_order_relaxed);
    }
    // pending_count_ is redundant with close_work_pending_ by invariant, but
    // retaining it as a second conservative gate makes any future reclaim
    // producer fail toward the slow path. Neither load is executed when the
    // default-OFF switch is unset.
    //
    // CloseWriteWindow returns void and its Runtime caller never branches on
    // whether a close did work; the only observable effects are the dirty
    // detach/re-protect/tombstone and reclaim side effects covered by the two
    // gates below.
    if (dirty_hint_enabled_ &&
        !close_work_pending_.load(std::memory_order_seq_cst) &&
        pending_count_.load(std::memory_order_acquire) == 0) {
        if (close_profile_enabled_) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - profile_start)
                                    .count();
            close_fast_returns_.fetch_add(1, std::memory_order_relaxed);
            close_fast_ns_.fetch_add(static_cast<unsigned long long>(ns),
                                    std::memory_order_relaxed);
            close_total_ns_.fetch_add(static_cast<unsigned long long>(ns),
                                     std::memory_order_relaxed);
        }
        return;
    }
    if (close_profile_enabled_) {
        close_slow_calls_.fetch_add(1, std::memory_order_relaxed);
    }
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
        // Lock order and irreversible sequence:
        // invalidation_mutex_ -> LinkManager (target inactive + BL restore) ->
        // patch epoch -> module/cache detach -> Retire/global epoch. The patch
        // epoch is advanced inside DelinkTargets after cache maintenance and
        // before any target allocation can enter the retired list.
        DelinkTargets(space, nodes);
        for (const auto& tracked : nodes) {
            if (auto* exec_ptr = tracked.module->DetachNode(tracked.node.Get()); exec_ptr) {
                (void)space.GetLinkManager().DetachSource(
                        {tracked.module.get(), exec_ptr});
                candidates.push_back(ReclaimCandidate{tracked.module, exec_ptr});
            }
        }
    }

    Retire(candidates);
    ReclaimRetiredLocked();
    if (close_profile_enabled_) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - profile_start)
                                .count();
        close_slow_ns_.fetch_add(static_cast<unsigned long long>(ns),
                                std::memory_order_relaxed);
        close_total_ns_.fetch_add(static_cast<unsigned long long>(ns),
                                 std::memory_order_relaxed);
    }
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
        DelinkTargets(space, nodes);
        for (const auto& tracked : nodes) {
            if (auto* exec_ptr = tracked.module->DetachNode(tracked.node.Get()); exec_ptr) {
                (void)space.GetLinkManager().DetachSource(
                        {tracked.module.get(), exec_ptr});
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
