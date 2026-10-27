#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>
#include "base/common_funcs.h"
#include "jit_context.h"
#include "runtime/backend/arm64/compile/edges.h"
#include "runtime/backend/code_cache.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/atomic_rmw.h"
#include "runtime/ir/block.h"
#include "terminal_location_publication.h"

namespace swift::runtime::compile {
struct PseudoFlagsDemand;
}
namespace swift::runtime::backend::arm64 {

// x87 TOP has no dedicated host register. Every inline X87Op extracts it into
// one of that opcode's ordinary allocator-protected scratch GPRs: one UBFX
// when the emitter already holds FSW, or LDRH+UBFX when it does not. Stack
// effects are merged into the emitter's existing FSW update and written back
// architecturally before the instruction ends, so no TOP value is live across
// an IR instruction, block edge, or helper call.
//
// This is a structural invariant, not merely the default configuration. The
// A retired TOP cache once named a fixed GPR; its first implementation
// silently reused x20 after the trampoline had pinned guest RBX there, because
// a second Mark() was a no-op. TrampolinesArm64::Build now rejects every static
// uniform mapping that overlaps the runtime ABI or another descriptor, while
// the x87 emitter names no fixed TOP register at all. In particular x22 stays
// an ordinary allocator/static-mapping candidate for the full-pin work.

namespace HostFlagsBit {
    constexpr auto N = 31;
    constexpr auto Z = 30;
    constexpr auto C = 29;
    constexpr auto V = 28;
    constexpr auto Parity = 27;
    constexpr auto AuxiliaryCarry = 26;
    constexpr auto ParityByte = 0;
    constexpr auto AFLeft = 11;
    constexpr auto AFRight = 15;
    constexpr u64 ParityByteMask = u64(0xF) << ParityByte;
}

// Exact platform/shape gate for folding the guest-memory host bias into a
// SIMD&FP register-offset load/store.  Exposed for the focused proof-matrix
// test; the emitter calls this same predicate.
[[nodiscard]] bool HostBaseFoldEligible(bool enabled,
                                        bool use_memory_base,
                                        u64 guest_addr_mask,
                                        ir::ValueType type,
                                        bool structured_guest_ea,
                                        bool guest_add_form,
                                        bool tso_or_atomic);

enum class HostFlags : u64 {
    N = 1u << HostFlagsBit::N,
    Z = 1u << HostFlagsBit::Z,
    C = 1u << HostFlagsBit::C,
    V = 1u << HostFlagsBit::V,
    NZCV = N | Z | C | V,
    NZ = N | Z,
};

DECLARE_ENUM_FLAG_OPERATORS(HostFlags)

enum class DensityCategory : size_t {
    Flags,
    Uniform,
    MoveWidth,
    NaN,
    Boundary,
    Work,
    Count,
};

class JitTranslator {
public:
    struct BackedgeBlockMetadata {
        u64 guest_start{};
        u32 host_begin{};
        u32 host_end{};
        u32 recovery_offset{};
    };

    struct FaultMetadata {
        u64 guest_start{};
        u32 host_begin{};
        u32 host_end{};
        u32 recovery_offset{};
        u32 recovery_reg{UINT32_MAX};
        FaultRecoveryKind recovery_kind{FaultRecoveryKind::GuestFault};
    };

    explicit JitTranslator(JitContext& ctx);

    bool Translate(const ir::Block* block);

    bool Translate(const ir::HIRFunction* function);

    [[nodiscard]] const std::vector<BackedgeBlockMetadata>&
    GetBackedgeBlockMetadata() const {
        return backedge_block_metadata;
    }

    [[nodiscard]] const std::vector<FaultMetadata>& GetFaultMetadata() const {
        return memory_state.fault_metadata;
    }

    Operand EmitOperand(ir::Operand &ir_op);

    // atomic: the operand feeds an instruction without register-offset
    // addressing forms (Ldar/Stlr), so under memory_base the pt bias is
    // folded into a scratch register instead of [base + pt].
    // structured_guest_ea is set only by ordinary V128 Load/StoreMemory; TSO
    // and atomic callers retain the established address materialization.
    MemOperand EmitMemOperand(ir::Operand& ir_op,
                              ir::ValueType type,
                              bool pair = false,
                              bool atomic = false,
                              bool allow_writeback = true,
                              bool structured_guest_ea = false,
                              const ir::Inst* memory_inst = nullptr);

