//
// Created by 甘尧 on 2023/9/15.
//

#include "jit_context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

// The spill slot count reserved in State must match the allocator's limit.
static_assert(kMaxSpillSlots == sizeof(State::spill_area) / sizeof(u64),
              "spill slot count mismatch between reg_alloc.h and context.h");

JitContext::JitContext(const std::shared_ptr<Module>& module, RegAlloc& reg_alloc)
        : module(module), reg_alloc(reg_alloc) {}

bool JitContext::HasAllocation(const ir::Value& value) {
    return reg_alloc.ValueType(value) != RegAlloc::NONE;
}

bool JitContext::IsFloatValue(const ir::Value& value) {
    auto type = value.Type();
    return type >= ir::ValueType::V8 && type <= ir::ValueType::V256;
}

CPUReg JitContext::Get(const ir::Value& value) {
    switch (reg_alloc.ValueType(value)) {
        case RegAlloc::GPR:
            return X(value);
        case RegAlloc::FPR:
            return V(value);
        case RegAlloc::MEM:
            // Spilled value: reload from (or def-scratch for) its slot.
            if (IsFloatValue(value)) {
                return SpillFPR(value);
            }
            return SpillGPR(value);
        default:
            ASSERT_MSG(false, "value has no register allocation");
    }
    return {};
}

Register JitContext::R(const ir::Value& value, bool auto_cast) {
    if (value.Type() == ir::ValueType::U64) {
        return X(value);
    } else {
        if (auto_cast && value.Def()->IsGetHostRegOperation()) {
            if (value.Type() == ir::ValueType::U8) {
                auto tmp = GetTmpX();
                __ Ubfx(tmp.W(), W(value), 0, 8);
                return tmp.W();
            } else if (value.Type() == ir::ValueType::U16) {
                auto tmp = GetTmpX();
                __ Ubfx(tmp.W(), W(value), 0, 16);
                return tmp.W();
            } else {
                return W(value);
            }
        } else {
            return W(value);
        }
    }
}

XRegister JitContext::X(const ir::Value& value) {
    if (reg_alloc.ValueType(value) == RegAlloc::MEM) {
        return XRegister(SpillGPR(value).GetCode());
    }
    auto reg = reg_alloc.ValueGPR(value);
    return XRegister(reg.id);
}

WRegister JitContext::W(const ir::Value& value) {
    if (reg_alloc.ValueType(value) == RegAlloc::MEM) {
        return WRegister(SpillGPR(value).GetCode());
    }
    auto reg = reg_alloc.ValueGPR(value);
    return WRegister(reg.id);
}

VRegister JitContext::V(const ir::Value& value) {
    if (reg_alloc.ValueType(value) == RegAlloc::MEM) {
        return SpillFPR(value);
    }
    auto reg = reg_alloc.ValueFPR(value);
    return VRegister::GetVRegFromCode(reg.id);
}

Register JitContext::SpillGPR(const ir::Value& value) {
    const auto slot = reg_alloc.ValueMem(value);
    ASSERT_MSG(slot.offset < kMaxSpillSlots, "spill slot beyond reserved area");
    const u32 offset = state_offset_spill_area + slot.offset * sizeof(u64);
    if (value.Defined() && value.Def() == cur_inst) {
        // Def access: nothing to reload yet. Hand out (or reuse) the
        // scratch register the emitter will compute into and queue the
        // deferred write-back (flushed at the next TickIR / block exit).
        if (auto it = spill_def_scratch.find(value.Id()); it != spill_def_scratch.end()) {
            return XRegister(it->second);
        }
        auto tmp = GetTmpX();
        spill_def_scratch.emplace(value.Id(), static_cast<u8>(tmp.GetCode()));
        pending_spill_writes.push_back({slot.offset, static_cast<u8>(tmp.GetCode()), false});
        return tmp;
    }
    // Use access: reload from the spill slot. Any write-back of a value
    // defined by an earlier instruction has already been flushed at this
    // instruction's TickIR, so the slot is current.
    auto tmp = GetTmpX();
    __ Ldr(tmp, MemOperand(state, offset));
    return tmp;
}

