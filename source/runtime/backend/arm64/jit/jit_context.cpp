//
// Created by 甘尧 on 2023/9/15.
//

#include "jit_context.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"
#include "runtime/common/backedge_control.h"


namespace swift::runtime::backend::arm64 {

#define __ masm.

namespace {

bool ScratchPrecisePeakAuditEnabled() {
    return GetSvmConfig().scratch_precise_peak_audit;
}

}  // namespace

// The spill slot count reserved in State must match the allocator's limit.
static_assert(kMaxSpillSlots == sizeof(State::spill_area) / sizeof(u64),
              "spill slot count mismatch between reg_alloc.h and context.h");

JitContext::JitContext(const std::shared_ptr<Module>& module,
                       RegAlloc& reg_alloc,
                       bool enable_direct_link)
        : module(module), features(ResolveFeatureSet(module->GetModuleConfig())),
          reg_alloc(reg_alloc) {
#if defined(__linux__) && !defined(__ANDROID__)
    const bool has_spill = reg_alloc.SpillCount() != 0;
    ASSERT_MSG(!has_spill || reg_alloc.GetGprs().Get(18),
               "spilling unit reached emission without conditional x18 reservation");
#endif
    const auto& svm_config = GetSvmConfig();
    exec_profile_enabled = svm_config.exec_prof;
    execution_trace_enabled = svm_config.exec_trace;
    if (exec_profile_enabled) {
        exec_access_pad = svm_config.exec_access_pad;
    }
    fpcr_tax_profile_enabled = FpcrTaxProfEnabled();
    hot_coalesce_enabled = HotCoalesceProfEnabled();
    indirect_l1_prof_enabled = IndirectL1ProfEnabled();
    hot_counter_storage_enabled =
            hot_coalesce_enabled || indirect_l1_prof_enabled;
    const bool density_enabled = GetSvmConfig().density_prof;
    density_profile_enabled = density_enabled;
    direct_link_active = enable_direct_link && module->IsDirectLinkConfigured() &&
            module->PrepareDirectLinkRegion();
}

void JitContext::RecordExecCounter(u32 offset, u32 amount) {
    if (!exec_profile_enabled || amount == 0) return;
    ASSERT(amount < 4096);
    // FixedGPRClobbers reserves ip0/ip1 for every opcode while profiling is
    // enabled. A terminal may emit several counters; using this fixed pair
    // keeps their scratch demand constant instead of leasing two more XPOOL
    // registers for every counter and eventually exceeding ScratchBudget.
    __ Ldr(ip0, MemOperand(state, state_offset_exec_profile_ptr));
    __ Ldr(ip1, MemOperand(ip0, offset));
    __ Add(ip1, ip1, amount);
    __ Str(ip1, MemOperand(ip0, offset));
}

void JitContext::RecordExecutionTrace(u64 guest_rip,
                                      const XRegister& guest_rsp) {
    if (!execution_trace_enabled) return;
    static_assert((kExecutionTraceEntryCount &
                   (kExecutionTraceEntryCount - 1)) == 0);
    static_assert(offsetof(ExecutionTraceBuffer, next) == 0);
    static_assert(sizeof(ExecutionTraceEntry) == 16);

    // 该探针只在块入口运行，此时动态值尚未装入 x14-x17。先写完整条目，
    // 再用 release store 发布序号，信号处理器不会看到半写条目。
    __ Ldr(ip0, MemOperand(state, state_offset_exec_profile_ptr));
    __ Ldr(ip0, MemOperand(ip0, profile_offset_execution_trace));
    __ Ldr(ip1, MemOperand(ip0, offsetof(ExecutionTraceBuffer, next)));
    __ And(ip2, ip1, kExecutionTraceEntryCount - 1);
    __ Add(ip2, ip0, Operand(ip2, LSL, 4));
    __ Mov(ip3, guest_rip);
    __ Stp(ip3,
           guest_rsp,
           MemOperand(ip2, offsetof(ExecutionTraceBuffer, entries)));
    __ Add(ip1, ip1, 1);
    __ Stlr(ip1, MemOperand(ip0));
}

void JitContext::RecordFpcrTaxCounter(FpcrTaxCounter counter) {
    if (!fpcr_tax_profile_enabled) return;
    const auto index = static_cast<u32>(counter);
    ASSERT(index < kFpcrTaxCounterCount);
    // Preserve ip0/ip1 instead of reserving them in RegAlloc. The probe must
    // observe the production RA shape rather than SVM_EXEC_PROF's fixed-
    // clobber variant.
    __ Stp(ip0, ip1, MemOperand(sp, -16, PreIndex));
    __ Ldr(ip0, MemOperand(state, state_offset_exec_profile_ptr));
    __ Ldr(ip1,
           MemOperand(ip0, profile_offset_fpcr_tax + index * sizeof(u64)));
    __ Add(ip1, ip1, 1);
    __ Str(ip1,
           MemOperand(ip0, profile_offset_fpcr_tax + index * sizeof(u64)));
    __ Ldp(ip0, ip1, MemOperand(sp, 16, PostIndex));
}

void JitContext::RecordHotCounter(HotCoalesceCounter counter, u32 amount) {
    if (!hot_counter_storage_enabled ||
        hot_coalesce_slot == kHotCoalesceInvalidSlot ||
        amount == 0) {
        return;
    }
    ASSERT(amount < 4096);
    const u32 begin = CurrentBufferSize();
    // Do not reserve ip0/ip1 in the allocator: doing so changes the spill
    // shape this probe is meant to observe.  The probe is deliberately slow
    // when enabled, so preserve any live allocator values across the counter
    // sequence on the aligned host stack instead.
    __ Stp(ip0, ip1, MemOperand(sp, -16, PreIndex));
    __ Ldr(ip0, MemOperand(state, state_offset_exec_profile_ptr));
    __ Ldr(ip0, MemOperand(ip0, profile_offset_hot_coalesce_counters));
    const u64 byte_offset =
            (static_cast<u64>(hot_coalesce_slot) * kHotCoalesceCounterCount +
             static_cast<u32>(counter)) *
            sizeof(u64);
    __ Mov(ip1, byte_offset);
    __ Add(ip0, ip0, ip1);
    __ Ldr(ip1, MemOperand(ip0));
    __ Add(ip1, ip1, amount);
    __ Str(ip1, MemOperand(ip0));
    __ Ldp(ip0, ip1, MemOperand(sp, 16, PostIndex));
    if (hot_collecting) {
        hot_probe_ranges.push_back({begin, CurrentBufferSize()});
    }
}

void JitContext::RecordHotSpillReload() {
    if (!hot_coalesce_enabled) return;
    ++hot_shape.spill_reloads;
    RecordHotCounter(HotCoalesceCounter::SpillReloads);
}

void JitContext::RecordHotSpillWriteback() {
    if (!hot_coalesce_enabled) return;
    ++hot_shape.spill_writebacks;
    RecordHotCounter(HotCoalesceCounter::SpillWritebacks);
}

void JitContext::BeginHotNaNGuard(u32 instruction_count) {
    if (density_profile_enabled) {
        ASSERT(!density_nan_open);
        density_nan_start = CurrentBufferSize();
        density_nan_open = true;
    }
    if (hot_coalesce_enabled) {
        ASSERT(!hot_nan_open);
        RecordHotCounter(HotCoalesceCounter::NaNGuardInstructions,
                         instruction_count);
        hot_nan_start = CurrentBufferSize();
        hot_nan_expected = instruction_count;
        hot_nan_open = true;
        hot_shape.nan_guard_instructions += instruction_count;
    }
}

void JitContext::EndHotNaNGuard() {
    const u32 end = CurrentBufferSize();
    if (density_profile_enabled) {
        ASSERT(density_nan_open);
        ASSERT(end >= density_nan_start);
        density_nan_bytes += end - density_nan_start;
        density_nan_open = false;
    }
    if (hot_coalesce_enabled) {
        ASSERT(hot_nan_open);
        ASSERT(end >= hot_nan_start);
        ASSERT((end - hot_nan_start) / vixl::aarch64::kInstructionSize ==
               hot_nan_expected);
        hot_nan_ranges.push_back({hot_nan_start, end});
        hot_nan_open = false;
        hot_nan_expected = 0;
    }
}

bool JitContext::HasAllocation(const ir::Value& value) {
    return reg_alloc.ValueType(value) != RegAlloc::NONE;
}

bool JitContext::SharesGPR(const ir::Value& left, const ir::Value& right) {
    return reg_alloc.ValueType(left) == RegAlloc::GPR &&
           reg_alloc.ValueType(right) == RegAlloc::GPR &&
           reg_alloc.ValueGPR(left).id == reg_alloc.ValueGPR(right).id;
}

bool JitContext::SharesFPR(const ir::Value& left, const ir::Value& right) {
    return reg_alloc.ValueType(left) == RegAlloc::FPR &&
           reg_alloc.ValueType(right) == RegAlloc::FPR &&
           reg_alloc.ValueFPR(left).id == reg_alloc.ValueFPR(right).id;
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
        auto tmp = GetSpillTmpX();
        spill_def_scratch.emplace(value.Id(), static_cast<u8>(tmp.GetCode()));
        pending_spill_writes.push_back({slot.offset, static_cast<u8>(tmp.GetCode()), false});
        return tmp;
    }
    // Use access: reload from the spill slot. Any write-back of a value
    // defined by an earlier instruction has already been flushed at this
    // instruction's TickIR, so the slot is current.
    //
    // One reload per (instruction, value): a second access within the same
    // instruction reuses the register the first reload landed in. The slot
    // cannot change under us mid-instruction (only TickIR flushes writes), and
    // emitters never write through a source register, so the reuse is exact --
    // and it is what makes the reload demand of an instruction bounded by the
    // number of distinct values it names.
    if (auto it = spill_use_scratch.find(value.Id()); it != spill_use_scratch.end()) {
        return XRegister(it->second);
    }
    auto tmp = GetSpillTmpX();
    __ Ldr(tmp, MemOperand(state, offset));
    if (RAShapeProfEnabled()) ++reg_alloc.RAShape().spill_loads;
    RecordHotSpillReload();
    spill_use_scratch.emplace(value.Id(), static_cast<u8>(tmp.GetCode()));
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
        auto tmp = GetSpillTmpV();
        spill_def_scratch.emplace(value.Id(), static_cast<u8>(tmp.GetCode()));
        pending_spill_writes.push_back({slot.offset, static_cast<u8>(tmp.GetCode()), true});
        return tmp;
    }
    // See SpillGPR: one reload per (instruction, value).
    if (auto it = spill_use_scratch.find(value.Id()); it != spill_use_scratch.end()) {
        return VRegister::GetVRegFromCode(it->second);
    }
    auto tmp = GetSpillTmpV();
    __ Ldr(tmp.Q(), MemOperand(state, offset));
    if (RAShapeProfEnabled()) ++reg_alloc.RAShape().spill_loads;
    RecordHotSpillReload();
    spill_use_scratch.emplace(value.Id(), static_cast<u8>(tmp.GetCode()));
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
        if (RAShapeProfEnabled()) ++reg_alloc.RAShape().spill_stores;
        RecordHotSpillWriteback();
    }
    pending_spill_writes.clear();
}

