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
#include "block_analysis_index.h"
#include "guest_state_map.h"
#include "raw_carry_branch_analysis.h"
#include "resident_scalar_fpr_analysis.h"
#include "scalar_fpr_liveness.h"
#include "scalar_copy_analysis.h"
#include "terminal_location_publication.h"
#include "runtime/backend/code_cache.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/atomic_rmw.h"
#include "runtime/ir/block.h"

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

    void Translate(ir::Block *block);

    void Translate(ir::HIRFunction *function);

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
    MemOperand EmitMemOperand(ir::Operand &ir_op,
                              ir::ValueType type,
                              bool pair = false,
                              bool atomic = false,
                              bool allow_writeback = true,
                              bool structured_guest_ea = false,
                              ir::Inst* memory_inst = nullptr);

#define INST(name, ...) void Emit##name(ir::Inst *inst);
#include "runtime/ir/ir.inc"
#undef INST

private:
    void EmitExecutionTrace(u64 guest_rip);
    void EmitByteMovMask(const VRegister& source,
                         const WRegister& result,
                         const VRegister& work,
                         const VRegister& packed);
    [[nodiscard]] bool CanUseZeroStoreRegister(ir::Value value);
    [[nodiscard]] bool CanConsumeForwardedWidthSpill(ir::Inst* inst);
    [[nodiscard]] std::optional<u32> ForwardedMemorySpillInput(ir::Inst* inst);
    [[nodiscard]] bool CanFusePinnedZeroExtendPublication(ir::Inst* inst);
    [[nodiscard]] bool CanAdoptPendingSpillWrite(
            ir::Inst* inst, bool reads_forwarded_memory_input);
    [[nodiscard]] bool IsZeroStoreValue(ir::Value value);
    [[nodiscard]] bool HasOnlyZeroStoreUses(ir::Inst* definition);
    [[nodiscard]] bool ReproveCoalescedHostWrite(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveCoalescedHostRead(ir::Inst* inst) const;
    [[nodiscard]] bool EmitCachedConstAddress(ir::Inst* inst,
                                              const Register& result);
    [[nodiscard]] std::optional<u64> CachedConstAddressOffset(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveCoalescedHostFPRWrite(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveScalarFPRTie(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveShufpsImmTie(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveCoalescedHostFPRRead(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveAesChainTie(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveAesChainHostWrite(ir::Inst* inst) const;
    struct MemoryUpdate {
        ir::Inst* memory{};
        ir::Inst* publication{};
        XRegister base{};
        s64 offset{};
    };
    [[nodiscard]] std::optional<MemoryUpdate>
    MatchMemoryUpdate(ir::Inst* update) const;
    [[nodiscard]] std::optional<MemoryUpdate>
    MatchPreIndexMemoryUpdate(ir::Inst* update) const;
    [[nodiscard]] std::optional<MemoryUpdate>
    MatchBiasedMemoryUpdate(ir::Inst* update) const;
    struct PinnedLoadUpdate {
        ir::Inst* load{};
        ir::Inst* update{};
        ir::Inst* publication{};
        ir::Inst* base_read{};
        u16 target{};
        s16 offset{};

        bool operator==(const PinnedLoadUpdate&) const = default;
    };
    void PreparePinnedLoadUpdates(ir::Block* block);
    [[nodiscard]] std::optional<PinnedLoadUpdate>
    MatchPinnedLoadUpdate(ir::Inst* update);
    struct ScalarFPRPublication {
        ir::Inst* low_store{};
        ir::Inst* high_store{};
        ir::Inst* zero{};
        ir::Inst* load_extension{};
        u16 target{};
    };
    void PrepareScalarFPRPublications(ir::Block* block);
    void PrepareBooleanSelects(ir::Block* block);
    [[nodiscard]] bool ReproveScalarFPRPublication(
            const ScalarFPRPublication& publication) const;
    [[nodiscard]] bool ReproveScalarLoadFPRFusion(
            ir::Inst* load, const ScalarFPRPublication& publication) const;
    [[nodiscard]] bool ReproveScalarValueFPRFusion(
            ir::Inst* low_store, const ScalarFPRPublication& publication) const;
    [[nodiscard]] bool EmitPshufdDirect(const VRegister& result,
                                        const VRegister& source,
                                        u32 control);
    [[nodiscard]] std::optional<u8> ReprovePshufdDirectConstant(
            ir::Inst* inst) const;
    [[nodiscard]] std::optional<u8> ReprovePshufdDirectShuffle(
            ir::Inst* inst) const;
    [[nodiscard]] bool ReproveWidthChainBridge(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveLow32ViewOwnership(ir::Inst* inst,
                                                 ir::Value source) const;
    [[nodiscard]] bool ReproveAdjacentLow32Copy(ir::Inst* inst,
                                                ir::Value source) const;
    [[nodiscard]] bool ReproveLow32Copy(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveCachedConstAddress(ir::Inst* inst) const;
    [[nodiscard]] bool CanReuseExactCachedConstAddress(ir::Inst* inst) const;

    struct PinnedGPRCopy {
        ir::Inst* read{};
        ir::Inst* narrow_extend{};
        ir::Inst* extend{};
        bool signed_load{};
        std::vector<ir::Inst*> aliases{};
        std::vector<std::pair<ir::Inst*, ir::Inst*>> transferred_uses{};
        std::optional<u16> source{};
        u16 target{};
        u8 width{};
        u32 last_use{};
    };
    void PrepareDeadPinnedGPRWrites(ir::Block* block);
    [[nodiscard]] bool IsDeadPinnedGPRWrite(ir::Inst* inst) const;
    void PreparePinnedGPRCopies(ir::Block* block);
    struct PinnedSelectPublication {
        ir::Inst* producer{};
        ir::Inst* extend{};
        ir::Inst* publication{};
        std::vector<ir::Inst*> aliases{};
        u16 target{};
        u32 last_use{};

        bool operator==(const PinnedSelectPublication&) const = default;
    };
    [[nodiscard]] std::optional<PinnedSelectPublication>
    MatchPinnedSelectPublication(ir::Inst* publication) const;
    void PreparePinnedSelectPublications(ir::Block* block);
    struct PinnedGPRValueTransfer {
        ir::Inst* read{};
        ir::Inst* publication{};
        std::vector<ir::Inst*> aliases{};
        u16 source{};
        u16 target{};
        u32 last_use{};

        bool operator==(const PinnedGPRValueTransfer&) const = default;
    };
    [[nodiscard]] std::optional<PinnedGPRValueTransfer>
    MatchPinnedGPRValueTransfer(ir::Inst* publication) const;
    void PreparePinnedGPRValueTransfers(ir::Block* block);
    struct PinnedGPRPublicationView {
        ir::Inst* value{};
        ir::Inst* publication{};
        std::vector<ir::Inst*> aliases{};
        u16 target{};
        u32 last_use{};

        bool operator==(const PinnedGPRPublicationView&) const = default;
    };
    [[nodiscard]] std::optional<PinnedGPRPublicationView>
    MatchPinnedGPRPublicationView(ir::Inst* publication) const;
    void PreparePinnedGPRPublicationViews(ir::Block* block);
    struct SpilledGPRPublication {
        ir::Inst* producer{};
        ir::Inst* publication{};
        u16 target{};

        bool operator==(const SpilledGPRPublication&) const = default;
    };
    [[nodiscard]] std::optional<SpilledGPRPublication>
    MatchSpilledGPRPublication(ir::Inst* publication);
    void PrepareSpilledGPRPublications(ir::Block* block);
    struct NarrowExtractExtension {
        ir::Inst* extract{};
        ir::Value source{};
        ir::Inst* shift{};
        u8 width{};
        bool source_high_zero{};

        bool operator==(const NarrowExtractExtension&) const = default;
    };
    [[nodiscard]] std::optional<NarrowExtractExtension>
    MatchNarrowExtractExtension(ir::Inst* wrapper) const;
    struct FunnelShiftRecipe {
        ir::Inst* first{};
        ir::Inst* second{};
        ir::Inst* result{};
        ir::Value high{};
        ir::Value low{};
        u8 amount{};

        bool operator==(const FunnelShiftRecipe&) const = default;
    };
    [[nodiscard]] std::optional<FunnelShiftRecipe>
    MatchFunnelShift(ir::Inst* result) const;
    void PrepareFunnelShifts(ir::Block* block);
    [[nodiscard]] bool EmitFunnelShiftPart(ir::Inst* inst);
    [[nodiscard]] bool EmitFunnelShiftResult(ir::Inst* inst);
    struct NarrowMaskedInput {
        ir::Inst* extract{};
        ir::Value source{};
        u8 width{};

        bool operator==(const NarrowMaskedInput&) const = default;
    };
    [[nodiscard]] std::optional<NarrowMaskedInput>
    MatchNarrowMaskedInput(ir::Inst* consumer) const;
    struct ShiftMaskedInput {
        ir::Inst* shift{};
        ir::Value source{};
        u8 offset{};

        bool operator==(const ShiftMaskedInput&) const = default;
    };
    [[nodiscard]] std::optional<ShiftMaskedInput>
    MatchShiftMaskedInput(ir::Inst* consumer);
    void PrepareNarrowExtractExtensions(ir::Block* block);
    void PrepareNarrowFlagsInputs(ir::Block* block);
    [[nodiscard]] std::optional<ir::Value>
    MatchNarrowFlagsInput(ir::Inst* extract);
    [[nodiscard]] ir::Value ResolveNarrowFlagsInput(ir::Value value,
                                                    ir::Inst* consumer);
    struct NarrowComparison {
        ir::Value left{};
        std::optional<ir::Value> right{};
        ir::Inst* immediate_load{};
        u32 immediate{};
        u8 width{};

        bool operator==(const NarrowComparison&) const = default;
    };
    [[nodiscard]] bool IsNarrowZeroExtended(ir::Value value, u32 width) const;
    [[nodiscard]] std::optional<NarrowComparison>
    MatchNarrowCompare(ir::Inst* inst);
    void PrepareNarrowCompares(ir::Block* block);
    struct NarrowCarryFusion {
        ir::Inst* carry_test{};
        ir::Inst* carry_add{};
        ir::Value value{};
        std::vector<ir::Inst*> dead_inputs{};
    };
    [[nodiscard]] std::optional<NarrowCarryFusion>
    MatchNarrowCarryFusion(ir::Inst* inst);
    void PrepareNarrowCarryFusions(ir::Block* block);
    [[nodiscard]] std::optional<PinnedGPRCopy>
    MatchPinnedGPRCopy(ir::Inst* inst) const;
    [[nodiscard]] std::optional<XRegister>
    ResolvePinnedGPRValue(ir::Value value) const;
    [[nodiscard]] std::optional<WRegister>
    ResolvePinnedGPRWUse(ir::Value value, const ir::Inst* consumer) const;
    [[nodiscard]] std::optional<Register>
    ResolvePinnedGPRUse(ir::Value value, const ir::Inst* consumer) const;
    struct PinnedMemorySource {
        u16 target{};
        u32 live_begin{};
    };
    struct PinnedMemoryAddress {
        ir::Inst* memory{};
        u16 target{};
        s64 offset{};
    };
    [[nodiscard]] std::optional<PinnedMemoryAddress>
    MatchPinnedMemoryAddress(ir::Inst* address) const;
    [[nodiscard]] std::optional<PinnedMemorySource>
    MatchPinnedMemorySource(ir::Value source) const;
    [[nodiscard]] std::optional<u16>
    MatchPinnedMemoryValue(ir::Inst* extract) const;
    void PreparePinnedMemoryValues(ir::Block* block);
    struct SpilledMemoryOperand {
        ir::Inst* consumer{};
        XRegister base{};
        s64 offset{};
    };
    [[nodiscard]] std::optional<MemOperand> TryEmitSpilledMemoryOperand(
            ir::Inst* address,
            ir::ValueType type,
            bool pair,
            bool atomic,
            ir::Inst* memory_inst);

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
        u32 loop_hoist_prefix_begin{};
        u32 loop_hoist_prefix_ops{};
        bool split_flags_entry{};
    };

    [[nodiscard]] BlockTranslateState PrepareBlockState(ir::Block* block);

    struct UniformDensityCounts {
        u32 gpr_uniform_accesses{};
        u32 xmm_uniform_accesses{};
    };

    [[nodiscard]] UniformDensityCounts
    CollectUniformDensity(ir::Block* block, bool density);

    void TranslateBlockInstructions(
            ir::Block* block,
            const ir::LoopHoistMetadata& loop_hoist,
            bool density,
            bool gap_audit,
            std::span<u32> density_ops,
            std::span<u32> density_bytes,
            u32& density_scalar_fp_ops,
            u32& loop_hoist_prefix_begin,
            u32& loop_hoist_prefix_ops);

    void EmitBlockTerminal(ir::Block* block,
                           bool density,
                           std::span<u32> density_bytes);

    struct BlockColdPathRecipe;
    [[nodiscard]] BlockColdPathRecipe CaptureBlockColdPathRecipe(
            ir::Block* block,
            bool density,
            std::span<const u32> density_ops,
            std::span<const u32> density_bytes,
            u32 density_scalar_fp_ops,
            const ir::LoopHoistMetadata& loop_hoist,
            u32 loop_hoist_prefix_ops);
    void EmitBlockColdPathRecipe(BlockColdPathRecipe recipe);

    void PrintBlockDensity(ir::Block* block,
                           bool density,
                           std::span<const u32> density_ops,
                           std::span<const u32> density_bytes,
                           u32 density_scalar_fp_ops,
                           const ir::LoopHoistMetadata& loop_hoist,
                           u32 loop_hoist_prefix_ops);

    void PlacementPoint(const char* kind, u64 guest_pc);

    struct BackedgeCarryRecipe {
        bool canonical{};
        u8 inverted{};
        ir::Inst* load{};
        ir::Inst* store{};
        ir::Inst* marker{};
    };

    struct BackedgeFlagsRecipe {
        bool optimized{true};
        // dead_successor=true 是 region 单边 flags-dead 形态：热边目标在
        // 任意观察点前完整覆写 flags，因此不需要跨块 recipe/双入口。
        bool dead_successor{};
        bool canonical_carry{};
        bool self_is_then{};
        ir::Location self_target{};
        ir::Location cold_target{};
        u8 carry_inverted{};
        HostFlags requested{};
        ir::Inst* polarity_load{};
        ir::Inst* polarity_store{};
        ir::Inst* final_save{};
        ir::Inst* final_advance{};
        struct DeferredOperand {
            enum class Kind : u8 { None, Imm, HostGPR, Uniform } kind{};
            u64 value{};
            u8 offset{};

            auto operator<=>(const DeferredOperand&) const = default;
        };
        // 严格窄 Sub 子集可在 cold edge 从架构家重读两个输入并重算 PF/AF，
        // 不延长 SSA interval，也不让 recipe 穿过目标块。
        bool defer_pfaf{};
        u8 pfaf_width{};
        DeferredOperand pfaf_left{};
        DeferredOperand pfaf_right{};
        std::unique_ptr<Label> local_entry{std::make_unique<Label>()};
        std::unique_ptr<Label> external_entry{std::make_unique<Label>()};
        std::unique_ptr<Label> cold_exit{std::make_unique<Label>()};
        std::unique_ptr<Label> fault_recovery{std::make_unique<Label>()};
        bool cold_referenced{};
    };

    [[nodiscard]] std::unique_ptr<BackedgeFlagsRecipe>
    RecipeBackedgeFlags(ir::Block* block);
    [[nodiscard]] std::optional<BackedgeCarryRecipe> RecipeBackedgeCarry(
            ir::Block* block, ir::Inst* final_save, ir::Inst* condition,
            bool dead_successor);
    [[nodiscard]] bool CanonicalCarryEnabled() const;
    [[nodiscard]] bool TargetKillsIncomingFlags(ir::Location target) const;
    [[nodiscard]] bool RecipeRegionBranchPFAF(BackedgeFlagsRecipe& recipe,
                                            ir::Inst* producer) const;
    [[nodiscard]] bool ReproveRegionBranchPFAF() const;
    [[nodiscard]] bool RegionBranchPFAFActive(ir::Inst* producer) const;
    [[nodiscard]] bool EmitBackedgeFlagsTerminal(const ir::Terminal& terminal);
    void EmitBackedgeMaterialize(const BackedgeFlagsRecipe& recipe);
    void EmitRegionBranchPFAF(const BackedgeFlagsRecipe& recipe);
    void EmitBackedgeColdPaths();
    [[nodiscard]] bool RetainsPendingHostNZCV(const ir::Inst& inst) const;
    [[nodiscard]] static bool PreservesHostNZCV(ir::OpCode op);
    [[nodiscard]] bool MayFaultOrObserve(const ir::Inst& inst) const;
    [[nodiscard]] static bool MayFaultOrObserve(ir::OpCode op);
    [[nodiscard]] EdgeFlagsState PendingEdgeFlagsState(
            HostFlags valid,
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

    enum class UnalignedAtomicFallbackKind : u8 {
        CompareAndSwap,
        Exchange,
        FetchAdd,
    };

    struct UnalignedAtomicFallbackKey {
        UnalignedAtomicFallbackKind kind{};
        ir::ValueType type{};
        u8 address{};
        u8 result{};
        u8 first{};
        u8 second{};

        auto operator<=>(const UnalignedAtomicFallbackKey&) const = default;
    };

    [[nodiscard]] std::optional<UnalignedAtomicFallbackKey>
    GetUnalignedAtomicFallbackKey(ir::Inst* inst);
    void PrepareUnalignedAtomicFallbacks(std::span<ir::Block* const> blocks);
    [[nodiscard]] Label* GetUnalignedAtomicFallback(ir::Inst* inst);
    void EmitUnalignedAtomicFallbacks();
    void EmitUnalignedAtomicFallback(const UnalignedAtomicFallbackKey& key);

    struct PseudoFlags {
        ir::Flags set{};
        ir::Flags clear{};
        bool branch_only{};

        [[nodiscard]] bool Null() const {
            return set == ir::Flags::None && clear == ir::Flags::None &&
                   !branch_only;
        }

        [[nodiscard]] bool IsNZCV() const {
            return True(set & ir::Flags::NZCV);
        }

        [[nodiscard]] bool IsCV() const {
            return True(set & ir::Flags::CV) && False(set & ir::Flags::NZ) && False(clear & ir::Flags::NZ);
        }

        [[nodiscard]] bool IsNZ_ZeroCV() const {
            return True(set & ir::Flags::NZ) && True(clear & ir::Flags::CV);
        }

        [[nodiscard]] bool NeedsResultBits() const {
            return True(set & (ir::Flags::Parity |
                               ir::Flags::AuxiliaryCarry));
        }
    };

    void Translate(ir::Inst *inst);
    void PublishPendingStaticLocation();

    // Terminals
    void EmitTerminal(const ir::Terminal &terminal,
                      LinkSiteKind direct_link_kind = LinkSiteKind::Unconditional,
                      DirectLinkFlagsBypass flags_bypass = {});
    void EmitDispatcherTerminal(LinkSiteKind direct_link_kind,
                                DirectLinkFlagsBypass flags_bypass,
                                bool external_edge,
                                bool call_exit);
    [[nodiscard]] bool CanBypassTerminalFlagsMerge(
            const ir::Terminal& terminal) const;
    void PrepareRegionEdges(ir::HIRFunction* function);
    void CollectRegionTargets(const ir::Terminal& terminal,
                              std::vector<u64>& targets) const;
    [[nodiscard]] std::optional<ir::Location>
    RegionLeafTarget(const ir::Terminal& terminal) const;
    [[nodiscard]] bool IsRegionInternalEdge(ir::Location target) const;
    [[nodiscard]] bool IsRegionCycleEdge(ir::Location target) const;
    [[nodiscard]] bool HasRegionCycleEdgeFromCurrent() const;
    [[nodiscard]] bool IsDirectCycleCutEdge(ir::Location source,
                                            ir::Location target) const;
    [[nodiscard]] bool IsDirectCycleCutEdge(ir::Location target) const;
    [[nodiscard]] u32 CountCycleExitCandidates(
            std::span<ir::Block* const> blocks) const;
    [[nodiscard]] Label* GetDirectCycleExit(ir::Location target);
    [[nodiscard]] Label* GetExternalCycleExit(ir::Location target);
    [[nodiscard]] bool CanUseRegionSuccessorLayout(ir::Location target) const;
    void EmitRegionEdge(ir::Location target,
                        bool fallthrough = false,
                        bool record_edge_counters = true,
                        bool commit_flags = true);
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
                                    bool allow_fallthrough);
    [[nodiscard]] bool BlockIsFlagsTransparent(ir::Block* block) const;
    [[nodiscard]] bool EmitRegionCondition(const ir::terminal::Condition& terminal,
                                           bool allow_fallthrough);
    [[nodiscard]] bool IsCanonicalTerminalEntry() const;
    [[nodiscard]] bool EmitCanonicalTerminalEdge(ir::Location target);
    [[nodiscard]] bool HasSelfEdge(const ir::Terminal& terminal) const;
    [[nodiscard]] bool IsSelfEdge(ir::Location target) const;
    [[nodiscard]] Label* LocalBranchTarget(ir::Location target) const;
    void EmitBackedgeExitStub();
    void EmitDirectCycleExitStubs();
    void EmitCycleExitReasonTail(Label* reason);
    void RecordExitPollFault(std::optional<JitContext::FaultRange> fault,
                             Label* recovery);
    void ResolveExitPollFaults(Label* recovery, ir::Location resume_location);
    void RecordDeferredFault(JitContext::FaultRange fault,
                             Label* recovery,
                             FaultRecoveryKind recovery_kind);
    void ResolveDeferredFaults(Label* recovery);

    // Labels used by Goto / NotGoto / BindLabel
    Label *GetLocalLabel(ir::Inst *inst);

    // Flags
    void SaveHostFlags(HostFlags host, ir::Flags guest);

    static HostFlags GuestNZCVToHost(ir::Flags guest);

    static Condition MapCond(ir::Cond cond);

    // Records a LocalCondSet/FCmpCondSet whose sole use is a branch or select.
    // The marker itself then emits no CSET; its consumer reads the producer's
    // still-live host NZCV directly.
    bool RecordLocalCondition(ir::Inst *inst, ir::Cond cond);
    [[nodiscard]] std::optional<Condition> LocalConditionFor(ir::Value value) const;
    [[nodiscard]] bool LaterNeedsHostPstate(ir::Inst* from) const;
    [[nodiscard]] bool CarryCanStayInPstate(ir::Inst* test) const;
    bool FoldCcFromCarryTest(ir::Inst* test_flags);
    [[nodiscard]] static bool IsCompactFCmp(ir::Value value);
    [[nodiscard]] bool CanUseCompactFCmpCarrier(ir::Inst* fcmp) const;
    [[nodiscard]] ir::Inst* RawFCmpCondition(ir::Inst* fcmp) const;

    struct DeadEdgeIntegerBranch {
        ir::Inst* producer{};
        ir::Inst* condition{};
        ir::Flags required{};
        ir::Cond raw_condition{};
        std::optional<u16> zero_target{};
        bool zero_is_64{};
        std::unordered_set<ir::Inst*> discarded{};
    };

    void PrepareDeadEdgeIntegerBranch(ir::Block* block);
    [[nodiscard]] bool IsDeadEdgeIntegerBranchProducer(ir::Inst* inst) const;
    [[nodiscard]] std::optional<ir::Cond>
    DeadEdgeIntegerBranchCondition(ir::Inst* inst) const;
    bool EmitDeadEdgeZeroBranch(ir::Value condition, Label* label,
                                bool on_true);

    struct DeadNarrowImmediateBranch {
        ir::Inst* producer{};
        ir::Inst* immediate_load{};
        u64 immediate{};
        ir::Flags required{};
        u8 width{};
    };
    void PrepareDeadNarrowImmediateBranch();
    [[nodiscard]] std::optional<DeadNarrowImmediateBranch>
    MatchDeadNarrowImmediateBranch(ir::Inst* inst) const;

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
    [[nodiscard]] bool TryEmitCondSetFromFlags(ir::Inst* inst, ir::Cond cond);
    void EmitZeroTestPreservingPstate(ir::Inst* inst, bool nonzero);

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
    void EmitHostCall(const ir::Lambda &lambda,
                      const std::vector<ir::DataClass> &args,
                      bool has_result,
                      const Register &result,
                      std::optional<Register> secondary_result = std::nullopt);
    void EmitSse42StrVectorCall(ir::Inst* inst,
                                const VRegister& left,
                                const VRegister& right,
                                const WRegister& result,
                                VAddr target,
                                u8 imm);
    void PrepareHostCallThunks(const std::vector<ir::Block*>& blocks);
    void MaterializeHostCallTarget(u64 target);
    bool TryEmitSharedHostCall(const ir::Lambda& lambda);
    bool TryEmitSharedHostBranch(const ir::Lambda& lambda);
    void EmitHostCallThunks();
    void EmitPreserveAllPairCall(ir::Inst* inst,
                                 VAddr target,
                                 const std::vector<ir::DataClass>& args,
                                 ir::OpCode secondary,
                                 ir::HostRegisterEffect host_registers =
                                         ir::HostRegisterEffect::MayTouchSIMD);
    void SpillStaticFPRUniforms();
    void RestoreStaticFPRUniforms();

    void ClearFlags(ir::Flags flags);

    void SaveParity(Register &value);

    void SaveNZ(Register &value, ir::ValueType type);

    void SaveCV(Register &value, ir::ValueType type);

    void SaveOF(Register &value, ir::ValueType type);

    void SaveAuxiliaryCarry(Register &left, const Operand &right, Register &result);

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
    [[nodiscard]] bool IsStrictInternalAdvancePC(ir::Block* block,
                                                 ir::Inst* advance) const;

    // ARM and x86 can choose different signs/payloads when a packed FP
    // operation consumes a NaN. Adjust each lane to x86's first-NaN,
    // quiet-preserving rule after the NEON arithmetic instruction.
    void EmitVecFloatNaNFixup(const VRegister &result,
                              const VRegister &left,
                              const VRegister &right,
                              u32 lane_bits,
                              u32 lane_count,
                              ir::Inst *inst);
    void EmitVecFScalarBinaryLegacy(ir::Inst *inst, u32 lane_bits);
    void EmitVecFScalarBinaryTied(ir::Inst *inst, u32 lane_bits);
    void EmitFRINTTSFloatToInt(const Register &result,
                               const VRegister &source,
                               u32 src_bits,
                               u32 dst_bits,
                               bool host_rounding);
    [[nodiscard]] bool UseAFPNaN(ir::Inst *inst) const;
    VRegister PreserveNaNColdSource(ir::Inst *inst,
                                    const VRegister &source,
                                    const VRegister &result,
                                    const VRegister &reserved);

    enum class VecNaNColdKind : u8 {
        BinaryScalar32,
        BinaryScalar64,
        BinaryPacked32,
        BinaryPacked64,
        SqrtScalar32,
        SqrtScalar64,
        SqrtPacked32,
        SqrtPacked64,
    };

    struct VecNaNColdSite {
        VecNaNColdKind kind;
        VRegister left;
        VRegister right;
        VRegister result;
        std::unique_ptr<Label> slow{std::make_unique<Label>()};
        std::unique_ptr<Label> continuation{std::make_unique<Label>()};
        std::unique_ptr<Label> repaired{std::make_unique<Label>()};
    };

    struct PendingExitPollFault {
        size_t metadata_index{};
        Label* recovery{};
    };

    struct BlockColdPathRecipe {
        ir::Block* block{};
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
        std::vector<std::unique_ptr<VecNaNColdSite>> vec_nan_cold_sites{};
        std::optional<JitContext::DeferredFlagsRegsAudit> flags_audit{};
    };

    void QueueVecNaNColdPath(VecNaNColdKind kind,
                             const VRegister &result,
                             const VRegister &left,
                             const VRegister &right = NoVReg);
    void EmitVecNaNColdPaths();
    void EmitVecNaNColdHandler(VecNaNColdKind kind);

    [[nodiscard]] PseudoFlags GetPseudoFlags(ir::Inst *inst);

    [[nodiscard]] bool MatchMemoryOffsetCase(ir::Inst *inst);
    [[nodiscard]] std::optional<u64> MatchInductionImmediate(ir::Inst *inst);
    void RecipeInductionTies(ir::Block *block);

    void FlushFlags();

    [[nodiscard]] Register FlagsResultRegister(
            ir::Inst* inst, const PseudoFlags& pseudo);
    void BeginFlagsTokenProducer(const PseudoFlags& pseudo);
    void CaptureFlagsToken(const Register& result,
                           ir::ValueType type,
                           ir::Inst* producer);
    void FinishFlagsTokenProducer(const Register& result,
                                  ir::ValueType type,
                                  const PseudoFlags& pseudo,
                                  ir::Inst* producer);
    [[nodiscard]] bool CanRetainFlagsTokenResult(
            ir::Inst* producer, const Register& result);
    [[nodiscard]] XRegister FlagsTokenResult() const;
    void MaterializeFlagsTokenResult();
    void InvalidateFlagsToken();
    void EmitSplitFlagsPublish();
    [[nodiscard]] bool TryEmitCycleExitFlags();
    [[nodiscard]] bool MatchCompoundLogicalClear(ir::Inst* inst) const;
    [[nodiscard]] bool MatchCompoundZeroLogicalClear(ir::Inst* inst) const;
    void ParkFlagsHot();
    void UnparkFlagsHot();
    void EmitFlagsPublishedVeneer(ir::Block* block);

    JitContext &context;
    MacroAssembler &masm;
    ir::Block *cur_block{};
    // Pinned GPR analysis and emission data belong to one block. Reset all
    // families together before candidate discovery; their existing emission
    // priority and safety checks remain in their respective handlers.
    struct PinnedGPRState {
        ir::Block* owner{};
        BlockAnalysisIndex block_analysis;
        std::map<ir::Inst*, u16> fused_pin_gpr_reads{};
        std::map<ir::Inst*, u16> pinned_gpr_values{};
        std::map<ir::Inst*, PinnedGPRCopy> pinned_gpr_copies{};
        std::map<ir::Inst*, PinnedSelectPublication> pinned_select_results{};
        std::map<ir::Inst*, PinnedSelectPublication> pinned_select_publications{};
        std::map<ir::Inst*, PinnedGPRValueTransfer> pinned_gpr_value_transfers{};
        std::map<ir::Inst*, PinnedGPRPublicationView> pinned_gpr_publication_views{};
        std::map<ir::Inst*, SpilledGPRPublication> spilled_gpr_publications{};
        std::map<ir::Inst*, PinnedLoadUpdate> pinned_load_updates{};
        std::map<ir::Inst*, PinnedLoadUpdate> pinned_load_update_instructions{};
        std::unordered_set<ir::Inst*> dead_pinned_gpr_writes{};

        void Reset(ir::Block* block) {
            owner = block;
            block_analysis.Build(block);
            fused_pin_gpr_reads.clear();
            pinned_gpr_values.clear();
            pinned_gpr_copies.clear();
            pinned_select_results.clear();
            pinned_select_publications.clear();
            pinned_gpr_value_transfers.clear();
            pinned_gpr_publication_views.clear();
            spilled_gpr_publications.clear();
            pinned_load_updates.clear();
            pinned_load_update_instructions.clear();
            dead_pinned_gpr_writes.clear();
        }
    } pinned_gprs;
    ir::Inst *cur_instr{};
    ir::Inst *terminal_body_inst{};
    TerminalLocationPublication terminal_location_publication{};
    Label* dynamic_location_miss{};
    BitVector disable_instructions{};
    std::map<ir::Inst *, Label> local_labels{};
    std::map<ir::Inst *, Condition> local_conditions{};
    std::unordered_set<ir::Inst*> fused_pin_zext32{};
    std::unordered_set<ir::Inst*> checked_pin_zext32_publications{};
    std::unordered_set<ir::Inst*> fused_pin_sign_extends{};
    std::map<ir::Inst*, NarrowExtractExtension> narrow_extract_extensions{};
    std::map<ir::Inst*, ir::Inst*> fused_narrow_extracts{};
    std::map<ir::Inst*, ir::Inst*> fused_narrow_extract_shifts{};
    std::map<ir::Inst*, FunnelShiftRecipe> funnel_shifts{};
    std::map<ir::Inst*, ir::Inst*> funnel_shift_parts{};
    std::map<ir::Inst*, ir::Inst*> funnel_shift_parity_producers{};
    std::map<ir::Inst*, NarrowMaskedInput> narrow_masked_inputs{};
    std::map<ir::Inst*, ir::Inst*> fused_narrow_masked_extracts{};
    std::map<ir::Inst*, ShiftMaskedInput> shift_masked_inputs{};
    std::map<ir::Inst*, ir::Inst*> fused_shift_masked_shifts{};
    std::map<ir::Inst*, ScalarFPRPublication> scalar_load_fpr_fusions{};
    std::map<ir::Inst*, ScalarFPRPublication> scalar_value_fpr_fusions{};
    ResidentScalarFPRAnalysis resident_scalar_fpr_analysis{};
    RawCarryBranchAnalysis raw_carry_branch_analysis{};
    ScalarFPRLiveness scalar_fpr_liveness{};
    ScalarCopyAnalysis scalar_copy_analysis{};
    GuestStateMap guest_state_map{};
    bool induct_tie{false};
    // Default-on exact policy: keep the common path to the host FP operation
    // plus one combined result-NaN test, and defer the x86 payload/indefinite
    // repair to shared block-local stubs. =0 restores the legacy inline
    // lowering byte-for-byte.
    bool sse_nan_coldpath{true};
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
    bool backedge_flags{false};
    bool region_branch_flags{false};
    bool region_edges_active{false};
    bool execution_trace_enabled{false};
    int execution_trace_rsp_reg{-1};
    bool cur_block_is_call{};
    std::optional<ir::Value> call_return_value{};
    std::optional<u64> call_return_pc{};
    // Set by EmitSetLocation when the next guest location is a compile-time
    // constant, cleared by every other instruction (Translate(ir::Inst*)).
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
                           bool external_edge = false);
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
    std::unique_ptr<Label> cycle_exit_reason{};
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
    std::vector<std::unique_ptr<VecNaNColdSite>> vec_nan_cold_sites{};
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
        u32 uses{};
        std::unique_ptr<Label> entry{};
    };
    std::map<u64, HostCallThunk> host_call_thunks{};
    FlagsRegsAuditEdgeKind flags_audit_block_edge{
            FlagsRegsAuditEdgeKind::Dispatcher};
    bool flags_audit_strict_advance{};
    bool flags_audit_cold{};
    std::unordered_set<u64> region_blocks{};
    std::map<u64, ir::Block*> region_block_map{};
    std::set<std::pair<u64, u64>> region_cycle_edges{};
    std::unordered_set<u64> canonical_terminal_entries{};
    std::optional<u64> next_region_block{};
    u64 placement_unit_pc{};
    bool translating_function{};

    struct FlagEmissionState {
        std::unordered_set<ir::Inst*> boolean_selects{};
        std::unordered_map<ir::Inst*, ir::Cond> direct_cond_selects{};
        std::map<ir::Inst*, ir::Value> narrow_flags_inputs{};
        std::map<ir::Inst*, NarrowComparison> narrow_compares{};
        std::map<ir::Inst*, NarrowCarryFusion> narrow_carry_fusions{};
        std::optional<DeadEdgeIntegerBranch> dead_edge_integer_branch{};
        std::optional<DeadNarrowImmediateBranch>
                dead_narrow_immediate_branch{};
        ir::Flags flags_set{};
        ir::Flags flags_clear{};
        bool save_in_nzcv{true};
        bool nzcv_dirty{false};
        ir::Inst* raw_carry_pending{};
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
        std::map<ir::Inst*, u16> pinned_memory_values{};
        std::map<ir::Inst*, SpilledMemoryOperand> spilled_memory_operands{};
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
        std::map<UnalignedAtomicFallbackKey, u32> unaligned_atomic_fallback_counts{};
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
