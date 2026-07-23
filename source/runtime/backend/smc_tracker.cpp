//
// Self-modifying code (SMC) tracking — see smc_tracker.h for the design.
//

#include "smc_tracker.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/module.h"
#include "runtime/common/logging.h"

namespace swift::runtime::backend {

SmcTracker::SmcTracker(u64 guest_bias)
        : bias_(guest_bias)
        , page_size_(static_cast<u64>(getpagesize()))
        , page_mask_(page_size_ - 1) {
    ASSERT((page_size_ & page_mask_) == 0);  // page size is a power of two
}

bool SmcTracker::SetPageProtected(VAddr page, bool prot_read_only) {
    const auto host = reinterpret_cast<void*>(page + bias_);
    const int prot = prot_read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
    if (mprotect(host, page_size_, prot) != 0) {
        LOG_ERROR("SMC: mprotect({:#x}, {}) failed: {}",
                  page + bias_,
                  prot_read_only ? "R" : "RW",
                  std::strerror(errno));
        return false;
    }
    return true;
}

void SmcTracker::RegisterBlock(ir::Block* block, VAddr guest_start, VAddr guest_end) {
    if (guest_end <= guest_start) {
        // Degenerate range (block end unknown): still track the start page —
        // better partial coverage than none.
        guest_end = guest_start + 1;
    }
    const VAddr first = PageKey(guest_start);
    const VAddr last = PageKey(guest_end - 1);
    for (VAddr page = first; page <= last; page += page_size_) {
        if (std::find(disabled_pages_.begin(), disabled_pages_.end(), page) !=
            disabled_pages_.end()) {
            continue;  // page gave up on SMC (data/text straddle livelock guard)
        }
        auto& rec = pages_[page];
        // A new block may only be registered outside of an open write window
        // (translation happens after CloseWriteWindow), so a dirty page here
        // means a previous invalidation left it writable — re-protecting is
        // correct.
        rec.blocks.push_back(TrackedBlock{block, guest_start, guest_end});
        if (!rec.write_protected) {
            if (SetPageProtected(page, true)) {
                rec.write_protected = true;
                rec.dirty = false;
            }
        }
    }
}

bool SmcTracker::HandleWriteFault(AddressSpace& space,
                                  TranslateTable& l1,
                                  std::uintptr_t fault_host_addr) {
    const VAddr guest = static_cast<VAddr>(fault_host_addr) - bias_;
    const auto it = pages_.find(PageKey(guest));
    if (it == pages_.end() || !it->second.write_protected) {
        return false;  // not an SMC write-protect fault
    }
    auto& rec = it->second;
    // Open the write window: let the faulting store complete on sigreturn.
    if (!SetPageProtected(it->first, false)) {
        return false;  // could not unprotect; let the default handler crash
    }
    rec.write_protected = false;
    rec.dirty = true;
    // Eager dispatch-slot invalidation: with BlockLink enabled the JIT reads
    // L2 slots directly, so without this the very next forward would jump
    // straight back into the stale code and the invalidation deferred to
    // CloseWriteWindow would never be observed. A zeroed slot falls back to
    // the dispatcher, which misses (L1/L2 both zeroed) and returns CodeMiss.
    for (const auto& tb : rec.blocks) {
        const auto loc = tb.block->GetStartLocation().Value();
        space.GetCodeCacheTable().Zero(loc);
        l1.Zero(loc);
    }
    return true;
}

void SmcTracker::InvalidateBlock(AddressSpace& space,
                                 TranslateTable* l1,
                                 const TrackedBlock& tb) {
    const auto loc = tb.block->GetStartLocation().Value();
    // Detach from every page record covering the block's guest range BEFORE
    // the module removal below possibly destroys the block object.
    const VAddr first = PageKey(tb.guest_start);
    const VAddr last = PageKey(tb.guest_end > tb.guest_start ? tb.guest_end - 1
                                                             : tb.guest_start);
    for (VAddr page = first; page <= last; page += page_size_) {
        if (auto it = pages_.find(page); it != pages_.end()) {
            auto& blocks = it->second.blocks;
            std::erase_if(blocks, [&](const TrackedBlock& o) { return o.block == tb.block; });
        }
    }
    if (auto module = space.GetModule(loc)) {
        module->InvalidateBlock(tb.block);
    }
    // Idempotent with the eager zeroing in HandleWriteFault; covers the
    // synchronous InvalidateRange path (no prior fault) as well.
    space.GetCodeCacheTable().Zero(loc);
    if (l1) {
        l1->Zero(loc);
    }
}

void SmcTracker::CloseWriteWindow(AddressSpace& space, TranslateTable& l1) {
    // Collect the dirty pages' blocks first; dedupe by block pointer (a
    // block spanning multiple dirty pages must be invalidated once).
    std::vector<TrackedBlock> to_invalidate;
    std::vector<VAddr> dirty_pages;
    for (auto& [page, rec] : pages_) {
        if (!rec.dirty) {
            continue;
        }
        dirty_pages.push_back(page);
        for (const auto& tb : rec.blocks) {
            const bool seen = std::any_of(to_invalidate.begin(),
                                          to_invalidate.end(),
                                          [&](const TrackedBlock& o) {
                                              return o.block == tb.block;
                                          });
            if (!seen) {
                to_invalidate.push_back(tb);
            }
        }
    }
    if (dirty_pages.empty()) {
        return;
    }
    for (const auto& tb : to_invalidate) {
        InvalidateBlock(space, &l1, tb);
    }
    for (const VAddr page : dirty_pages) {
        auto it = pages_.find(page);
        if (it == pages_.end()) {
            continue;
        }
        auto& rec = it->second;
        rec.dirty = false;
        rec.invalidations++;
        if (rec.blocks.empty()) {
            // No translated code left on the page: stop tracking it, leave
            // it writable (already unprotected by HandleWriteFault).
            pages_.erase(it);
            continue;
        }
        if (rec.invalidations > kMaxInvalidations) {
            // Data/text-straddling page: give up on SMC for it rather than
            // faulting on every data write forever.
            disabled_pages_.push_back(page);
            pages_.erase(it);
            continue;
        }
        // Remaining blocks still need guarding.
        if (SetPageProtected(page, true)) {
            rec.write_protected = true;
        }
    }
}

void SmcTracker::InvalidateRange(AddressSpace& space,
                                 TranslateTable* l1,
                                 VAddr guest_start,
                                 VAddr guest_end) {
    if (guest_end <= guest_start) {
        return;
    }
    std::vector<TrackedBlock> to_invalidate;
    const VAddr first = PageKey(guest_start);
    const VAddr last = PageKey(guest_end - 1);
    for (VAddr page = first; page <= last; page += page_size_) {
        auto it = pages_.find(page);
        if (it == pages_.end()) {
            continue;
        }
        for (const auto& tb : it->second.blocks) {
            to_invalidate.push_back(tb);
        }
    }
    for (const auto& tb : to_invalidate) {
        InvalidateBlock(space, l1, tb);
    }
    // The guest is changing the mapping/permissions of these pages: drop
    // tracking and restore writability (guest_memory does not enforce guest
    // permissions itself, so a page we protected stays protected unless we
    // undo it here).
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
}

bool SmcTracker::HasProtectedPages() const {
    for (const auto& [page, rec] : pages_) {
        if (rec.write_protected) {
            return true;
        }
    }
    return false;
}

}  // namespace swift::runtime::backend