backend::ScratchNeed JitContext::CurrentBudget() const {
    if (auxiliary_scratch) {
        return {7, backend::kDefaultScratchFPR};
    }
    // Block terminals are emitted after the last instruction's TickIR and
    // share its masks; they take no scratch of their own, so charging them to
    // the default budget is exact.
    return cur_inst ? backend::ScratchBudget(*cur_inst, features)
                    : backend::ScratchNeed{backend::kDefaultScratchGPR,
                                           backend::kDefaultScratchFPR};
}

XRegister JitContext::GetTmpX() {
    // Budget check first: an emitter that outgrows its declared budget must
    // say so by name here, at the instruction that did it, rather than
    // surface later as a register-pool exhaustion PANIC in some unrelated
    // high-pressure block (or, if the pool happened to have room, as no
    // symptom at all until the pressure changes).
    const u32 used = static_cast<u32>(cur_dirty_gprs.GetMarkedCount() -
                                      tick_dirty_gprs.GetMarkedCount()) -
                     spill_tmp_gprs;
    if (ScratchPrecisePeakAuditEnabled() && cur_inst &&
        (cur_inst->GetOp() == ir::OpCode::Add ||
         cur_inst->GetOp() == ir::OpCode::Sub)) {
        std::fprintf(stderr,
                     "[svm-scratch-lease] unit=0x%llx id=%u op=%s type=%u "
                     "asked=%u budget=%u\n",
                     static_cast<unsigned long long>(unit_start), cur_inst->Id(),
                     ir::GetIRMetaInfo(cur_inst->GetOp()).name,
                     static_cast<u32>(cur_inst->ReturnType()), used + 1,
                     CurrentBudget().gpr);
    }
    ASSERT_MSG(used < CurrentBudget().gpr,
               "scratch GPR budget exceeded in unit {:#x} id {} opcode {} type {}: "
               "declared {}, asked for {}. "
               "Raise its entry in backend::ScratchBudget (reg_alloc.cpp)",
               unit_start, cur_inst ? cur_inst->Id() : 0u,
               cur_inst ? static_cast<u32>(cur_inst->GetOp()) : 0u,
               cur_inst ? static_cast<u32>(cur_inst->ReturnType()) : 0u,
               CurrentBudget().gpr, used + 1);
    if (auto alloc = cur_dirty_gprs.GetFirstClear(); alloc >= 0) {
        cur_dirty_gprs.Mark(alloc);
        XRegister result(alloc);
        ExcludeVixlScratch(result);
        return result;
    }
    // Unreachable while the linear scan honours ScratchBudget: it never
    // assigns a value if that would drop the free count to the unit's
    // reserve, and the assert above caps demand at that same reserve.
    PANIC("No free temporary GPR");
}