    // Residue (mod 8) of a value whose alignment is derivable from a
    // pinned-home read with a recorded residue, an immediate constant, or
    // size-multiple Add/Sub steps from either; nullopt when unknown.
    [[nodiscard]] std::optional<u32> AlignmentResidueMod8(ir::Value value) const;
    // Per-instruction maintenance of home_alignment_mod8: guest-register
    // homes are rewritten both by SetHostGPR publications and by ordinary
    // producers the allocator placed on the home, so both must refresh or
    // drop the recorded residue before the instruction emits.
    void TrackHomeAlignment(const ir::Inst* inst);

#define INST(name, ...) void Emit##name(const ir::Inst* inst);
#include "runtime/ir/ir.inc"
#undef INST

private:
    void EmitIntegerShift(const ir::Inst* inst);
    void EmitIntegerPredicate(const ir::Inst* inst);
    void CompareIntegerZero(ir::Value value);
    [[nodiscard]] Register ConditionRegister(ir::Value value);
    friend class FunctionCodeObjectEmitter;
    bool EmitPreparedFunction(const ir::HIRFunction* function);
    friend class JitTranslatorTestSupport;
    void EmitExecutionTrace(u64 guest_rip);
    void EmitByteMovMask(const VRegister& source,
                         const WRegister& result,
                         const VRegister& work,
                         const VRegister& packed);
    [[nodiscard]] bool CanUseZeroStoreRegister(ir::Value value);
    [[nodiscard]] bool CanConsumeForwardedWidthSpill(const ir::Inst* inst);
    [[nodiscard]] std::optional<u32> ForwardedMemorySpillInput(const ir::Inst* inst);
    [[nodiscard]] bool CanFusePinnedZeroExtendPublication(const ir::Inst* inst) const;
    [[nodiscard]] bool CanAdoptPendingSpillWrite(const ir::Inst* inst,
                                                 bool reads_forwarded_memory_input);
    [[nodiscard]] const MemoryTransferAction* PreparedMemoryUpdate(
            const ir::Inst* update, MemoryTransferAction::Kind kind) const;
    [[nodiscard]] bool EmitPshufdDirect(const VRegister& result,
                                        const VRegister& source,
                                        u32 control);

    [[nodiscard]] ir::Value ResolveNarrowFlagsInput(ir::Value value, const ir::Inst* consumer);
    const IntegerFlagsAction* NarrowCarryFor(const ir::Inst* inst) const {
        return context.GetActions().NarrowCarry(inst->Id());
    }
    const IntegerFlagsAction* NarrowCompareFor(const ir::Inst* inst) const {
        return context.GetActions().NarrowCompare(inst->Id());
    }
    struct SpilledMemoryOperand {
        u16 consumer{UINT16_MAX};
        XRegister base{};
        s64 offset{};
    };
    [[nodiscard]] std::optional<MemOperand> TryEmitSpilledMemoryOperand(
            const ir::Inst* address,
            ir::ValueType type,
            bool pair,
            bool atomic,
            const ir::Inst* memory_inst);

    enum class BoundarySubsequence : size_t {
        Prologue,
        TerminalMain,
        LinkTail,
        ColdTail,
        Count,
    };

    void ResetBoundaryDensity();
    void RecordBoundaryRange(BoundarySubsequence category, u32 begin, u32 end);
    void PrintBoundaryDensity(u64 guest_pc, u32 expected_boundary_bytes);

    struct BlockTranslateState {
        const ir::LoopHoistMetadata* loop_hoist{};
        const EdgeBlockAction* edges{};
        u32 loop_hoist_prefix_begin{};
        u32 loop_hoist_prefix_ops{};
        bool split_flags_entry{};
    };

    [[nodiscard]] BlockTranslateState PrepareBlockState(const ir::Block* block);

    struct UniformDensityCounts {
        u32 gpr_uniform_accesses{};
        u32 xmm_uniform_accesses{};
    };

    [[nodiscard]] UniformDensityCounts CollectUniformDensity(const ir::Block* block, bool density);

    void TranslateBlockInstructions(const ir::Block* block,
                                    const EdgeBlockAction& edges,
                                    const ir::LoopHoistMetadata& loop_hoist,
                                    bool density,
                                    bool gap_audit,
                                    std::span<u32> density_ops,
                                    std::span<u32> density_bytes,
                                    u32& density_scalar_fp_ops,
                                    u32& loop_hoist_prefix_begin,
                                    u32& loop_hoist_prefix_ops);

    void EmitBlockTerminal(const ir::Block* block, bool density, std::span<u32> density_bytes);

    struct BlockColdPathRecipe;
    [[nodiscard]] BlockColdPathRecipe CaptureBlockColdPathRecipe(
            const ir::Block* block,
            bool density,
            std::span<const u32> density_ops,
            std::span<const u32> density_bytes,
            u32 density_scalar_fp_ops,
            const ir::LoopHoistMetadata& loop_hoist,
            u32 loop_hoist_prefix_ops);
    void EmitBlockColdPathRecipe(BlockColdPathRecipe recipe);

    void PrintBlockDensity(const ir::Block* block,
                           bool density,
                           std::span<const u32> density_ops,
                           std::span<const u32> density_bytes,
                           u32 density_scalar_fp_ops,
                           const ir::LoopHoistMetadata& loop_hoist,
                           u32 loop_hoist_prefix_ops);

    struct BackedgeFlagsRecipe {
        bool optimized{true};
        const BackedgeAction& plan;
        const PFAFAction* pfaf{};
        const HostFlags requested;
        BackedgeFlagsRecipe(const BackedgeAction& plan, const PFAFAction* pfaf, HostFlags mask)
                : plan(plan), pfaf(pfaf), requested(mask) {}
        std::unique_ptr<Label> local_entry{std::make_unique<Label>()};
        std::unique_ptr<Label> external_entry{std::make_unique<Label>()};
        std::unique_ptr<Label> cold_exit{std::make_unique<Label>()};
        std::unique_ptr<Label> fault_recovery{std::make_unique<Label>()};
        bool cold_referenced{};
    };

