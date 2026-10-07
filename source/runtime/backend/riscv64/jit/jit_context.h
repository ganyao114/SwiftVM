#pragma once

#include <biscuit/assembler.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <span>
#include "runtime/backend/code_cache.h"
#include "runtime/backend/riscv64/defines.h"
#include "runtime/backend/riscv64/host_features.h"
#include "runtime/ir/args.h"
#include "runtime/ir/location.h"

namespace swift::runtime::ir { class Block; }

namespace swift::runtime::backend::riscv64 {

struct ScalarValueStats {
    u32 loads{};
    u32 stores{};
    u32 cache_hits{};
    u32 spills{};
    u32 dead_values{};
    u32 initializations{};
};

struct UniformBinding {
    UniformMapDesc desc{0, 0, 0, false};
    std::array<u32, 2> gpr_indices{UINT32_MAX, UINT32_MAX};
    u32 vector_index{UINT32_MAX};
    u32 accesses{};
    bool written{}, spanning{};
};

struct PhiBinding {
    ir::Inst* inst{};
    std::array<u32, 2> gpr_indices{UINT32_MAX, UINT32_MAX};
    u32 vector_index{UINT32_MAX};
};

// Each SSA value has a canonical 16-byte home. Direct scalar lowering defers
// writes while values reside in callee-saved GPRs or RVV registers. Integer
// vectors use GPR pairs without V. CFG edges publish live-in homes and assign
// resident phis in parallel; callbacks preserve caller-saved vectors and reload
// logical uniform bindings. No ARM64 physical-register allocation runs.
class JitContext {
public:
    struct ValueCacheSnapshot {
        std::array<ir::Inst*, scalar_registers.size()> gprs;
        std::array<u32, scalar_registers.size()> parts;
        std::array<ir::Inst*, 16> vectors;
        size_t next_gpr, next_vector;
    };
    ValueCacheSnapshot CaptureValues() const { return {cached_values, cached_parts, cached_vectors, next_register, next_vector}; }
    void RestoreValues(const ValueCacheSnapshot& snapshot) {
        cached_values = snapshot.gprs; cached_parts = snapshot.parts; cached_vectors = snapshot.vectors;
        next_register = snapshot.next_gpr; next_vector = snapshot.next_vector; ResetVectorType();
    }
    explicit JitContext(bool cache_scalars = true, HostFeatures features = HostFeatures::Detect(),
                        std::span<const UniformMapDesc> uniforms = {})
            : cache_scalars(cache_scalars), features(features), uniform_descriptors(uniforms) {}
    biscuit::Assembler& GetMasm() { return masm; }
    const HostFeatures& Features() const { return features; }
    void EnsureSpace();
    void Address(biscuit::GPR result, biscuit::GPR base, s64 offset);
    void Load(biscuit::GPR result, biscuit::GPR base, s64 offset, u32 size = 8);
    void Store(biscuit::GPR value, biscuit::GPR base, s64 offset, u32 size = 8);
    void Read(biscuit::GPR result, ir::Value value);
    void ReadPart(biscuit::GPR result, ir::Value value, u32 part);
    biscuit::GPR SourcePart(ir::Value value, u32 part, biscuit::GPR scratch);
    static bool ZeroHigh(ir::Inst* inst);
    void SetPosition(u32 position) { current_position = position; }
    void EnablePairCoalescing(bool enabled) { pair_coalescing = enabled; }
    bool PairCoalescingEnabled() const { return pair_coalescing; }
    std::array<biscuit::GPR, 2> ResultPair(ir::Inst* inst);
    void WritePair(ir::Inst* inst, const std::array<biscuit::GPR, 2>& registers);
    void SetVectorType(u32 bits, u32 lanes);
    void ResetVectorType() { vector_bits = vector_lanes = 0; vector_round_mode = ~u32{0}; }
    void SetVectorRounding(u32 mode);
    biscuit::Vec ResultVector(ir::Inst* inst);
    biscuit::Vec SourceVector(ir::Value value, biscuit::Vec scratch);
    void WriteVector(ir::Inst* inst, biscuit::Vec value);
    void SaveVectorsForCall(bool abi_call = true);
    void RestoreVectorsAfterCall();
    void EnableVectorFloat(bool enabled) { vector_float = enabled; }
    bool VectorFloatEnabled() const { return vector_float; }
    void EnterFloatMode();
    void LeaveFloatMode();
    void BeginBlock() { abi_calls = false; used_gprs = reserved_gprs = reserved_vectors = 0; float_mode_offset = 200; phi_bindings.clear(); phi_edge_uses.clear(); }
    void ConfigureABIRecovery(bool eager, biscuit::Label& recovery, biscuit::Label& save) {
        eager_abi_save = eager; abi_recovery = &recovery; abi_save = &save;
    }
    void MarkABICall();
    void EnableMemoryLease(bool enabled) { memory_lease = enabled; }
    void AcquireMemoryLease();
    void ReleaseMemoryLease();
    void PollMemoryLease();
    bool CallsABI() const { return abi_calls; }
    bool EagerABISave() const { return eager_abi_save; }
    u32 UsedGPRs() const { return used_gprs | (flags_enabled ? 1u << 8 : 0); }
    void FinalizeFrame() { float_mode_offset = abi_calls ? 200 : 104; }
    u32 FloatModeOffset() const { return float_mode_offset; }
    void Prepend(u32 start, std::span<const u8> code);
    void ConfigureUniformBindings(ir::Block* block);
    UniformBinding* FindUniformBinding(bool vector, u32 reg);
    UniformBinding* FindUniformRange(u32 offset, u32 size);
    const auto& UniformBindings() const { return uniform_bindings; }
    void LoadUniformBindings();
    void PublishUniformBindings();
    void LoadUniformInteger(biscuit::GPR result, u32 offset, u32 size);
    void StoreUniformInteger(biscuit::GPR source, u32 offset, u32 size);
    void ConfigurePhiRegisters(std::span<ir::Inst* const> phis);
    void AddPhiEdgeUse(ir::Value value, u32 position) { phi_edge_uses.emplace_back(value, position); }
    void AssignPhi(ir::Inst* inst, biscuit::GPR low, biscuit::GPR high);
    bool AssignPhiValue(ir::Inst* inst, ir::Value value);
    void PublishPhiHomes();
    biscuit::GPR SourceRegister(ir::Value value, biscuit::GPR scratch);
    void Data(biscuit::GPR result, const ir::DataClass& data);
    void Operand(biscuit::GPR result, const ir::Operand& operand);
    void Mask(biscuit::GPR value, u32 bits);
    void SignExtend(biscuit::GPR value, u32 bits);
    // Reserve/evict before any operands are read; Write publishes the result.
    biscuit::GPR ResultRegister(ir::Inst* inst);
    void Write(ir::Inst* inst, biscuit::GPR value, bool normalized = false);
    // Blocks that consume/publish flags reserve s11; other blocks retain all
    // nine SSA registers. ABI calls publish/reload the architectural word.
    void EnableFlagsCache(bool enabled) { flags_enabled = enabled; flags_dirty = false; }
    bool FlagsEnabled() const { return flags_enabled; }
    void MarkFlagsDirty() { ASSERT(flags_enabled); flags_dirty = true; }
    void PublishFlags();
    void ReloadFlags();
    void ConfigureLiveness(ir::Block* block);
    // Function CFGs share one SSA frame. A branch can name a common entry
    // rather than owning its own BindLabel, and exits have no fallthrough.
    void ConfigureFlow(std::unordered_map<ir::Inst*, ir::Inst*> targets,
                       std::unordered_set<ir::Inst*> stops,
                       std::unordered_map<ir::Inst*, ir::Location> locations = {},
                       std::unordered_set<ir::Inst*> synthetic_labels = {}) {
        flow_targets = std::move(targets); flow_stops = std::move(stops); flow_locations = std::move(locations);
        flow_synthetic_labels = std::move(synthetic_labels);
    }
    void RemapFlow(const std::unordered_map<ir::Inst*, ir::Inst*>& mapping) {
        std::unordered_map<ir::Inst*, ir::Inst*> targets;
        for (const auto& [branch, target] : flow_targets) targets.emplace(mapping.at(branch), mapping.at(target));
        std::unordered_set<ir::Inst*> stops;
        for (auto* inst : flow_stops) stops.insert(mapping.at(inst));
        std::unordered_map<ir::Inst*, ir::Location> locations;
        for (const auto& [inst, location] : flow_locations) locations.emplace(mapping.at(inst), location);
        std::unordered_set<ir::Inst*> synthetic_labels;
        for (auto* inst : flow_synthetic_labels) synthetic_labels.insert(mapping.at(inst));
        flow_targets = std::move(targets); flow_stops = std::move(stops); flow_locations = std::move(locations);
        flow_synthetic_labels = std::move(synthetic_labels);
    }
    bool IsSyntheticLabel(ir::Inst* inst) const { return flow_synthetic_labels.contains(inst); }
    const ir::Location* GuestBranchTarget(ir::Inst* inst) const {
        auto found = flow_locations.find(BranchTarget(inst));
        return found == flow_locations.end() ? nullptr : &found->second;
    }
    ir::Inst* BranchTarget(ir::Inst* branch) const {
        auto found = flow_targets.find(branch);
        return found == flow_targets.end() ? branch : found->second;
    }
    bool FallsThrough(ir::Inst* inst) const { return !flow_stops.contains(inst); }
    void ConfigureInitialization(ir::Block* block);
    void InitializeValues();
    void ReleaseDeadValues(u32 position);
    void FlushValues();
    void FlushValuesTo(const std::vector<ir::Inst*>& live);
    const auto& LiveAt(ir::Inst* leader) const { return live_entries.at(leader); }
    const auto& LiveForBranch(ir::Inst* branch) const { return branch_live_entries.at(branch); }
    void DiscardValues();
    [[nodiscard]] const ScalarValueStats& ValueStats() const { return value_stats; }
    void Jump(biscuit::Label& label);
    void BranchZero(biscuit::GPR value, biscuit::Label& label, bool zero = true);
    void Condition(biscuit::GPR result, ir::Cond condition);
    [[nodiscard]] u32 CurrentBufferSize();
    void Flush(const CodeBuffer& buffer);

private:
    biscuit::GPR ReserveRegister(biscuit::GPR excluded = biscuit::x0);
    void SpillVector(size_t index);
    biscuit::Assembler masm{};
    const bool cache_scalars;
    const HostFeatures features;
    std::array<ir::Inst*, scalar_registers.size()> cached_values{};
    std::array<u32, scalar_registers.size()> cached_parts{};
    std::array<ir::Inst*, 16> cached_vectors{};
    size_t next_vector{};
    u32 vector_bits{}, vector_lanes{};
    u32 vector_round_mode{~u32{0}};
    size_t next_register{};
    bool flags_enabled{};
    bool vector_float{};
    bool abi_calls{};
    bool memory_lease{};
    bool pair_coalescing{};
    u32 current_position{};
    bool eager_abi_save{};
    biscuit::Label* abi_recovery{};
    biscuit::Label* abi_save{};
    u32 used_gprs{}, float_mode_offset{200};
    bool flags_dirty{};
    std::unordered_map<ir::Inst*, u32> last_uses;
    std::unordered_map<ir::Inst*, ir::Inst*> flow_targets;
    std::unordered_map<ir::Inst*, ir::Location> flow_locations;
    std::unordered_set<ir::Inst*> flow_stops;
    std::unordered_set<ir::Inst*> flow_synthetic_labels;
    std::unordered_map<ir::Inst*, std::vector<ir::Inst*>> live_entries, branch_live_entries;
    std::vector<ir::Inst*> initial_values;
    std::span<const UniformMapDesc> uniform_descriptors;
    std::vector<UniformBinding> uniform_bindings;
    std::vector<PhiBinding> phi_bindings;
    std::vector<std::pair<ir::Value, u32>> phi_edge_uses;
    u32 reserved_gprs{}, reserved_vectors{};
    ScalarValueStats value_stats{};
};

}  // namespace swift::runtime::backend::riscv64