// Reload scratch is tracked apart from emitter scratch so the per-opcode
// budget above stays a statement about the emitter alone. What bounds *this*
// counter is the memoization in SpillGPR/SpillFPR (one register per distinct
// value per instruction) together with the allocation pass, which verifies
// that every instruction was left room for exactly that many.
XRegister JitContext::GetSpillTmpX() {
#if defined(__linux__) && !defined(__ANDROID__)
    // The first scalar spill in an instruction gets the register reserved
    // expressly for it. This leaves all allocator headroom available for any
    // additional distinct spilled operands and makes total value-pool
    // exhaustion unable to remove the final reload/write-back scratch.
    if (!spill_scratch_in_use) {
        spill_scratch_in_use = true;
        return spill_scratch;
    }
#endif
    if (auto alloc = cur_dirty_gprs.GetFirstClear(); alloc >= 0) {
        cur_dirty_gprs.Mark(alloc);
        spill_tmp_gprs++;
        XRegister result(alloc);
        ExcludeVixlScratch(result);
        return result;
    }
    PANIC("No free temporary GPR for spill reload");
}

VRegister JitContext::GetSpillTmpV() {
    if (auto alloc = cur_dirty_fprs.GetFirstClear(); alloc >= 0) {
        cur_dirty_fprs.Mark(alloc);
        spill_tmp_fprs++;
        return VRegister::GetVRegFromCode(alloc);
    }
    PANIC("No free temporary VREG for spill reload");
}

Register JitContext::GetTmpGPR(ir::ValueType type) {
    auto x = GetTmpX();
    return type == ir::ValueType::U64 ? x : x.W();
}

VRegister JitContext::GetTmpV() {
    const u32 used = static_cast<u32>(cur_dirty_fprs.GetMarkedCount() -
                                      tick_dirty_fprs.GetMarkedCount()) -
                     spill_tmp_fprs;
    ASSERT_MSG(used < CurrentBudget().fpr,
               "scratch FPR budget exceeded emitting opcode {}: declared {}, asked for {}. "
               "Raise its entry in backend::ScratchBudget (reg_alloc.cpp)",
               cur_inst ? static_cast<u32>(cur_inst->GetOp()) : 0u, CurrentBudget().fpr, used + 1);
    if (auto alloc = cur_dirty_fprs.GetFirstClear(); alloc >= 0) {
        cur_dirty_fprs.Mark(alloc);
        return VRegister::GetVRegFromCode(alloc);
    }
    PANIC("No free temporary VREG");
}

bool JitContext::TryGetConsecutiveTmpV2(VRegister& first, VRegister& second) {
    const u32 used = static_cast<u32>(cur_dirty_fprs.GetMarkedCount() -
                                      tick_dirty_fprs.GetMarkedCount()) -
                     spill_tmp_fprs;
    ASSERT_MSG(used + 2 <= CurrentBudget().fpr,
               "scratch FPR budget exceeded emitting opcode {}: declared {}, asked for {}. "
               "Raise its entry in backend::ScratchBudget (reg_alloc.cpp)",
               cur_inst ? static_cast<u32>(cur_inst->GetOp()) : 0u,
               CurrentBudget().fpr,
               used + 2);
    for (u32 code = 0; code + 1 < cur_dirty_fprs.GetAllCount(); ++code) {
        if (!cur_dirty_fprs.Get(code) && !cur_dirty_fprs.Get(code + 1)) {
            cur_dirty_fprs.Mark(code);
            cur_dirty_fprs.Mark(code + 1);
            first = VRegister::GetVRegFromCode(code);
            second = VRegister::GetVRegFromCode(code + 1);
            return true;
        }
    }
    if (RAShapeProfEnabled()) ++reg_alloc.RAShape().consecutive_pair_fallbacks;
    return false;
}

void JitContext::ReserveTmpX(const XRegister& reg) {
    cur_dirty_gprs.Mark(reg.GetCode());
    ExcludeVixlScratch(reg);
}

XRegister JitContext::GetSharedTmpX() {
    if (!backend::ScratchXPoolEnabled(features)) {
        return ip;
    }
    if (shared_tmp_gpr < 0) {
        shared_tmp_gpr = GetTmpX().GetCode();
    }
    return XRegister(shared_tmp_gpr);
}