    [[nodiscard]] std::unique_ptr<BackedgeFlagsRecipe>
    ImportBackedgeFlagsRecipe();
    [[nodiscard]] bool CanImportBackedgeFlagsRecipe() const;
    [[nodiscard]] bool RequiresCanonicalBackedgeEntry(ir::Location target) const;
    [[nodiscard]] bool CanonicalCarryEnabled() const;
    [[nodiscard]] bool TargetKillsIncomingFlags(ir::Location target) const;
    [[nodiscard]] bool RegionBranchPFAFActive(const ir::Inst* producer) const;
    [[nodiscard]] bool EmitBackedgeFlagsTerminal(const ir::Terminal& terminal);
    void EmitBackedgeMaterialize(const BackedgeFlagsRecipe& recipe);
    void EmitRegionBranchPFAF(const BackedgeFlagsRecipe& recipe);
    void EmitBackedgeColdPaths();
    [[nodiscard]] bool RetainsPendingHostNZCV(const ir::Inst& inst) const;
    [[nodiscard]] static bool PreservesHostNZCV(ir::OpCode op);
    [[nodiscard]] EdgeFlagsState PendingEdgeFlagsState(HostFlags valid,
                                                       EdgeFlagsProducer producer) const;
    void ObserveEdgeCarryPolarity(ir::Uniform uniform, ir::Value value);
    [[nodiscard]] EdgeFlagsTargetContract AnalyzeEdgeFlagsTarget(
            ir::Location target) const;
    [[nodiscard]] bool TargetAcceptsEdgeFlags(
            ir::Location target,
            const EdgeFlagsState& incoming) const;
    VRegister GetVecScalarOperand(ir::Value value, u32 lane_bits);

    void LoadUnalignedAtomicLockAddress(const Register& lock);
    void AcquireUnalignedAtomicLock(const Register& lock,
                                    const Register& scratch);
    void ReleaseUnalignedAtomicLock(const Register& lock);
    void EmitBasicAtomicLoad(ir::ValueType type,
                             const Register& result,
                             const Register& address);
    void EmitBasicAtomicStore(ir::ValueType type,
                              const Register& value,
                              const Register& address);
    void EmitAtomicRMWValue(ir::AtomicRMWOp op,
                            ir::ValueType type,
                            const Register& output,
                            const Register& old,
                            ir::Value operand,
                            ir::Value carry);

    [[nodiscard]] Label* GetUnalignedAtomicFallback(const ir::Inst* inst);
    void EmitUnalignedAtomicFallbacks();
    void EmitUnalignedAtomicFallback(const UnalignedAtomicFallbackKey& key);

    using PseudoFlags = compile::PseudoFlagsDemand;

    void Translate(const ir::Inst* inst);
    void PublishPendingStaticLocation();

