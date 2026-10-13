#pragma once

#include <array>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include "runtime/backend/code_cache.h"
#include "runtime/backend/loongarch64/assembler.h"
#include "runtime/backend/loongarch64/defines.h"
#include "runtime/backend/loongarch64/host_features.h"
#include "runtime/backend/loongarch64/jit/flags_plan.h"
#include "runtime/backend/loongarch64/jit/register_plan.h"
#include "runtime/backend/loongarch64/jit/scalar_allocation_plan.h"
#include "runtime/backend/loongarch64/jit/sparse_homes_plan.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend { class Module; }
namespace swift::runtime::backend::loongarch64 {

enum class ScalarAllocationMode { CanonicalHomes, LocalCache, GlobalCFG };

struct ScalarValueStats {
    u32 loads{}, stores{}, cache_hits{}, spills{}, dead_values{}, register_reads{};
};

struct FlagValueStats {
    u32 loads{}, stores{}, skipped_stores{};
};

// Whole-CFG scalar assignments survive joins, backedges and ABI helpers in
// callee-saved GPRs. Sparse homes serve spills, typed helpers and raw containers;
// LocalCache/CanonicalHomes retain dense homes as explicit baselines. tp/r21 are reserved.
// FPR/LSX/LASX aliases are owned by each emitter.
class JitContext {
public:
    explicit JitContext(bool cache_values = true, HostFeatures features = HostFeatures::Detect(),
                        std::span<const UniformMapDesc> uniforms = {},
                        ScalarAllocationMode allocation = ScalarAllocationMode::GlobalCFG);
    Assembler& GetMasm() { return masm; }
    const HostFeatures& Features() const { return features; }
    u32 CurrentBufferSize() const;
    void Flush(const CodeBuffer& buffer);
    void ConfigureModule(Module* owner);
    void ConfigureBlock(ir::Block* block);
    void ConfigureRegisters(ir::Block* block, const RegisterPlan::Exits& exits = {});
    // Call after ConfigureRegisters and only on a private emitted clone when
    // sparse mode is requested. Public source instruction IDs must not change.
    void ConfigureHomes(ir::Block* block, const RegisterPlan::Exits& exits = {});
    void SetSparseHomes(bool enabled) { sparse_homes_enabled = enabled; }
    bool UsesSparseHomes() const { return sparse_homes_enabled && allocation_mode == ScalarAllocationMode::GlobalCFG; }
    bool SparseHomesActive() const { return sparse_homes.has_value(); }
    const SparseHomesStats& HomeStats() const { return home_stats; }
    u32 HomeOffset(ir::Inst* inst, u32 part = 0) const;
    void ConfigureFlags(ir::Block* block, const FlagsPlan::Exits& exits = {},
                        const FlagsPlan::Instructions& halt_checks = {});
    void BeginInstruction(ir::Inst* inst);
    void FinishInstruction();
    const RegisterPlanStats& RegisterStats() const { return register_plan->Stats(); }
    void ConfigureFlow(std::unordered_map<ir::Inst*, ir::Inst*> targets,
                       std::unordered_set<ir::Inst*> stops,
                       std::unordered_map<ir::Inst*, ir::Location> locations,
                       std::unordered_set<ir::Inst*> synthetic = {});
    void RemapFlow(const std::unordered_map<ir::Inst*, ir::Inst*>& instructions);
    ir::Inst* BranchTarget(ir::Inst* branch) const;
    bool FallsThrough(ir::Inst* inst) const { return !stops.contains(inst); }
    bool IsSyntheticLabel(ir::Inst* inst) const { return synthetic_labels.contains(inst); }
    std::optional<ir::Location> LocationAt(ir::Inst* inst) const;
    u32 SlotCount() const { return slot_count; }
    void Address(GPR result, GPR base, s64 offset);
    void Load(GPR result, GPR base, s64 offset, u32 size = 8);
    void Store(GPR source, GPR base, s64 offset, u32 size = 8);
    void Read(GPR result, ir::Value value) { ReadPart(result, value, 0); }
    void ReadPart(GPR result, ir::Value value, u32 part);
    void Write(ir::Inst* inst, GPR value) { WritePart(inst, value, 0); }
    void WritePart(ir::Inst* inst, GPR value, u32 part);
    // Raw containers retain both halves even through narrow integer views.
    void ReadRawPart(GPR result, ir::Value value, u32 part);
    void WriteRawPart(ir::Inst* inst, GPR value, u32 part);
    void PublishInput(ir::Inst* value);
    void PublishInputs(ir::Inst* inst);
    void ReloadDefinition(ir::Inst* value);
    void ReloadDefinitions(ir::Inst* inst);
    ScalarAllocationMode AllocationMode() const { return allocation_mode; }
    const ScalarAllocationStats& AllocationStats() const {
        static const ScalarAllocationStats empty{};
        return scalar_allocation ? scalar_allocation->Stats() : empty;
    }
    void Operand(GPR result, const ir::Operand& operand);
    void Lambda(GPR result, const ir::Lambda& lambda);
    void Mask(GPR result, u32 bits);
    void Sign(GPR result, u32 bits);
    void ReadFlags(GPR result);
    void WriteFlags(GPR source);
    // This switch changes publication frequency, never the resident r31 ABI.
    // Call before Translate. The shared epilogue always forces its store.
    void SetFlagsPublication(bool enabled) { flags_publication = enabled; }
    const FlagValueStats& FlagStats() const { return flag_stats; }
    void PublishFlags(bool force = false);
    void ReloadFlags();
    void FlushValues();
    void DiscardValues();
    const ScalarValueStats& ValueStats() const { return stats; }
    void HostAddress(GPR result, u64 address) { masm.LI(result, address); }
    void Jump(Label& label) { masm.J(&label); }
    void BranchZero(GPR value, Label& label, bool zero_test = true);

private:
    Assembler masm;
    HostFeatures features;
    Module* module{};
    u32 slot_count{1};
    struct CachedValue { ir::Inst* inst{}; u32 part{}; bool dirty{}; };
    std::array<CachedValue, 6> cache{};
    size_t next_register{};
    ScalarValueStats stats{};
    bool cache_values{true};
    ScalarAllocationMode allocation_mode{ScalarAllocationMode::GlobalCFG};
    std::optional<ScalarAllocationPlan> scalar_allocation;
    std::optional<SparseHomesPlan> sparse_homes;
    SparseHomesStats home_stats{};
    LiveSet home_members;
    bool sparse_homes_enabled{true};
    std::optional<RegisterPlan> register_plan;
    std::optional<FlagsPlan> flags_plan;
    ir::Flags flags_dirty{ir::Flags::All};
    FlagValueStats flag_stats{};
    bool flags_publication{true};
    ir::Inst* current_inst{};
    bool IsLive(ir::Inst* inst) const;
    void ReleaseDead(const LiveSet& live);
    std::unordered_map<ir::Inst*, ir::Inst*> targets;
    std::unordered_set<ir::Inst*> stops, synthetic_labels;
    std::unordered_map<ir::Inst*, ir::Location> locations;
};
}  // namespace swift::runtime::backend::loongarch64
