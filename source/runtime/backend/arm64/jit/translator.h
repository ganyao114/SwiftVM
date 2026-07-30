#pragma once

#include <memory>
#include <optional>
#include <vector>
#include "base/common_funcs.h"
#include "jit_context.h"
#include "runtime/backend/code_cache.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/atomic_rmw.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {

// Host GPR that SVM_X87_TOPVIRT dedicates to the cached x87 TOP.
//
// THE ONE HARD CONSTRAINT: the code must still be *free* in the mask the
// trampoline hands to the allocator, i.e. absent from the runtime ABI set and
// from Config::buffers_static_alloc. This is why it is NOT x20. The x86
// frontend statically pins guest RBX to x20 (translator/x86/translator.cpp
// arm64_backend_regs_map), TrampolinesArm64::Build had therefore already
// marked it, the runtime's `gprs.Mark(20)` was a silent no-op, and the
// emitter's `mov w20, ...` landed directly in guest RBX -- guest RBX read back
// as a TOP value 0..7 in ~92% of the x87 fuzz iterations. Reserving is now
// guarded by an ASSERT at the site in runtime.cpp so a future static-register
// map cannot re-create that quietly.
//
// x22 is `arg` in defines.h, named only by the mutually exclusive
// enable_asm_interp trampoline path, and never by generated code.
//
// Callee-saved is NOT a requirement, though x22 happens to be one. Mutating
// this to a caller-saved code (x2) survived every suite, and the reason is
// real rather than a coverage gap: the TOP cache is dead at block boundaries
// (BeginX87TopVirtBlock clears x87_top_cache_valid), so the only call it can
// live across is EmitHostCall, whose save set is context.GetLiveGPRs() --
// which already contains the runtime's reserved registers. Codes <= 17 are
// therefore preserved across a helper call too. x16/x17 remain unusable for a
// different reason (vixl's UseScratchRegisterScope).
constexpr u32 kX87TopVirtGPR = 22;

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
    explicit JitTranslator(JitContext& ctx);

    void Translate(ir::Block *block);

    void Translate(ir::HIRFunction *function);

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
    VRegister GetVecScalarOperand(ir::Value value, u32 lane_bits);

    struct X87TopExpression {
        // relative=true means (translation-unit entry TOP + value) & 7.
        // Otherwise value is an architectural absolute TOP.
        bool relative{true};
        u8 value{};

        bool operator==(const X87TopExpression&) const = default;
    };

    struct X87TopTransfer {
        // Unknown transfers (currently FLDENV) make the whole function
        // ineligible. reset=true means the input TOP is discarded.
        bool known{true};
        bool reset{};
        u8 value{};
    };

    struct X87TopBlockInfo {
        bool eligible{};
        X87TopExpression entry{};
        X87TopExpression exit{};
    };

    [[nodiscard]] X87TopTransfer AnalyzeX87TopTransfer(ir::Block* block) const;
    [[nodiscard]] X87TopExpression ApplyX87TopTransfer(
            const X87TopExpression& entry,
            const X87TopTransfer& transfer) const;
    void AnalyzeX87TopVirt(ir::Block* block);
    void AnalyzeX87TopVirt(ir::HIRFunction* function);
    void BeginX87TopVirtBlock(ir::Block* block);
    void PrepareX87TopCache(ir::Inst* inst);
    void FinishX87TopCache(ir::Inst* inst);

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

        [[nodiscard]] bool Null() const {
            return set == ir::Flags::None && clear == ir::Flags::None;
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
    void EmitTerminal(const ir::Terminal &terminal);

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

    // Merge pending guest flags kept in host NZCV into the flags register
    void MergeNZCV();

    // Restore host NZCV from the flags register (clobbers ip)
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
    // Helpers such as XSAVE/XRSTOR dereference ThreadContext64 directly and
    // therefore need the statically resident SIMD uniforms synchronized at
    // that exact call boundary.
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

    // ARM and x86 can choose different signs/payloads when a packed FP
    // operation consumes a NaN. Normalize each lane to x86's first-NaN,
    // quiet-preserving rule after the NEON arithmetic instruction.
    void EmitVecFloatNaNFixup(const VRegister &result,
                              const VRegister &left,
                              const VRegister &right,
                              u32 lane_bits,
                              u32 lane_count = 0);
    void EmitVecFScalarBinaryTied(ir::Inst *inst, u32 lane_bits);
    VRegister PreserveNaNColdSource(const VRegister &source,
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

    void FlushFlags();

    JitContext &context;
    MacroAssembler &masm;
    ir::Block *cur_block{};
    ir::Inst *cur_instr{};
    BitVector disable_instructions{};
    std::map<ir::Inst *, Label> local_labels{};
    std::map<ir::Inst *, Condition> local_conditions{};
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
    bool shift_imm_fast{true};
    // W29 lowering: signed/unsigned narrow loads consume their
    // extension destination directly, and GetOperand computes into its
    // allocated address register. SVM_MEM_NARROW_FUSE=0 restores the old
    // load+extend and temporary+transport-move shapes.
    // Safe by construction after the GetOperand RA-tie fix: the emitter only
    // peels when the allocator transferred register ownership (SharesGPR).
    bool mem_narrow_fuse{true};
    // TOP virtualization is deliberately a second opt-in layered on the
    // reduced x87 JIT. It stays default-off until host Unicorn qualification
    // has covered the new block/function paths.
    bool x87_topvirt_requested{false};
    bool x87_topvirt_function_eligible{false};
    bool translating_function{false};
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
    bool x87_top_block_codegen_enabled{false};
    bool x87_top_cache_valid{false};
    bool x87_top_cache_for_current{false};
    std::map<ir::Block*, X87TopBlockInfo> x87_top_blocks{};
    std::vector<std::unique_ptr<VecNaNColdSite>> vec_nan_cold_sites{};
};

}
