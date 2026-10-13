#pragma once

#include "jit_context.h"
#include "vector_allocation_plan.h"
#include "vector_publication_plan.h"

namespace swift::runtime::backend::loongarch64 {

struct VectorValueStats {
    u32 loads{}, stores{}, cache_hits{}, moves{}, spills{}, dead_values{}, barriers{};
    u32 lasx_fusions{}, lasx_operations{}, gathers{}, scatters{}, permutations{};
    u64 load_bytes{}, store_bytes{};
    bool global_cfg{};
    u32 assigned_values{}, spilled_values{}, interference_edges{}, cfg_nodes{}, edge_preserved{}, register_reads{};
    u32 direct_reads{}, direct_writes{};
    u32 skipped_publications{}, publication_iterations{};
    u64 skipped_publication_bytes{};
    u64 boundary_load_bytes{}, boundary_store_bytes{};
};

class VectorContext {
public:
    explicit VectorContext(JitContext& context) : context(context) {}
    void Configure(ir::Block* block, std::span<const WideVectorPair> wide = {}, bool allocation = true,
            const RegisterPlan::Targets& targets = {}, const RegisterPlan::Stops& stops = {},
            const RegisterPlan::Exits& exits = {}, bool cfg_allocation = true);
    void BeginInstruction(ir::Inst* inst);
    void FinishInstruction();
    void Barrier();
    bool GlobalCFGActive() const { return global_cfg; }
    void PublishPhiInput(ir::Inst* inst);
    void ReloadPhiDefinition(ir::Inst* inst);
    bool StoreUniform(ir::Inst* inst);
    void ReadCheckpointPart(GPR result, ir::Value value, u32 part);
    // LSX reads preserve the complete raw128 payload even for narrow views.
    VReg Read(ir::Value value, VReg scratch);
    // Native emitters only read these operands; a mapped input must never be
    // used as destructive scratch. Region low-half hits pin their cache slot
    // until the instruction finishes; high halves and misses use scratch.
    VReg ReadOperand(ir::Value value, VReg scratch);
    VReg Destination(ir::Inst* inst, VReg scratch) const;
    void Write(ir::Inst* inst, VReg source);
    bool EmitWide(ir::Inst* inst);
    JitContext& Context() { return context; }
    const VectorValueStats& Stats() const { return stats; }
private:
    struct Entry { ir::Inst* low{}; ir::Inst* high{}; bool dirty{}; };
    JitContext& context;
    std::optional<VectorAllocationPlan> plan;
    std::optional<VectorPublicationPlan> publication;
    std::array<Entry, VectorAllocationPlan::kRegisterCount> cache{};
    std::array<bool, VectorAllocationPlan::kRegisterCount> pinned{};
    ir::Inst* current{};
    bool allocation{true};
    bool global_cfg{}, instruction_active{}, canonical_boundary{};
    VectorValueStats stats{};
    std::unordered_set<ir::Inst*> completed_tails;
    u32 Allocate(ir::Inst* low, ir::Inst* high = nullptr);
    bool Live(const Entry& entry, const LiveSet& live) const;
    void LoadHome(VReg destination, ir::Inst* value);
    void StoreHome(VReg source, ir::Inst* value);
    void Store(const Entry& entry, u32 reg);
    void Publish(u32 index);
    XVReg ReadWide(const std::array<ir::Inst*, 2>& pair, XVReg scratch);
    void WriteWide(const WideVectorPair& pair, XVReg source);
    void PublishMapped(ir::Inst* inst);
    void ReloadMapped(ir::Inst* inst);
    void PublishLive(const LiveSet& live);
    void ReloadLive(const LiveSet& live);
    void PublishCanonicalInputs(ir::Inst* inst);
    std::optional<u32> UniformSourceRegister(ir::Inst* inst) const;
};

} // namespace swift::runtime::backend::loongarch64