VRegister JitContext::SpillFPR(const ir::Value& value) {
    const auto slot = reg_alloc.ValueMem(value);
    // A spilled SIMD value occupies two consecutive u64 slots (16 bytes).
    ASSERT_MSG(slot.offset + 1 < kMaxSpillSlots, "spill slot beyond reserved area");
    const u32 offset = state_offset_spill_area + slot.offset * sizeof(u64);
    if (value.Defined() && value.Def() == cur_inst) {
        if (auto it = spill_def_scratch.find(value.Id()); it != spill_def_scratch.end()) {
            return VRegister::GetVRegFromCode(it->second);
        }
        auto tmp = GetTmpV();
        spill_def_scratch.emplace(value.Id(), static_cast<u8>(tmp.GetCode()));
        pending_spill_writes.push_back({slot.offset, static_cast<u8>(tmp.GetCode()), true});
        return tmp;
    }
    auto tmp = GetTmpV();
    __ Ldr(tmp.Q(), MemOperand(state, offset));
    return tmp;
}

void JitContext::FlushSpillWrites() {
    for (auto& write : pending_spill_writes) {
        const u32 offset = state_offset_spill_area + write.slot * sizeof(u64);
        if (write.is_fpr) {
            __ Str(VRegister::GetVRegFromCode(write.reg).Q(), MemOperand(state, offset));
        } else {
            __ Str(XRegister(write.reg), MemOperand(state, offset));
        }
    }
    pending_spill_writes.clear();
}

XRegister JitContext::GetTmpX() {
    if (auto alloc = cur_dirty_gprs.GetFirstClear(); alloc >= 0) {
        cur_dirty_gprs.Mark(alloc);
        return XRegister(alloc);
    }
    PANIC("No free temporary GPR");
}

Register JitContext::GetTmpGPR(ir::ValueType type) {
    auto x = GetTmpX();
    return type == ir::ValueType::U64 ? x : x.W();
}

VRegister JitContext::GetTmpV() {
    if (auto alloc = cur_dirty_fprs.GetFirstClear(); alloc >= 0) {
        cur_dirty_fprs.Mark(alloc);
        return VRegister::GetVRegFromCode(alloc);
    }
    PANIC("No free temporary VREG");
}

