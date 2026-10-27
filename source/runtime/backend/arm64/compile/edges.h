#pragma once

#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "runtime/backend/edge_flags_state.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {

struct EdgeBlockAction {
    u64 location;
    const ir::Block* block;
    EdgeFlagsTargetContract edge_flags;
    u16 accepted_masks{};
    // Optional audit summaries use the existing row padding. The suffix is
    // identified by its first AdvancePC ID, without assuming ID order.
    u16 strict_advance_first{UINT16_MAX}, guest_instructions{};
    bool transparent : 1 {};
    bool token_overwritten : 1 {};
    bool requires_unit_state : 1 {};
};

struct PFAFOperand {
    enum class Kind : u8 { None, Imm, HostGPR, Uniform };
    u64 value{};
    Kind kind{};
    u8 offset{};
    PFAFOperand() = default;
    PFAFOperand(Kind kind, u64 value, u8 offset) : value(value), kind(kind), offset(offset) {}
    bool operator==(const PFAFOperand&) const = default;
    auto operator<=>(const PFAFOperand& other) const {
        return std::tie(kind, value, offset) <=> std::tie(other.kind, other.value, other.offset);
    }
};

struct PFAFAction {
    PFAFOperand left, right;
    u16 producer;
    u8 width;
};

struct BackedgeAction {
    ir::Location self_target{}, cold_target{};
    u16 polarity_load{}, polarity_store{}, final_advance{};
    u16 pfaf{UINT16_MAX};
    u8 requested_mask{}, carry_inverted{};
    bool dead_successor{}, canonical_carry{}, self_is_then{};
    bool omit_polarity_load{}, omit_polarity_store{};
    [[nodiscard]] ir::Flags Required() const { return static_cast<ir::Flags>(requested_mask); }
};

// Only optional reporting retains the raw target order/duplicates. Normal
// emission never needs to rediscover terminals for these diagnostics.
struct CFGReportRow {
    u64 location;
    u32 begin{}, count{};
    bool external{};
};

struct EdgeActions {
    bool active{};
    std::vector<EdgeBlockAction> blocks;
    std::vector<u64> canonical_terminal_entries;
    std::vector<std::pair<u64, u64>> cycle_edges;
    std::vector<std::pair<u64, u64>> backward_terminal_candidates;
    // Ordinary HIR transfers that retain values required by the destination.
    // Guest call-return relations do not carry an implicit preservation ABI.
    std::vector<std::pair<u64, u64>> unit_state_edges;
    u32 local_cycle_source_count{};
    std::vector<CFGReportRow> report;
    std::vector<u64> report_targets;

    [[nodiscard]] const EdgeBlockAction* Block(u64 location) const;
    [[nodiscard]] const EdgeBlockAction* ForBlock(const ir::Block*) const;
    [[nodiscard]] const EdgeBlockAction* InternalBlock(u64 location) const {
        return active ? Block(location) : nullptr;
    }
    [[nodiscard]] bool ContainsInternalBlock(u64 location) const {
        return InternalBlock(location) != nullptr;
    }
    [[nodiscard]] std::optional<bool> BlockTransparency(const ir::Block*) const;
    [[nodiscard]] bool IsCycleEdge(u64 source, u64 target) const;
    [[nodiscard]] bool CarriesUnitState(u64 source, u64 target) const;
    [[nodiscard]] bool HasCycleSource(u64 source) const;
    [[nodiscard]] bool IsCanonicalEntry(u64 location) const;
};

enum class UnalignedAtomicFallbackKind : u8 { CompareAndSwap, Exchange, FetchAdd };
struct UnalignedAtomicFallbackKey {
    UnalignedAtomicFallbackKind kind{};
    ir::ValueType type{};
    u8 address{}, result{}, first{}, second{};
    auto operator<=>(const UnalignedAtomicFallbackKey&) const = default;
};
struct AtomicAction {
    u16 instruction, cohort;
    bool operator==(const AtomicAction&) const = default;
};
struct AtomicCohort {
    UnalignedAtomicFallbackKey key;
    u32 count{};
    bool operator==(const AtomicCohort&) const = default;
};
struct AtomicActions {
    std::vector<AtomicAction> instructions;
    std::vector<AtomicCohort> cohorts;
    [[nodiscard]] const AtomicCohort* For(u32 instruction) const;
};

void CollectEdgeTargets(const ir::Terminal&, std::vector<u64>&);
std::optional<ir::Location> EdgeLeafTarget(const ir::Terminal&);

}  // namespace swift::runtime::backend::arm64