    // Terminals
    void EmitTerminal(const ir::Terminal &terminal,
                      LinkSiteKind direct_link_kind = LinkSiteKind::Unconditional,
                      DirectLinkFlagsBypass flags_bypass = {},
                      bool canonical_transfer = false);
    void EmitDispatcherTerminal(LinkSiteKind direct_link_kind,
                                DirectLinkFlagsBypass flags_bypass,
                                bool external_edge,
                                bool call_exit);
    [[nodiscard]] bool CanBypassTerminalFlagsMerge(
            const ir::Terminal& terminal) const;
    bool ImportUnitEdges(const ir::Block* block, const ir::HIRFunction* function);
    void ReportRegionEdges();
    const EdgeActions& Edges() const {
        static const EdgeActions empty;
        return edge_actions ? *edge_actions : empty;
    }
    bool RegionEdgesActive() const { return region_function_mode && Edges().active; }
    bool IsCanonicalTerminalEntry(u64 location) const {
        return canonical_entries_active && Edges().IsCanonicalEntry(location);
    }
    [[nodiscard]] bool IsRegionInternalEdge(ir::Location target) const;
    [[nodiscard]] bool IsRegionCycleEdge(ir::Location target) const;
    [[nodiscard]] bool HasRegionCycleEdgeFromCurrent() const;
    [[nodiscard]] bool IsDirectCycleCutEdge(ir::Location source,
                                            ir::Location target) const;
    [[nodiscard]] bool IsDirectCycleCutEdge(ir::Location target) const;
    [[nodiscard]] u32 CountCycleExitCandidates() const;
    [[nodiscard]] Label* GetDirectCycleExit(ir::Location target);
    [[nodiscard]] Label* GetExternalCycleExit(ir::Location target);
    [[nodiscard]] bool CanUseRegionSuccessorLayout(ir::Location target) const;
    void EmitRegionEdge(ir::Location target,
                        bool fallthrough = false,
                        bool record_edge_counters = true,
                        bool commit_flags = true,
                        bool poll_request = true);
    enum class RegionFlagsJoinMode : u8 {
        Canonical,
        Deferred,
        Split,
        CanonicalTail,
    };
    struct RegionFlagsJoinRecipe {
        RegionFlagsJoinMode mode{RegionFlagsJoinMode::Canonical};
        EdgeFlagsState incoming{};
        ir::Location compatible_target{};
        ir::Location canonical_target{};
        bool compatible_on_true{};
        bool compatible_fallthrough{};
        bool canonical_fallthrough{};
        bool canonical_merge_token{};
    };
    [[nodiscard]] RegionFlagsJoinRecipe RecipeRegionFlagsJoin(
            ir::Location then_target,
            ir::Location else_target,
            bool allow_fallthrough) const;
    [[nodiscard]] bool EmitRegionFlagsJoin(
            const RegionFlagsJoinRecipe& recipe,
            const std::function<void(vixl::aarch64::Label*, bool)>& branch);
    [[nodiscard]] bool RegionSuccessorAcceptsEdgeFlags(
            ir::Location target,
            const EdgeFlagsState& incoming) const;
    [[nodiscard]] bool RegionSuccessorOverwritesFlagsToken(
            ir::Location target) const;
    [[nodiscard]] Label* GetRegionFlagsCanonicalStub(
            const RegionFlagsJoinRecipe& recipe);
    void EmitRegionFlagsCanonicalStubs();
    [[nodiscard]] bool EmitRegionIf(const ir::terminal::If& terminal,
                                    bool allow_fallthrough,
                                    bool canonical_transfer = false);
    [[nodiscard]] bool BlockIsFlagsTransparent(const ir::Block* block) const;
    [[nodiscard]] bool EmitRegionCondition(const ir::terminal::Condition& terminal,
                                           bool allow_fallthrough,
                                           bool canonical_transfer = false);
    [[nodiscard]] bool IsCanonicalTerminalEntry() const;
    [[nodiscard]] bool EmitCanonicalTerminalEdge(ir::Location target);
    [[nodiscard]] bool HasSelfEdge(const ir::Terminal& terminal) const;
    [[nodiscard]] bool IsSelfEdge(ir::Location target) const;
    [[nodiscard]] Label* LocalBranchTarget(ir::Location target) const;
    void EmitBackedgeExitStub();
    void EmitDirectCycleExitStubs();
    void EmitCycleExitReasonTail(Label* reason, bool allow_outline = true);
    [[nodiscard]] Label* GetTerminalRequestExit();
    void EmitTerminalRequestExitStub();
    // The edge body has executed: even a shared cycle-exit stub must resume
    // at this edge's target, never at the source block.
    void RecordExitPollFault(std::optional<JitContext::FaultRange> fault,
                             Label* recovery,
                             ir::Location resume_location);
    void ResolveExitPollFaults(Label* recovery, std::optional<u32> frame_recovery = std::nullopt);
    void RecordDeferredFault(JitContext::FaultRange fault,
                             Label* recovery,
                             FaultRecoveryKind recovery_kind);
    void ResolveDeferredFaults(Label* recovery);

    // Labels used by Goto / NotGoto / BindLabel
    Label* GetLocalLabel(const ir::Inst* inst);

    // Flags
    void SaveHostFlags(HostFlags host, ir::Flags guest);

    static HostFlags GuestNZCVToHost(ir::Flags guest);

    static Condition MapCond(ir::Cond cond);

    // Records a LocalCondSet/FCmpCondSet whose sole use is a branch or select.
    // The marker itself then emits no CSET; its consumer reads the producer's
    // still-live host NZCV directly.
    bool RecordLocalCondition(const ir::Inst* inst, ir::Cond cond);
    [[nodiscard]] std::optional<Condition> LocalConditionFor(ir::Value value) const;
    [[nodiscard]] bool LaterNeedsHostPstate(const ir::Inst* from) const;
    bool PstateAvailable(const ir::Inst* test, ir::Flags flag) const {
        const auto* row = test ? context.GetActions().Consumer(test->Id()) : nullptr;
        return row && True(row->Pstate() & flag);
    }
    bool FoldCcFromCarryTest(const ir::Inst* test_flags);
    [[nodiscard]] bool IsCompactFCmp(ir::Value value) const;
    [[nodiscard]] bool CanUseCompactFCmpCarrier(const ir::Inst* fcmp) const;
    [[nodiscard]] bool RawFCmpCondition(const ir::Inst* fcmp,
                                        const ir::Inst* consumer = nullptr) const;

    void ImportIntegerBranchCondition();
    const IntegerBranchAction* DeadEdgePlan() const { return integer_branch; }
    const IntegerBranchAction* DeadNarrowPlan() const {
        const auto* row = DeadEdgePlan();
        return row && row->IsNarrow() ? row : nullptr;
    }
    [[nodiscard]] bool IsDeadEdgeIntegerBranchProducer(const ir::Inst* inst) const;
    [[nodiscard]] std::optional<ir::Cond> DeadEdgeIntegerBranchCondition(
            const ir::Inst* inst) const;
    bool EmitDeadEdgeZeroBranch(ir::Value condition, Label* label, bool on_true);

