#pragma once

#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <memory>
#include <optional>
#include <xbyak/xbyak.h>
#include "runtime/backend/context.h"
#include "runtime/ir/block.h"
#include "runtime/backend/function_plan.h"
#include "statistics.h"
#include "scalar_register_plan.h"
#include "cfg_register_plan.h"
#include "cfg_home_reload_preparation.h"
#include "sparse_frame_plan.h"
#include "guest_access.h"
#include "cfg_uniform_forward.h"
#include "cfg_state_promote.h"
#include "cfg_uniform_dead_store_pc.h"
#include "flags_profitability.h"
#include "carry_chain_plan.h"
#include "vector_register_plan.h"

namespace swift::runtime::backend::x64 {

struct HostFeatures {
    bool sse41{}, fma{}, aes{}, pclmul{}, sse42{}, sha{};
    static HostFeatures Detect();
};

// SingleInstruction requires a complete fresh frontend unit. It supplies a
// direct State ABI boundary, not Runtime admission or unpublished fault recovery.
enum class ExecutionMode : u8 { Ordinary, SingleInstruction };

struct FunctionPreparationOptions {
    bool uniform_forwarding{}, state_promotion{}, capture_forward_changes{};
    bool scalar_rematerialization{true};
    bool scalar_binary_immediates{true};
    bool loop_weighted_costs{true};
    // Selection runs once on the promoted, pre-forwarding CFG. Explicit
    // forced residency remains available for controlled comparisons.
    bool profitable_flags_residency{false};
    bool scalar_unary_phases{true};
    // Recolor the immutable selection graph rather than rebuilding it.
    bool flags_selection_recolor{true};
    // Emitter-only policy; allocation and home decisions are unchanged.
    bool scalar_unary_coalescing{true};
    // Lower fixed scalar shifts without loading or masking RCX.
    bool scalar_shift_immediates{true};
    // Emitter-only omission of proved redundant unary consumer masks.
    bool scalar_unary_mask_elision{true};
    // Ordinary x64 only: exact pure PC markers are not observers.
    bool uniform_dead_store_cross_pc{false};
    // Experimental two-producer native-CF transport. Full packed flags still
    // publish at the final NZCV marker; metadata snapshots remain unchanged.
    bool native_cf_chains{false};
    // Independent whole-CFG 128-bit vector cache. Canonical homes retained.
    bool vector_cfg_allocation{false};
    VectorRegisterOptions vector_register_options{};
    // Existing spilled-source hot reload localization, not assigned-range splitting.
    // First selective integration declines Uniform forwarding feedback.
    bool scalar_home_reload_split{false};
};
struct FunctionPreparationStats {
    u32 full_allocation_builds{};
    bool reused_final_allocation{};
    u32 flags_policy_allocation_builds{}, flags_policy_allocation_recolors{};
};
struct CfgUniformForwardChange {
    ir::Inst* load{};
    u32 position{}, uniform_offset{};
    ir::ValueType type{};
    ir::Value source;
    u32 byte{}, bytes{};
};

// SysV ABI; canonical SSA homes are stack-resident and uniforms are committed.
// CFG-assigned scalar values may omit their canonical stack home when every
// native read is audited; independent producer metadata still owns storage.
// R12 names State and R13 names the bottom of the complete recovery frame.
class Translator : public Xbyak::CodeGenerator {
public:
    explicit Translator(ir::Block* block, HostFeatures requested = HostFeatures::Detect(),
                        const Config* config = nullptr,
                        const backend::FunctionPlan* plan = nullptr,
                        bool scalar_cache = false,
                        ExecutionMode mode = ExecutionMode::Ordinary,
                        bool cfg_allocation = false, bool flags_residency = false,
                        bool scalar_rematerialization = true,
                        bool scalar_binary_immediates = true, bool loop_weighted_costs = true,
                        bool scalar_unary_phases = true, bool scalar_unary_coalescing = true,
                        bool scalar_shift_immediates = true,
                        bool scalar_unary_mask_elision = true, bool native_cf_chains = false,
                        bool vector_cfg_allocation = false,
                        VectorRegisterOptions vector_register_options = {});
    // Mutations and allocation adoption are one synchronous preparation.
    // No allocation token or callback escapes between the pass and constructor.
    static std::unique_ptr<Translator> PrepareFunction(backend::FunctionPlan& plan,
                        HostFeatures requested, const Config* config, bool scalar_cache,
                        bool cfg_allocation, bool flags_residency,
                        FunctionPreparationOptions options = {});
    void Translate();
    // The same ordinary CFG budget used by this emitter, including the
    // actual packed-flags use test before reserving RBX.
    static CfgRegisterOptions AllocationBudget(ir::Block* block, const backend::FunctionPlan* plan,
                                              bool flags_residency, bool loop_weighted_costs = true,
                                              bool scalar_unary_phases = true);
    u32 RecoveryOffset() const { return recovery_offset; }
    u32 FrameSize() const { return frame_size; }
    const EmitStatistics& Statistics() const noexcept { return statistics; }
    const ScalarRegisterPlan& ScalarPlan() const noexcept { return scalar_register_plan; }
    const CfgRegisterPlan& CfgPlan() const noexcept { return cfg_register_plan; }
    const SparseFramePlan& FramePlan() const noexcept { return sparse_frame_plan; }
    bool FlagsResident() const noexcept { return flags_resident; }
    const FlagsResidencyDecision& FlagsDecision() const noexcept { return flags_decision; }
    bool ScalarBinaryImmediatesEnabled() const noexcept { return scalar_binary_immediates; }
    bool LoopWeightedCostsEnabled() const noexcept { return cfg_register_plan.LoopWeightedCostsEnabled(); }
    bool ScalarUnaryPhasesEnabled() const noexcept { return cfg_register_plan.ScalarUnaryPhasesEnabled(); }
    bool ScalarUnaryCoalescingEnabled() const noexcept { return scalar_unary_coalescing; }
    bool ScalarShiftImmediatesEnabled() const noexcept { return scalar_shift_immediates; }
    bool ScalarUnaryMaskElisionEnabled() const noexcept { return scalar_unary_mask_elision; }
    bool NativeCfChainsEnabled() const noexcept { return native_cf_chains; }
    const CarryChainPlan& CarryPlan() const noexcept { return carry_chain_plan; }
    const VectorRegisterPlan& VectorPlan() const noexcept { return vector_register_plan; }
    bool VectorCfgAllocationEnabled() const noexcept { return vector_cfg_allocation; }
    u32 VectorCallFrameBytes() const noexcept { return vector_call_frame_bytes; }
    const FunctionPreparationStats& PreparationStats() const noexcept { return preparation_stats; }
    const CfgHomeReloadPreparationStats& HomeReloadSplitStats() const noexcept { return home_reload_split_stats; }
    const CfgStatePromoteStats& StatePromotionStats() const noexcept { return promotion_stats; }
    const CfgUniformDeadStorePcStats& UniformDeadStorePcStats() const noexcept { return dead_store_pc_stats; }
    const CfgUniformForwardStats& UniformForwardStats() const noexcept { return forward_stats; }
    const std::vector<CfgUniformForwardChange>& ForwardChanges() const noexcept { return forward_changes; }
    const std::vector<GuestAccessRange>& GuestAccesses() const noexcept { return guest_accesses; }
    GuestAccessCoverage GuestFaultCoverage() const noexcept { return guest_access_coverage; }

private:
    struct PreparedTag {};
    Translator(PreparedTag, ir::Block* block, HostFeatures requested, const Config* config,
               const backend::FunctionPlan* plan, bool scalar_cache, ExecutionMode mode,
               bool cfg_allocation, bool flags_residency,
               std::optional<CfgRegisterPlan> prepared_allocation, u32 preparation_builds,
               bool scalar_rematerialization, bool scalar_binary_immediates, bool loop_weighted_costs,
               bool scalar_unary_phases, bool scalar_unary_coalescing, bool scalar_shift_immediates,
               bool scalar_unary_mask_elision, bool native_cf_chains, bool vector_cfg_allocation,
               VectorRegisterOptions vector_register_options);
    static FlagsResidencyDecision SelectFlagsForPreparation(ir::Block* body,
               const backend::FunctionPlan& function, CfgRegisterOptions options,
               std::optional<CfgRegisterPlan>& allocation, bool retain_final,
               bool reuse_interference);
    static CfgUniformForwardStats ForwardForPreparation(backend::FunctionPlan& plan,
               u32 extent, const CfgRegisterPlan& graph, CfgUniformForwardOptions options,
               std::optional<CfgRegisterPlan>& allocation);
    friend struct TransferStatisticsProbe;
    friend struct CfgDiagnosticProbe;
    TransferRegion ClassifyTransfer(const Xbyak::Address& address, u32 bytes) const;
    void ObserveTransfer(const Xbyak::Operand& operand, bool store, bool vector, u32 bits);
    template<class Destination, class Source>
    void ObserveMove(const Destination& destination, const Source& source, bool vector, u32 bits = 0) {
        u32 destination_bits = bits, source_bits = bits;
        if constexpr (std::is_base_of_v<Xbyak::Operand, Destination>)
            if (!destination_bits) destination_bits = destination.getBit();
        if constexpr (std::is_base_of_v<Xbyak::Operand, Source>)
            if (!source_bits) source_bits = source.getBit();
        if (!destination_bits) destination_bits = source_bits;
        if (!source_bits) source_bits = destination_bits;
        if constexpr (std::is_base_of_v<Xbyak::Operand, Destination>)
            ObserveTransfer(destination, true, vector, destination_bits);
        if constexpr (std::is_base_of_v<Xbyak::Operand, Source>)
            ObserveTransfer(source, false, vector, source_bits);
    }
    // Observe only after Xbyak successfully emits. Qualified base calls keep
    // instruction selection/encoding unchanged and avoid observer recursion.
#define X64_OBSERVED_MOVE(name, vector, bits) \
    template<class Destination, class Source> \
    void name(const Destination& destination, const Source& source) { \
        Xbyak::CodeGenerator::name(destination, source); \
        ObserveMove(destination, source, vector, bits); \
    }
    X64_OBSERVED_MOVE(mov, false, 0)
    X64_OBSERVED_MOVE(movzx, false, 0)
    X64_OBSERVED_MOVE(movsx, false, 0)
    X64_OBSERVED_MOVE(movsxd, false, 0)
    X64_OBSERVED_MOVE(movd, true, 32)
    X64_OBSERVED_MOVE(movq, true, 64)
    X64_OBSERVED_MOVE(movdqu, true, 128)
    X64_OBSERVED_MOVE(movdqa, true, 128)
    X64_OBSERVED_MOVE(movss, true, 32)
    X64_OBSERVED_MOVE(movsd, true, 64)
#undef X64_OBSERVED_MOVE
    void pop(const Xbyak::Operand& operand) {
        Xbyak::CodeGenerator::pop(operand);
        ObserveTransfer(operand, true, false, 64);
        if (operand.isMEM() && ClassifyTransfer(static_cast<const Xbyak::Address&>(operand), 8) == TransferRegion::ProducerMetadata)
            ++statistics.flags_snapshot_sites;
    }
    void call(const Xbyak::Operand& operand, Xbyak::CodeGenerator::LabelType type = T_AUTO) {
        FlushGuestFlags();
        SaveVectorCallRegisters();
        Xbyak::CodeGenerator::call(operand, type);
        ++statistics.helper_call_sites;
        RestoreVectorCallRegisters();
        if (flags_residency_active) ReloadGuestFlags();
    }
    void SaveVectorCallRegisters();
    void RestoreVectorCallRegisters();
    struct PhiMove { ir::Inst* destination; ir::DataClass source; };
    void CountPhiCopies(const std::vector<PhiMove>& moves);
    void ConfigurePhis();
    void EmitPhiMoves(const std::vector<PhiMove>& moves);
    ir::Inst* BranchTarget(ir::Inst* branch) const;
    bool FallsThrough(ir::Inst* inst) const;
    void CommitGuestEdge(ir::Inst* branch);
    void Emit(ir::Inst* inst);
#define INST(name, ...) void Emit##name(ir::Inst* inst);
#include "runtime/ir/ir.inc"
#undef INST
    [[noreturn]] void Unsupported(ir::Inst* inst);