void JitContext::Forward(ir::Location location) {
    ASSERT(cur_block);
    // Block exit: land any pending spill write-back before the transfer
    // (a spilled value defined by the block's last instruction may be live
    // into the target block in function mode).
    FlushSpillWrites();
    auto self_forward = location == cur_block->GetStartLocation();
    if (!self_forward && cur_function) {
        self_forward = location == cur_function->GetStartLocation();
    }
    if (self_forward) {
        auto self_label{GetLabel(location.Value())};
        __ B(self_label);
    } else {
        auto target_module = module->GetAddressSpace().GetModule(location.Value());
        if (!target_module) {
            // Module miss
            __ Mov(ipw, static_cast<u32>(HaltReason::ModuleMiss));
            __ Str(ipw, MemOperand(state, state_offset_halt_reason));
            __ Ret();
            return;
        }

        const bool self_module_forward{module == target_module};
        const ModuleConfig& module_config{module->GetModuleConfig()};
        const ModuleConfig& target_module_config{target_module->GetModuleConfig()};

        bool direct_link{
                (self_module_forward && module_config.HasOpt(Optimizations::DirectBlockLink)) ||
                target_module_config.read_only};

        if (direct_link) {
            bool in_function{false};
            if (cur_function) {
                in_function = cur_function->FindBlock(location.Value());
            }
            if (in_function) {
                // Intra-function branch: the target label is bound within
                // this same code buffer by SetCurrent(block).
                __ B(GetLabel(location.Value()));
            } else {
                if (auto code = target_module->GetJitCache(location.Value()); code) {
                    // Target already compiled: branch directly via Mov+Br.
                    // A plain B(label) cannot reach across code buffers;
                    // Mov+Br is position-independent and SMC-safe (the
                    // address is loaded at JIT time, not backpatched).
                    __ Mov(ip, reinterpret_cast<VAddr>(code));
                    __ Br(ip);
                } else {
                    // Target not yet compiled: fall back to the indirect
                    // link (dispatch table) if available, otherwise the
                    // dispatcher.  This avoids the BlockLinkStub backpatch
                    // path which is not SMC-safe (the patched B would
                    // dangle after invalidation).
                    if (self_module_forward &&
                        module_config.HasOpt(Optimizations::BlockLink)) {
                        u32 dispatcher_index = target_module->GetDispatchIndex(location);
                        Label empty_slot;
                        __ Mov(ipw, dispatcher_index);
                        __ Ldr(ip, MemOperand(cache, ip, LSL, 3));
                        __ Cbz(ip, &empty_slot);
                        __ Br(ip);
                        __ Bind(&empty_slot);
                        __ Mov(ip, location.Value());
                        __ Str(ip, MemOperand(state, state_offset_current_loc));
                        __ Ret();
                    } else {
                        __ Mov(ip, location.Value());
                        __ Str(ip, MemOperand(state, state_offset_current_loc));
                        __ Ret();
                    }
                }
            }
        } else if (self_module_forward && module_config.HasOpt(Optimizations::BlockLink)) {
            // Indirect link: jump straight to the target through the module's
            // dispatch-table slot. GetDispatchIndex reserves the slot (value 0)
            // for `location`; once the target is translated, PushCodeCache fills
            // that exact slot with its code pointer, so later forwards to the
            // same target branch directly to it and skip the dispatcher entirely.
            //
            // Empty-slot safety: if the target has not been translated yet, the
            // slot still reads 0. Do NOT `br 0x0` (that crashed before this
            // fix). Fall back exactly like the "do not link" path below: write
            // the target location into current_loc and Ret to the trampoline.
            // halt_reason is 0 here (a normal block-end forward never sets it),
            // so the trampoline's post-block "Ldr w0, halt_reason; Cbz w0,
            // code_dispatcher" re-enters the dispatcher, which re-runs the L1/L2
            // lookup for the target — hitting it if it was compiled in the
            // meantime, or CodeMiss-ing back to the host to translate it (the
            // slot then gets filled, so the next forward links directly). The
            // current_loc write is essential: the dispatcher re-dispatches on
            // current_loc, so without it we would re-look-up the *source* block
            // and loop instead of reaching the target.
            u32 dispatcher_index = target_module->GetDispatchIndex(location);
            Label empty_slot;
            __ Mov(ipw, dispatcher_index);
            __ Ldr(ip, MemOperand(cache, ip, LSL, 3));
            __ Cbz(ip, &empty_slot);
            __ Br(ip);
            // empty slot -> back to the dispatcher for the target location.
            __ Bind(&empty_slot);
            __ Mov(ip, location.Value());
            __ Str(ip, MemOperand(state, state_offset_current_loc));
            __ Ret();
        } else {
            // do not link
            __ Mov(ip, location.Value());
            __ Str(ip, MemOperand(state, state_offset_current_loc));
            __ Ret();
        }
    }
}

void JitContext::ReturnToDispatcher(const Register& location) {
    // Block exit: see Forward.
    FlushSpillWrites();
    __ Str(location, MemOperand(state, state_offset_current_loc));
    __ Ret();
}

// --- Return Stack Buffer (RSB) -------------------------------------------
// The RSB is a small stack of 16-byte frames in host memory, pointed to by
// the reserved rsb_ptr register (x25).  Each frame holds:
//   offset 0: guest_location  (u64) — the guest return address (validation)
//   offset 8: dispatch_index  (u64) — L2 dispatch-table slot for fast lookup
//
// Push (guest call): pre-decrement rsb_ptr by 16 and store the frame.
// Pop  (guest ret):  load the frame, compare guest_location with the actual
//   return target in state->current_loc; on a hit, load the compiled code
//   pointer from the L2 dispatch table and branch directly — skipping the
//   trampoline dispatcher round-trip entirely.  On a miss (mismatch, empty
//   slot, or underflow) fall through to the normal Ret-to-dispatcher path.

void JitContext::EmitRSBPush(u64 guest_return_addr, u32 dispatch_index) {
    // ip0 (x16) = guest return address, ip1 (x17) = dispatch table slot.
    __ Mov(ip0, guest_return_addr);
    __ Mov(ip1, static_cast<u64>(dispatch_index));
    // Pre-decrement push: rsb_ptr -= 16, then store the pair.
    __ Stp(ip0, ip1, MemOperand(rsb_ptr, -16, PreIndex));
}

