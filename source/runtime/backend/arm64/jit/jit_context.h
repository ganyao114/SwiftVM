//
// Created by 甘尧 on 2023/9/27.
//

#pragma once

#include <map>
#include <string>
#include <vector>
#include "aarch64/macro-assembler-aarch64.h"
#include "base/common_funcs.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/constant.h"
#include "runtime/backend/code_cache.h"
#include "runtime/backend/reg_alloc.h"
#include "runtime/common/fpcr_tax_prof.h"
#include "runtime/common/hot_coalesce_prof.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/instr.h"
#include "runtime/ir/location.h"

namespace swift::runtime::backend::arm64 {

using namespace vixl::aarch64;

struct NoneReg {};
using CPUReg = boost::variant<NoneReg, Register, VRegister>;

class JitContext : DeleteCopyAndMove {
public:
    explicit JitContext(const std::shared_ptr<Module> &module, RegAlloc& reg_alloc);

    [[nodiscard]] CPUReg Get(const ir::Value& value);
    [[nodiscard]] bool HasAllocation(const ir::Value& value);
    [[nodiscard]] bool SharesGPR(const ir::Value& left, const ir::Value& right);
    // Audit-only allocation/host-code views. These are read-only and emit no
    // guest instructions; SVM_UNIFORM_PAIR_AUDIT uses them after RA/codegen.
    [[nodiscard]] bool SharesPhysical(const ir::Value& left,
                                      const ir::Value& right);
    [[nodiscard]] std::string AllocationName(const ir::Value& value);
    [[nodiscard]] std::string DisassembleRange(u32 begin, u32 end);
    [[nodiscard]] Register R(const ir::Value& value, bool auto_cast = false);
    [[nodiscard]] XRegister X(const ir::Value& value);
    [[nodiscard]] WRegister W(const ir::Value& value);
    [[nodiscard]] VRegister V(const ir::Value& value);

    [[nodiscard]] XRegister GetTmpX();
    [[nodiscard]] Register GetTmpGPR(ir::ValueType type);
    [[nodiscard]] VRegister GetTmpV();
    // TBL with two tables encodes a consecutive register pair. Return false
    // without changing the scratch mask when pressure leaves no such pair.
    [[nodiscard]] bool TryGetConsecutiveTmpV2(VRegister& first, VRegister& second);
    // Registers unavailable for the current IR instruction: RegAlloc's live set
    // here (a conservative superset -- see register_alloc_pass.cpp), the
    // runtime's reserved registers, and every scratch GetTmpX/GetTmpV has
    // already handed out while emitting this instruction.  A register absent
    // from these holds no value the block will read again, which is what lets
    // EmitHostCall save a subset instead of everything.
    [[nodiscard]] const GPRSMask& GetLiveGPRs() const { return cur_dirty_gprs; }
    [[nodiscard]] const FPRSMask& GetLiveFPRs() const { return cur_dirty_fprs; }
    // Exclude a register from GetTmpX for the current IR instruction only.
    void ReserveTmpX(const XRegister& reg);
    // Reusable helper scratch for short, non-overlapping backend bookkeeping
    // (principally flag materialization). It is leased once per emission and
    // then reused, matching the old fixed x11 lifetime without globally
    // reserving x11.
    [[nodiscard]] XRegister GetSharedTmpX();

    // TickIR opens the per-opcode VIXL scratch contract. The translator closes
    // it immediately after the opcode emitter returns. Terminals get their own
    // contract because they share the last instruction's live mask.
    void EndInstructionScratch();
    void BeginTerminalScratch();
    void EndTerminalScratch();
    // Cold NaN stubs resume an earlier instruction and therefore may not use
    // any implicit GPR scratch at all; their explicit x13 ABI is declared on
    // the originating opcode.
    void BeginColdScratch();
    void EndColdScratch();

    void Forward(ir::Location location,
                 Label* backedge_exit = nullptr,
                 Label* self_target = nullptr);
    // Inline dispatch to a compile-time-constant guest location, for the
    // "SetLocation(imm) + ReturnToDispatch" shape a direct jmp/call decodes to.
    // Emits nothing and returns false when the target is not linkable (unknown
    // module, cross-module, BlockLink disabled); the caller then Rets to the
    // trampoline dispatcher as before. state->current_loc has already been
    // written by EmitSetLocation, so the fallback path needs no fixup.
    [[nodiscard]] bool ForwardStatic(ir::Location location);
    void ReturnToDispatcher(const Register& location);

