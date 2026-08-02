#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "runtime/backend/code_cache.h"
#include "runtime/common/types.h"

namespace swift::runtime::backend {

enum class LinkSiteState : u8 {
    Unlinked,
    Linked,
    Far,
    Retiring,
};

struct LinkSiteKey {
    CodeRegionId region_id{};
    u32 offset{};

    bool operator==(const LinkSiteKey&) const = default;
};

struct LinkSourceOwner {
    const void* module{};
    const void* allocation{};

    bool operator==(const LinkSourceOwner&) const = default;
};

struct LinkSiteRecord {
    LinkSiteKey site{};
    u64 guest_target{};
    LinkSourceOwner source_owner{};
    u64 target_generation{};
    LinkSiteState state{LinkSiteState::Unlinked};
};

// Fully resolved at ordinary publication time. SignalInvalidation never
// performs region/module lookup, allocation, encoding, or address conversion.
struct LinkSignalPatchSite {
    CodeRegion region{};
    void* rx_site{};
    void* rw_site{};
    u32 unlinked_bl{};
};

// Host publication paired with the generation checked by the cold linker.
// region_id==0/host_pc==nullptr is retained for metadata-only users and tests;
// such a target can never be direct-linked by the region trampoline.
struct LinkTargetRecord {
    u64 guest_target{};
    void* host_pc{};
    CodeRegionId region_id{};
    u64 generation{};
    LinkSourceOwner target_owner{};
};

struct LinkManagerStats {
    size_t sites{};
    size_t linked{};
    size_t far{};
    size_t retiring{};
    size_t incoming_targets{};
    size_t outgoing_owners{};
    size_t target_records{};
    size_t max_in_degree{};
    size_t estimated_bytes{};
    u64 sites_registered{};
    u64 linker_calls{};
    u64 delinks{};
    u64 signal_invalidations{};
    size_t signal_targets_retained{};
    size_t signal_sites_retained{};
};

struct LinkSignalInvalidationResult {
    bool found{};
    size_t linked_sites{};
};

class LinkManager {
public:
    // Global lock order for future SMC/reclaim integration:
    //   SmcTracker::invalidation_mutex_ -> LinkManager::mutex_ -> module/cache locks.
    // The signal path instead enters under SmcTracker::metadata_lock_ and uses
    // only the lock-free signal index; it never acquires mutex_ or a module lock.
    // Query/link code that does not hold invalidation_mutex_ may take only mutex_.
    // Code holding a module/cache lock must never call back into LinkManager.
    [[nodiscard]] bool RegisterSite(LinkSiteKey site,
                                    u64 guest_target,
                                    LinkSourceOwner source_owner,
                                    const LinkSignalPatchSite* signal_patch = nullptr);
    [[nodiscard]] std::optional<LinkSiteRecord> QuerySite(LinkSiteKey site) const;

    // Publishing a target assigns a process-local, globally monotonic generation.
    // MarkLinked/MarkFar are the linkage-time generation recheck boundary.
    [[nodiscard]] u64 PublishTarget(u64 guest_target,
                                    void* host_pc = nullptr,
                                    CodeRegionId region_id = 0,
                                    LinkSourceOwner target_owner = {});
    [[nodiscard]] std::optional<LinkTargetRecord> QueryTarget(u64 guest_target) const;
    [[nodiscard]] std::optional<u64> QueryTargetGeneration(u64 guest_target) const;
    [[nodiscard]] bool ValidateTargetGeneration(u64 guest_target, u64 generation) const;
    using LinkCommit = std::function<bool(const LinkSiteRecord&)>;

    // commit runs while mutex_ is held, after generation validation and before
    // the record becomes Linked. The production region trampoline supplies
    // PatchDirectBranch here, preventing deferred invalidation from slipping
    // between metadata linkage and patch.
    [[nodiscard]] bool MarkLinked(LinkSiteKey site,
                                  u64 expected_generation,
                                  const LinkCommit& commit);
    [[nodiscard]] bool MarkFar(LinkSiteKey site, u64 expected_generation);

    // Marks the target unavailable before returning its incoming-site snapshot.
    // Linked/Far records become Unlinked. Retiring records remain queryable and
    // are returned too, so a later integration can conservatively restore BL at
    // a source that is detached but still protected by QSBR.
    [[nodiscard]] std::vector<LinkSiteRecord> BeginTargetInvalidation(u64 guest_target);

    // Detach preserves all three indexes and site lookup until QSBR reclamation.
    [[nodiscard]] size_t DetachSource(LinkSourceOwner source_owner);
    // Purge is the pre-ReclaimCode transaction that removes every owner site
    // from site, incoming-target and outgoing-owner indexes together.
    [[nodiscard]] size_t PurgeSource(LinkSourceOwner source_owner);

