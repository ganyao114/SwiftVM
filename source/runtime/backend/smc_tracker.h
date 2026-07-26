//
// Self-modifying code (SMC) tracking for the SwiftVM runtime backend.
//
// Translated guest-code pages are write-protected. A guest write opens a
// temporary RW window and removes every affected translation from all
// dispatch tables. Module visibility is detached after the current JitRun
// returns, but executable storage and fault metadata are retained under a
// QSBR epoch until no Runtime can still be executing the old code.
//
// Cross-thread semantics:
//  - A writer observes modified code on its next dispatch.
//  - Another guest CPU may finish code it entered before invalidation.
//  - Every per-Runtime L1 and the shared AddressSpace L2 are cleared, so a
//    later dispatch cannot newly enter the retired translation.
//  - Direct intra-function/self-label edges may remain stale until that CPU
//    reaches a host boundary; its published epoch keeps the code alive.
//
// Limitations:
//  - No mid-block rewind: a block that patches a later instruction in itself
//    finishes the current translated instance once.
//  - Blocks interpreted with JIT disabled are not tracked. The interpreter
//    reuses decoded IR and needs a separate SMC design.
//

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "runtime/backend/translate_table.h"
#include "runtime/common/types.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend {

class AddressSpace;
class Module;

class SmcTracker {
public:
    static constexpr u64 kInactiveEpoch = UINT64_MAX;

    struct RuntimeEpoch {
        explicit RuntimeEpoch(TranslateTable* table) : l1(table) {}

        std::atomic<u64> active_epoch{kInactiveEpoch};
        TranslateTable* l1{};
    };
    using RuntimeToken = std::shared_ptr<RuntimeEpoch>;

    // guest_bias: guest->host address bias (host = guest + bias), from
    // Config::memory_base (0 = identity mapping).
    // guest_addr_mask: bounded-guest-window mask (see Config), 0 = disabled.
    explicit SmcTracker(u64 guest_bias, u64 guest_addr_mask = 0);

    // Called after a block/function is fully published. Every host page
    // overlapping [guest_start, guest_end) is write-protected and records the
    // owning module/node. Idempotent for an identical range.
    void RegisterNode(const std::shared_ptr<Module>& module,
                      ir::AddressNode* node,
                      VAddr guest_start,
                      VAddr guest_end);

    // Runtime/QSBR registration. BeginJit runs before any cache lookup that
    // can yield a JIT pointer; EndJit runs immediately after trampoline
    // return. Entry publishes and validates a generation using atomics only;
    // reclamation locking occurs only while pending_count_ is non-zero.
    [[nodiscard]] RuntimeToken RegisterRuntime(TranslateTable& l1);
    void UnregisterRuntime(const RuntimeToken& token);
    void BeginJit(const RuntimeToken& token);
    void EndJit(const RuntimeToken& token);

    // Called before the first guest thread is spawned. Single-threaded guests
    // leave runtime epochs inactive and retain their cheaper path.
    void EnableMultithreading();

    // Signal-handler path. Opens the page's write window and eagerly zeroes
    // the shared L2 plus every registered L1 slot for affected nodes.
    bool HandleWriteFault(AddressSpace& space,
                          TranslateTable& current_l1,
                          std::uintptr_t fault_host_addr);

    // Host-side boundary after JitRun. Detaches all translations on dirty
    // pages, retires their code/fault metadata, and closes the write window.
    void CloseWriteWindow(AddressSpace& space, TranslateTable& current_l1);

    // Synchronous invalidation for guest mprotect/mmap/munmap. All registered
    // Runtime L1 caches are cleared; current_l1 may be null for callers that
    // are not associated with a Runtime.
    void InvalidateRange(AddressSpace& space,
                         TranslateTable* current_l1,
                         VAddr guest_start,
                         VAddr guest_end);

    [[nodiscard]] bool HasProtectedPages() const;

    // Process-wide test-harness switch. When false, RegisterNode is a no-op.
    static void SetEnabled(bool enabled);
    [[nodiscard]] static bool IsEnabled();

    // Diagnostic MT kill switch used by SVM_SMC_MT=0. Restores every tracked
    // page to RW, clears metadata, and suppresses future registration.
    void DisableAndUnprotectAll();

private:
    class MetadataGuard {
    public:
        explicit MetadataGuard(const SmcTracker& tracker) : tracker_(tracker) {
            tracker_.LockMetadata();
        }
        ~MetadataGuard() { tracker_.UnlockMetadata(); }

    private:
        const SmcTracker& tracker_;
    };

    struct TrackedNode {
        std::shared_ptr<Module> module;
        ir::AddressNode* node{};
        VAddr guest_start{};
        VAddr guest_end{};
    };

    struct PageRecord {
        bool write_protected{};
        bool dirty{};
        bool claim_stale_fault{};
        u32 invalidations{};
        std::vector<TrackedNode> nodes;
    };

    struct RetiredCode {
        std::shared_ptr<Module> module;
        u8* exec_ptr{};
        u64 retire_epoch{};
    };

    struct ReclaimCandidate {
        std::shared_ptr<Module> module;
        u8* exec_ptr{};
    };

    static constexpr u32 kMaxInvalidations = 8;

    [[nodiscard]] VAddr PageKey(VAddr guest_addr) const { return guest_addr & ~page_mask_; }

    bool SetPageProtected(VAddr page, bool prot_read_only);
    void LockMetadata() const;
    void UnlockMetadata() const;

    // The following metadata helpers require metadata_lock_.
    void ClearDispatchSlots(AddressSpace& space,
                            TranslateTable* extra_l1,
                            const TrackedNode& tracked);
    [[nodiscard]] std::vector<TrackedNode> TakeDirtyNodes(AddressSpace& space,
                                                          TranslateTable* extra_l1);
    [[nodiscard]] std::vector<TrackedNode> TakeRangeNodes(AddressSpace& space,
                                                          TranslateTable* extra_l1,
                                                          VAddr first,
                                                          VAddr last);
    void RemoveTrackedNode(ir::AddressNode* node);
    [[nodiscard]] bool CanReclaim(u64 retire_epoch) const;

    // invalidation_mutex_ must be held.
    void Retire(std::vector<ReclaimCandidate>& candidates);
    void ReclaimRetiredLocked();
    void ReclaimRetired();

    const u64 bias_;
    // Guest window mask; UINT64_MAX when the window is disabled. Every guest
    // page is truncated with it before bias_ is added, so the write
    // protection we install can only ever land inside the guest window --
    // never on a host mapping (the translator's own __TEXT included).
    const u64 mask_;
    const u64 page_size_;
    const u64 page_mask_;
    std::map<VAddr, PageRecord> pages_{};
    std::vector<VAddr> disabled_pages_{};
    std::vector<RuntimeToken> runtimes_{};
    std::vector<RetiredCode> retired_{};
    mutable std::atomic_flag metadata_lock_ = ATOMIC_FLAG_INIT;
    std::mutex invalidation_mutex_{};
    std::atomic<u64> global_epoch_{1};
    std::atomic<size_t> pending_count_{0};
    std::atomic_bool multithreaded_{false};
    bool locally_enabled_{true};
};

}  // namespace swift::runtime::backend
