#pragma once
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>
#include "cfg_register_plan.h"

namespace swift::runtime::backend::x64 {
struct VectorRegisterOptions {
    std::vector<unsigned> physical_registers{8, 9, 10, 11, 12, 13, 14, 15};
    // Independent opt-in analysis limits, never scalar/CF allocation budgets.
    // Nodes count instructions + CFG blocks + CFG edges before sized storage.
    u64 max_analysis_nodes{32768};
    // Includes scans, set insert/copy/compare/propagation and coloring work.
    // Empty blocks/edges are charged too, bounding sparse fixed-point passes.
    u64 max_analysis_work{4 * 1024 * 1024};
    // Every attempted interference pair, including repeated clique attempts.
    u64 max_pair_work{2 * 1024 * 1024};
    // Actual directed adjacency entries: a new undirected edge costs two.
    u64 max_interference_storage_entries{256 * 1024};
};
enum class VectorPlanRejection : u8 {
    None, Disabled, InvalidGraph, InvalidRegisterBudget, AnalysisBudget, InvalidAnalysis
};
enum class VectorAnalysisLimit : u8 { None, Nodes, Work, PairWork, InterferenceStorage };
struct VectorAnalysisStats {
    u64 nodes{}, work{}, pair_work{}, interference_storage_entries{}, fixed_point_passes{};
    u64 work_before_fixed_point{};
    VectorAnalysisLimit exhausted{VectorAnalysisLimit::None};
};
struct VectorAllocation {
    ir::Inst* producer{};
    std::optional<unsigned> slot;
    u32 audited_reads{};
    bool cacheable{};
};
struct VectorCfgBlock {
    u32 begin{}, end{};
    bool reachable{};
    std::vector<ir::Inst*> live_in, live_out;
};
struct VectorCfgEdge {
    u32 from{}, to{};
    bool host_entry{}, taken{};
    ir::Inst* source{};
    std::vector<ir::Inst*> live;
};
// Canonical homes are always retained. This is a whole-function immutable
// assignment of complete initialized 128-bit containers; narrow annotations
// do not truncate cache contents or authorize scalar/vector reinterpretation.
// Independent vector liveness/interference, never scalar Interferes().
class VectorRegisterPlan {
public:
    // Graph supplies validated topology/Phi/dominance only. Build independently
    // analyzes all vector-container SSA, raw/delayed/terminal and Phi-edge uses.
    // Private caller must retain fresh immutable IR (as for scalar plans).
    static VectorRegisterPlan Build(const CfgRegisterPlan& graph,
                                    const backend::FunctionPlan* function,
                                    VectorRegisterOptions options = {});
    bool Enabled() const noexcept { return enabled; }
    VectorPlanRejection Rejection() const noexcept { return rejection; }
    const VectorAnalysisStats& AnalysisStats() const noexcept { return analysis_stats; }
    bool Validate() const noexcept;
    std::optional<unsigned> Register(const ir::Inst* producer) const noexcept;
    bool Interferes(const ir::Inst* left, const ir::Inst* right) const noexcept;
    const std::vector<unsigned>& UsedRegisters() const noexcept { return used_registers; }
    const std::vector<VectorAllocation>& Allocations() const noexcept { return allocations; }
    const std::vector<VectorCfgBlock>& Blocks() const noexcept { return blocks; }
    const std::vector<VectorCfgEdge>& Edges() const noexcept { return edges; }
    static constexpr bool retains_all_canonical_homes = true;
private:
    bool enabled{};
    VectorPlanRejection rejection{VectorPlanRejection::Disabled};
    VectorAnalysisStats analysis_stats;
    std::vector<unsigned> physical_registers, used_registers;
    std::vector<VectorAllocation> allocations;
    std::vector<VectorCfgBlock> blocks;
    std::vector<VectorCfgEdge> edges;
    std::unordered_map<const ir::Inst*, u32> indices;
    std::vector<std::set<u32>> interference;
};
} // namespace swift::runtime::backend::x64