    // Merge pending guest flags kept in host NZCV into the flags register.
    // B0 tags the existing sequence only; the tags never affect emission.
    void MergeNZCV();
    DirectLinkFlagsBypass MergeNZCV(FlagsRegsAuditMergeCause cause,
                                    FlagsRegsAuditEdgeKind edge,
                                    bool outline_direct_link = false);
    DirectLinkFlagsBypass EmitDeferredNZCVMerge(
            const XRegister& scratch,
            const XRegister& token);
    DirectLinkFlagsBypass EmitOutlinedNZCVMerge();
    DirectLinkFlagsBypass EmitOutlinedNZCVMergeResume(bool token);
    void EmitDeferredNZCVMergeStubs();
    [[nodiscard]] bool
    TryEmitReturnFlagsBypass(const DirectLinkFlagsBypass& flags_bypass);
    [[nodiscard]] std::optional<u64>
    PendingNZCVMergeMask(FlagsRegsAuditMergeCause cause) const;
    [[nodiscard]] bool
    CanDeferFullNZCVMerge(FlagsRegsAuditMergeCause cause) const;
    void EmitNZCVMerge(u64 requested, const Register& scratch,
                       const Register* mask_scratch = nullptr);
    void PublishFlagsToken();

    // Restore host NZCV from the flags register (uses the emission's shared scratch).
    void LoadNZCVFromFlags();
    void PrepareFlagRead(ir::Flags requested);
    void PrepareCarryInput();
    [[nodiscard]] bool PendingNZCVCovers(ir::Flags flags) const;
    [[nodiscard]] bool TryEmitCondSetFromFlags(const ir::Inst* inst, ir::Cond cond);
    void EmitZeroTestPreservingPstate(const ir::Inst* inst, bool nonzero);

    // Merge host N/Z into the flags register and clear stale C/V (x86 logical ops)
    void MergeLogicalFlagsNZ(ir::Flags requested);

    // Compute N/Z from a result value and merge them (for ops without a flag setting form)
    void SaveLogicalResultFlags(Register &result, ir::ValueType type, const PseudoFlags &pseudo);
    void RecordLogicalResultFlags(Register& result, const PseudoFlags& pseudo);
    void EmitLogicalNZFlags(const Register& value, ir::ValueType type);

    // Materialize an IR operand into a scratch register
    Register MaterializeOperand(const Operand &operand, ir::ValueType type);

    // Guest address virtualization (Config::memory_base): the pt register
    // holds the guest->host bias for the whole guest run. These wrap a guest
    // base register into [base + pt] (+ optional immediate). atomic=true
    // folds the bias into a scratch register (for instructions without
    // register-offset forms). Only called when use_memory_base is set;
    // direct mode never pays for this.
    MemOperand BiasMem(const Register &base, bool atomic = false);
    MemOperand BiasMem(const Register &base, s64 imm, bool atomic = false);

    // Bounded guest window (Config::guest_addr_mask): materializes the host
    // address of a guest address into `dst` as (guest & mask) + pt. Used by
    // the forms that need a single base register (exclusives, host calls).
    // With a 32-bit window this is one instruction — the same Add the
    // unbounded path emitted — because UXTW does the truncation for free.
    void EmitGuestToHost(const Register &dst, const Register &guest_addr);
    [[nodiscard]] bool CanUseLSE() const;
    void EmitLSECompareAndSwap(ir::ValueType type,
                               const Register& result,
                               const Register& expected,
                               const Register& desired,
                               const Register& address);
    void EmitLSEExchange(ir::ValueType type,
                         const Register& result,
                         const Register& desired,
                         const Register& address);
    void EmitLSEFetchAdd(ir::ValueType type,
                         const Register& result,
                         const Register& addend,
                         const Register& address);

    // Host C-ABI call helper (saves/restores caller-saved allocated GPRs)
    void EmitHostCall(const ir::Lambda& lambda,
                      const std::vector<ir::DataClass>& args,
                      bool has_result,
                      const Register& result,
                      std::optional<Register> secondary_result = std::nullopt);
    void EmitSse42StrVectorCall(const ir::Inst* inst,
                                const VRegister& left,
                                const VRegister& right,
                                const WRegister& result,
                                VAddr target,
                                u8 imm);
    void BindHostCallThunks();
    [[nodiscard]] Label* FindHostCallThunk(u64 target) const;
    void MaterializeHostCallTarget(u64 target);
    bool TryEmitSharedHostCall(const ir::Lambda& lambda);
    bool TryEmitSharedHostBranch(const ir::Lambda& lambda);
    void EmitHostCallThunks();
    std::optional<u16> PairSecondaryResult(const ir::Inst* inst, ir::OpCode secondary) const;
    void EmitPreserveAllPairCall(
            const ir::Inst* inst,
            VAddr target,
            const std::array<ir::DataClass, 3>& args,
            std::optional<u16> secondary,
            ir::HostRegisterEffect host_registers = ir::HostRegisterEffect::MayTouchSIMD);
    void SpillStaticFPRUniforms();
    void RestoreStaticFPRUniforms();

    void ClearFlags(ir::Flags flags);

    void SaveParity(Register &value);

    void SaveNZ(Register &value, ir::ValueType type);

    void SaveCV(Register &value, ir::ValueType type);

    void SaveOF(Register &value, ir::ValueType type);

    void SaveAuxiliaryCarry(Register &left, const Operand &right, Register &result,
                            bool reuse_left = false);

    void GetParityFlag(const Register &result);

    void TestParityFlag(const Register &result);

    void TestAuxiliaryCarry(const Register &result);

