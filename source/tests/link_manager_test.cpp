#include <array>
#include <cstdint>
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/code_cache.h"
#include "runtime/backend/link_manager.h"

namespace {

using swift::u32;
using swift::u64;
using swift::u8;
using swift::runtime::Config;
using swift::runtime::FeatureSet;
using swift::runtime::backend::CodeCache;
using swift::runtime::backend::CodeRegion;
using swift::runtime::backend::DecodeBranchTarget;
using swift::runtime::backend::EdgeCarryPolarity;
using swift::runtime::backend::EdgeCarrySourceState;
using swift::runtime::backend::EdgeFlagsProducer;
using swift::runtime::backend::EdgeFlagsState;
using swift::runtime::backend::EdgeFlagsTargetContract;
using swift::runtime::backend::EncodeB;
using swift::runtime::backend::EncodeBL;
using swift::runtime::backend::Imm26Reachable;
using swift::runtime::backend::kEdgeCarryMask;
using swift::runtime::backend::kEdgeNZCVMask;
using swift::runtime::backend::LinkManager;
using swift::runtime::backend::LinkFlagsBypassPatch;
using swift::runtime::backend::LinkSignalPatchSite;
using swift::runtime::backend::LinkSiteKey;
using swift::runtime::backend::LinkSiteKind;
using swift::runtime::backend::LinkSiteRecord;
using swift::runtime::backend::LinkSiteState;
using swift::runtime::backend::LinkSourceOwner;
using swift::runtime::backend::LinkTargetRecord;
using swift::runtime::backend::PatchDirectBranch;
using swift::runtime::backend::SameRegion;
using swift::runtime::backend::SiteRwToRx;
using swift::runtime::backend::SiteRxToRw;

constexpr std::intptr_t kImm26Boundary = (std::intptr_t{1} << 27) - 4;

static_assert(sizeof(LinkSiteKey) == 16);
static_assert(sizeof(LinkSourceOwner) == 16);
static_assert(sizeof(LinkSiteRecord) == 72);
// The region ISA tag adds one aligned word to a region and its signal copy;
// the canonical RV64 instruction fits the existing site-record padding.
static_assert(sizeof(LinkSignalPatchSite) == 96);
static_assert(sizeof(LinkTargetRecord) == 88);
static_assert(sizeof(CodeRegion) == 48);

constexpr auto kPendingNZCV = EdgeFlagsState::Pending(
        kEdgeNZCVMask,
        EdgeCarryPolarity::Unknown,
        EdgeFlagsProducer::Restore);
constexpr EdgeFlagsTargetContract kOverwriteNZCV{
        .overwrite_before_observe = kEdgeNZCVMask,
        .commits_before_fault = true,
};

TEST_CASE("edge flags contracts accept only overwritten compatible state",
          "[direct-link][flags]") {
    constexpr u32 nz_mask = 0xC000'0000u;
    constexpr auto pending_nz = EdgeFlagsState::Pending(
            nz_mask,
            EdgeCarryPolarity::Unknown,
            EdgeFlagsProducer::Logical);
    constexpr auto pending_nc = EdgeFlagsState::Pending(
            0xA000'0000u,
            EdgeCarryPolarity::Unknown,
            EdgeFlagsProducer::Arithmetic);
    constexpr EdgeFlagsTargetContract overwrite_nz{
            .overwrite_before_observe = nz_mask,
            .commits_before_fault = true,
    };
    constexpr EdgeFlagsTargetContract overwrite_nc{
            .overwrite_before_observe = 0xA000'0000u,
            .commits_before_fault = true,
    };
    constexpr EdgeFlagsTargetContract observe_z_overwrite_nc{
            .overwrite_before_observe = 0xA000'0000u,
            .observed_nzcv_mask = 0x4,
            .commits_before_fault = true,
    };
    constexpr auto direct_c = EdgeFlagsState::Pending(
            0x2000'0000u,
            EdgeCarryPolarity::Direct,
            EdgeFlagsProducer::Arithmetic);
    constexpr auto invalid_direct_nz = EdgeFlagsState::Pending(
            nz_mask,
            EdgeCarryPolarity::Direct,
            EdgeFlagsProducer::Logical);

    STATIC_REQUIRE(overwrite_nz.Accepts(pending_nz));
    STATIC_REQUIRE(overwrite_nc.Accepts(pending_nc));
    STATIC_REQUIRE(observe_z_overwrite_nc.Accepts(pending_nc));
    STATIC_REQUIRE_FALSE(observe_z_overwrite_nc.Accepts(pending_nz));
    STATIC_REQUIRE_FALSE(overwrite_nz.Accepts(kPendingNZCV));
    STATIC_REQUIRE(direct_c.IsWellFormed());
    STATIC_REQUIRE_FALSE(invalid_direct_nz.IsWellFormed());
}

TEST_CASE("edge carry source resolves canonical and runtime polarity",
          "[direct-link][flags]") {
    EdgeCarrySourceState source;
    REQUIRE(source.Resolve(kEdgeCarryMask, false) ==
            EdgeCarryPolarity::Unknown);
    source.PublishRuntimePolarity(true);
    REQUIRE(source.Resolve(kEdgeCarryMask, false) ==
            EdgeCarryPolarity::Inverted);
    REQUIRE(source.Resolve(kEdgeCarryMask, true) ==
            EdgeCarryPolarity::Direct);
    REQUIRE(source.Resolve(0xC000'0000u, false) ==
            EdgeCarryPolarity::Unknown);
    source.InvalidateRuntimePolarity();
    REQUIRE(source.Resolve(kEdgeCarryMask, false) ==
            EdgeCarryPolarity::Unknown);
}

Config Arm64Config() {
    return Config{
            .enable_jit = true,
            .backend_isa = swift::runtime::kArm64,
    };
}

u32 LoadInstruction(const void* address) {
    u32 value{};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

void StoreInstruction(void* address, u32 value) { std::memcpy(address, &value, sizeof(value)); }

}  // namespace

TEST_CASE("direct link imm26 B and BL encode/decode boundaries", "[direct-link]") {
    constexpr uintptr_t kSite = 0x1'0000'0000ull;
    const auto* site = reinterpret_cast<const void*>(kSite);

    for (const auto offset :
         std::array<std::intptr_t, 5>{-kImm26Boundary, -4, 0, 4, kImm26Boundary}) {
        const auto expected = offset >= 0 ? kSite + static_cast<uintptr_t>(offset)
                                          : kSite - static_cast<uintptr_t>(-offset);
        const auto b = EncodeB(offset);
        const auto bl = EncodeBL(offset);
        REQUIRE(b);
        REQUIRE(bl);
        REQUIRE(DecodeBranchTarget(site, *b) == expected);
        REQUIRE(DecodeBranchTarget(site, *bl) == expected);
    }

    REQUIRE_FALSE(EncodeB(std::intptr_t{1} << 27));
    REQUIRE_FALSE(EncodeB(-(std::intptr_t{1} << 27)));
    REQUIRE_FALSE(EncodeBL(2));
    REQUIRE_FALSE(DecodeBranchTarget(site, 0xD503'201Fu));

    REQUIRE(Imm26Reachable(site, reinterpret_cast<const void*>(kSite + kImm26Boundary)));
    REQUIRE(Imm26Reachable(site, reinterpret_cast<const void*>(kSite - kImm26Boundary)));
    REQUIRE_FALSE(
            Imm26Reachable(site, reinterpret_cast<const void*>(kSite + (uintptr_t{1} << 27))));
    REQUIRE_FALSE(
            Imm26Reachable(site, reinterpret_cast<const void*>(kSite - (uintptr_t{1} << 27))));
    REQUIRE_FALSE(Imm26Reachable(site, reinterpret_cast<const void*>(kSite + 2)));
}

TEST_CASE("CodeCache region converts RX and RW aliases by one offset", "[direct-link][region]") {
    auto config = Arm64Config();
    CodeCache cache{config, 1u << 20, FeatureSet{}, true};
    const auto buffer = cache.AllocCode(64);
    REQUIRE(buffer);
    const CodeRegion& region = cache.GetRegion();

    REQUIRE(region.id != 0);
    REQUIRE(region.capacity == 1u << 20);
    REQUIRE(region.trampoline_offset == CodeRegion::kInvalidTrampolineOffset);
    REQUIRE(SiteRxToRw(region, buffer->exec_data + 12) == buffer->rw_data + 12);
    REQUIRE(SiteRwToRx(region, buffer->rw_data + 28) == buffer->exec_data + 28);
    REQUIRE(SameRegion(region, buffer->exec_data, buffer->exec_data + 60));
    REQUIRE_FALSE(SameRegion(region, buffer->exec_data, buffer->exec_data + region.capacity));

    CodeCache other{config, 1u << 20, FeatureSet{}, true};
    REQUIRE_FALSE(SameRegion(region, other.GetRegion()));
}

TEST_CASE("LinkManager maintains site target and owner indexes transactionally",
          "[direct-link][manager]") {
    LinkManager manager;
    const int module_a{};
    const int module_b{};
    const int allocation_a{};
    const int allocation_b{};
    const LinkSourceOwner owner_a{&module_a, &allocation_a};
    const LinkSourceOwner owner_b{&module_b, &allocation_b};
    constexpr u64 target_a = 0x401000;
    constexpr u64 target_b = 0x402000;
    constexpr LinkSiteKey site_a{1, 4};
    constexpr LinkSiteKey site_b{1, 20};
    constexpr LinkSiteKey site_c{2, 8};

    REQUIRE(manager.RegisterSite(site_a,
                                 target_a,
                                 owner_a,
                                 nullptr,
                                 LinkSiteKind::ConditionalThen));
    REQUIRE(manager.RegisterSite(site_b,
                                 target_a,
                                 owner_a,
                                 nullptr,
                                 LinkSiteKind::ConditionalElse));
    REQUIRE(manager.RegisterSite(site_c, target_b, owner_b));
    REQUIRE_FALSE(manager.RegisterSite(LinkSiteKey{3, 2}, target_b, owner_b));
    REQUIRE_FALSE(manager.RegisterSite(site_a, target_b, owner_b));
    REQUIRE(manager.GetStats().sites == 3);
    REQUIRE(manager.GetStats().incoming_targets == 2);
    REQUIRE(manager.GetStats().outgoing_owners == 2);

    const auto generation_a = manager.PublishTarget(target_a);
    const auto generation_b = manager.PublishTarget(target_b);
    REQUIRE(generation_b > generation_a);
    REQUIRE(manager.ValidateTargetGeneration(target_a, generation_a));
    const auto commit = [](const LinkSiteRecord&) { return true; };
    const auto reject_commit = [](const LinkSiteRecord&) { return false; };
    REQUIRE_FALSE(manager.MarkLinked(site_a, generation_b, commit));
    REQUIRE_FALSE(manager.MarkLinked(site_a, generation_a, reject_commit));
    REQUIRE(manager.QuerySite(site_a)->state == LinkSiteState::Unlinked);
    REQUIRE(manager.MarkLinked(site_a, generation_a, commit));
    REQUIRE(manager.MarkFar(site_b, generation_a));
    REQUIRE(manager.MarkLinked(site_c, generation_b, commit));
    REQUIRE(manager.QuerySite(site_a)->state == LinkSiteState::Linked);
    REQUIRE(manager.QuerySite(site_b)->state == LinkSiteState::Far);
    REQUIRE(manager.QuerySite(site_a)->kind == LinkSiteKind::ConditionalThen);
    REQUIRE(manager.QuerySite(site_b)->kind == LinkSiteKind::ConditionalElse);
    const auto mixed_stats = manager.GetStats();
    REQUIRE(mixed_stats.sites_by_kind[
                    static_cast<size_t>(LinkSiteKind::ConditionalThen)] == 1);
    REQUIRE(mixed_stats.linked_by_kind[
                    static_cast<size_t>(LinkSiteKind::ConditionalThen)] == 1);
    REQUIRE(mixed_stats.far_by_kind[
                    static_cast<size_t>(LinkSiteKind::ConditionalElse)] == 1);

    const auto incoming_a = manager.BeginTargetInvalidation(target_a);
    REQUIRE(incoming_a.size() == 2);
    REQUIRE_FALSE(manager.QueryTargetGeneration(target_a));
    REQUIRE(manager.QuerySite(site_a)->state == LinkSiteState::Unlinked);
    REQUIRE(manager.QuerySite(site_b)->state == LinkSiteState::Unlinked);

    const auto next_generation_a = manager.PublishTarget(target_a);
    REQUIRE(next_generation_a > generation_b);
    REQUIRE_FALSE(manager.MarkLinked(site_a, generation_a, commit));
    REQUIRE(manager.MarkLinked(site_a, next_generation_a, commit));

    REQUIRE(manager.DetachSource(owner_a) == 2);
    REQUIRE(manager.QuerySite(site_a)->state == LinkSiteState::Retiring);
    REQUIRE(manager.QuerySite(site_b)->state == LinkSiteState::Retiring);
    const auto retiring_incoming = manager.BeginTargetInvalidation(target_a);
    REQUIRE(retiring_incoming.size() == 2);
    REQUIRE(manager.QuerySite(site_a)->state == LinkSiteState::Retiring);
    REQUIRE_FALSE(manager.RegisterSite(LinkSiteKey{3, 12}, target_a, owner_a));

    REQUIRE(manager.PurgeSource(owner_a) == 2);
    REQUIRE_FALSE(manager.QuerySite(site_a));
    REQUIRE_FALSE(manager.QuerySite(site_b));
    REQUIRE(manager.GetStats().sites == 1);
    REQUIRE(manager.GetStats().incoming_targets == 1);
    REQUIRE(manager.GetStats().outgoing_owners == 1);
    REQUIRE(manager.PurgeSource(owner_b) == 0);
    REQUIRE(manager.DetachSource(owner_b) == 1);
    REQUIRE(manager.PurgeSource(owner_b) == 1);
    REQUIRE(manager.GetStats().sites == 0);
    REQUIRE(manager.GetStats().incoming_targets == 0);
    REQUIRE(manager.GetStats().outgoing_owners == 0);
}

TEST_CASE("direct branch patch uses matching RX and RW aliases", "[direct-link][patch]") {
    auto config = Arm64Config();
    CodeCache cache{config, 1u << 20, FeatureSet{}, true};
    const auto buffer = cache.AllocCode(64);
    REQUIRE(buffer);
    const auto& region = cache.GetRegion();
    auto* rw = buffer->rw_data;
    auto* rx = buffer->exec_data;

    // entry: save caller LR in x9; site: BL trampoline; continuation restores
    // LR and returns. A direct B target preserves the original LR and returns
    // straight to the C++ caller.
    StoreInstruction(rw + 0, 0xAA1E'03E9u);   // mov x9, x30
    StoreInstruction(rw + 8, 0xAA09'03FEu);   // mov x30, x9
    StoreInstruction(rw + 12, 0xD65F'03C0u);  // ret
    StoreInstruction(rw + 16, 0x5280'0020u);  // mov w0, #1
    StoreInstruction(rw + 20, 0xD65F'03C0u);  // ret
    StoreInstruction(rw + 24, 0x5280'0040u);  // mov w0, #2
    StoreInstruction(rw + 28, 0xD65F'03C0u);  // ret

    const auto bl_trampoline = EncodeBL((rx + 16) - (rx + 4));
    const auto b_target = EncodeB((rx + 24) - (rx + 4));
    REQUIRE(bl_trampoline);
    REQUIRE(b_target);
    StoreInstruction(rw + 4, *bl_trampoline);
    buffer->Flush();

    REQUIRE(PatchDirectBranch(region, rx + 4, rw + 4, *bl_trampoline));
    REQUIRE(LoadInstruction(rw + 4) == *bl_trampoline);
#if defined(__aarch64__)
    using TestFunction = int (*)();
    auto function = reinterpret_cast<TestFunction>(rx);
    REQUIRE(function() == 1);
#endif

    REQUIRE(PatchDirectBranch(region, rx + 4, rw + 4, *b_target));
    REQUIRE(LoadInstruction(rw + 4) == *b_target);
    REQUIRE(DecodeBranchTarget(rx + 4, LoadInstruction(rx + 4)) ==
            reinterpret_cast<uintptr_t>(rx + 24));
#if defined(__aarch64__)
    REQUIRE(function() == 2);
#endif

    REQUIRE(PatchDirectBranch(region, rx + 4, rw + 4, *bl_trampoline));
    REQUIRE(LoadInstruction(rw + 4) == *bl_trampoline);
#if defined(__aarch64__)
    REQUIRE(function() == 1);
#endif
}

TEST_CASE("signal delink deactivates before deferred invalidation resets state",
          "[direct-link][manager][signal]") {
    auto config = Arm64Config();
    CodeCache cache{config, 1u << 20, FeatureSet{}, true};
    const auto buffer = cache.AllocCode(256);
    REQUIRE(buffer);
    const auto& region = cache.GetRegion();
    auto* rx_site = buffer->exec_data;
    auto* rw_site = buffer->rw_data;
    const auto bl = EncodeBL((buffer->exec_data + 64) - rx_site);
    const auto direct = EncodeB((buffer->exec_data + 128) - rx_site);
    REQUIRE(bl);
    REQUIRE(direct);
    StoreInstruction(rw_site, *bl);
    buffer->Flush();

    LinkManager manager;
    const int owner_module{};
    const int owner_allocation{};
    constexpr u64 kTarget = 0x5000;
    const LinkSiteKey key{region.id, buffer->offset};
    const LinkSignalPatchSite patch{
            .region = region,
            .rx_site = rx_site,
            .rw_site = rw_site,
            .unlinked_bl = *bl,
    };
    REQUIRE(manager.RegisterSite(
            key, kTarget, {&owner_module, &owner_allocation}, &patch));
    const auto generation = manager.PublishTarget(
            kTarget, buffer->exec_data + 128, region.id);
    REQUIRE(manager.MarkLinked(key, generation, [&](const LinkSiteRecord&) {
        return PatchDirectBranch(region, rx_site, rw_site, *direct);
    }));
    REQUIRE(LoadInstruction(rx_site) == *direct);

    // Models HandleWriteFault winning first. QueryTarget must reject the old
    // generation immediately, before CloseWriteWindow takes manager mutex_.
    const auto signal = manager.SignalInvalidateTarget(kTarget);
    REQUIRE(signal.found);
    REQUIRE(signal.linked_sites == 1);
    REQUIRE_FALSE(manager.QueryTarget(kTarget));
    REQUIRE(LoadInstruction(rx_site) == *bl);

    // Deferred cleanup is intentionally not gated by active==false: it still
    // returns the incoming record and resets Linked/Far metadata idempotently.
    const auto deferred = manager.BeginTargetInvalidation(kTarget);
    REQUIRE(deferred.size() == 1);
    REQUIRE(deferred.front().state == LinkSiteState::Linked);
    REQUIRE(manager.QuerySite(key)->state == LinkSiteState::Unlinked);
    REQUIRE(PatchDirectBranch(region, rx_site, rw_site, *bl));
    REQUIRE(LoadInstruction(rx_site) == *bl);

    REQUIRE(manager.DetachSource({&owner_module, &owner_allocation}) == 1);
    REQUIRE(manager.PurgeSource({&owner_module, &owner_allocation}) == 1);
    REQUIRE_FALSE(manager.QuerySite(key));

    // Model immediate mspace address reuse after PurgeSource. The retained
    // signal target may still be found, but its physically unlinked/tombstoned
    // site must never write the recycled allocation.
    StoreInstruction(rw_site, *direct);
    buffer->Flush();
    REQUIRE(manager.SignalInvalidateTarget(kTarget).found);
    REQUIRE(LoadInstruction(rx_site) == *direct);
    const auto future_generation = manager.PublishTarget(
            kTarget, buffer->exec_data + 128, region.id);
    REQUIRE(manager.QueryTarget(kTarget)->generation == future_generation);
}

TEST_CASE("pending flags bypass rejects incompatible linked targets",
          "[direct-link][manager][flags]") {
    auto config = Arm64Config();
    CodeCache cache{config, 1u << 20, FeatureSet{}, true};
    const auto buffer = cache.AllocCode(256);
    REQUIRE(buffer);
    const auto& region = cache.GetRegion();
    auto* bypass_rx = buffer->exec_data;
    auto* bypass_rw = buffer->rw_data;
    auto* first_rx = buffer->exec_data + 32;
    auto* first_rw = buffer->rw_data + 32;
    auto* second_rx = buffer->exec_data + 48;
    auto* second_rw = buffer->rw_data + 48;
    constexpr u32 kLinkedPublish = 0xb3401c1a;
    const auto cold_merge = EncodeBL((buffer->exec_data + 96) - bypass_rx);
    const auto first_bl = EncodeBL((buffer->exec_data + 128) - first_rx);
    const auto second_bl = EncodeBL((buffer->exec_data + 128) - second_rx);
    const auto first_direct = EncodeB((buffer->exec_data + 160) - first_rx);
    const auto second_direct = EncodeB((buffer->exec_data + 176) - second_rx);
    REQUIRE(cold_merge);
    REQUIRE(first_bl);
    REQUIRE(second_bl);
    REQUIRE(first_direct);
    REQUIRE(second_direct);
    StoreInstruction(bypass_rw, kLinkedPublish);
    StoreInstruction(first_rw, *first_bl);
    StoreInstruction(second_rw, *second_bl);
    buffer->Flush();

    LinkManager manager;
    const int owner_module{};
    const int owner_allocation{};
    const LinkSourceOwner owner{&owner_module, &owner_allocation};
    constexpr u64 kFirstTarget = 0x601000;
    constexpr u64 kSecondTarget = 0x602000;
    const LinkSiteKey first_key{region.id, buffer->offset + 32};
    const LinkSiteKey second_key{region.id, buffer->offset + 48};
    const LinkFlagsBypassPatch bypass_patch{
            .rx_site = bypass_rx,
            .rw_site = bypass_rw,
            .unlinked_instruction = *cold_merge,
            .linked_instruction = kLinkedPublish,
    };
    const LinkSignalPatchSite first_patch{
            .region = region,
            .rx_site = first_rx,
            .rw_site = first_rw,
            .unlinked_bl = *first_bl,
            .flags_bypass = bypass_patch,
    };
    const LinkSignalPatchSite second_patch{
            .region = region,
            .rx_site = second_rx,
            .rw_site = second_rw,
            .unlinked_bl = *second_bl,
            .flags_bypass = bypass_patch,
    };
    REQUIRE(manager.RegisterSite(first_key,
                                 kFirstTarget,
                                 owner,
                                 &first_patch,
                                 LinkSiteKind::Unconditional,
                                 kPendingNZCV));
    REQUIRE(manager.RegisterSite(second_key,
                                 kSecondTarget,
                                 owner,
                                 &second_patch,
                                 LinkSiteKind::Unconditional,
                                 kPendingNZCV));

    const auto first_generation = manager.PublishTarget(
            kFirstTarget,
            buffer->exec_data + 160,
            region.id,
            {},
            buffer->exec_data + 160,
            buffer->exec_data + 160,
            nullptr,
            nullptr,
            kOverwriteNZCV);
    auto second_generation = manager.PublishTarget(
            kSecondTarget,
            buffer->exec_data + 176,
            region.id,
            {},
            buffer->exec_data + 176);
    REQUIRE(manager.MarkLinked(first_key, first_generation, [&](const LinkSiteRecord&) {
        return PatchDirectBranch(region, first_rx, first_rw, *first_direct);
    }));
    REQUIRE(LoadInstruction(bypass_rx) == kLinkedPublish);
    REQUIRE(manager.MarkLinked(second_key, second_generation, [&](const LinkSiteRecord&) {
        return PatchDirectBranch(region, second_rx, second_rw, *second_direct);
    }));
    REQUIRE(LoadInstruction(bypass_rx) == *cold_merge);

    REQUIRE(manager.SignalInvalidateTarget(kSecondTarget).linked_sites == 1);
    REQUIRE(manager.BeginTargetInvalidation(kSecondTarget).size() == 1);
    second_generation = manager.PublishTarget(
            kSecondTarget,
            buffer->exec_data + 176,
            region.id,
            {},
            buffer->exec_data + 176,
            buffer->exec_data + 176,
            nullptr,
            nullptr,
            kOverwriteNZCV);
    REQUIRE(manager.MarkLinked(second_key, second_generation, [&](const LinkSiteRecord&) {
        return PatchDirectBranch(region, second_rx, second_rw, *second_direct);
    }));
    REQUIRE(LoadInstruction(bypass_rx) == kLinkedPublish);

    REQUIRE(manager.SignalInvalidateTarget(kFirstTarget).linked_sites == 1);
    REQUIRE(LoadInstruction(first_rx) == *first_bl);
    REQUIRE(LoadInstruction(bypass_rx) == kLinkedPublish);
}
