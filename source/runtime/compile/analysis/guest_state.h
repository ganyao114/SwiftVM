#pragma once

#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_map>
#include <vector>

#include "runtime/backend/register_mask.h"
#include "runtime/common/range_map.h"
#include "runtime/ir/args.h"
#include "runtime/ir/host_reg.h"
#include "runtime/ir/opcodes.h"

namespace swift::runtime {
struct FeatureSet;
}

namespace swift::runtime::ir {
class Block;
}

namespace swift::runtime::compile {

class SemanticFacts;

// Logical IR state only. A standalone block has no incoming state; a function
// may inherit it through ordinary edges. External and call-return entries are
// unknown in either view, independently of physical register-width seeds.
enum class GuestStateEntryPolicy : u8 { Block, Function };

// A byte names its defining value and offset within that value. Value equality
// deliberately compares definitions, not the type view attached to a use edge.
struct GuestStateByteFact {
    ir::Value value{};
    u8 offset{};

    [[nodiscard]] bool Defined() const { return value.Defined(); }
};

enum class GuestStateReadKind : u8 { Missing, Alias, Extract, VectorLane64 };

// The pending-store consumer additionally supports XMM lane extraction.
// Selecting that policy does not prove that all bytes came from one store;
// its caller must establish that stronger identity before classifying a view.
enum class GuestStateReadPolicy : u8 { Forward, ForwardNarrowFPR, PendingStore };

struct GuestStateRead {
    ir::Value value{};
    GuestStateReadKind kind{};
    u8 offset{};

