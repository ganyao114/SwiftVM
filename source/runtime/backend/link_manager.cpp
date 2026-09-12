#include "runtime/backend/link_manager.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>
#include "runtime/backend/cache_clear.h"
#include "runtime/common/logging.h"

namespace swift::runtime::backend {

namespace {

constexpr std::intptr_t kImm26MaxDistance = (std::intptr_t{1} << 27) - 4;
constexpr u32 kBranchImmediateMask = 0x03FF'FFFFu;
constexpr u32 kBranchOpcodeMask = 0x7C00'0000u;
constexpr u32 kBranchTypeMask = 0xFC00'0000u;
constexpr u32 kBranchOpcode = 0x1400'0000u;
constexpr u32 kBOpcode = 0x1400'0000u;
constexpr u32 kBLOpcode = 0x9400'0000u;
constexpr u32 kParityPublishRegisterMask = 0x3E0u;
constexpr u32 kParityPublishOpcode = 0xB340'1C1Au;
constexpr u64 kSignalInvalidatingGeneration = std::numeric_limits<u64>::max();

// Every atomic touched by SignalInvalidateTarget must compile to an inline,
// lock-free instruction: a libatomic fallback would not be signal-safe.
static_assert(std::atomic<u64>::is_always_lock_free);
static_assert(std::atomic<u32>::is_always_lock_free);
static_assert(std::atomic_bool::is_always_lock_free);
static_assert(std::atomic<void*>::is_always_lock_free);

[[nodiscard]] std::optional<u32> EncodeBranch(std::intptr_t offset, u32 opcode) {
    if ((offset & 3) != 0 || offset < -kImm26MaxDistance || offset > kImm26MaxDistance) {
        return std::nullopt;
    }
    const auto words = offset / 4;
    return opcode | (static_cast<u32>(words) & kBranchImmediateMask);
}

[[nodiscard]] size_t HashCombine(size_t lhs, size_t rhs) {
    return lhs ^ (rhs + 0x9E37'79B9u + (lhs << 6) + (lhs >> 2));
}

}  // namespace

size_t LinkManager::LinkSiteKeyHash::operator()(const LinkSiteKey& key) const noexcept {
    return HashCombine(std::hash<CodeRegionId>{}(key.region_id), std::hash<u32>{}(key.offset));
}

size_t LinkManager::LinkSourceOwnerHash::operator()(const LinkSourceOwner& owner) const noexcept {
    return HashCombine(std::hash<const void*>{}(owner.module),
                       std::hash<const void*>{}(owner.allocation));
}

LinkManager::SignalTarget* LinkManager::FindSignalTarget(u64 guest_target) const noexcept {
    auto* buckets = signal_buckets_.load(std::memory_order_acquire);
    if (!buckets) {
        return nullptr;
    }
    const size_t bucket = std::hash<u64>{}(guest_target) % kSignalBucketCount;
    for (auto* target = (*buckets)[bucket].load(std::memory_order_acquire); target;
         target = target->hash_next.load(std::memory_order_acquire)) {
        if (target->guest_target == guest_target) {
            return target;
        }
    }
    return nullptr;
}

LinkManager::SignalTarget* LinkManager::GetOrCreateSignalTargetLocked(u64 guest_target) {
    if (auto* target = FindSignalTarget(guest_target)) {
        return target;
    }
    if (!signal_buckets_storage_) {
        signal_buckets_storage_ = std::make_unique<SignalBucketArray>();
        signal_buckets_.store(signal_buckets_storage_.get(), std::memory_order_release);
    }
    auto target = std::make_unique<SignalTarget>(guest_target);
    auto* raw = target.get();
    const size_t bucket = std::hash<u64>{}(guest_target) % kSignalBucketCount;
    raw->hash_next.store((*signal_buckets_storage_)[bucket].load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
    signal_targets_storage_.push_back(std::move(target));
    (*signal_buckets_storage_)[bucket].store(raw, std::memory_order_release);
    return raw;
}

void LinkManager::UnlinkSignalSiteLocked(SignalTarget& target, SignalSite* site) {
    SignalSite* previous{};
    auto* current = target.sites.load(std::memory_order_acquire);
    while (current) {
        auto* next = current->next.load(std::memory_order_acquire);
        if (current == site) {
            if (previous) {
                previous->next.store(next, std::memory_order_release);
            } else {
                target.sites.store(next, std::memory_order_release);
            }
            site->linked.store(false, std::memory_order_release);
            return;
        }
        previous = current;
        current = next;
    }
}

bool LinkManager::RegisterSite(LinkSiteKey site,
                               u64 guest_target,
                               LinkSourceOwner source_owner,
                               const LinkSignalPatchSite* signal_patch,
                               LinkSiteKind kind,
                               EdgeFlagsState edge_flags) {
    if (site.region_id == 0 || (site.offset & 3u) != 0 || !source_owner.module ||
        !source_owner.allocation || kind == LinkSiteKind::Count) {
        return false;
    }
    const bool has_flags_bypass = signal_patch &&
            signal_patch->flags_bypass.rx_site;
    if (!edge_flags.IsWellFormed() ||
        has_flags_bypass != edge_flags.HasPendingPState()) {
        return false;
    }
    if (signal_patch &&
        (signal_patch->region.id != site.region_id ||
         !signal_patch->region.ContainsRx(signal_patch->rx_site) ||
         !signal_patch->region.ContainsRw(signal_patch->rw_site) ||
         SiteRxToRw(signal_patch->region, signal_patch->rx_site) != signal_patch->rw_site ||
         static_cast<u32>(static_cast<u8*>(signal_patch->rx_site) -
                          signal_patch->region.rx_base) != site.offset ||
         (signal_patch->unlinked_bl & kBranchOpcodeMask) != kBranchOpcode)) {
        return false;
    }
    if (has_flags_bypass) {
        const auto& bypass = signal_patch->flags_bypass;
        const bool linked_branch =
                (bypass.linked_instruction & kBranchOpcodeMask) == kBranchOpcode;
        const bool linked_parity_publish =
                (bypass.linked_instruction & ~kParityPublishRegisterMask) ==
                kParityPublishOpcode;
        if (!bypass.rw_site ||
            !signal_patch->region.ContainsRx(bypass.rx_site) ||
            !signal_patch->region.ContainsRw(bypass.rw_site) ||
            SiteRxToRw(signal_patch->region, bypass.rx_site) != bypass.rw_site ||
            (!linked_branch && !linked_parity_publish) ||
            (linked_parity_publish &&
             (bypass.unlinked_instruction & kBranchTypeMask) != kBLOpcode)) {
            return false;
        }
    }
    std::lock_guard guard(mutex_);
    if (const auto owner = outgoing_.find(source_owner);
        owner != outgoing_.end() && owner->second.retiring) {
        return false;
    }
    const auto flags_bypass_offset = has_flags_bypass
            ? static_cast<u32>(
                      static_cast<u8*>(signal_patch->flags_bypass.rx_site) -
                      signal_patch->region.rx_base)
            : UINT32_MAX;
    const auto flags_bypass_instruction = has_flags_bypass
            ? signal_patch->flags_bypass.unlinked_instruction
            : 0;
    const auto [it, inserted] = sites_.try_emplace(
            site,
            LinkSiteRecord{
                    .site = site,
                    .guest_target = guest_target,
                    .source_owner = source_owner,
                    .edge_flags = edge_flags,
                    .flags_bypass_offset = flags_bypass_offset,
                    .flags_bypass_instruction = flags_bypass_instruction,
                    .kind = kind,
            });
    if (!inserted) {
        return false;
    }

    SignalSite* published_signal_site{};
    SignalTarget* signal_target{};
    try {
        incoming_[guest_target].insert(site);
        outgoing_[source_owner].sites.insert(site);
        if (signal_patch) {
            signal_target = GetOrCreateSignalTargetLocked(guest_target);
            auto signal_site = std::make_unique<SignalSite>();
            signal_site->patch = *signal_patch;
            signal_site->live.store(true, std::memory_order_relaxed);
            published_signal_site = signal_site.get();
            signal_sites_.emplace(site, published_signal_site);
            signal_sites_storage_.push_back(std::move(signal_site));
        }
    } catch (...) {
        if (auto incoming = incoming_.find(guest_target); incoming != incoming_.end()) {
            incoming->second.erase(site);
            if (incoming->second.empty()) {
                incoming_.erase(incoming);
            }
        }
        if (auto outgoing = outgoing_.find(source_owner); outgoing != outgoing_.end()) {
            outgoing->second.sites.erase(site);
            if (outgoing->second.sites.empty()) {
                outgoing_.erase(outgoing);
            }
        }
        signal_sites_.erase(site);
        sites_.erase(it);
        throw;
    }
    // Publishing the list link is last: from this point the async-signal
    // reader can observe a completely initialized, permanently stable node.
    if (published_signal_site) {
        published_signal_site->next.store(signal_target->sites.load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);
        signal_target->sites.store(published_signal_site, std::memory_order_seq_cst);
    }
    ++sites_registered_;
    max_in_degree_ = std::max(max_in_degree_, incoming_[guest_target].size());
    return true;
}

std::optional<LinkSiteRecord> LinkManager::QuerySite(LinkSiteKey site) const {
    std::lock_guard guard(mutex_);
    if (const auto it = sites_.find(site); it != sites_.end()) {
        return it->second;
    }
    return std::nullopt;
}

u64 LinkManager::PublishTarget(u64 guest_target,
                               void* host_pc,
                               CodeRegionId region_id,
                               LinkSourceOwner target_owner,
                               void* direct_host_pc,
                               void* pending_flags_host_pc,
                               void* call_host_pc,
                               void* call_pending_flags_host_pc,
                               EdgeFlagsTargetContract pending_flags_contract) {
    ASSERT(pending_flags_contract.IsWellFormed());
    ASSERT((pending_flags_host_pc != nullptr ||
            call_pending_flags_host_pc != nullptr) ==
           pending_flags_contract.CanPublishPendingEntry());
    std::lock_guard guard(mutex_);
    const u64 generation = next_target_generation_++;
    ASSERT(generation != kSignalInvalidatingGeneration);
    auto* signal_target = GetOrCreateSignalTargetLocked(guest_target);
    // Linearize publication against the lock-free signal invalidator. A basic
    // final store is incorrect: a handler could publish inactive after hash
    // insertion and then have the publisher overwrite it with a stale active
    // generation. The invalidating sentinel is never a valid generation, and
    // CAS makes either publication or invalidation win without lost updates.
    signal_target->publishing_count.fetch_add(1, std::memory_order_seq_cst);
    u64 observed = signal_target->active_generation.load(std::memory_order_seq_cst);
    bool published{};
    while (observed != kSignalInvalidatingGeneration) {
        if (signal_target->active_generation.compare_exchange_weak(
                    observed,
                    generation,
                    std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
            published = true;
            break;
        }
    }
    targets_[guest_target] = TargetRecord{
            .generation = generation,
            .host_pc = host_pc,
            .direct_host_pc = direct_host_pc ? direct_host_pc : host_pc,
            .pending_flags_host_pc = pending_flags_host_pc,
            .pending_flags_contract = pending_flags_contract,
            .call_host_pc = call_host_pc,
            .call_pending_flags_host_pc = call_pending_flags_host_pc,
            .region_id = region_id,
            .target_owner = target_owner,
            .signal_target = signal_target,
            .active = published,
    };
    signal_target->publishing_count.fetch_sub(1, std::memory_order_seq_cst);
    if (target_owner.module && target_owner.allocation) {
        // An allocation with no outgoing link sites still needs an owner
        // lifecycle so its target publication is removed at QSBR reclaim.
        outgoing_.try_emplace(target_owner);
    }
    return generation;
}

std::optional<LinkTargetRecord> LinkManager::QueryTarget(u64 guest_target) const {
    std::lock_guard guard(mutex_);
    if (const auto it = targets_.find(guest_target);
        it != targets_.end() && it->second.active && it->second.signal_target &&
        it->second.signal_target->active_generation.load(std::memory_order_acquire) ==
                it->second.generation) {
        return LinkTargetRecord{
                .guest_target = guest_target,
                .host_pc = it->second.host_pc,
                .direct_host_pc = it->second.direct_host_pc,
                .pending_flags_host_pc = it->second.pending_flags_host_pc,
                .pending_flags_contract = it->second.pending_flags_contract,
                .call_host_pc = it->second.call_host_pc,
                .call_pending_flags_host_pc =
                        it->second.call_pending_flags_host_pc,
                .region_id = it->second.region_id,
                .generation = it->second.generation,
                .target_owner = it->second.target_owner,
        };
    }
    return std::nullopt;
}

std::vector<u64> LinkManager::QueryTargets(LinkSourceOwner target_owner) const {
    std::lock_guard guard(mutex_);
    std::vector<u64> result;
    for (const auto& [guest_target, target] : targets_) {
        if (target.target_owner == target_owner) {
            result.push_back(guest_target);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::optional<u64> LinkManager::QueryTargetGeneration(u64 guest_target) const {
    std::lock_guard guard(mutex_);
    if (const auto it = targets_.find(guest_target);
        it != targets_.end() && it->second.active && it->second.signal_target &&
        it->second.signal_target->active_generation.load(std::memory_order_acquire) ==
                it->second.generation) {
        return it->second.generation;
    }
    return std::nullopt;
}

bool LinkManager::ValidateTargetGeneration(u64 guest_target, u64 generation) const {
    std::lock_guard guard(mutex_);
    const auto it = targets_.find(guest_target);
    return it != targets_.end() && it->second.active && it->second.generation == generation &&
           it->second.signal_target &&
           it->second.signal_target->active_generation.load(std::memory_order_acquire) ==
                   generation;
}

bool LinkManager::ValidateMarkLocked(LinkSiteKey site,
                                     u64 expected_generation,
                                     LinkSiteRecord*& record) {
    const auto site_it = sites_.find(site);
    if (site_it == sites_.end() || site_it->second.state != LinkSiteState::Unlinked) {
        return false;
    }
    const auto target_it = targets_.find(site_it->second.guest_target);
    if (target_it == targets_.end() || !target_it->second.active ||
        target_it->second.generation != expected_generation || !target_it->second.signal_target ||
        target_it->second.signal_target->active_generation.load(std::memory_order_acquire) !=
                expected_generation) {
        return false;
    }
    record = &site_it->second;
    return true;
}

bool LinkManager::MarkLinked(LinkSiteKey site, u64 expected_generation, const LinkCommit& commit) {
    std::lock_guard guard(mutex_);
    const auto site_it = sites_.find(site);
    if (site_it == sites_.end() || site_it->second.state != LinkSiteState::Unlinked || !commit) {
        return false;
    }
    const auto target_it = targets_.find(site_it->second.guest_target);
    if (target_it == targets_.end() || !target_it->second.signal_target) {
        return false;
    }
    auto* signal_target = target_it->second.signal_target;
    // The signal path deactivates first, then waits for this count and writes
    // BL a second time. Thus a B commit that started before deactivation can
    // never become the final instruction at the site.
    signal_target->linking_count.fetch_add(1, std::memory_order_seq_cst);
    const bool valid = target_it->second.active &&
                       target_it->second.generation == expected_generation &&
                       signal_target->active_generation.load(std::memory_order_seq_cst) ==
                               expected_generation;
    const bool call = site_it->second.kind == LinkSiteKind::Call;
    const bool pending_compatible =
            target_it->second.pending_flags_contract.Accepts(
                    site_it->second.edge_flags) &&
            (call ? target_it->second.call_pending_flags_host_pc != nullptr
                  : target_it->second.pending_flags_host_pc != nullptr);
    bool committed{};
    if (valid) {
        if (site_it->second.flags_bypass_offset != UINT32_MAX &&
            !pending_compatible) {
            DisableFlagsBypassLocked(site_it->second);
        }
        committed = commit(site_it->second);
    }
    const bool still_active =
            signal_target->active_generation.load(std::memory_order_seq_cst) ==
            expected_generation;
    if (committed && still_active) {
        site_it->second.target_generation = expected_generation;
        site_it->second.state = LinkSiteState::Linked;
        site_it->second.pending_flags_compatible =
                pending_compatible;
        if (const auto signal_site = signal_sites_.find(site);
            signal_site != signal_sites_.end()) {
            signal_site->second->linked.store(true, std::memory_order_release);
        }
        TryEnableFlagsBypassLocked(site_it->second);
    }
    signal_target->linking_count.fetch_sub(1, std::memory_order_seq_cst);
    return committed && still_active;
}

bool LinkManager::MarkFar(LinkSiteKey site, u64 expected_generation) {
    std::lock_guard guard(mutex_);
    LinkSiteRecord* record{};
    if (!ValidateMarkLocked(site, expected_generation, record)) {
        return false;
    }
    record->target_generation = expected_generation;
    record->state = LinkSiteState::Far;
    record->pending_flags_compatible = false;
    if (const auto signal_site = signal_sites_.find(site); signal_site != signal_sites_.end()) {
        signal_site->second->linked.store(false, std::memory_order_release);
    }
    return true;
}

std::vector<LinkSiteRecord> LinkManager::BeginTargetInvalidation(u64 guest_target) {
    std::lock_guard guard(mutex_);
    const auto target = targets_.find(guest_target);
    if (target == targets_.end()) {
        return {};
    }

    std::vector<LinkSiteRecord> result;
    const auto incoming = incoming_.find(guest_target);
    if (incoming != incoming_.end()) {
        // Reserve before making the target inactive. An allocation failure
        // must not strand an active branch without returning its delink list.
        result.reserve(incoming->second.size());
    }
    target->second.active = false;
    if (target->second.signal_target) {
        target->second.signal_target->active_generation.store(0, std::memory_order_seq_cst);
    }
    if (incoming == incoming_.end()) {
        return result;
    }
    for (const auto site : incoming->second) {
        const auto record = sites_.find(site);
        if (record == sites_.end()) {
            continue;
        }
        result.push_back(record->second);
        record->second.target_generation = 0;
        record->second.pending_flags_compatible = false;
        if (record->second.state != LinkSiteState::Retiring) {
            record->second.state = LinkSiteState::Unlinked;
        }
        if (const auto signal_site = signal_sites_.find(site);
            signal_site != signal_sites_.end()) {
            signal_site->second->linked.store(false, std::memory_order_release);
        }
    }
    return result;
}

size_t LinkManager::DetachSource(LinkSourceOwner source_owner) {
    std::lock_guard guard(mutex_);
    const auto outgoing = outgoing_.find(source_owner);
    if (outgoing == outgoing_.end()) {
        return 0;
    }
    outgoing->second.retiring = true;
    size_t detached{};
    for (const auto site : outgoing->second.sites) {
        if (auto record = sites_.find(site); record != sites_.end()) {
            record->second.state = LinkSiteState::Retiring;
            ++detached;
        }
    }
    return detached;
}

size_t LinkManager::PurgeSource(LinkSourceOwner source_owner) {
    std::lock_guard guard(mutex_);
    const auto outgoing = outgoing_.find(source_owner);
    if (outgoing == outgoing_.end()) {
        return 0;
    }
    if (!outgoing->second.retiring) {
        return 0;
    }

    // Enforce the two-phase owner lifecycle: a live source cannot disappear
    // from the indexes merely because a caller reached purge too early.
    for (const auto site : outgoing->second.sites) {
        const auto record = sites_.find(site);
        if (record == sites_.end() || record->second.state != LinkSiteState::Retiring) {
            return 0;
        }
    }

    // Logical removal precedes the grace wait. All operations are seq_cst so
    // either a signal reader was admitted before the reader-count observation
    // (and purge waits for it), or its later live load must observe false and
    // skip the source address. This closes the reader-starts-during-purge gap
    // without taking a lock in the synchronous signal handler.
    for (const auto site : outgoing->second.sites) {
        if (const auto signal_site = signal_sites_.find(site);
            signal_site != signal_sites_.end()) {
            signal_site->second->live.store(false, std::memory_order_seq_cst);
        }
    }
    while (signal_readers_.load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield();
    }

    size_t purged{};
    for (const auto site : outgoing->second.sites) {
        const auto record = sites_.find(site);
        if (record == sites_.end()) {
            continue;
        }
        if (auto incoming = incoming_.find(record->second.guest_target);
            incoming != incoming_.end()) {
            incoming->second.erase(site);
            if (incoming->second.empty()) {
                incoming_.erase(incoming);
            }
        }
        if (const auto signal_site = signal_sites_.find(site);
            signal_site != signal_sites_.end()) {
            if (auto* signal_target = FindSignalTarget(record->second.guest_target)) {
                UnlinkSignalSiteLocked(*signal_target, signal_site->second);
            }
            signal_sites_.erase(signal_site);
        }
        sites_.erase(record);
        ++purged;
    }
    std::erase_if(targets_, [&](const auto& item) {
        if (item.second.target_owner != source_owner) {
            return false;
        }
        if (item.second.signal_target) {
            item.second.signal_target->active_generation.store(0, std::memory_order_seq_cst);
        }
        return true;
    });
    outgoing_.erase(outgoing);
    // SignalSite storage is intentionally stable for the manager lifetime.
    // The live tombstone and grace wait above guarantee no handler can still
    // patch this source allocation before Module::ReclaimCode reuses it.
    return purged;
}

LinkSignalInvalidationResult LinkManager::SignalInvalidateTarget(u64 guest_target) noexcept {
    signal_readers_.fetch_add(1, std::memory_order_seq_cst);
    auto finish = [this] {
        signal_readers_.fetch_sub(1, std::memory_order_seq_cst);
    };
    auto* target = FindSignalTarget(guest_target);
    if (!target) {
        finish();
        return {};
    }

    // Read publication admission before active_generation. If a publisher was
    // already admitted, the sentinel makes its CAS fail (or supersedes a CAS
    // that already won). If it starts after both reads, invalidation linearizes
    // first and that later publication is a fresh generation. An idle retained
    // tombstone stays at zero, so a delayed stale fault cannot permanently
    // poison all future compilations of the same guest target.
    if (target->publishing_count.load(std::memory_order_seq_cst) != 0 ||
        target->active_generation.load(std::memory_order_seq_cst) != 0) {
        target->active_generation.store(kSignalInvalidatingGeneration,
                                        std::memory_order_seq_cst);
    }
    signal_invalidations_.fetch_add(1, std::memory_order_relaxed);
    size_t linked_sites{};
    auto restore = [&](bool count_linked) {
        for (auto* site = target->sites.load(std::memory_order_seq_cst); site;
             site = site->next.load(std::memory_order_acquire)) {
            if (!site->live.load(std::memory_order_seq_cst)) {
                continue;
            }
            if (count_linked && site->linked.exchange(false, std::memory_order_acq_rel)) {
                ++linked_sites;
            }
            (void)PatchDirectBranch(site->patch.region,
                                    site->patch.rx_site,
                                    site->patch.rw_site,
                                    site->patch.unlinked_bl);
        }
    };
    restore(true);
    // No libc call or scheduler primitive here: this bounded spin only covers
    // a cold linker commit already executing under the ordinary manager lock.
    while (target->linking_count.load(std::memory_order_seq_cst) != 0) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    restore(false);
    if (linked_sites) {
        signal_delinks_.fetch_add(linked_sites, std::memory_order_relaxed);
    }
    finish();
    return {.found = true, .linked_sites = linked_sites};
}

LinkManagerStats LinkManager::GetStats() const {
    std::lock_guard guard(mutex_);
    size_t linked{};
    size_t far{};
    size_t retiring{};
    std::array<size_t, kLinkSiteKindCount> sites_by_kind{};
    std::array<size_t, kLinkSiteKindCount> linked_by_kind{};
    std::array<size_t, kLinkSiteKindCount> far_by_kind{};
    for (const auto& [key, record] : sites_) {
        (void)key;
        const auto kind = static_cast<size_t>(record.kind);
        ASSERT(kind < kLinkSiteKindCount);
        ++sites_by_kind[kind];
        linked += record.state == LinkSiteState::Linked;
        far += record.state == LinkSiteState::Far;
        retiring += record.state == LinkSiteState::Retiring;
        linked_by_kind[kind] += record.state == LinkSiteState::Linked;
        far_by_kind[kind] += record.state == LinkSiteState::Far;
    }
    size_t incoming_sites{};
    for (const auto& [target, sites] : incoming_) {
        (void)target;
        incoming_sites += sites.size();
    }
    size_t outgoing_sites{};
    for (const auto& [owner, record] : outgoing_) {
        (void)owner;
        outgoing_sites += record.sites.size();
    }
    // unordered_* node/bucket implementations are library-specific. This is
    // a stable logical-byte estimate: payloads plus one pointer-sized node/
    // bucket charge per keyed entry, explicitly not allocator RSS.
    const size_t estimated_bytes =
            sites_.size() * (sizeof(LinkSiteKey) + sizeof(LinkSiteRecord) + 2 * sizeof(void*)) +
            incoming_.size() * (sizeof(u64) + sizeof(SiteSet) + 2 * sizeof(void*)) +
            incoming_sites * (sizeof(LinkSiteKey) + 2 * sizeof(void*)) +
            outgoing_.size() *
                    (sizeof(LinkSourceOwner) + sizeof(OutgoingRecord) + 2 * sizeof(void*)) +
            outgoing_sites * (sizeof(LinkSiteKey) + 2 * sizeof(void*)) +
            targets_.size() * (sizeof(u64) + sizeof(TargetRecord) + 2 * sizeof(void*)) +
            signal_targets_storage_.size() * sizeof(SignalTarget) +
            signal_sites_storage_.size() * sizeof(SignalSite) +
            signal_sites_.size() * (sizeof(LinkSiteKey) + sizeof(SignalSite*) + 2 * sizeof(void*)) +
            (signal_buckets_storage_ ? sizeof(SignalBucketArray) : 0);
    return LinkManagerStats{
            .sites = sites_.size(),
            .linked = linked,
            .far = far,
            .retiring = retiring,
            .incoming_targets = incoming_.size(),
            .outgoing_owners = outgoing_.size(),
            .target_records = targets_.size(),
            .max_in_degree = max_in_degree_,
            .estimated_bytes = estimated_bytes,
            .sites_registered = sites_registered_,
            .linker_calls = linker_calls_,
            // Deferred invalidation owns the logical delink count. The signal
            // transaction may have already written identical BL bytes, but
            // must not make one target invalidation appear twice here.
            .delinks = delinks_,
            .signal_invalidations = signal_invalidations_.load(std::memory_order_relaxed),
            .signal_targets_retained = signal_targets_storage_.size(),
            .signal_sites_retained = signal_sites_storage_.size(),
            .sites_by_kind = sites_by_kind,
            .linked_by_kind = linked_by_kind,
            .far_by_kind = far_by_kind,
    };
}

void LinkManager::RecordLinkerCall() {
    std::lock_guard guard(mutex_);
    ++linker_calls_;
}

void LinkManager::RecordDelink(size_t count) {
    std::lock_guard guard(mutex_);
    delinks_ += count;
}

std::optional<u32> EncodeB(std::intptr_t offset) { return EncodeBranch(offset, kBOpcode); }

std::optional<u32> EncodeBL(std::intptr_t offset) { return EncodeBranch(offset, kBLOpcode); }

std::optional<uintptr_t> DecodeBranchTarget(const void* site, u32 insn) {
    if (!site || (reinterpret_cast<uintptr_t>(site) & 3u) != 0 ||
        (insn & kBranchOpcodeMask) != kBranchOpcode) {
        return std::nullopt;
    }
    const auto signed_words = static_cast<std::int32_t>(insn << 6) >> 6;
    const auto offset = static_cast<std::intptr_t>(signed_words) * 4;
    if (offset < -kImm26MaxDistance || offset > kImm26MaxDistance) {
        return std::nullopt;
    }
    const auto source = reinterpret_cast<uintptr_t>(site);
    return offset >= 0 ? source + static_cast<uintptr_t>(offset)
                       : source - static_cast<uintptr_t>(-offset);
}

bool PatchDirectBranch(const CodeRegion& region, void* rx_site, void* rw_site, u32 insn) {
    if ((insn & kBranchOpcodeMask) != kBranchOpcode) {
        return false;
    }
    return PatchCodeWord(region, rx_site, rw_site, insn);
}

bool PatchCodeWord(const CodeRegion& region, void* rx_site, void* rw_site, u32 insn) {
    ASSERT(region.ContainsRx(rx_site));
    ASSERT(region.ContainsRw(rw_site));
    ASSERT(SiteRxToRw(region, rx_site) == rw_site);
    ASSERT((reinterpret_cast<uintptr_t>(rx_site) & 3u) == 0);
    ASSERT((reinterpret_cast<uintptr_t>(rw_site) & 3u) == 0);
    static_assert(std::atomic_ref<u32>::required_alignment <= alignof(u32));
    static_assert(std::atomic_ref<u32>::is_always_lock_free);
    auto& word = *static_cast<u32*>(rw_site);
    std::atomic_ref<u32>(word).store(insn, std::memory_order_seq_cst);
    ClearDCache(rw_site, sizeof(insn));
    ClearICache(rx_site, sizeof(insn));
    return true;
}

}  // namespace swift::runtime::backend
