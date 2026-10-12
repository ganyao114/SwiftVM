#pragma once

#include <optional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "runtime/backend/function_plan.h"

namespace swift::runtime::backend::x64 {

enum class CfgPlanRejection { None, MissingBody, InvalidBudget, InvalidMetadata, InvalidPhi, InvalidSSA };
struct CfgRegisterOptions {
    // Caller can reserve RBX for flags by supplying {5, 15}, or use no slots.
    // No caller-clobbered registers are permitted by this initial plan.
    std::vector<unsigned> physical_registers{3, 5, 15};
    // Temporary interference word storage (matrix plus clique mask) only.
    // Zero uses the original sparse builder; this never changes allocation.
    u64 interference_storage_budget_bytes{8u * 1024u * 1024u};
    // Natural-loop weights affect coloring costs only, never graph validity,
    // liveness, interference or the stable-register/home contract.
    bool loop_weighted_costs{false};
    // Opt-in read-before-publication phases for the fifteen audited unary
    // lowerings. The legacy binary phase contract remains independent.
    bool scalar_unary_phases{false};
};
struct CfgConstructionStats {
    bool dense_used{}, sparse_fallback{};
    u64 dense_bytes{}, clique_calls{};
    u32 natural_loops{}, max_loop_depth{};
};
struct CfgPhiCopy { ir::Inst* destination{}; ir::DataClass source{}; };
struct CfgPlanEdge {
    u32 from{}, to{};
    ir::Inst* source{}; // Last source instruction; null for host entry.
    bool taken{}, host_entry{};
    // Parallel semantics: capture every source in ordinal scratch slot first,
    // then publish every destination's canonical home (if required) and slot.
    // Never serialize these into in-place moves, even if assignments coalesce.
    std::vector<CfgPhiCopy> copies;
    std::vector<ir::Inst*> live;
};
struct CfgPlanBlock {
    u32 begin{}, end{}; // Half-open positions in the explicitly supplied body.
    bool reachable{};
    std::vector<u32> incoming_edges, outgoing_edges;
    std::vector<ir::Inst*> live_in, live_out;
    u32 loop_depth{}, loop_weight{1};
};
struct CfgScalarAllocation {
    ir::Inst* producer{};
    std::optional<unsigned> slot;
    bool requires_canonical_home{true};
    u32 uses{};
    // Static audited native Read sites; opaque/raw/indirect metadata uses
    // are deliberately excluded. This is not dynamic execution frequency.
    u32 scalar_reads{}, estimated_benefit{};
    // Raw/opaque runtime consumers may read the complete initialized low
    // 16-byte container. Independent of scalar spills and native metadata.
    bool requires_full_container{true};
    // Static natural-loop estimates, not observed execution counts. Reads
    // use their consumer block (Phi inputs use the source edge block), while
    // saved home stores and cache publications use the definition block.
    u32 weighted_reads{}, weighted_benefit{};
};

// Whole lowered-CFG SSA allocation, independent of lexical segment order.
// Every assigned producer owns one stable callee-saved physical register at
// every definition/use and across returning calls. Uncolored values retain
// their canonical homes for their entire lifetime; there are no dynamic
// evictions, body-local reload assumptions or per-segment assignments.
class CfgRegisterPlan {
public:
    // Independently verifies the lowered FunctionPlan CFG/Phi/dominance
    // contracts. Production calls this after ConfigurePhis validation too.
    static CfgRegisterPlan Build(const ir::Block* block, const backend::FunctionPlan& function,
                                 CfgRegisterOptions options = {});
    // Share an already valid immutable analysis and recolor only its physical
    // budget in independently owned allocations. Base home obligations survive
    // any previous spilling; topology/liveness/interference are never copied.
    // This does not revalidate changed IR arguments or prove body freshness;
    // callers must keep the analyzed IR immutable throughout its lifetime.
    CfgRegisterPlan Recolor(std::vector<unsigned> physical_registers) const;
    // Diagnostic identity proof only; it does not grant IR freshness or make
    // allocations interchangeable between budgets.
    bool SharesAnalysisWith(const CfgRegisterPlan& other) const noexcept {
        return Enabled() && other.Enabled() && analysis && analysis == other.analysis;
    }
    // Identical metadata/Phi/SSA validation, including every physical input,
    // edge input and terminal selector. Supplies topology and dominance only:
    // no liveness, interference, allocations or scalar phase reuse proofs.
    // Enabled means the graph is valid; Validate() deliberately returns false
    // because this result cannot authorize register allocation.
    static CfgRegisterPlan ValidateGraph(const ir::Block* block, const backend::FunctionPlan& function);
    bool Enabled() const noexcept { return rejection == CfgPlanRejection::None; }
    bool HasLiveness() const noexcept { return Enabled() && Data().has_liveness; }
    bool HasInterference() const noexcept { return Enabled() && Data().has_interference; }
    bool LoopWeightedCostsEnabled() const noexcept { return HasInterference() && Data().loop_weighted_costs; }
    bool ScalarUnaryPhasesEnabled() const noexcept { return HasInterference() && Data().scalar_unary_phases; }
    CfgPlanRejection Rejection() const noexcept { return rejection; }
    std::optional<unsigned> Slot(const ir::Inst* producer) const noexcept;
    std::optional<unsigned> Register(const ir::Inst* producer) const noexcept;
    bool RequiresCanonicalHome(const ir::Inst* producer) const noexcept;
    // Only a full valid allocation can prove that scalar Home+8 is unread.
    // Unknown producers, invalid plans and graph-only results return true.
    bool RequiresFullContainer(const ir::Inst* producer) const noexcept;
    bool Interferes(const ir::Inst* left, const ir::Inst* right) const noexcept;
    // Only the eight audited scalar binary lowerings: lhs is dead after
    // this dynamic instruction and every occurrence is captured before Write.
    // The emitter separately requires matching assigned physical registers.
    bool CanReuseBinaryInput(const ir::Inst* output, const ir::Inst* lhs) const noexcept;
    // Only the fifteen audited scalar unary lowerings. Every physical input
    // occurrence must be captured before publication; raw/opaque late uses
    // and CFG-live sources prevent reuse, independently of binary proofs.
    bool CanReuseUnaryInput(const ir::Inst* output, const ir::Inst* source) const noexcept;
    bool Validate() const noexcept;
    const ir::Block* Body() const noexcept { return Data().body; }
    const std::vector<ir::Inst*>& Instructions() const noexcept { return Data().instructions; }
    bool MatchesBody(const ir::Block* candidate) const noexcept;
    bool DominatesBlock(u32 from, u32 to) const noexcept;
    const std::vector<CfgPlanBlock>& Blocks() const noexcept { return Data().blocks; }
    const std::vector<CfgPlanEdge>& Edges() const noexcept { return Data().edges; }
    const std::vector<CfgScalarAllocation>& Allocations() const noexcept { return allocations; }
    const std::vector<unsigned>& PhysicalRegisters() const noexcept { return physical_registers; }
    const CfgConstructionStats& ConstructionStats() const noexcept { return Data().construction_stats; }
private:
    static CfgRegisterPlan BuildImpl(const ir::Block* block, const backend::FunctionPlan& function,
                                     CfgRegisterOptions options, bool build_interference);
    void ColorAllocations();
    struct Analysis {
        bool has_liveness{}, has_interference{};
        bool loop_weighted_costs{}, scalar_unary_phases{};
        const ir::Block* body{};
        std::vector<ir::Inst*> instructions;
        std::vector<std::unordered_set<u32>> dominators;
        std::vector<CfgPlanBlock> blocks;
        std::vector<CfgPlanEdge> edges;
        // Immutable analysis facts, indexed exactly like each independently
        // owned allocation array. Public home flags may gain spill obligations.
        std::vector<bool> base_canonical_homes, allocation_eligible;
        std::unordered_map<const ir::Inst*, u32> allocation_indices;
        std::unordered_map<const ir::Inst*, std::unordered_set<const ir::Inst*>> interference;
        std::unordered_map<const ir::Inst*, const ir::Inst*> binary_reuse, unary_reuse;
        CfgConstructionStats construction_stats;
    };
    const Analysis& Data() const noexcept;
    const std::unordered_set<const ir::Inst*>& Neighbors(const ir::Inst* producer) const noexcept;
    CfgPlanRejection rejection{CfgPlanRejection::MissingBody};
    std::shared_ptr<const Analysis> analysis;
    std::vector<unsigned> physical_registers;
    std::vector<CfgScalarAllocation> allocations;
};
}  // namespace swift::runtime::backend::x64