    [[nodiscard]] bool Found() const { return kind != GuestStateReadKind::Missing; }
};

// Pure logical queries: no feature/layout eligibility, physical homes, mutation
// of IR, or ownership of the caller's byte table is hidden in these operations.
[[nodiscard]] GuestStateRead ClassifyGuestStateRead(ir::Value source, u32 source_offset,
                                                   ir::ValueType type,
                                                   GuestStateReadPolicy policy);
[[nodiscard]] GuestStateRead ReadGuestStateBytes(std::span<const GuestStateByteFact> bytes,
                                                ir::Uniform uniform,
                                                GuestStateReadPolicy policy);
// Intersect equal-size tables in place and return the number of surviving bytes.
u32 MeetGuestStateBytes(std::span<GuestStateByteFact> fallthrough,
                       std::span<const GuestStateByteFact> taken);

// Temporary logical edge snapshots for decode and the residual state pass.
// The caller owns the live byte table; no physical entry state is seeded here.
enum class GuestStateForwardEvent : u8 { None, Branch, FullInvalidation, RangeInvalidation, Merge };

struct GuestStateForwardUpdate {
    GuestStateForwardEvent event{};
    u32 merged_bytes{};
};

// Shared mutable-state transfers, before IR sealing. Storage and its lifetime
// belong to the caller; no borrowed byte span survives one operation. These
// rules describe logical uniform bytes, never physical register contents or
// the fault/publication obligations of a dead-store consumer.
class GuestStateForwarding final {
public:
    static void Write(std::span<GuestStateByteFact> bytes, u32 offset, ir::Value value);
    static void Invalidate(std::span<GuestStateByteFact> bytes);
    [[nodiscard]] GuestStateForwardUpdate Observe(std::span<GuestStateByteFact> bytes,
                                                  const ir::Inst& inst,
                                                  bool range_writes, bool path_forward);
    void Reset() { snapshots.clear(); }

private:
    void Remember(const ir::Inst* branch, std::span<const GuestStateByteFact> bytes);
    [[nodiscard]] std::optional<u32> Merge(const ir::Inst* branch,
                                          std::span<GuestStateByteFact> bytes);
    std::unordered_map<const ir::Inst*, std::vector<GuestStateByteFact>> snapshots;
};

// A conservative, policy-independent superset. Consumers still apply their
// helper, target, fault-capture and entry rules before treating an event as an
// actual publication boundary.
[[nodiscard]] bool IsGuestStateObservationCandidate(ir::OpCode op);
[[nodiscard]] std::span<const ir::Inst* const> GuestStateObservationWindow(
        std::span<const ir::Inst* const> observations, u32 after, u32 before);

// Potential GPR state transitions, before applying target/helper contracts.
// Declarations and explicit SetHostGPR writes are indexed separately.
[[nodiscard]] constexpr bool IsGuestStateGPRBarrierCandidate(ir::OpCode op) {
    using O = ir::OpCode;
    return op == O::CallLambda || op == O::CallLocation || op == O::CallDynamic ||
           op == O::X87Op || op == O::Sse42Str || op == O::XchgBarrier;
}

// Immutable numbered-IR inputs. A target may ignore a publication or resolve a
// barrier differently; those query decisions do not change this shared index.
enum class GuestStateRegisterFile : u8 { GPR, FPR };

class GuestStateRegisterAccesses final {
public:
    GuestStateRegisterAccesses(const GuestStateRegisterAccesses&) = delete;
    GuestStateRegisterAccesses& operator=(const GuestStateRegisterAccesses&) = delete;
    [[nodiscard]] std::optional<u32> FirstAccess(GuestStateRegisterFile file,
                                               u32 home, bool reads, u32 after,
                                               std::optional<u32> ignored = {}) const;
    // Positions are strictly inside (after, before), including partial accesses.
    // This borrowed view allocates nothing and hides the packed representation.
    [[nodiscard]] auto Positions(GuestStateRegisterFile file, u32 home, bool reads,
                                 u32 after, u32 before) const {
        return std::views::transform(AccessRange(file, home, reads, after, before),
                                     [](u32 entry) { return static_cast<u16>(entry); });
    }
    [[nodiscard]] std::span<const ir::Inst* const> GPRBarriers() const { return barriers; }

private:
    friend class GuestStateFacts;
    explicit GuestStateRegisterAccesses(const ir::Block* block);
    [[nodiscard]] std::span<const u32> AccessRange(GuestStateRegisterFile file,
                                                  u32 home, bool reads,
                                                  u32 after, u32 before) const;
    const ir::Block* block;
    // Packed file/home/read-role and u16 instruction position, sorted once.
    std::vector<u32> accesses;
    std::vector<const ir::Inst*> barriers;
};

// One semantic owner for independently requested guest-state input facts.
// Payloads contain identities, not physical homes, entry-width seeds or the
// result of a consumer's ignored/coalesced-publication policy.
class GuestStateFacts final {
public:
    GuestStateFacts();
    ~GuestStateFacts();
    GuestStateFacts(const GuestStateFacts&) = delete;
    GuestStateFacts& operator=(const GuestStateFacts&) = delete;
    [[nodiscard]] std::optional<std::span<const ir::Inst* const>> FindObservations(
            const ir::Block* block) const;
    // GPR declarations and publication sources, including their extension
    // aliases. The sorted identity catalog is a conservative universe, not a
    // statement that a value is currently resident or has been published.
    [[nodiscard]] std::optional<std::span<const ir::Inst* const>> FindGPRDefinitions(
            const ir::Block* block) const;
    [[nodiscard]] const GuestStateRegisterAccesses* FindRegisterAccesses(const ir::Block* block) const;

private:
    friend class SemanticFacts;
    [[nodiscard]] std::span<const ir::Inst* const> PrepareObservations(const ir::Block* block);
    [[nodiscard]] std::span<const ir::Inst* const> PrepareGPRDefinitions(const ir::Block* block);
    [[nodiscard]] const GuestStateRegisterAccesses& PrepareRegisterAccesses(const ir::Block* block);
    [[nodiscard]] bool PrepareGPRMatch(const ir::Block* block, u32 home, u32 offset,
                                      const ir::Inst& before, ir::Value expected,
                                      GuestStateEntryPolicy policy, SemanticFacts& facts);
    [[nodiscard]] std::optional<bool> FindGPRMatch(
            const ir::Block* block, u32 home, u32 offset, const ir::Inst& before,
            ir::Value expected, GuestStateEntryPolicy policy) const;
    struct BlockEntries {
        const ir::Block* block;
        std::vector<const ir::Inst*> instructions;
    };
    [[nodiscard]] static std::optional<std::span<const ir::Inst* const>> Find(
            const std::vector<BlockEntries>& entries, const ir::Block* block);
    // Separate vectors keep an observation-only block free of definition
    // payloads and preserve every published span across either family's growth.
    std::vector<BlockEntries> observations;
    std::vector<BlockEntries> gpr_definitions;
    // Stable pointees survive the roster's growth and other family's prepare.
    std::vector<std::unique_ptr<GuestStateRegisterAccesses>> register_accesses;
    struct VersionViews;
    std::unique_ptr<VersionViews> version_views;
};

struct GuestRegisterBinding {
    ir::Uniform uniform{};
    ir::HostReg host_reg{};