    // --- Return Stack Buffer (RSB) emission --------------------------------
    // Called from the JitTranslator for PushRSB instructions and PopRSBHint
    // terminals when Optimizations::ReturnStackBuffer is enabled.
    //
    // Push: stores (guest_return_addr, dispatch_index) as a 16-byte frame
    //   via pre-decrement of rsb_ptr (x25).
    // Pop:  pops a frame, validates guest_return_addr against
    //   state->current_loc, and on a hit loads the compiled code pointer
    //   from the L2 dispatch table and branches directly — skipping the
    //   trampoline dispatcher.  On a miss (mismatch, empty slot, or
    //   underflow) falls through to the normal Ret-to-dispatcher path.
    //
    // Bounds guards (State::rsb_bottom / rsb_top, wired in runtime.cpp):
    //   Push skips when rsb_ptr has reached the buffer bottom (stack full) so
    //   the pre-decrement store never writes out of bounds.
    //   Pop falls back to the dispatcher when rsb_ptr has reached the empty
    //   top (more guest rets than calls) so the speculative load never reads
    //   past the buffer. Both convert an RSB imbalance into a safe dispatcher
    //   round-trip instead of a SIGSEGV.
    void EmitRSBPush(u64 guest_return_addr, u32 dispatch_index);
    void EmitRSBPop();

    // Reserves (GetOrPut) the L2 dispatch-table slot for a guest address and
    // returns its slot index (2*entry+1, pointing at the entry's value word).
    // Used by the translator to build RSB push frames at call sites.
    [[nodiscard]] u32 GetDispatchIndex(u64 guest_addr);

    void Finish();
    [[nodiscard]] u32 CurrentBufferSize();
    [[nodiscard]] ptrdiff_t GetCodeOffset(LocationDescriptor location) const;
    [[nodiscard]] bool IsUniform(const Register& reg);
    u8* Flush(const CodeBuffer& code_cache);

    [[nodiscard]] MacroAssembler& GetMasm();

    // Address-space config (JIT needs it for the memory_base bias fast-path
    // decision in EmitMemOperand).
    [[nodiscard]] const Config& GetConfig() { return module->GetAddressSpace().GetConfig(); }
    [[nodiscard]] RAShapeUnitCounters& GetRAShapeCounters() {
        return reg_alloc.RAShape();
    }

    void SetCurrent(ir::Function *function);
    void SetCurrent(ir::Block *block, bool split_backedge_entry = false);
    // Completes a split block entry after the translator has emitted the
    // published-entry branch and bound the self-only body label.
    void BeginBackedgeBody();
    void TickIR(ir::Inst* instr);

    [[nodiscard]] vixl::aarch64::Label *GetLabel(LocationDescriptor loc);

    [[nodiscard]] bool ExecProfileEnabled() const { return exec_profile_enabled; }
    void RecordExecCounter(u32 state_offset, u32 amount = 1);
    void RecordFpcrTaxCounter(FpcrTaxCounter counter);
    [[nodiscard]] bool HotCoalesceEnabled() const { return hot_coalesce_enabled; }
    void FinishHotCoalesceBlock();
    void BeginHotNaNGuard(u32 instruction_count);
    void EndHotNaNGuard();
    [[nodiscard]] bool DensityProfileEnabled() const { return density_profile_enabled; }
    [[nodiscard]] u32 DensityNaNBytes() const { return density_nan_bytes; }

private:
    void MaybeDumpHostBytes();
    void FlushLabels(VAddr target);
    void RecordHotCounter(HotCoalesceCounter counter, u32 amount = 1);
    void RecordHotSpillReload();
    void RecordHotSpillWriteback();

    // --- RegAlloc::MEM (spilled value) support ---------------------------
    // A value the linear scan could not keep in a host register lives in
    // State::spill_area (backend/context.h). Every *use* reloads it into a
    // scratch register (Ldr). A spilled *def* computes into a scratch
    // register exactly like a register-allocated def; the write-back (Str)
    // is deferred to the next TickIR / block-exit boundary because
    // JitContext never observes the moment an emitter finishes writing the
    // destination register. Reads of the just-defined value from the same
    // instruction or from the block terminal are served the scratch
    // register directly (value.Def() == cur_inst), so no stale slot is
    // ever observed.
    //
    // Limitations (spilling has never triggered on current workloads, so
    // this path is defensive):
    //  - On desktop Linux, units with any spill are reallocated with x18 in
    //    their private reserved baseline. The first scalar spill reload/write-
    //    back of each instruction uses x18; further scalar reloads use the
    //    allocator's verified headroom. Zero-spill units leave x18 dynamic.
    //    SIMD spill scratch still comes from GetTmpV and PANICs loudly if that
    //    contract is ever violated.
    //  - The spill area holds kMaxSpillSlots u64 slots; the allocator
    //    PANICs beyond that rather than overrunning the uniform buffer.
    //  - A few block terminals (Invalid/ReturnToDispatch/ReturnToHost/
    //    PopRSBHint/Switch-fallthrough) Ret directly out of the translator
    //    without touching JitContext, so a pending write-back from the
    //    block's last instruction is skipped there. Harmless in block mode
    //    (spill slots are block-local); only a function-mode spill at the
    //    final instruction into such a terminal would be affected.
    [[nodiscard]] Register SpillGPR(const ir::Value& value);
    [[nodiscard]] VRegister SpillFPR(const ir::Value& value);
    void FlushSpillWrites();
    [[nodiscard]] static bool IsFloatValue(const ir::Value& value);

