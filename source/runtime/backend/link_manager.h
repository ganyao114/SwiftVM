#pragma once

#include <cstdint>
#include <functional>
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

// Host publication paired with the generation checked by the cold linker.
// region_id==0/host_pc==nullptr is retained for metadata-only users and tests;
// such a target can never be direct-linked by the region trampoline.
struct LinkTargetRecord {
    u64 guest_target{};
    void* host_pc{};
    CodeRegionId region_id{};
    u64 generation{};
};

struct LinkManagerStats {
    size_t sites{};
    size_t incoming_targets{};
    size_t outgoing_owners{};
    size_t target_records{};
};

class LinkManager {
public:
    // Global lock order for future SMC/reclaim integration:
    //   SmcTracker::invalidation_mutex_ -> LinkManager::mutex_ -> module/cache locks.
    // Query/link code that does not hold invalidation_mutex_ may take only mutex_.
    // Code holding a module/cache lock must never call back into LinkManager.
    [[nodiscard]] bool RegisterSite(LinkSiteKey site,
                                    u64 guest_target,
                                    LinkSourceOwner source_owner);
    [[nodiscard]] std::optional<LinkSiteRecord> QuerySite(LinkSiteKey site) const;

    // Publishing a target assigns a process-local, globally monotonic generation.
    // MarkLinked/MarkFar are the linkage-time generation recheck boundary.
    [[nodiscard]] u64 PublishTarget(u64 guest_target,
                                    void* host_pc = nullptr,
                                    CodeRegionId region_id = 0);
    [[nodiscard]] std::optional<LinkTargetRecord> QueryTarget(u64 guest_target) const;
    [[nodiscard]] std::optional<u64> QueryTargetGeneration(u64 guest_target) const;
    [[nodiscard]] bool ValidateTargetGeneration(u64 guest_target, u64 generation) const;
    using LinkCommit = std::function<bool(const LinkSiteRecord&)>;

    // commit runs while mutex_ is held, after generation validation and before
    // the record becomes Linked. P0-B will supply PatchDirectBranch here; this
    // prevents invalidation from slipping between metadata linkage and patch.
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

    [[nodiscard]] LinkManagerStats GetStats() const;

private:
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
        bool active{};
    };

    using SiteSet = std::unordered_set<LinkSiteKey, LinkSiteKeyHash>;
    struct OutgoingRecord {
        SiteSet sites{};
        bool retiring{};
    };

    [[nodiscard]] bool ValidateMarkLocked(LinkSiteKey site,
                                          u64 expected_generation,
                                          LinkSiteRecord*& record);

    mutable std::mutex mutex_{};
    std::unordered_map<LinkSiteKey, LinkSiteRecord, LinkSiteKeyHash> sites_{};
    std::unordered_map<u64, SiteSet> incoming_{};
    std::unordered_map<LinkSourceOwner, OutgoingRecord, LinkSourceOwnerHash> outgoing_{};
    std::unordered_map<u64, TargetRecord> targets_{};
    u64 next_target_generation_{1};
};

// Default-OFF P0-A placeholder. No production path consumes this switch yet.
[[nodiscard]] bool DirectLinkV2Enabled();

[[nodiscard]] std::optional<u32> EncodeB(std::intptr_t offset);
[[nodiscard]] std::optional<u32> EncodeBL(std::intptr_t offset);
[[nodiscard]] std::optional<uintptr_t> DecodeBranchTarget(const void* site, u32 insn);

// Performs exactly one aligned 32-bit atomic instruction write through the RW
// alias, then maintains D-cache at RW and I-cache at RX. seq_cst is deliberately
// conservative for this cold P0 primitive; it does not replace the P0-B ISB/
// epoch protocol required before retired target code can be reclaimed.
[[nodiscard]] bool PatchDirectBranch(const CodeRegion& region,
                                     void* rx_site,
                                     void* rw_site,
                                     u32 insn);

}  // namespace swift::runtime::backend