bool JitContext::ForwardStatic(ir::Location location) {
    if (!module->GetModuleConfig().HasOpt(Optimizations::BlockLink)) {
        return false;
    }
    // Same-module only, like the BlockLink path in Forward(): a slot filled by
    // another module outlives this module's view of it. The lookup also keeps
    // dispatch slots (a finite shared table) from being reserved for addresses
    // no module owns -- a computed jmp into unmapped memory must not consume
    // one.
    auto target_module = module->GetAddressSpace().GetModule(location.Value());
    if (!target_module || target_module != module) {
        return false;
    }
    // The Ret this replaces leaves the translator without touching JitContext,
    // so a spilled def from the block's last instruction would never reach its
    // slot; branching straight to the next unit makes that visible.
    FlushSpillWrites();
    const u32 dispatcher_index = target_module->GetDispatchIndex(location);
    Label empty_slot;
    __ Mov(ipw, dispatcher_index);
    __ Ldr(ip, MemOperand(cache, ip, LSL, 3));
    __ Cbz(ip, &empty_slot);
    RecordExecCounter(exec_offset_link_hit);
    __ Br(ip);
    __ Bind(&empty_slot);
    RecordExecCounter(exec_offset_link_miss);
    __ Ret();
    return true;
}

void JitContext::Forward(ir::Location location,
                         Label* backedge_exit,
                         Label* self_target,
                         LinkSiteKind direct_link_kind) {
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
        auto self_label = self_target ? self_target : GetLabel(location.Value());
        if (backedge_exit) {
            // State::exit_request is deliberately the first field, so this
            // acquire poll is exactly two hot instructions. The release
            // publishers are SignalInterrupt and SmcTracker invalidation.
            __ Ldar(ip0, MemOperand(state, state_offset_exit_request));
            __ Cbnz(ip0, backedge_exit);
        }
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
        if (direct_link_active && self_module_forward) {
            // The final RX address is unknown until TranslateIR allocates its
            // CodeBuffer. Emit exactly one 4-byte BL placeholder and relocate
            // it to this allocation's region trampoline in Flush(). The site
            // record is installed there, before any L2 publication.
            pending_direct_link_sites.push_back(
                    {CurrentBufferSize(), location.Value(), direct_link_kind});
            __ dc32(*EncodeBL(0));
            return;
        }

        if (self_module_forward && module_config.HasOpt(Optimizations::BlockLink)) {
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
            RecordExecCounter(exec_offset_link_hit);
            __ Br(ip);
            // empty slot -> back to the dispatcher for the target location.
            __ Bind(&empty_slot);
            RecordExecCounter(exec_offset_link_miss);
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

void JitContext::ForwardLocal(ir::Location location,
                              Label* cycle_exit,
                              bool fallthrough) {
    ASSERT(cur_block);
    FlushSpillWrites();
    if (cycle_exit) {
        // release 侧在信号和 SMC 失效路径发布请求；这里只对成环边付 acquire 税。
        __ Ldar(ip0, MemOperand(state, state_offset_exit_request));
        __ Cbnz(ip0, cycle_exit);
    }
    if (!fallthrough) {
        __ B(GetInternalLabel(location.Value()));
    }
}

bool JitContext::CanEmitDirectLink(ir::Location location) const {
    if (!direct_link_active || !cur_block ||
        location == cur_block->GetStartLocation() ||
        (cur_function && location == cur_function->GetStartLocation())) {
        return false;
    }
    const auto target_module = module->GetAddressSpace().GetModule(location.Value());
    return target_module == module &&
           module->GetModuleConfig().HasOpt(Optimizations::BlockLink);
}

void JitContext::ReturnToDispatcher(const Register& location) {
    // Block exit: see Forward.
    FlushSpillWrites();
    __ Str(location, MemOperand(state, state_offset_current_loc));
    __ Ret();
}

void JitContext::ForwardIndirectL1(const Register& location) {
    const auto index = GetTmpX();
    const auto entry = GetTmpX();
    Label signal;

    if (!indirect_l1_prof_enabled) {
        // SignalInterrupt publishes halt_reason before setting bit 63 with a
        // release operation. Reuse the existing index temporary for the
        // acquire poll, so the production fast path gains no scratch pressure.
        // If a signal races after this load, the next inline indirect exit sees
        // it; a pure L1-hit loop therefore has a one-edge bounded safepoint.
        __ Ldar(index, MemOperand(state, state_offset_exit_request));
        __ Tbnz(index, 63, &signal);
    }

    // L1 is direct-low-bit hashed; L2 retains the folded hash. The Runtime
    // publishes this stable per-thread base in an otherwise frozen State slot,
    // avoiding the latch/profile pointer chase without changing State size.
    __ Ldr(entry, MemOperand(state, state_offset_indirect_l1_code_cache));
    __ And(index, location, L1_CODE_CACHE_HASH);
    __ Add(entry, entry, Operand(index, LSL, 4));
    __ Ldp(index, entry, MemOperand(entry));
    __ Cmp(index, location);
    if (indirect_l1_prof_enabled) {
        Label miss;
        // The profiling Runtime keeps invalid values at zero so this arm can
        // distinguish SMC fallback from a real hit.
        __ Ccmp(entry, xzr, ZFlag, eq);
        __ B(&miss, eq);
        RecordHotCounter(HotCoalesceCounter::IndirectL1Hit);
        __ Br(entry);
        __ Bind(&miss);
        RecordHotCounter(HotCoalesceCounter::IndirectL1Miss);
        __ Ret();
        return;
    }

    // On a key mismatch, x30 is the unchanged trampoline continuation and is
    // equivalent to Ret. On an SMC-invalidated key hit, entry is the safe L2
    // continuation installed by TranslateTable::Zero. No stale/zero pointer
    // can reach Br. With the two-instruction signal poll the production hot
    // path is exactly nine instructions and still rents only two temporaries.
    __ Csel(entry, entry, x30, eq);
    __ Br(entry);
    __ Bind(&signal);
    // halt_reason was release-published before the request bit. Ret enters the
    // normal trampoline epilogue, which snapshots all architectural state and
    // returns HaltReason::Signal to the driver.
    __ Ret();
}

// --- Return Stack Buffer (RSB) -------------------------------------------
// The RSB is a small stack of 16-byte frames in host memory, pointed to by
// the reserved rsb_ptr register (x25). The default format stores the guest
// return address followed by the L2 dispatch slot. The lean-shadow format
// stores that stable slot in both words, so its pop can fetch the L2 key and
// value together without materializing the guest address at every call site.
//
// Push (guest call): pre-decrement rsb_ptr by 16 and store the frame.
// Pop  (guest ret): load the predicted key and code pointer, compare the key
//   with state->current_loc, and branch on a hit. On a miss (mismatch, empty
//   slot, or underflow), fall through to the normal dispatcher path. The two
//   formats are safe to mix: a default pop rejects a lean-shadow frame, while
//   a lean-shadow pop accepts either because the second word is always slot.

void JitContext::EmitRSBPush(u64 guest_return_addr, u32 dispatch_index) {
    if (features.shadow_lean) {
        Label rsb_full;
        const auto bound = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip0;
        const auto slot = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip1;
        __ Ldr(bound, MemOperand(state, state_offset_rsb_bottom));
        __ Cmp(rsb_ptr, bound);
        __ B(&rsb_full, ls);
        // Keep the 16-byte frame ABI. Both words carry the stable L2 value-slot
        // index; an OFF pop seeing this frame merely fails its guest-PC compare
        // and takes the safe dispatcher path.
        __ Mov(slot, static_cast<u64>(dispatch_index));
        __ Stp(slot, slot, MemOperand(rsb_ptr, -16, PreIndex));
        __ Bind(&rsb_full);
        return;
    }
    Label rsb_full;
    const auto bound = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip0;
    const auto guest = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip0;
    const auto slot = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip1;
    // Overflow guard: if rsb_ptr has already reached the bottom of the buffer
    // (state->rsb_bottom == &rsb_frames[0], stack full), a pre-decrement push
    // would store out of bounds — skip the push and let the ret take the slow
    // dispatcher path instead. Unsigned compare: skip when rsb_ptr <= bottom.
    __ Ldr(bound, MemOperand(state, state_offset_rsb_bottom));
    __ Cmp(rsb_ptr, bound);
    __ B(&rsb_full, ls);
    // ip0 (x16) = guest return address, ip1 (x17) = dispatch table slot.
    __ Mov(guest, guest_return_addr);
    __ Mov(slot, static_cast<u64>(dispatch_index));
    // Pre-decrement push: rsb_ptr -= 16, then store the pair.
    __ Stp(guest, slot, MemOperand(rsb_ptr, -16, PreIndex));
    __ Bind(&rsb_full);
}

void JitContext::EmitRSBPop() {
    if (features.shadow_lean) {
        Label rsb_miss, rsb_empty;
        const auto predicted = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip0;
        const auto slot = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip1;
        __ Ldr(predicted, MemOperand(state, state_offset_rsb_top));
        __ Cmp(rsb_ptr, predicted);
        __ B(&rsb_empty, hs);
        // Pop first: a mismatched prediction is consumed exactly like the old
        // path. The slot points at the L2 value word; the preceding word is
        // its immutable guest key. SMC clears the value word before reclaim.
        __ Ldp(predicted, slot, MemOperand(rsb_ptr, 16, PostIndex));
        __ Add(slot, cache, Operand(slot, LSL, 3));
        __ Ldp(predicted, slot, MemOperand(slot, -8));
        __ Ldr(ip, MemOperand(state, state_offset_current_loc));
        __ Cmp(predicted, ip);
        __ B(&rsb_miss, ne);
        __ Cbz(slot, &rsb_miss);
        RecordExecCounter(exec_offset_rsb_hit);
        __ Br(slot);
        __ Bind(&rsb_miss);
        __ Bind(&rsb_empty);
        RecordExecCounter(exec_offset_rsb_miss);
        __ Ret();
        return;
    }
    Label rsb_miss, rsb_empty;
    const auto predicted = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip0;
    const auto actual = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip1;
    // Underflow guard: if rsb_ptr has reached the empty top of the stack
    // (state->rsb_top == &rsb_frames[rsb_stack_size]), there are more guest
    // rets than recorded calls, so no valid prediction exists — fall back to
    // the dispatcher without reading the buffer (avoids an out-of-bounds load
    // and a wild branch). Unsigned compare: fall back when rsb_ptr >= top.
    __ Ldr(predicted, MemOperand(state, state_offset_rsb_top));
    __ Cmp(rsb_ptr, predicted);
    __ B(&rsb_empty, hs);
    // Load the predicted guest return address from the top RSB frame.
    __ Ldr(predicted, MemOperand(rsb_ptr, 0));
    // Load the actual return target (set by the frontend's ret instruction).
    __ Ldr(actual, MemOperand(state, state_offset_current_loc));
    __ Cmp(predicted, actual);
    __ B(&rsb_miss, ne);
    // Prediction hit: load the L2 dispatch-table slot index and look up the
    // compiled code pointer.  cache (x27) holds the L2 table base at all
    // times (loaded once at runtime entry).  dispatch_index == 2*entry+1
    // points straight at the entry's value word (an 8-byte code pointer).
    __ Ldr(ip, MemOperand(rsb_ptr, 8));   // ip (x11) = dispatch_index
    __ Ldr(ip2, MemOperand(cache, ip, LSL, 3));  // ip2 (x14) = code ptr
    __ Cbz(ip2, &rsb_miss);               // empty slot → fallback
    // Commit the pop and jump directly to the target's compiled code.
    __ Add(rsb_ptr, rsb_ptr, 16);
    RecordExecCounter(exec_offset_rsb_hit);
    __ Br(ip2);
    // Miss with a frame present: DISCARD that frame before falling back.
    //
    // This ret consumes a return address either way, so leaving the frame in
    // place desynchronises the buffer permanently: the very next ret compares
    // against the same stale entry and misses again, while pushes keep
    // stacking until rsb_ptr reaches rsb_bottom and every later push is
    // skipped by the overflow guard.  Measured on HEAD (SVM_RSB_STATS
    // instrumentation, docs/perf-baseline.md 6b): func_tests 3587 pops /
    // 65 hits (1.81%), of which 3469 were address mismatches and *3463 of
    // 3592 pushes were skipped because the buffer was full* — one unmatched
    // guest call early in glibc startup wedged the buffer for the whole run.
    // Popping here makes the buffer self-healing: an unmatched call costs at
    // most the predictions of the rets that drain it.
    //
    // Discarding is unconditionally safe: the RSB is a prediction only.  The
    // architectural return target lives in state->current_loc and the fallback
    // below returns to the trampoline dispatcher, which uses it.  A wrong or
    // missing prediction can only cost a dispatcher round-trip.
    __ Bind(&rsb_miss);
    __ Add(rsb_ptr, rsb_ptr, 16);
    // Underflow: no frame was read, so there is nothing to discard.
    __ Bind(&rsb_empty);
    RecordExecCounter(exec_offset_rsb_miss);
    __ Ret();
}

u32 JitContext::GetDispatchIndex(u64 guest_addr) {
    return module->GetDispatchIndex(ir::Location{guest_addr});
}

void JitContext::Finish() {
    vixl::svm_vixl_prof::JitScope prof{features.vixl_fast};
    __ FinalizeCode();
    MaybeDumpHostBytes();
    if (RAShapeProfEnabled() && !ra_shape_submitted) {
        RAShapeSubmitUnit(reg_alloc.RAShape());
        ra_shape_submitted = true;
    }
}

void JitContext::FinishHotCoalesceBlock() {
    if (!hot_counter_storage_enabled ||
        hot_coalesce_slot == kHotCoalesceInvalidSlot) {
        return;
    }
    if (!hot_coalesce_enabled) {
        hot_coalesce_slot = kHotCoalesceInvalidSlot;
        return;
    }
    ASSERT(hot_collecting);
    ASSERT(!hot_nan_open);
    hot_collecting = false;
    const u32 end = CurrentBufferSize();
    ASSERT(end >= hot_code_start);
    ASSERT(end >= hot_shape.host_offset);
    hot_shape.host_bytes = end - hot_shape.host_offset;

    auto in_range = [](u32 offset, const std::vector<HotCodeRange>& ranges) {
        return std::any_of(ranges.begin(), ranges.end(), [offset](const auto& range) {
            return offset >= range.begin && offset < range.end;
        });
    };
    u32 probe_instructions = 0;
    for (const auto& range : hot_probe_ranges) {
        ASSERT(range.end >= range.begin);
        probe_instructions +=
                (range.end - range.begin) / vixl::aarch64::kInstructionSize;
    }
    hot_shape.host_instructions =
            (end - hot_code_start) / vixl::aarch64::kInstructionSize -
            probe_instructions;

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    const auto* bytes = masm.GetBuffer()->GetStartAddress<const u8*>();
    for (u32 offset = hot_code_start; offset < end;
         offset += vixl::aarch64::kInstructionSize) {
        if (in_range(offset, hot_probe_ranges) || in_range(offset, hot_nan_ranges)) {
            continue;
        }
        const auto* instruction =
                reinterpret_cast<const vixl::aarch64::Instruction*>(bytes + offset);
        decoder.Decode(instruction);
        if (HotCoalesceIsMoveBridge(disassembler.GetOutput())) {
            ++hot_shape.move_bridges;
        }
    }
    HotCoalesceUpdateUnit(hot_coalesce_slot, hot_shape);
    hot_coalesce_slot = kHotCoalesceInvalidSlot;
    hot_probe_ranges.clear();
    hot_nan_ranges.clear();
}

u8* JitContext::Flush(const CodeBuffer& code_cache) {
    FlushLabels(reinterpret_cast<VAddr>(code_cache.exec_data));
    Finish();
    for (u32 slot : hot_coalesce_slots) {
        HotCoalesceSetUnitHostBase(
                slot, reinterpret_cast<VAddr>(code_cache.exec_data));
    }
    if (GetSvmConfig().exec_map_is_set ||
        GetSvmConfig().vixl_host_dump_is_set) {
        std::fprintf(stderr, "[svm-host-map] pc=0x%llx exec=%p size=%u\n",
                     static_cast<unsigned long long>(unit_start),
                     static_cast<void*>(code_cache.exec_data),
                     CurrentBufferSize());
    }
    if (!pending_direct_link_sites.empty()) {
        auto* cache = module->GetCodeCache(code_cache.exec_data);
        ASSERT(cache);
        const auto& region = cache->GetRegion();
        auto* trampoline = static_cast<u8*>(cache->GetRegionTrampoline());
        ASSERT(trampoline && region.ContainsRx(trampoline));
        const LinkSourceOwner owner{module.get(), code_cache.exec_data};
        auto* emitted = masm.GetBuffer()->GetStartAddress<u8*>();
        for (const auto& pending : pending_direct_link_sites) {
            ASSERT(static_cast<size_t>(pending.code_offset) + sizeof(u32) <= code_cache.size);
            auto* rx_site = code_cache.exec_data + pending.code_offset;
            const auto branch = EncodeBL(trampoline - rx_site);
            ASSERT(branch);
            std::memcpy(emitted + pending.code_offset, &*branch, sizeof(*branch));
            const LinkSiteKey key{
                    region.id,
                    code_cache.offset + pending.code_offset,
            };
            const LinkSignalPatchSite signal_patch{
                    .region = region,
                    .rx_site = rx_site,
                    .rw_site = code_cache.rw_data + pending.code_offset,
                    .unlinked_bl = *branch,
            };
            ASSERT(module->GetAddressSpace().GetLinkManager().RegisterSite(
                    key, pending.guest_target, owner, &signal_patch, pending.kind));
        }
    }
    std::memcpy(code_cache.rw_data, masm.GetBuffer()->GetStartAddress<u8*>(), code_cache.size);
    code_cache.Flush();
    return code_cache.exec_data;
}

u32 JitContext::CurrentBufferSize() { return __ GetBuffer() -> GetSizeInBytes(); }

ptrdiff_t JitContext::GetCodeOffset(LocationDescriptor location) const {
    auto it = labels.find(location);
    if (it == labels.end() || !it->second.IsBound()) {
        return -1;
    }
    return it->second.GetLocation();
}

bool JitContext::IsUniform(const Register& reg) {
    auto &uniform_info = module->GetAddressSpace().GetUniformInfo();
    if (reg.IsV()) {
        return uniform_info.uni_fprs.Get(reg.GetCode());
    } else {
        return uniform_info.uni_gprs.Get(reg.GetCode());
    }
}

void JitContext::SetCurrent(ir::Block* block, bool split_backedge_entry) {
    cur_block = block;
    if (!unit_start_set) {
        unit_start = block->GetStartLocation().Value();
        unit_start_set = true;
    }
    auto label = GetLabel(block->GetStartLocation().Value());
    __ Bind(label);
    if (hot_counter_storage_enabled) {
        if (hot_coalesce_enabled) ASSERT(!hot_collecting);
        hot_coalesce_slot =
                HotCoalesceRegisterUnit(block->GetStartLocation().Value());
    }
    if (hot_coalesce_enabled) {
        hot_shape = {};
        hot_shape.guest_entry = block->GetStartLocation().Value();
        hot_shape.host_offset = CurrentBufferSize();
        hot_shape.uniform = HotCoalesceAnalyzeUniformSequences(block);
        HotCoalesceAnalyzeLinkTargets(block, hot_shape);
        if (hot_coalesce_slot != kHotCoalesceInvalidSlot) {
            hot_coalesce_slots.push_back(hot_coalesce_slot);
        }
        hot_probe_ranges.clear();
        hot_nan_ranges.clear();
        hot_collecting = false;
    }
    if (!split_backedge_entry) {
        BeginBackedgeBody();
    }
}

void JitContext::BindInternalEntry(LocationDescriptor location) {
    auto* label = GetInternalLabel(location);
    ASSERT(!label->IsBound());
    // 当前只内部化控制边，状态 ABI 仍逐块提交，因此两个入口暂时同址。
    __ Bind(label);
}

void JitContext::BeginBackedgeBody() {
    if (hot_coalesce_enabled) {
        RecordHotCounter(HotCoalesceCounter::Entries);
        hot_code_start = CurrentBufferSize();
        hot_collecting = hot_coalesce_slot != kHotCoalesceInvalidSlot;
    }
    if (exec_access_pad) {
        __ Ldr(ip0, MemOperand(state, state_offset_exec_profile_ptr));
    }
    for (u32 i = 0; i < exec_access_pad; ++i) {
        __ Ldr(ip1, MemOperand(ip0, exec_offset_access_pad));
        __ Str(ip1, MemOperand(ip0, exec_offset_access_pad));
    }
}

void JitContext::SetCurrent(ir::Function* function) {
    cur_function = function;
    if (!unit_start_set) {
        unit_start = function->GetStartLocation().Value();
        unit_start_set = true;
    }
    auto label = GetLabel(function->GetStartLocation().Value());
    __ Bind(label);
}

void JitContext::TickIR(ir::Inst* instr) {
    EndVixlScratch();
    // Deferred spill write-back for the previous instruction's def (if
    // any) must land before anything else: from this instruction on the
    // scratch register holding it may be reused, and uses reload from the
    // slot.
    FlushSpillWrites();
    spill_def_scratch.clear();
    spill_use_scratch.clear();
    cur_inst = instr;
    reg_alloc.SetCurrent(instr);
    cur_dirty_gprs = reg_alloc.GetDirtyGPR();
    cur_dirty_fprs = reg_alloc.GetDirtyFPR();
    // Baseline for the per-instruction scratch budget (see GetTmpX).
    tick_dirty_gprs = cur_dirty_gprs;
    tick_dirty_fprs = cur_dirty_fprs;
    spill_tmp_gprs = 0;
    spill_tmp_fprs = 0;
#if defined(__linux__) && !defined(__ANDROID__)
    spill_scratch_in_use = false;
#endif
    shared_tmp_gpr = -1;
    auxiliary_scratch = false;
    const bool scratch_only =
            backend::X86PinExtScratchOnlyEnabled(reg_alloc.GetGprs(), features);
    const u32 fixed = backend::FixedGPRClobbers(*instr, features, scratch_only);
    for (u32 code = 0; code < 32; ++code) {
        if (fixed & (1u << code)) {
            cur_dirty_gprs.Mark(code);
            tick_dirty_gprs.Mark(code);
        }
    }
    if (scratch_only) {
        // x12/x13 remain outside the allocator's value pool, preserving the
        // documented six-register level-2 pool. Lease them only as explicit
        // instruction-local scratch when the opcode has no fixed use.
        for (u32 code : {12u, 13u}) {
            if (!(fixed & (1u << code))) {
                cur_dirty_gprs.Clear(code);
                tick_dirty_gprs.Clear(code);
            }
        }
    }
    // In bias mode x10 is permanently outside the value pool. Level 3 leases
    // it only to pure high-pressure ALU emission, where it cannot overlap
    // mem_scratch's address role. This is the eighth slot needed by a
    // five-register emitter plus high-pressure spill reloads.
    if (backend::X86PinExtLevel3AluScratchEnabled(reg_alloc.GetGprs(),
                                                  instr->GetOp())) {
        cur_dirty_gprs.Clear(10);
        tick_dirty_gprs.Clear(10);
    }
    BeginVixlScratch(true);
}

void JitContext::BeginVixlScratch(bool allow_pool) {
    if (!backend::ScratchXPoolEnabled(features)) {
        return;
    }
    ASSERT(!vixl_scratch_scope);
    u32 allowed = 0;
    if (allow_pool) {
        const bool x87 = cur_inst && cur_inst->GetOp() == ir::OpCode::X87Op;
        if (!x87) {
            for (u32 code = 11; code <= 17; ++code) {
                if (!cur_dirty_gprs.Get(code)) {
                    allowed |= 1u << code;
                }
            }
        } else {
            // X87's widest inline arms can occupy every dynamic pool slot.
            // ip0 is an opcode fixed clobber, so RA cannot place a live value
            // there; make it VIXL's sole immediate-synthesis register.
            allowed |= 1u << ip0.GetCode();
        }
    }
    masm.SvmBeginScratchContract(allowed);
    vixl_scratch_scope = std::make_unique<UseScratchRegisterScope>(&masm);
    vixl_scratch_scope->Exclude(*masm.GetScratchRegisterList());
    if (!allow_pool) {
        return;
    }
    for (u32 code = 11; code <= 17; ++code) {
        if (allowed & (1u << code)) {
            vixl_scratch_scope->Include(XRegister(code));
        }
    }
}

void JitContext::EndVixlScratch() {
    if (vixl_scratch_scope) {
        vixl_scratch_scope.reset();
        const auto acquired = masm.SvmEndScratchContract();
        const u32 explicit_used =
                static_cast<u32>(cur_dirty_gprs.GetMarkedCount() -
                                 tick_dirty_gprs.GetMarkedCount()) -
                spill_tmp_gprs;
        // A VIXL register already declared as a fixed opcode clobber consumes
        // no dynamic scratch headroom; RA excluded it independently.
        const u32 fixed = cur_inst ? backend::FixedGPRClobbers(*cur_inst, features) : 0;
        const u32 vixl_used = static_cast<u32>(
                __builtin_popcountll(acquired & ~fixed));
        last_instruction_scratch_gpr = explicit_used + vixl_used;
        if (ScratchPrecisePeakAuditEnabled() && cur_inst &&
            (cur_inst->GetOp() == ir::OpCode::Add ||
             cur_inst->GetOp() == ir::OpCode::Sub)) {
            ir::Flags flags{};
            bool branch_only = false;
            for (auto* pseudo : cur_inst->GetPseudoOperations()) {
                if (pseudo->GetOp() == ir::OpCode::SaveFlags) {
                    flags |= pseudo->GetArg<ir::Flags>(1);
                } else if (pseudo->GetOp() == ir::OpCode::BranchOnlyFlags) {
                    flags |= pseudo->GetArg<ir::Flags>(1);
                    branch_only = true;
                }
            }
            const auto right = cur_inst->GetArg<ir::Operand>(1);
            const auto right_op = right.GetOp();
            const char* right_shape = right.GetRight().Null()
                    ? (right.GetLeft().IsImm() ? "imm" : "reg")
                    : (right_op == ir::OperandOp::LSL ||
                               right_op == ir::OperandOp::LSR
                               ? "shift"
                               : "composite");
            std::fprintf(stderr,
                         "[svm-scratch-peak] unit=0x%llx id=%u op=%s type=%u "
                         "flags=0x%llx branch_only=%u right=%s right_op=%u "
                         "explicit=%u vixl=%u peak=%u budget=%u\n",
                         static_cast<unsigned long long>(unit_start),
                         cur_inst->Id(),
                         ir::GetIRMetaInfo(cur_inst->GetOp()).name,
                         static_cast<u32>(cur_inst->ReturnType()),
                         static_cast<unsigned long long>(flags),
                         branch_only ? 1u : 0u,
                         right_shape,
                         static_cast<u32>(right_op.type),
                         explicit_used,
                         vixl_used,
                         explicit_used + vixl_used,
                         CurrentBudget().gpr);
        }
        ASSERT_MSG(explicit_used + vixl_used <= CurrentBudget().gpr,
                   "combined scratch GPR budget exceeded emitting opcode {}: "
                   "declared {}, explicit {}, VIXL {}",
                   cur_inst ? static_cast<u32>(cur_inst->GetOp()) : 0u,
                   CurrentBudget().gpr,
                   explicit_used,
                   vixl_used);
    }
}

void JitContext::ExcludeVixlScratch(const XRegister& reg) {
    if (vixl_scratch_scope) {
        vixl_scratch_scope->Exclude(reg);
    }
}

void JitContext::EndInstructionScratch() {
    EndVixlScratch();
}

void JitContext::BeginTerminalScratch() {
    EndVixlScratch();
    cur_dirty_gprs = tick_dirty_gprs;
    cur_dirty_fprs = tick_dirty_fprs;
    spill_tmp_gprs = 0;
    spill_tmp_fprs = 0;
    shared_tmp_gpr = -1;
    auxiliary_scratch = true;
    for (u32 code = 0; code < 32; ++code) {
        if (backend::kTerminalFixedGPRClobbers & (1u << code)) {
            cur_dirty_gprs.Mark(code);
            tick_dirty_gprs.Mark(code);
        }
    }
    if (backend::X86PinExtScratchOnlyEnabled(reg_alloc.GetGprs(), features)) {
        cur_dirty_gprs.Clear(12);
        cur_dirty_gprs.Clear(13);
        tick_dirty_gprs.Clear(12);
        tick_dirty_gprs.Clear(13);
    }
    BeginVixlScratch(true);
}

void JitContext::EndTerminalScratch() {
    EndVixlScratch();
    auxiliary_scratch = false;
}

void JitContext::BeginColdScratch() {
    EndVixlScratch();
    auxiliary_scratch = true;
    BeginVixlScratch(false);
}

void JitContext::EndColdScratch() {
    EndVixlScratch();
    auxiliary_scratch = false;
}

void JitContext::MaybeDumpHostBytes() {
    const bool enabled = GetSvmConfig().vixl_host_dump_is_set &&
                         GetSvmConfig().vixl_host_dump != "0";
    if (!enabled || host_bytes_dumped || !unit_start_set) return;
    host_bytes_dumped = true;
    const auto size = masm.GetBuffer()->GetSizeInBytes();
    const auto* bytes = masm.GetBuffer()->GetStartAddress<const u8*>();
    unsigned long long hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    std::fprintf(stderr, "[svm-host] pc=0x%llx size=%zu hash=%016llx bytes=",
                 static_cast<unsigned long long>(unit_start), size, hash);
    for (size_t i = 0; i < size; ++i) {
        std::fprintf(stderr, "%02x", static_cast<unsigned>(bytes[i]));
    }
    std::fputc('\n', stderr);
}

MacroAssembler& JitContext::GetMasm() { return masm; }

vixl::aarch64::Label* JitContext::GetLabel(LocationDescriptor location) {
    if (auto itr = labels.find(location); itr != labels.end()) {
        return &itr->second;
    } else {
        return &labels.try_emplace(location).first->second;
    }
}

vixl::aarch64::Label* JitContext::GetInternalLabel(LocationDescriptor location) {
    if (auto itr = internal_labels.find(location); itr != internal_labels.end()) {
        return &itr->second;
    }
    return &internal_labels.try_emplace(location).first->second;
}

void JitContext::FlushLabels(VAddr target) {
    for (auto &[location, label] : labels) {
        if (label.IsBound()) {
            continue;
        }
        ptrdiff_t offset = location - target;
        __ BindToOffset(&label, offset);
    }
    for (auto& [location, label] : internal_labels) {
        ASSERT_MSG(label.IsBound(),
                   "region internal target {:#x} was not emitted in this unit",
                   location);
    }
}

#undef masm

}  // namespace swift::runtime::backend::arm64