void JitContext::EmitRSBPop() {
    Label rsb_miss;
    // Load the predicted guest return address from the top RSB frame.
    __ Ldr(ip0, MemOperand(rsb_ptr, 0));
    // Load the actual return target (set by the frontend's ret instruction).
    __ Ldr(ip1, MemOperand(state, state_offset_current_loc));
    __ Cmp(ip0, ip1);
    __ B(&rsb_miss, ne);
    // Prediction hit: load the L2 dispatch-table slot index and look up the
    // compiled code pointer.  cache (x27) holds the L2 table base at all
    // times (loaded once at runtime entry).
    __ Ldr(ip, MemOperand(rsb_ptr, 8));   // ip (x11) = dispatch_index
    __ Ldr(ip2, MemOperand(cache, ip, LSL, 3));  // ip2 (x14) = code ptr
    __ Cbz(ip2, &rsb_miss);               // empty slot → fallback
    // Commit the pop and jump directly to the target's compiled code.
    __ Add(rsb_ptr, rsb_ptr, 16);
    __ Br(ip2);
    // Miss / underflow: fall back to the trampoline dispatcher.
    __ Bind(&rsb_miss);
    __ Ret();
}

void JitContext::Finish() { __ FinalizeCode(); }

u8* JitContext::Flush(const CodeBuffer& code_cache) {
    FlushLabels(reinterpret_cast<VAddr>(code_cache.exec_data));
    Finish();
    std::memcpy(code_cache.rw_data, masm.GetBuffer()->GetStartAddress<u8*>(), code_cache.size);
    code_cache.Flush();
    return code_cache.exec_data;
}

u32 JitContext::CurrentBufferSize() { return __ GetBuffer() -> GetSizeInBytes(); }

bool JitContext::IsUniform(const Register& reg) {
    auto &uniform_info = module->GetAddressSpace().GetUniformInfo();
    if (reg.IsV()) {
        return uniform_info.uni_fprs.Get(reg.GetCode());
    } else {
        return uniform_info.uni_gprs.Get(reg.GetCode());
    }
}

void JitContext::SetCurrent(ir::Block* block) {
    cur_block = block;
    auto label = GetLabel(block->GetStartLocation().Value());
    __ Bind(label);
}

void JitContext::SetCurrent(ir::Function* function) {
    cur_function = function;
    auto label = GetLabel(function->GetStartLocation().Value());
    __ Bind(label);
}

void JitContext::TickIR(ir::Inst* instr) {
    // Deferred spill write-back for the previous instruction's def (if
    // any) must land before anything else: from this instruction on the
    // scratch register holding it may be reused, and uses reload from the
    // slot.
    FlushSpillWrites();
    spill_def_scratch.clear();
    cur_inst = instr;
    reg_alloc.SetCurrent(instr);
    cur_dirty_gprs = reg_alloc.GetDirtyGPR();
    cur_dirty_fprs = reg_alloc.GetDirtyFPR();
}

MacroAssembler& JitContext::GetMasm() { return masm; }

vixl::aarch64::Label* JitContext::GetLabel(LocationDescriptor location) {
    if (auto itr = labels.find(location); itr != labels.end()) {
        return &itr->second;
    } else {
        return &labels.try_emplace(location).first->second;
    }
}

void JitContext::BlockLinkStub(ir::Location location) {
    Label current;
    __ Bind(&current);
    __ Adr(ip0, &current);
    __ Str(ip0, MemOperand(state, state_offset_blocking_linkage_address));
    __ Mov(ipw0, (u32)HaltReason::BlockLinkage);
    __ Str(ipw0, MemOperand(state, state_offset_halt_reason));
    __ Mov(ip0, cur_block->GetStartLocation().Value());
    __ Str(ip0, MemOperand(state, state_offset_prev_loc));
    __ Mov(ip0, location.Value());
    __ Str(ip0, MemOperand(state, state_offset_current_loc));
    __ Ret();
}

void JitContext::FlushLabels(VAddr target) {
    for (auto &[location, label] : labels) {
        if (label.IsBound()) {
            continue;
        }
        ptrdiff_t offset = location - target;
        __ BindToOffset(&label, offset);
    }
}

#undef masm

}  // namespace swift::runtime::backend::arm64
