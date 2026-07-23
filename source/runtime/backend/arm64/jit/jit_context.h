//
// Created by 甘尧 on 2023/9/27.
//

#pragma once

#include <map>
#include <vector>
#include "aarch64/macro-assembler-aarch64.h"
#include "base/common_funcs.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/constant.h"
#include "runtime/backend/code_cache.h"
#include "runtime/backend/reg_alloc.h"
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
    [[nodiscard]] Register R(const ir::Value& value, bool auto_cast = false);
    [[nodiscard]] XRegister X(const ir::Value& value);
    [[nodiscard]] WRegister W(const ir::Value& value);
    [[nodiscard]] VRegister V(const ir::Value& value);

    [[nodiscard]] XRegister GetTmpX();
    [[nodiscard]] Register GetTmpGPR(ir::ValueType type);
    [[nodiscard]] VRegister GetTmpV();

    void Forward(ir::Location location);
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
    void EmitRSBPush(u64 guest_return_addr, u32 dispatch_index);
    void EmitRSBPop();

    void Finish();
    [[nodiscard]] u32 CurrentBufferSize();
    [[nodiscard]] bool IsUniform(const Register& reg);
    u8* Flush(const CodeBuffer& code_cache);

    [[nodiscard]] MacroAssembler& GetMasm();

    // Address-space config (JIT needs it for the memory_base bias fast-path
    // decision in EmitMemOperand).
    [[nodiscard]] const Config& GetConfig() { return module->GetAddressSpace().GetConfig(); }

    void SetCurrent(ir::Function *function);
    void SetCurrent(ir::Block *block);
    void TickIR(ir::Inst* instr);

    [[nodiscard]] vixl::aarch64::Label *GetLabel(LocationDescriptor loc);

private:
    void BlockLinkStub(ir::Location location);

    void FlushLabels(VAddr target);

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
    //  - Reload/write-back scratch registers come from GetTmpX/GetTmpV;
    //    under total register exhaustion those PANIC (loudly) instead of
    //    silently corrupting state. A fully robust spill needs one
    //    permanently reserved scratch register in the trampoline mask.
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
    std::array<ir::HostGPR, ARM64_MAX_X_REGS> spilled_gprs;
    std::array<ir::HostGPR, ARM64_MAX_X_REGS> spilled_fprs;
    std::map<LocationDescriptor, Label> labels;
    // value id -> scratch reg code for the current instruction's spilled
    // def (repeated def accesses within one instruction must return the
    // same register); cleared at every TickIR.
    std::map<u32, u8> spill_def_scratch;
    std::vector<PendingSpillWrite> pending_spill_writes;

    GPRSMask cur_dirty_gprs{};
    GPRSMask cur_dirty_fprs{};
};

}  // namespace swift::runtime::backend::arm64