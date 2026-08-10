// Default-off execution-shape probe for W71.
//
// Generated code writes only to a Runtime-private counter array. Runtime
// destruction merges those arrays into process counters with relaxed atomics,
// so guest threads never contend on a shared counter in the JIT hot path.
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "runtime/common/types.h"

namespace swift::runtime::ir {
class Block;
}

namespace swift::runtime {

constexpr u32 kHotCoalesceMaxUnits = 32768;
constexpr u32 kHotCoalesceInvalidSlot = UINT32_MAX;

enum class HotCoalesceCounter : u32 {
    Entries,
    SpillReloads,
    SpillWritebacks,
    NaNGuardInstructions,
    IndirectL1Hit,
    IndirectL1Miss,
    Count,
};

constexpr u32 kHotCoalesceCounterCount =
        static_cast<u32>(HotCoalesceCounter::Count);
constexpr u32 kHotCoalesceMaxLinkTargets = 4;

// W-beta B0 is deliberately metadata-only.  These enums describe why a
// current packed-flags operation exists, which runtime edge will consume it,
// and the conservative instruction delta of the proposed representation.  No
// counter in this family is emitted into guest code; dynamic weighting reuses
// the pre-existing W71 block-entry profile when that independent probe is on.
enum class FlagsRegsAuditMergeCause : u8 {
    AdvancePC,
    TerminalInternal,
    TerminalDispatcher,
    HostExit,
    Helper,
    PStateClobber,
    ClearOrPartialWrite,
    FaultVeneer,
    Count,
};

enum class FlagsRegsAuditEdgeKind : u8 {
    RegionInternal,
    PatchedDirect,
    DirectSlow,
    Dispatcher,
    RSBHit,
    RSBMiss,
    Host,
    Count,
};

enum class FlagsRegsAuditCost : u8 {
    PFSavedInstructions,
    AFSavedInstructions,
    MergeSavedInstructions,
    PackInstructions,
    UnpackInstructions,
    CacheBaseReloadInstructions,
    RecoveryColdBytes,
    Count,
};

inline constexpr size_t kFlagsRegsAuditCostCount =
        static_cast<size_t>(FlagsRegsAuditCost::Count);

struct FlagsRegsAuditRecord {
    FlagsRegsAuditMergeCause cause{};
    FlagsRegsAuditEdgeKind edge{};
    u32 sites{};
    std::array<u32, kFlagsRegsAuditCostCount> cost{};
};

struct HotCoalesceUniformStats {
    u32 sequences{};
    u32 load_pairs{};
    u32 store_pairs{};
    u32 same_offset{};
    u32 saved_instructions{};
};

struct HotCoalesceUnitStatic {
    VAddr guest_entry{};
    VAddr host_address{};
    u32 host_offset{};
    u32 host_bytes{};
    u32 host_instructions{};
    u32 spill_reloads{};
    u32 spill_writebacks{};
    u32 move_bridges{};
    u32 nan_guard_instructions{};
    u32 helper_calls{};
    u32 helper_snapshot_instructions{};
    u32 helper_snapshot_code_bytes{};
    u32 helper_snapshot_memory_bytes{};
    std::array<VAddr, kHotCoalesceMaxLinkTargets> link_targets{};
    u8 link_target_count{};
    u8 link_target_overflow{};
    HotCoalesceUniformStats uniform{};
    std::vector<FlagsRegsAuditRecord> flags_regs_audit{};
};

[[nodiscard]] bool HotCoalesceProfEnabled();
[[nodiscard]] bool IndirectL1ProfEnabled();
[[nodiscard]] bool FlagsRegsAuditEnabled();
[[nodiscard]] bool HotCounterStorageEnabled();
[[nodiscard]] u32 HotCoalesceRegisterUnit(VAddr guest_entry);
void HotCoalesceUpdateUnit(u32 slot, const HotCoalesceUnitStatic& counters);
void HotCoalesceSetUnitHostBase(u32 slot, VAddr host_base);
void HotCoalesceSubmitThread(std::span<const u64> counters);

[[nodiscard]] HotCoalesceUniformStats HotCoalesceAnalyzeUniformSequences(
        const ir::Block* block);
void HotCoalesceAnalyzeLinkTargets(const ir::Block* block,
                                   HotCoalesceUnitStatic& out);
[[nodiscard]] bool HotCoalesceIsMoveBridge(std::string_view disassembly);

}  // namespace swift::runtime