    [[nodiscard]] bool Null() const {
        return uniform.GetType() == ir::ValueType::VOID;
    }

    bool operator==(const GuestRegisterBinding& rhs) const { return uniform == rhs.uniform; }
    bool operator!=(const GuestRegisterBinding& rhs) const { return !(rhs == *this); }
};

struct GuestStateRange {
    u32 begin{};
    u32 end{};
};

// Target-declared optimization eligibility. The default has no implicit host
// register numbering convention. It does not seed physical high-bit facts.
struct GuestStateOptimizationPolicy {
    backend::GPRSMask forwarded_gprs{};
    bool dse_before_mapping{};
};

struct GuestStateLayout {
    u32 uniform_size{};
    backend::GPRSMask uni_gprs{};
    backend::FPRSMask uni_fprs{};
    RangeMap<u32, GuestRegisterBinding> uniform_regs_map{};
    std::vector<GuestStateRange> xmm_uniform_ranges{};
    std::vector<GuestStateRange> loop_gpr_uniform_ranges{};
    GuestStateOptimizationPolicy optimization{};

    [[nodiscard]] bool IsXmmRange(u32 offset, u32 size) const {
        for (const auto& range : xmm_uniform_ranges) {
            if (offset >= range.begin && offset + size <= range.end) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool IsLoopInvariantGPRRange(u32 offset, u32 size) const {
        for (const auto& range : loop_gpr_uniform_ranges) {
            if (offset >= range.begin && offset + size <= range.end) {
                return true;
            }
        }
        return false;
    }
};

enum class GuestStateAccessKind : u8 { Invalid, Memory, Register };

struct GuestStateAccess {
    GuestStateAccessKind kind{GuestStateAccessKind::Invalid};
    GuestRegisterBinding binding{};
};

// Classify the actual access type, including scalar lanes of an FPR mapping.
// FPR deferral chooses memory before testing register containment, as in the
// original consumers. Decode's full-width store restriction is a separate rule.
[[nodiscard]] GuestStateAccess ResolveGuestStateAccess(const GuestStateLayout& layout,
                                                       u32 offset, ir::ValueType type,
                                                       bool defer_mapped_fpr);

enum class GuestStateForwardWrite : u8 { Ignore, Write, Invalidate };

// Eligibility is separate from byte identity and from conversion into Get/Set
// instructions. Both construction and the residual pass use the same policy;
// decode still defers partial mapped stores to the residual rewrite.
struct GuestStateForwardAccess {
    GuestStateAccess access;
    bool read{}, seed_load{};
    GuestStateForwardWrite write{};
};

[[nodiscard]] GuestStateForwardAccess DescribeGuestStateForwardAccess(
        const GuestStateLayout& layout, const FeatureSet& features,
        u32 offset, ir::ValueType type, bool defer_mapped_fpr);

}  // namespace swift::runtime::compile