    enum class PFAFDensityKind : size_t {
        PFWrite,
        PFRead,
        AFWrite,
        AFRead,
        SharedPack,
        WholeFlags,
        Count,
    };

    void RecordPFAFDensity(PFAFDensityKind kind, u32 begin);
    [[nodiscard]] FlagsRegsAuditEdgeKind
    ClassifyFlagsAuditEdge(const ir::Terminal& terminal) const;

    void EmitVecFScalarBinaryLegacy(const ir::Inst* inst, u32 lane_bits);
    void EmitVecFScalarBinaryTied(const ir::Inst* inst, u32 lane_bits);
    void EmitFRINTTSFloatToInt(const Register& result,
                               const VRegister& source,
                               u32 src_bits,
                               u32 dst_bits,
                               bool host_rounding);
    struct PendingExitPollFault {
        size_t metadata_index{};
        Label* recovery{};
        bool publish_frame{};
    };

    struct BlockColdPathRecipe {
        const ir::Block* block{};
        bool density{};
        std::array<u32, static_cast<size_t>(DensityCategory::Count)>
                density_ops{};
        std::array<u32, static_cast<size_t>(DensityCategory::Count)>
                density_bytes{};
        u32 density_scalar_fp_ops{};
        const ir::LoopHoistMetadata* loop_hoist{};
        u32 loop_hoist_prefix_ops{};
        std::array<u32, static_cast<size_t>(PFAFDensityKind::Count)>
                pfaf_density_bytes{};
        bool boundary_density_enabled{};
        u32 boundary_terminal_link_bytes{};
        std::array<u32, static_cast<size_t>(BoundarySubsequence::Count)>
                boundary_density_bytes{};
        std::array<std::map<std::string, u32>,
                   static_cast<size_t>(BoundarySubsequence::Count)>
                boundary_density_mnemonics{};
        std::map<std::string, u32> boundary_terminal_link_mnemonics{};
        std::vector<std::pair<u32, u32>> boundary_terminal_link_ranges{};
        bool save_in_nzcv{};
        bool nzcv_dirty{};
        HostFlags nzcv_requested{};
        bool flags_token_valid{};
        u32 flags_token_result_code{};
        bool flags_token_keep{};
        FlagsRegsAuditEdgeKind flags_audit_block_edge{};
        bool flags_audit_strict_advance{};
        std::unique_ptr<Label> backedge_exit_label{};
        bool backedge_exit_referenced{};
        std::map<u64, std::unique_ptr<Label>> direct_cycle_exits{};
        std::unique_ptr<Label> terminal_request_exit{};
        u32 direct_cycle_cut_edges{};
        std::unique_ptr<BackedgeFlagsRecipe> backedge_flags_recipe{};
        std::unique_ptr<Label> loop_hoist_body_entry{};
        u32 backedge_host_begin{};
        u32 backedge_host_end{};
        u32 region_block_edges{};
        u32 region_block_cycles{};
        u32 region_block_fallthroughs{};
        u32 region_block_local_branch_bytes{};
        std::vector<PendingExitPollFault> pending_exit_poll_faults{};
        std::optional<JitContext::DeferredFlagsRegsAudit> flags_audit{};
    };

    [[nodiscard]] PseudoFlags GetPseudoFlags(const ir::Inst* inst);

    [[nodiscard]] bool MatchMemoryOffsetCase(const ir::Inst* inst);

    void FlushFlags();

    [[nodiscard]] Register FlagsResultRegister(const ir::Inst* inst, const PseudoFlags& pseudo);
    void BeginFlagsTokenProducer(const PseudoFlags& pseudo);
    void CaptureFlagsToken(const Register& result, ir::ValueType type, const ir::Inst* producer);
    void FinishFlagsTokenProducer(const Register& result,
                                  ir::ValueType type,
                                  const PseudoFlags& pseudo,
                                  const ir::Inst* producer);
    [[nodiscard]] bool CanRetainFlagsTokenResult(const ir::Inst* producer, const Register& result);
    [[nodiscard]] XRegister FlagsTokenResult() const;
    void MaterializeFlagsTokenResult();
    void InvalidateFlagsToken();
    void EmitSplitFlagsPublish();
    [[nodiscard]] bool TryEmitCycleExitFlags();
    [[nodiscard]] std::optional<u16> PlannedCompoundProducer(const ir::Inst* inst) const;
    void ParkFlagsHot();
    void UnparkFlagsHot();
    void EmitFlagsPublishedVeneer(const ir::Block* block);