    // Shared operand access and lane movement do not dispatch IR opcodes.
    static u64 Immediate(ir::Inst* inst, u32 index) { return inst->GetArg<ir::Imm>(index).Get(); }
    static u32 VectorImmediate(ir::Inst* inst, u32 index) { return static_cast<u32>(Immediate(inst, index)); }
    static ir::Value Source(ir::Inst* inst, u32 index) { return inst->GetArg<ir::Value>(index); }
    static u32 ResultBits(ir::Inst* inst) {
        return inst->ReturnType() == ir::ValueType::VOID ? 0 : ir::GetValueSizeByte(inst->ReturnType()) * 8;
    }
    void ReadArgument(const Xbyak::Reg64& reg, ir::Inst* inst, u32 index) { Read(reg, Source(inst, index)); }
    Xbyak::Reg64 ReadUnaryInput(ir::Inst* inst);
    void ReadVectorOperands(ir::Inst* inst);
    void LoadLane(const Xbyak::Reg64& reg, u32 offset, u32 bits);
    void StoreLane(const Xbyak::Reg64& reg, u32 offset, u32 bits);
    static void ValidateLaneWidth(u32 bits);

    // Closely related operations share only their semantic lowering.
    void EmitMemoryCopyOrdered(ir::Inst* inst, ir::OpCode op);
    void EmitMemoryStore(ir::Inst* inst, ir::OpCode op);
    void EmitAtomic(ir::Inst* inst, ir::OpCode op);
    void EmitScalarBinary(ir::Inst* inst, ir::OpCode op);
    void EmitDivide(ir::Inst* inst, ir::OpCode op);
    void EmitScalarShift(ir::Inst* inst, ir::OpCode op);
    void EmitCountZeros(ir::Inst* inst, ir::OpCode op);
    void EmitBitUpdate(ir::Inst* inst, ir::OpCode op);
    void EmitZeroTest(ir::Inst* inst, ir::OpCode op);
    void EmitFlagTest(ir::Inst* inst, ir::OpCode op);
    void EmitSetFlag(ir::Inst* inst, ir::OpCode op);
    void EmitSelectValue(ir::Inst* inst, ir::OpCode op);
    void EmitConditionalGoto(ir::Inst* inst, ir::OpCode op);
    void EmitSha256Message(ir::Inst* inst, ir::OpCode op);
    void EmitAesRound(ir::Inst* inst, ir::OpCode op);
    void EmitVectorAddSub(ir::Inst* inst, ir::OpCode op);
    void EmitVectorCompare(ir::Inst* inst, bool equal);
    void EmitVectorMultiply(ir::Inst* inst, u32 bits);
    void EmitVectorMinMax(ir::Inst* inst, bool minimum);
    void EmitVectorSaturatingAddSub(ir::Inst* inst, ir::OpCode op);
    void EmitVectorShift(ir::Inst* inst, ir::OpCode op);
    void EmitVectorTableLookup(ir::Inst* inst, ir::OpCode op);
    void EmitVectorFloatBinary(ir::Inst* inst, ir::OpCode op);
    void Terminal(const ir::Terminal& terminal);
    void Read(const Xbyak::Reg64& reg, ir::Value value);
    std::optional<unsigned> AllocatedScalarRegister(ir::Inst* producer) const noexcept;
    bool RequiresScalarHome(ir::Inst* producer) const noexcept;
    void ReadVector(const Xbyak::Xmm& reg, ir::Value value);
    void Data(const Xbyak::Reg64& reg, const ir::DataClass& data);
    void Operand(const Xbyak::Reg64& reg, const ir::Operand& operand);
    void Write(ir::Inst* inst, const Xbyak::Reg64& reg);
    void WriteVector(ir::Inst* inst, const Xbyak::Xmm& reg);
    void PublishPseudoResults(ir::Inst* inst, ir::OpCode op, u32 metadata_offset);
    void Mask(const Xbyak::Reg64& reg, u32 bits);
    void SignExtend(const Xbyak::Reg64& reg, u32 bits);
    void Call(u64 target);
    void Condition(ir::Cond condition);
    void SaveProducerFlags(ir::Inst* producer, ir::Flags requested);
    void ReadGuestFlags(const Xbyak::Reg64& destination);
    void WriteGuestFlags(const Xbyak::Reg64& source);
    void FlushGuestFlags();
    void ReloadGuestFlags();
    void PublishLocation();
    void MemoryAddress(ir::Inst* inst, u32 size);
    void MemoryAddressValue(ir::Inst* inst, ir::Value value, u32 size);
    void EndMemoryAccess();
    void RecordGuestAccess(size_t begin, u32 size, GuestAccessKind kind);
    void MarkUntrackedGuestAccess() noexcept;
    void Return(HaltReason reason);
    void PollInterrupt();
    void CheckHalt(bool poll_signal = true);
    void ConfigureUniformBindings(const Config* config);
    const UniformMapDesc& HostBinding(ir::Inst* inst) const;
    void EmitHostUniform(ir::Inst* inst, bool get);
    u32 Home(ir::Inst* inst) const;
    ir::Block* block;
    const backend::FunctionPlan* function_plan{};
    const ExecutionMode execution_mode;
    std::vector<GuestAccessRange> guest_accesses;
    GuestAccessCoverage guest_access_coverage;
    std::vector<PhiMove> entry_phis;
    std::unordered_map<ir::Inst*, std::vector<PhiMove>> taken_phis, fallthrough_phis;
    u32 phi_scratch{}, max_phi_moves{};
    u32 vector_call_frame_begin{}, vector_call_frame_bytes{};
    HostFeatures features;
    std::unordered_map<ir::Inst*, u32> homes;
    std::unordered_map<u32, UniformMapDesc> uniform_bindings;
    std::unordered_map<u16, u32> locals;
    std::unordered_map<ir::Inst*, Xbyak::Label> labels;
    std::unordered_set<ir::Inst*> flag_producers;
    std::unordered_set<ir::Inst*> vector_forwarding;
    ir::Inst* cached_vector{};
    Xbyak::Label epilogue, recovery, retirement;
    u32 frame_size{}, recovery_offset{};
    // Home/local slots are aligned 32 bytes. CFG sparse homes are distinct;
    // the legacy flat-body allocator may reuse complete slots.
    u32 home_bytes{}, local_begin{}, local_bytes{};
    u64 uniform_bytes{};
    bool uniform_extent_known{};
    u64 guest_pc{};
    bool location_dirty{}, location_from_metadata{};
    bool guest_instructions{};
    const bool flags_resident;
    const bool scalar_binary_immediates;
    const bool loop_weighted_costs;
    const bool scalar_unary_phases;
    const bool scalar_unary_coalescing;
    const bool scalar_shift_immediates;
    const bool scalar_unary_mask_elision;
    const bool native_cf_chains;
    const bool vector_cfg_allocation;
    bool flags_residency_active{};
    bool vector_allocation_active{};
    EmitStatistics statistics;
    ScalarRegisterPlan scalar_register_plan;
    CfgRegisterPlan cfg_register_plan;
    SparseFramePlan sparse_frame_plan;
    CarryChainPlan carry_chain_plan;
    VectorRegisterPlan vector_register_plan;
    FunctionPreparationStats preparation_stats;
    CfgHomeReloadPreparationStats home_reload_split_stats;
    FlagsResidencyDecision flags_decision;
    CfgStatePromoteStats promotion_stats;
    CfgUniformDeadStorePcStats dead_store_pc_stats;
    CfgUniformForwardStats forward_stats;
    std::vector<CfgUniformForwardChange> forward_changes;
    ir::Inst* emitting_inst{};
};

}  // namespace swift::runtime::backend::x64
