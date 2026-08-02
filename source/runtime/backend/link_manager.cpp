#include "runtime/backend/link_manager.h"

#include <atomic>
#include <cstring>
#include "runtime/backend/cache_clear.h"
#include "runtime/common/logging.h"
#include "runtime/common/perf_stats.h"

namespace swift::runtime::backend {

namespace {

constexpr std::intptr_t kImm26MaxDistance = (std::intptr_t{1} << 27) - 4;
constexpr u32 kBranchImmediateMask = 0x03FF'FFFFu;
constexpr u32 kBranchOpcodeMask = 0x7C00'0000u;
constexpr u32 kBranchOpcode = 0x1400'0000u;
constexpr u32 kBOpcode = 0x1400'0000u;
constexpr u32 kBLOpcode = 0x9400'0000u;

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

bool LinkManager::RegisterSite(LinkSiteKey site, u64 guest_target, LinkSourceOwner source_owner) {
    if (site.region_id == 0 || (site.offset & 3u) != 0 || !source_owner.module ||
        !source_owner.allocation) {
        return false;
    }
    std::lock_guard guard(mutex_);
    if (const auto owner = outgoing_.find(source_owner);
        owner != outgoing_.end() && owner->second.retiring) {
        return false;
    }
    const auto [it, inserted] = sites_.try_emplace(site,
                                                   LinkSiteRecord{
                                                           .site = site,
                                                           .guest_target = guest_target,
                                                           .source_owner = source_owner,
                                                   });
    if (!inserted) {
        return false;
    }

    try {
        incoming_[guest_target].insert(site);
        outgoing_[source_owner].sites.insert(site);
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
        sites_.erase(it);
        throw;
    }
    return true;
}

std::optional<LinkSiteRecord> LinkManager::QuerySite(LinkSiteKey site) const {
    std::lock_guard guard(mutex_);
    if (const auto it = sites_.find(site); it != sites_.end()) {
        return it->second;
    }
    return std::nullopt;
}

u64 LinkManager::PublishTarget(u64 guest_target, void* host_pc, CodeRegionId region_id) {
    std::lock_guard guard(mutex_);
    const u64 generation = next_target_generation_++;
    targets_[guest_target] = TargetRecord{
            .generation = generation,
            .host_pc = host_pc,
            .region_id = region_id,
            .active = true,
    };
    return generation;
}

std::optional<LinkTargetRecord> LinkManager::QueryTarget(u64 guest_target) const {
    std::lock_guard guard(mutex_);
    if (const auto it = targets_.find(guest_target); it != targets_.end() && it->second.active) {
        return LinkTargetRecord{
                .guest_target = guest_target,
                .host_pc = it->second.host_pc,
                .region_id = it->second.region_id,
                .generation = it->second.generation,
        };
    }
    return std::nullopt;
}

std::optional<u64> LinkManager::QueryTargetGeneration(u64 guest_target) const {
    std::lock_guard guard(mutex_);
    if (const auto it = targets_.find(guest_target); it != targets_.end() && it->second.active) {
        return it->second.generation;
    }
    return std::nullopt;
}

bool LinkManager::ValidateTargetGeneration(u64 guest_target, u64 generation) const {
    std::lock_guard guard(mutex_);
    const auto it = targets_.find(guest_target);
    return it != targets_.end() && it->second.active && it->second.generation == generation;
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
        target_it->second.generation != expected_generation) {
        return false;
    }
    record = &site_it->second;
    return true;
}

bool LinkManager::MarkLinked(LinkSiteKey site, u64 expected_generation, const LinkCommit& commit) {
    std::lock_guard guard(mutex_);
    LinkSiteRecord* record{};
    if (!ValidateMarkLocked(site, expected_generation, record) || !commit || !commit(*record)) {
        return false;
    }
    record->target_generation = expected_generation;
    record->state = LinkSiteState::Linked;
    return true;
}

bool LinkManager::MarkFar(LinkSiteKey site, u64 expected_generation) {
    std::lock_guard guard(mutex_);
    LinkSiteRecord* record{};
    if (!ValidateMarkLocked(site, expected_generation, record)) {
        return false;
    }
    record->target_generation = expected_generation;
    record->state = LinkSiteState::Far;
    return true;
}

std::vector<LinkSiteRecord> LinkManager::BeginTargetInvalidation(u64 guest_target) {
    std::lock_guard guard(mutex_);
    const auto target = targets_.find(guest_target);
    if (target == targets_.end() || !target->second.active) {
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
        if (record->second.state != LinkSiteState::Retiring) {
            record->second.state = LinkSiteState::Unlinked;
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
        sites_.erase(record);
        ++purged;
    }
    outgoing_.erase(outgoing);
    return purged;
}

LinkManagerStats LinkManager::GetStats() const {
    std::lock_guard guard(mutex_);
    return LinkManagerStats{
            .sites = sites_.size(),
            .incoming_targets = incoming_.size(),
            .outgoing_owners = outgoing_.size(),
            .target_records = targets_.size(),
    };
}

bool DirectLinkV2Enabled() {
    static const bool enabled = [] {
        const char* value = PerfGetenv("SVM_DIRECT_LINK_V2");
        return value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
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
    ASSERT(region.ContainsRx(rx_site));
    ASSERT(region.ContainsRw(rw_site));
    ASSERT(SiteRxToRw(region, rx_site) == rw_site);
    ASSERT((reinterpret_cast<uintptr_t>(rx_site) & 3u) == 0);
    ASSERT((reinterpret_cast<uintptr_t>(rw_site) & 3u) == 0);
    if ((insn & kBranchOpcodeMask) != kBranchOpcode) {
        return false;
    }
    static_assert(std::atomic_ref<u32>::required_alignment <= alignof(u32));
    static_assert(std::atomic_ref<u32>::is_always_lock_free);
    auto& word = *static_cast<u32*>(rw_site);
    std::atomic_ref<u32>(word).store(insn, std::memory_order_seq_cst);
    ClearDCache(rw_site, sizeof(insn));
    ClearICache(rx_site, sizeof(insn));
    return true;
}

}  // namespace swift::runtime::backend