    JitContext &context;
    MacroAssembler &masm;
    const ir::Block* cur_block{};
    const ir::Inst* cur_instr{};
    const ir::Inst* terminal_body_inst{};
    TerminalLocationPublication terminal_location_publication{};
    bool dynamic_location_deferred{};
    BitVector disable_instructions{};
    bool InstructionDisabled(u32 id) const {
        return (id < disable_instructions.size() && disable_instructions.test(id)) ||
               context.GetActions().InstructionCovered(id);
    }
    std::map<const ir::Inst*, Label> local_labels{};
    // Instruction IDs are unique throughout the owning prepared unit.
    std::map<u32, Condition> local_conditions{};
    void ImportBlockActions(const ir::Block* block);
    const GPRResidenceAction* ActiveGPRResidence(const ir::Inst* inst) const {
        return block_actions ? context.GetActions().Residence(inst) : nullptr;
    }
    bool IsGPRZeroExtendElided(const ir::Inst* inst) const {
        const auto* action = ActiveGPRResidence(inst);
        return action && action->Has(GPRResidenceAction::ZeroExtend);
    }
    bool IsGPRSignExtendElided(const ir::Inst* inst) const {
        const auto* action = ActiveGPRResidence(inst);
        return action && action->Has(GPRResidenceAction::SignExtend);
    }
    std::optional<u16> GPRLowViewHome(const ir::Inst* inst) const {
        return inst ? context.GetAllocation().GPRLowView(inst->Id()) : std::nullopt;
    }
    bool HasGPRLowView(const ir::Inst* inst) const { return GPRLowViewHome(inst).has_value(); }
    std::optional<FixedGPRValue> FixedGPRHomeForUse(ir::Value value,
                                                    const ir::Inst* consumer) const {
        return value.Defined() && consumer ? context.GetAllocation().GPRUseHome(value.Id(), consumer->Id())
                                          : std::nullopt;
    }
    const BlockActions* block_actions{};
    compile::PseudoFlagsView pseudo_flags;
    const IntegerBranchAction* integer_branch{};
    // FEAT_AFP + FPCR.NEP is active for guest code, so scalar Advanced SIMD
    // instructions can update a tied destination's lane 0 in place.
    bool sse_scalar_insert{false};
    bool sse_scalar_tie{false};
    bool sse_shufps_imm{false};
    bool sse_afp_nan{false};
    // FPCR.AH gives scalar FMIN/FMAX the x86 source-2 selection rule for
    // unordered and equal inputs.  NEP, enabled by the same AFP contract,
    // preserves the tied destination's upper lanes.
    bool sse_afp_minmax{false};
    bool shift_imm_fast{true};
    bool direct_cycle_latch{false};
    bool backedge_latch{false};
    bool execution_trace_enabled{false};
    int execution_trace_rsp_reg{-1};
    bool cur_block_is_call{};
    std::optional<ir::Value> call_return_value{};
    std::optional<u64> call_return_pc{};
    // Set by EmitSetLocation when the next guest location is a compile-time
    // constant, cleared by every other instruction (Translate(const ir::Inst*)).
    // A trailing SetLocation remains pending until its terminal needs a
    // dispatcher fallback.
    // A ReturnToDispatch/Invalid terminal reached with this set is a direct
    // jmp/call: use a tracked direct link when possible, otherwise read the
    // exact dispatch-table slot instead of returning to the hash lookup.
    std::optional<u64> static_next_loc{};
    // Emits the inline dispatch for `static_next_loc`; returns false when no
    // static target is known and the caller must Ret to the dispatcher.
    bool EmitStaticForward(LinkSiteKind direct_link_kind,
                           DirectLinkFlagsBypass flags_bypass = {},
                           bool external_edge = false,
                           bool preserve_unit_state = true);
    // Dynamic SetLocation is remembered through the no-op PopRSB marker while
    // it remains the final semantic body value. The terminal can reuse its
    // register without extending an SSA lifetime or reloading State::current_loc.
    std::optional<ir::Value> dynamic_next_loc{};
    bool EmitIndirectForward();
    bool EmitContinuationForward();
    [[nodiscard]] bool CanUseCallContinuation() const;
    [[nodiscard]] bool CanUseIndirectCallContinuation() const;
    bool EmitIndirectCallForward(bool pending_flags = false);
    void EmitIndirectExitColdPaths();
    std::unique_ptr<Label> backedge_exit_label{};
    bool backedge_exit_referenced{};
    std::map<u64, std::unique_ptr<Label>> direct_cycle_exits{};
    std::unique_ptr<Label> terminal_request_exit{};
    std::unique_ptr<Label> cycle_exit_reason{};
    HostFlags terminal_bypass_nzcv{};
    bool share_cycle_exit_reason{};
    u32 direct_cycle_cut_edges{};
    std::unique_ptr<BackedgeFlagsRecipe> backedge_flags_recipe{};
    std::unique_ptr<Label> loop_hoist_body_entry{};
    u32 backedge_host_begin{};
    u32 backedge_host_end{};
    std::vector<BackedgeBlockMetadata> backedge_block_metadata{};
    struct PendingDeferredFault {
        size_t metadata_index{};
        Label* recovery{};
    };
    struct IndirectExitMissSite {
        std::unique_ptr<Label> label{};
        u64 guest_start{};
    };
    std::array<IndirectExitMissSite, 32> indirect_dispatch_sites{};
    std::array<IndirectExitMissSite, 32> indirect_call_miss_sites{};
    std::array<IndirectExitMissSite, 32> pending_call_miss_sites{};
    std::array<IndirectExitMissSite, 32> continuation_mismatch_sites{};
    std::vector<BlockColdPathRecipe> block_cold_path_recipes{};
    struct DeferredNZCVMergeStub {
        XRegister scratch{};
        XRegister token{};
        std::unique_ptr<Label> entry{};
    };
    std::vector<DeferredNZCVMergeStub> deferred_nzcv_merge_stubs{};
    struct RegionFlagsCanonicalStubKey {
        u64 target{};
        u32 mask{};
        EdgeCarryPolarity polarity{EdgeCarryPolarity::Unknown};
        bool token{};