    // Cold observability hooks. RecordLinkerCall counts entries to the C++
    // helper; RecordDelink counts source instructions actually restored from
    // B to BL by an invalidation transaction.
    void RecordLinkerCall();
    void RecordDelink(size_t count = 1);

    // Async-signal path: lock-free, allocation-free and noexcept. It first
    // deactivates the published target generation, restores all current and
    // retiring incoming sites, waits out link commits that began before the
    // deactivate, then restores a second time so BL is the final instruction.
    // The returned count is the number of sites that had reached Linked.
    [[nodiscard]] LinkSignalInvalidationResult SignalInvalidateTarget(u64 guest_target) noexcept;

    [[nodiscard]] LinkManagerStats GetStats() const;

private:
    struct SignalSite;
    struct SignalTarget;

    struct LinkSiteKeyHash {
        size_t operator()(const LinkSiteKey& key) const noexcept;
    };

    struct LinkSourceOwnerHash {
        size_t operator()(const LinkSourceOwner& owner) const noexcept;
    };

    struct TargetRecord {
        u64 generation{};
        void* host_pc{};
        CodeRegionId region_id{};
        LinkSourceOwner target_owner{};
        SignalTarget* signal_target{};
        bool active{};
    };

    struct SignalSite {
        LinkSignalPatchSite patch{};
        std::atomic<SignalSite*> next{};
        std::atomic_bool linked{};
        // Purge publishes this tombstone before waiting for signal readers.
        // Nodes remain stable for the manager lifetime, so even a reader that
        // observes an old list link can safely see false and skip the address.
        std::atomic_bool live{};
    };

    struct SignalTarget {
        explicit SignalTarget(u64 guest_target) : guest_target(guest_target) {}
        u64 guest_target{};
        std::atomic<u64> active_generation{};
        std::atomic<u32> publishing_count{};
        std::atomic<u32> linking_count{};
        std::atomic<SignalSite*> sites{};
        std::atomic<SignalTarget*> hash_next{};
    };

    using SiteSet = std::unordered_set<LinkSiteKey, LinkSiteKeyHash>;
    struct OutgoingRecord {
        SiteSet sites{};
        bool retiring{};
    };

    [[nodiscard]] bool ValidateMarkLocked(LinkSiteKey site,
                                          u64 expected_generation,
                                          LinkSiteRecord*& record);
    [[nodiscard]] SignalTarget* FindSignalTarget(u64 guest_target) const noexcept;
    [[nodiscard]] SignalTarget* GetOrCreateSignalTargetLocked(u64 guest_target);
    void UnlinkSignalSiteLocked(SignalTarget& target, SignalSite* site);

    static constexpr size_t kSignalBucketCount = 4096;
    using SignalBucketArray = std::array<std::atomic<SignalTarget*>, kSignalBucketCount>;

    mutable std::mutex mutex_{};
    std::unordered_map<LinkSiteKey, LinkSiteRecord, LinkSiteKeyHash> sites_{};
    std::unordered_map<u64, SiteSet> incoming_{};
    std::unordered_map<LinkSourceOwner, OutgoingRecord, LinkSourceOwnerHash> outgoing_{};
    std::unordered_map<u64, TargetRecord> targets_{};
    std::unordered_map<LinkSiteKey, SignalSite*, LinkSiteKeyHash> signal_sites_{};
    std::vector<std::unique_ptr<SignalTarget>> signal_targets_storage_{};
    std::vector<std::unique_ptr<SignalSite>> signal_sites_storage_{};
    std::unique_ptr<SignalBucketArray> signal_buckets_storage_{};
    std::atomic<SignalBucketArray*> signal_buckets_{};
    std::atomic<u32> signal_readers_{};
    std::atomic<u64> signal_invalidations_{};
    std::atomic<u64> signal_delinks_{};
    u64 next_target_generation_{1};
    u64 sites_registered_{};
    u64 linker_calls_{};
    u64 delinks_{};
    size_t max_in_degree_{};
};

// Default-OFF direct-link-v2 production switch. It is intentionally separate
// from Optimizations::DirectBlockLink.
[[nodiscard]] bool DirectLinkV2Enabled();

[[nodiscard]] std::optional<u32> EncodeB(std::intptr_t offset);
[[nodiscard]] std::optional<u32> EncodeBL(std::intptr_t offset);
[[nodiscard]] std::optional<uintptr_t> DecodeBranchTarget(const void* site, u32 insn);

// Performs exactly one aligned 32-bit atomic instruction write through the RW
// alias, then maintains D-cache at RW and I-cache at RX. seq_cst is deliberately
// conservative for this cold patch primitive; it does not replace the
// BeginJit ISB/epoch protocol required before retired target code is reclaimed.
[[nodiscard]] bool PatchDirectBranch(const CodeRegion& region,
                                     void* rx_site,
                                     void* rw_site,
                                     u32 insn);

}  // namespace swift::runtime::backend
