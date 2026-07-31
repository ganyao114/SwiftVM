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

// x87 TOP has no dedicated host register. Every inline X87Op extracts it into
// one of that opcode's ordinary allocator-protected scratch GPRs: one UBFX
// when the emitter already holds FSW, or LDRH+UBFX when it does not. Stack
// effects are merged into the emitter's existing FSW update and written back
// architecturally before the instruction ends, so no TOP value is live across
// an IR instruction, block edge, or helper call.
//
// This is a structural invariant, not merely the default configuration. The
// retired SVM_X87_TOPVIRT cache once named a fixed GPR; its first implementation
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
    std::vector<std::unique_ptr<VecNaNColdSite>> vec_nan_cold_sites{};
};

}