        auto operator<=>(const RegionFlagsCanonicalStubKey&) const = default;
    };
    std::map<RegionFlagsCanonicalStubKey, std::unique_ptr<Label>>
            region_flags_canonical_stubs{};
    struct HostCallThunk {
        u64 target;
        std::unique_ptr<Label> entry{};
    };
    std::vector<HostCallThunk> host_call_thunks{};
    FlagsRegsAuditEdgeKind flags_audit_block_edge{
            FlagsRegsAuditEdgeKind::Dispatcher};
    bool flags_audit_strict_advance{};
    bool flags_audit_cold{};
    const EdgeActions* edge_actions{};
    bool region_function_mode{}, canonical_entries_active{};
    bool current_flags_transparent{};
    std::optional<u64> next_region_block{};
    u64 unit_start_pc{};
    bool translating_function{};

    struct FlagEmissionState {
        ir::Flags flags_set{};
        ir::Flags flags_clear{};
        bool save_in_nzcv{true};
        bool nzcv_dirty{false};
        const ir::Inst* raw_carry_pending{};
        bool flags_token_valid{false};
        u32 flags_token_result_code{};
        // Terminals may emit several successors. Keep the compile-time token
        // live so every arm packs; mid-block Merge still consumes it.
        bool flags_token_keep{false};
        bool compound_logical_clear_pending{false};
        bool compound_logical_zero_pending{false};
        // Which host NZCV bits were actually requested by SaveFlags since the
        // last MergeNZCV. Only these bits are merged; the rest keep their
        // existing value in the flags register (so a ClearFlags(CF) between
        // two flag-setting instructions is not overwritten by the merge).
        HostFlags nzcv_requested{};
        EdgeCarrySourceState edge_carry_source{};
    } flag_state;

    struct MemoryEmissionState {
        std::map<const ir::Inst*, SpilledMemoryOperand> spilled_memory_operands{};
        // Statically proven residues (mod 8) of pinned guest-register
        // homes: seeded only from immediate-constant roots and maintained
        // across home rewrites by TrackHomeAlignment. A dynamic alignment
        // check does NOT seed this map: its unaligned fallback rejoins the
        // same continuation, so reaching later code proves nothing about
        // the address bits. Cleared per block and at local-label joins.
        std::map<u32, u32> home_alignment_mod8{};
        // True when Config::memory_base / page_table is set: every guest memory
        // access goes through the pt bias register (guest addr + pt = host addr).
        bool use_memory_base{false};
        // Bounded guest window (Config::guest_addr_mask, 0 = disabled). Every
        // guest address is truncated to `guest_addr_mask` before pt is added, so
        // the access can only land inside the embedder's window reservation.
        u64 guest_addr_mask{0};
        // guest_addr_mask == 0xFFFFFFFF: the arm64 [Xn, Wm, UXTW] addressing mode
        // computes pt + zext32(guest) in the *same* instruction the unbounded
        // path already used, so a 32-bit window costs nothing.
        bool window_uxtw{false};
        bool mem_hostbase_fold{false};
        // W29 lowering: signed/unsigned narrow loads consume their
        // extension destination directly, and GetOperand computes into its
        // allocated address register. SVM_MEM_NARROW_FUSE=0 restores the old
        // load+extend and temporary+transport-move shapes.
        // Safe by construction after the GetOperand RA-tie fix: the emitter only
        // peels when the allocator transferred register ownership (SharesGPR).
        bool mem_narrow_fuse{true};
        // 只处理寻址 EA：固定别名的末次使用可转交给 GetOperand result；
        // direct frontend 另把简单复合地址直接保留到 memory IR。
        bool addr_ea_tie{true};
        // 绝对地址常量直接物化到 GetOperand 的分配结果，避免临时寄存器搬运。
        bool abs_const_mat{false};
        std::vector<FaultMetadata> fault_metadata{};
        std::vector<PendingExitPollFault> pending_exit_poll_faults{};
        std::vector<PendingDeferredFault> pending_deferred_faults{};
        std::map<UnalignedAtomicFallbackKey, std::unique_ptr<Label>>
                unaligned_atomic_fallbacks{};
    } memory_state;

    struct EmissionStatistics {
        bool boundary_density_enabled{};
        bool boundary_terminal_open{};
        u32 boundary_terminal_link_bytes{};
        std::array<u32, static_cast<size_t>(BoundarySubsequence::Count)>
                boundary_density_bytes{};
        std::array<std::map<std::string, u32>,
                   static_cast<size_t>(BoundarySubsequence::Count)>
                boundary_density_mnemonics{};
        std::array<u32, static_cast<size_t>(PFAFDensityKind::Count)>
                pfaf_density_bytes{};
        std::map<std::string, u32> boundary_terminal_link_mnemonics{};
        std::vector<std::pair<u32, u32>> boundary_terminal_link_ranges{};
        u32 region_block_edges{};
        u32 region_block_cycles{};
        u32 region_block_fallthroughs{};
        u32 region_block_local_branch_bytes{};
    } statistics;
};

}
