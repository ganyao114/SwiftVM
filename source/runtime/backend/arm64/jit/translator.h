#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>
#include "base/common_funcs.h"
#include "jit_context.h"
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

class JitTranslator {
public:
    struct BackedgeBlockMetadata {
        u64 guest_start{};
        u32 host_begin{};
        u32 host_end{};
        u32 recovery_offset{};
    };

    explicit JitTranslator(JitContext& ctx);

    void Translate(ir::Block *block);

    void Translate(ir::HIRFunction *function);

    [[nodiscard]] const std::vector<BackedgeBlockMetadata>&
    GetBackedgeBlockMetadata() const {
        return backedge_block_metadata;
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
                              bool structured_guest_ea = false);

#define INST(name, ...) void Emit##name(ir::Inst *inst);
#include "runtime/ir/ir.inc"
#undef INST

private:
    void EmitExecutionTrace(u64 guest_rip);
    [[nodiscard]] bool ReproveCoalescedHostWrite(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveCoalescedHostRead(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveCoalescedHostFPRWrite(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveCoalescedHostFPRRead(ir::Inst* inst) const;
    [[nodiscard]] bool ReproveCachedConstAddress(ir::Inst* inst) const;

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

    struct BackedgeFlagsPlan {
        bool optimized{true};
        bool self_is_then{};
        ir::Location self_target{};
        ir::Location cold_target{};
        u8 carry_inverted{};
        HostFlags requested{};
        ir::Inst* polarity_load{};
        ir::Inst* polarity_store{};
        ir::Inst* final_advance{};
        std::unique_ptr<Label> local_entry{std::make_unique<Label>()};
        std::unique_ptr<Label> external_entry{std::make_unique<Label>()};
        std::unique_ptr<Label> cold_exit{std::make_unique<Label>()};
        std::unique_ptr<Label> fault_recovery{std::make_unique<Label>()};
        bool cold_referenced{};
    };

    [[nodiscard]] std::unique_ptr<BackedgeFlagsPlan>
    PlanBackedgeFlags(ir::Block* block);
    [[nodiscard]] bool EmitBackedgeFlagsTerminal(const ir::Terminal& terminal);
    void EmitBackedgeMaterialize(const BackedgeFlagsPlan& plan);
    void EmitBackedgeColdPaths();
    [[nodiscard]] static bool PreservesHostNZCV(ir::OpCode op);
    [[nodiscard]] static bool MayFaultOrObserve(ir::OpCode op);
    VRegister GetVecScalarOperand(ir::Value value, u32 lane_bits);

    void AcquireUnalignedAtomicLock(const Register& scratch);
    void ReleaseUnalignedAtomicLock();
    void EmitPlainAtomicLoad(ir::ValueType type,
                             const Register& result,
                             const Register& address);
    void EmitPlainAtomicStore(ir::ValueType type,
                              const Register& value,
                              const Register& address);
    void EmitAtomicRMWValue(ir::AtomicRMWOp op,
                            ir::ValueType type,
                            const Register& output,
                            const Register& old,
                            ir::Value operand,
                            ir::Value carry);

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
    };

    void Translate(ir::Inst *inst);

    // Terminals
    void EmitTerminal(const ir::Terminal &terminal,
                      LinkSiteKind direct_link_kind = LinkSiteKind::Unconditional);
    void PrepareRegionEdges(ir::HIRFunction* function);
    void CollectRegionTargets(const ir::Terminal& terminal,
                              std::vector<u64>& targets) const;
    [[nodiscard]] std::optional<ir::Location>
    RegionLeafTarget(const ir::Terminal& terminal) const;
    [[nodiscard]] bool IsRegionInternalEdge(ir::Location target) const;
    [[nodiscard]] bool IsRegionCycleEdge(ir::Location target) const;
    [[nodiscard]] bool HasRegionCycleEdgeFromCurrent() const;
    [[nodiscard]] bool CanRegionFallThrough(ir::Location target) const;
    void EmitRegionEdge(ir::Location target,
                        bool fallthrough = false,
                        bool record_edge_counters = true);
    [[nodiscard]] bool EmitRegionIf(const ir::terminal::If& terminal,
                                    bool allow_fallthrough);
    [[nodiscard]] bool EmitRegionCondition(const ir::terminal::Condition& terminal,
                                           bool allow_fallthrough);
    [[nodiscard]] bool HasSelfEdge(const ir::Terminal& terminal) const;
    [[nodiscard]] bool IsSelfEdge(ir::Location target) const;
    void EmitBackedgeExitStub();

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
    [[nodiscard]] static bool IsCompactFCmp(ir::Value value);

    // Merge pending guest flags kept in host NZCV into the flags register
    void MergeNZCV();

    // Restore host NZCV from the flags register (uses the emission's shared scratch).
    void LoadNZCVFromFlags();

    // Merge host N/Z into the flags register and clear stale C/V (x86 logical ops)
    void MergeLogicalFlagsNZ(ir::Flags requested);

    // Compute N/Z from a result value and merge them (for ops without a flag setting form)
    void SaveLogicalResultFlags(Register &result, ir::ValueType type, const PseudoFlags &pseudo);

    // Materialize an IR operand into a scratch register
    Register MaterializeOperand(const Operand &operand, ir::ValueType type);

    // Guest address virtualization (Config::memory_base): the pt register
    // holds the guest->host bias for the whole guest run. These wrap a guest
    // base register into [base + pt] (+ optional immediate). atomic=true
    // folds the bias into a scratch register (for instructions without
    // register-offset forms). Only called when use_memory_base is set;
    // identity mode never pays for this.
    MemOperand BiasMem(const Register &base, bool atomic = false);
    MemOperand BiasMem(const Register &base, s64 imm, bool atomic = false);

    // Bounded guest window (Config::guest_addr_mask): materializes the host
    // address of a guest address into `dst` as (guest & mask) + pt. Used by
    // the forms that need a single base register (exclusives, host calls).
    // With a 32-bit window this is one instruction — the same Add the
    // unbounded path emitted — because UXTW does the truncation for free.
    void EmitGuestToHost(const Register &dst, const Register &guest_addr);

    // Host C-ABI call helper (saves/restores caller-saved allocated GPRs)
    void EmitHostCall(const ir::Lambda &lambda,
                      const std::vector<ir::DataClass> &args,
                      bool has_result,
                      const Register &result);
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

    // ARM and x86 can choose different signs/payloads when a packed FP
    // operation consumes a NaN. Normalize each lane to x86's first-NaN,
    // quiet-preserving rule after the NEON arithmetic instruction.
    void EmitVecFloatNaNFixup(const VRegister &result,
                              const VRegister &left,
                              const VRegister &right,
                              u32 lane_bits,
                              u32 lane_count,
                              ir::Inst *inst);
    void EmitVecFScalarBinaryTied(ir::Inst *inst, u32 lane_bits);
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

    void QueueVecNaNColdPath(VecNaNColdKind kind,
                             const VRegister &result,
                             const VRegister &left,
                             const VRegister &right = NoVReg);
    void EmitVecNaNColdPaths();
    void EmitVecNaNColdHandler(VecNaNColdKind kind);

    [[nodiscard]] PseudoFlags GetPseudoFlags(ir::Inst *inst);

    [[nodiscard]] bool MatchMemoryOffsetCase(ir::Inst *inst);
    [[nodiscard]] std::optional<u64> MatchInductionImmediate(ir::Inst *inst);
    void PlanInductionTies(ir::Block *block);

    void FlushFlags();

    JitContext &context;
    MacroAssembler &masm;
    ir::Block *cur_block{};
    ir::Inst *cur_instr{};
    BitVector disable_instructions{};
    std::map<ir::Inst *, Label> local_labels{};
    std::map<ir::Inst *, Condition> local_conditions{};
    // ZeroExtend32To64 values whose sole consumer is a W55 full pinned write.
    // Their producer emits nothing; EmitSetHostGPR reads the original W value.
    std::unordered_set<ir::Inst*> fused_pin_zext32{};
    // Narrow mapped reads whose single audited consumer can use the pinned W
    // register directly (for example CL masking and U32 XOR).
    std::map<ir::Inst*, u16> fused_pin_gpr_reads{};
    ir::Flags flags_set{};
    ir::Flags flags_clear{};
    bool save_in_nzcv{true};
    bool nzcv_dirty{false};
    // Which host NZCV bits were actually requested by SaveFlags since the
    // last MergeNZCV. Only these bits are merged; the rest keep their
    // existing value in the flags register (so a ClearFlags(CF) between
    // two flag-setting instructions is not overwritten by the merge).
    HostFlags nzcv_requested{};
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
    bool induct_tie{false};
    // Default-off aggressive SSE/AVX floating-point policy: keep the raw NEON
    // result instead of repairing x86 NaN payload priority and quieting.
    bool sse_nan_fast{false};
    // Default-on exact policy: keep the common path to the host FP operation
    // plus one combined result-NaN test, and defer the x86 payload/indefinite
    // repair to shared block-local stubs. =0 restores the legacy inline
    // lowering byte-for-byte; SVM_SSE_NAN_FAST still wins when explicitly set.
    bool sse_nan_coldpath{true};
    // FEAT_AFP + FPCR.NEP is active for guest code, so scalar Advanced SIMD
    // instructions can update a tied destination's lane 0 in place.
    bool sse_scalar_insert{false};
    bool sse_afp_nan{false};
    // W90 phase-2 diagnostics. skip_switch is intentionally unsafe and only
    // affects direct helper FPCR bracketing when explicitly requested.
    bool fpcr_tax_skip_switch{false};
    bool fpcr_tax_timing{false};
    bool shift_imm_fast{true};
    // W29 lowering: signed/unsigned narrow loads consume their
    // extension destination directly, and GetOperand computes into its
    // allocated address register. SVM_MEM_NARROW_FUSE=0 restores the old
    // load+extend and temporary+transport-move shapes.
    // Safe by construction after the GetOperand RA-tie fix: the emitter only
    // peels when the allocator transferred register ownership (SharesGPR).
    bool mem_narrow_fuse{true};
    // 只处理寻址 EA：固定别名的末次使用可转交给 GetOperand result；
    // identity frontend 另把简单复合地址直接保留到 memory IR。
    bool addr_ea_tie{true};
    // 绝对地址常量直接物化到 GetOperand 的分配结果，避免临时寄存器搬运。
    bool abs_const_mat{false};
    bool backedge_latch{false};
    bool backedge_flags{false};
    bool region_edges_active{false};
    bool execution_trace_enabled{false};
    int execution_trace_rsp_reg{-1};
    bool cur_block_is_call{};
    // Set by EmitSetLocation when the next guest location is a compile-time
    // constant, cleared by every other instruction (Translate(ir::Inst*)).
    // A ReturnToDispatch/Invalid terminal reached with this set is a direct
    // jmp/call: the dispatch-table slot for that exact address can be read
    // inline instead of returning to the trampoline's hash lookup.
    std::optional<u64> static_next_loc{};
    // Emits the inline dispatch for `static_next_loc`; returns false when no
    // static target is known and the caller must Ret to the dispatcher.
    bool EmitStaticForward();
    // Dynamic SetLocation is remembered only while it remains the final body
    // instruction. The terminal can therefore reuse its still-live register
    // without extending an SSA lifetime or reloading State::current_loc.
    std::optional<ir::Value> dynamic_next_loc{};
    bool EmitIndirectForward();
    std::unique_ptr<Label> backedge_exit_label{};
    bool backedge_exit_referenced{};
    std::unique_ptr<BackedgeFlagsPlan> backedge_flags_plan{};
    u32 backedge_host_begin{};
    u32 backedge_host_end{};
    std::vector<BackedgeBlockMetadata> backedge_block_metadata{};
    std::vector<std::unique_ptr<VecNaNColdSite>> vec_nan_cold_sites{};
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
    std::unordered_set<u64> region_blocks{};
    std::set<std::pair<u64, u64>> region_cycle_edges{};
    std::optional<u64> next_region_block{};
    u32 region_block_edges{};
    u32 region_block_cycles{};
    u32 region_block_fallthroughs{};
    u32 region_block_local_branch_bytes{};
};

}