    // Scratch handed to a spill reload rather than to the emitter. Budgeted
    // separately (backend::kSpillReloadHeadroom) because the linear scan pays
    // for it only in units that actually spilled.
    [[nodiscard]] XRegister GetSpillTmpX();
    [[nodiscard]] VRegister GetSpillTmpV();
    [[nodiscard]] backend::ScratchNeed CurrentBudget() const;
    void BeginVixlScratch(bool allow_pool);
    void EndVixlScratch();
    void ExcludeVixlScratch(const XRegister& reg);

    struct PendingSpillWrite {
        u16 slot;    // spill slot index
        u8 reg;      // scratch register code holding the value
        bool is_fpr;
    };

    std::shared_ptr<Module> module;
    ir::Function *cur_function{};
    ir::Block *cur_block{};
    ir::Inst *cur_inst{};
    RegAlloc& reg_alloc;
    MacroAssembler masm;
    LocationDescriptor unit_start{};
    bool unit_start_set{};
    bool host_bytes_dumped{};
    bool ra_shape_submitted{};
    bool exec_profile_enabled{};
    bool fpcr_tax_profile_enabled{};
    bool hot_coalesce_enabled{};
    bool density_profile_enabled{};
    bool density_nan_open{};
    u32 density_nan_start{};
    u32 density_nan_bytes{};
    u32 exec_access_pad{};
    u32 hot_coalesce_slot{kHotCoalesceInvalidSlot};
    std::vector<u32> hot_coalesce_slots{};
    u32 hot_code_start{};
    u32 hot_nan_start{};
    u32 hot_nan_expected{};
    bool hot_collecting{};
    bool hot_nan_open{};
    HotCoalesceUnitStatic hot_shape{};
    struct HotCodeRange {
        u32 begin{};
        u32 end{};
    };
    std::vector<HotCodeRange> hot_probe_ranges{};
    std::vector<HotCodeRange> hot_nan_ranges{};
    std::array<ir::HostGPR, ARM64_MAX_X_REGS> spilled_gprs;
    std::array<ir::HostGPR, ARM64_MAX_X_REGS> spilled_fprs;
    std::map<LocationDescriptor, Label> labels;
    // value id -> scratch reg code for the current instruction's spilled
    // def (repeated def accesses within one instruction must return the
    // same register); cleared at every TickIR.
    std::map<u32, u8> spill_def_scratch;
    // value id -> scratch reg code holding a reloaded *use* of a spilled
    // value, for the current instruction only; also cleared at every TickIR.
    // Memoizing reloads is not just a code-size win: it is what bounds the
    // reload demand of one instruction to the number of distinct values it
    // names, which is the bound backend::kSpillReloadHeadroom encodes.
    std::map<u32, u8> spill_use_scratch;
    std::vector<PendingSpillWrite> pending_spill_writes;

    GPRSMask cur_dirty_gprs{};
    GPRSMask cur_dirty_fprs{};
    // cur_dirty_* as of the last TickIR. The difference in marked bits is how
    // much scratch this instruction has consumed so far, which is what
    // backend::ScratchBudget bounds.
    GPRSMask tick_dirty_gprs{};
    FPRSMask tick_dirty_fprs{};
    u32 spill_tmp_gprs{};
    u32 spill_tmp_fprs{};
#if defined(__linux__) && !defined(__ANDROID__)
    // x18 is already marked in this spilling unit's pool baseline, so using it does not
    // contribute to spill_tmp_gprs (that counter only discounts newly marked
    // dynamic reload registers from the emitter scratch budget).
    bool spill_scratch_in_use{};
#endif
    int shared_tmp_gpr{-1};
    bool auxiliary_scratch{};
    std::unique_ptr<UseScratchRegisterScope> vixl_scratch_scope{};
};

}  // namespace swift::runtime::backend::arm64
