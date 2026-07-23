//
// Self-modifying code (SMC) tracking for the SwiftVM runtime backend.
//
// Purpose (Phase 4): when guest code writes to a guest page that contains
// JIT-translated code, the stale translations must be invalidated so the
// next execution re-translates from the modified bytes.
//
// Design (modeled on cross86's SmcTracker + FEX's mtrack, simplified for the
// single-threaded drivers):
//  - RegisterBlock() is called right after a block is JIT-compiled. Every
//    *host* page overlapping the block's guest range is write-protected
//    (mprotect PROT_READ) and a page -> blocks record is kept.
//  - A guest store to a protected page raises a host SIGSEGV/SIGBUS. The
//    runtime's SMC fault handler (SignalHandler chain, priority 0 — ahead of
//    the JIT guest-fault recovery) calls HandleWriteFault(), which:
//      1. marks the page dirty (a "write window" opens),
//      2. drops the page back to RW so the faulting store can complete on
//         sigreturn,
//      3. eagerly zeroes the L1/L2 dispatch-table slots of every block on
//         the page, so linked control flow (JitContext::Forward indirect
//         links read the L2 slot directly) can no longer jump into the stale
//         code and falls back to the dispatcher instead.
//  - CloseWriteWindow() runs after every JitRun return (the guest is never
//    inside JIT code at that point, so freeing is safe). For every dirty
//    page: all its blocks are removed from the module, their JIT code is
//    freed, their fault-table entries dropped, and the page is either
//    re-protected (blocks remain) or left writable and untracked (none
//    remain). The next dispatch on the invalidated locations misses and
//    re-translates from the patched guest bytes.
//
// Deliberate deviations / limitations:
//  - No mid-block rewind: a block that patches an instruction *later in the
//    same block instance* finishes with stale code once (the faulting store
//    itself completes and the modification is picked up the next time the
//    block is entered). Rewinding to the block entry from the signal handler
//    would re-execute the store against a re-protected page and loop
//    forever; block-boundary invalidation is the standard DBT compromise.
//  - A block that jumps to its own start (JitContext::Forward self-label
//    branch, no dispatch slot) and writes its own page inside the loop is
//    not detected until it leaves the loop.
//  - Host-page granularity (16KB on macOS arm64): a write to a non-code
//    guest page sharing a host page with translated code causes a spurious
//    (but correct) invalidation. Pages that exceed kMaxInvalidations stop
//    being re-protected (SMC disabled for them) so data/text-straddling
//    pages cannot livelock the translator.
//  - Blocks interpreted (JIT off) are not tracked; the IR interpreter
//    re-uses decoded blocks and would need its own SMC story.
//

#pragma once

#include <map>
#include <vector>
#include "runtime/backend/translate_table.h"
#include "runtime/common/types.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend {

class AddressSpace;

class SmcTracker {
public:
    // guest_bias: guest->host address bias (host = guest + bias), from
    // Config::memory_base (0 = identity mapping).
    explicit SmcTracker(u64 guest_bias);

    // Called after a block is JIT-compiled: write-protect the host pages
    // covering [guest_start, guest_end) and record block ownership.
    // Idempotent per page; safe to call for a page that is already tracked.
    void RegisterBlock(ir::Block* block, VAddr guest_start, VAddr guest_end);

    // Signal-handler path. If fault_host_addr hits a write-protected tracked
    // page: open the write window (unprotect), mark the page dirty, and
    // eagerly zero the L1 (per-runtime) / L2 (address-space) dispatch slots
    // of all blocks on the page. Returns true when the fault was claimed
    // (the faulting store may be resumed).
    bool HandleWriteFault(AddressSpace& space,
                          TranslateTable& l1,
                          std::uintptr_t fault_host_addr);

    // Called from Runtime::Run after every JitRun return. Invalidates all
    // blocks on dirty pages (module removal + code free + fault-table
    // cleanup) and restores / drops page protections. No-op when no write
    // window is open.
    void CloseWriteWindow(AddressSpace& space, TranslateTable& l1);

    // Synchronous invalidation for guest mprotect/mmap/munmap: invalidates
    // every tracked block overlapping [guest_start, guest_end) and stops
    // tracking (and write-protecting) the affected pages. `l1` may be
    // nullptr (the caller — the syscall layer — is then responsible for
    // flushing per-runtime L1 caches; with the current single-runtime
    // drivers that is the Runtime that triggered the syscall).
    void InvalidateRange(AddressSpace& space,
                         TranslateTable* l1,
                         VAddr guest_start,
                         VAddr guest_end);

    // True if any page is currently write-protected (tests / diagnostics).
    [[nodiscard]] bool HasProtectedPages() const;

    // Process-wide enable switch (default: enabled). When disabled,
    // RegisterBlock is a no-op, so no page is ever write-protected. Intended
    // for test harnesses that rewrite a guest code arena from OUTSIDE
    // Runtime::Run (e.g. the differential fuzzer): the fault raised by their
    // memcpy into a previously translated — and thus write-protected — page
    // cannot be claimed by the SMC handler (no active runtime on the thread)
    // and would kill the process. Such harnesses disable tracking for their
    // lifetime instead of going through InvalidateRange, to which they have
    // no access (the AddressSpace is not reachable through the public
    // translator Instance/Core API).
    static void SetEnabled(bool enabled);
    [[nodiscard]] static bool IsEnabled();

private:
    struct TrackedBlock {
        ir::Block* block{};
        VAddr guest_start{};
        VAddr guest_end{};
    };

    struct PageRecord {
        bool write_protected{};
        bool dirty{};
        u32 invalidations{};
        std::vector<TrackedBlock> blocks;
    };

    // After this many invalidations a page is considered data/text-straddled
    // and is never write-protected again (avoids fault/invalidate livelock).
    static constexpr u32 kMaxInvalidations = 8;

    [[nodiscard]] VAddr PageKey(VAddr guest_addr) const { return guest_addr & ~page_mask_; }

    // (Un)protects the host page backing guest page key `page`.
    // Returns false when the mprotect failed (page left untracked-as-
    // protected; SMC detection for it degrades gracefully).
    bool SetPageProtected(VAddr page, bool prot_read_only);

    // Invalidates `tb` fully: erases it from every page record covering its
    // guest range, removes it from its module, frees the JIT code and drops
    // the fault-table entry, and zeroes the L1/L2 dispatch slots.
    void InvalidateBlock(AddressSpace& space, TranslateTable* l1, const TrackedBlock& tb);

    const u64 bias_;
    const u64 page_size_;
    const u64 page_mask_;
    std::map<VAddr, PageRecord> pages_{};
    // Pages that gave up on SMC tracking (see kMaxInvalidations).
    std::vector<VAddr> disabled_pages_{};
};

}  // namespace swift::runtime::backend
