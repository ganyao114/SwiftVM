#include "base/logging.h"
//
// Created by 甘尧 on 2023/9/15.
//

#include "jit_context.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>

#include "runtime/backend/arm64/continuation_contract.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/svm_config.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

// The spill slot count reserved in State must match the allocator's limit.
static_assert(kMaxSpillSlots == sizeof(State::spill_area) / sizeof(u64),
              "spill slot count mismatch between reg_alloc.h and context.h");

namespace {

bool IsSpillForwardBarrier(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::LoadMemory:
        case O::StoreMemory:
        case O::LoadMemoryTSO:
        case O::StoreMemoryTSO:
        case O::MemoryCopy:
        case O::MemoryCopyTSO:
        case O::CompareAndSwap:
        case O::CompareAndSwap128:
        case O::CheckMemoryAlignment:
        case O::AtomicExchange:
        case O::AtomicFetchAdd:
        case O::AtomicRMW:
        case O::CallLambda:
        case O::CallLocation:
        case O::CallDynamic:
        case O::X87Op:
        case O::Sse42Str:
        case O::Goto:
        case O::NotGoto:
        case O::BindLabel:
            return true;
        default:
            return false;
    }
}

bool IsPortableSpillForwardConsumer(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::Add:
        case O::Sub:
        case O::Adc:
        case O::Sbb:
        case O::And:
        case O::Or:
        case O::Xor:
        case O::Not:
        case O::Select:
        case O::SelectZero:
        case O::CondSelect:
        case O::LslImm:
        case O::LsrImm:
        case O::AsrImm:
        case O::LslValue:
        case O::LsrValue:
        case O::AsrValue:
        case O::GetOperand:
        case O::SetHostGPR:
        case O::StoreUniform:
            return true;
        default:
            return false;
    }
}

bool IsFlagsOnlySpillConsumer(ir::OpCode op) {
    return op == ir::OpCode::SaveFlags || op == ir::OpCode::BranchOnlyFlags;
}

}  // namespace

JitContext::JitContext(const std::shared_ptr<Module>& module,
                       RegAlloc& reg_alloc,
                       bool enable_direct_link)
        : module(module), features(ResolveFeatureSet(module->GetModuleConfig())),
          reg_alloc(reg_alloc), owned_masm(std::make_unique<MacroAssembler>()),
          masm(*owned_masm) {
    Initialize(enable_direct_link);
}

JitContext::JitContext(const std::shared_ptr<Module>& module,
                       RegAlloc& reg_alloc,
                       MacroAssembler& assembler,
                       bool enable_direct_link)
        : module(module), features(ResolveFeatureSet(module->GetModuleConfig())),
          reg_alloc(reg_alloc), masm(assembler) {
    Initialize(enable_direct_link);
}

void JitContext::Initialize(bool enable_direct_link) {
    const auto& svm_config = GetSvmConfig();
    exec_profile_enabled = svm_config.exec_prof;
    execution_trace_enabled = svm_config.exec_trace;
    if (exec_profile_enabled) {
        exec_access_pad = svm_config.exec_access_pad;
    }
    hot_coalesce_enabled = HotCoalesceProfEnabled();
    indirect_l1_prof_enabled = IndirectL1ProfEnabled();
    flags_regs_audit_enabled = features.flags_regs_audit;
    hot_static_shape_enabled = hot_coalesce_enabled || flags_regs_audit_enabled;
    hot_counter_storage_enabled =
            hot_coalesce_enabled || indirect_l1_prof_enabled ||
            flags_regs_audit_enabled;
    const bool density_enabled = GetSvmConfig().density_prof;
    density_profile_enabled = density_enabled;
    direct_link_active = enable_direct_link && module->IsDirectLinkConfigured() &&
            module->PrepareDirectLinkRegion();
}

void JitContext::AbsorbEmissionState(JitContext& region) {
    ASSERT(module == region.module && &masm == &region.masm);
    ASSERT(!vixl_scratch_contract_active &&
           !region.vixl_scratch_contract_active);

    const auto merge_map = [](auto& target, auto& source) {
        target.merge(source);
        ASSERT(source.empty());
    };
    merge_map(labels, region.labels);
    merge_map(direct_link_entry_offsets,
              region.direct_link_entry_offsets);
    merge_map(pending_flags_entry_offsets,
              region.pending_flags_entry_offsets);
    merge_map(pending_flags_target_contracts,
              region.pending_flags_target_contracts);
    merge_map(call_entry_offsets, region.call_entry_offsets);
    merge_map(call_pending_flags_entry_offsets,
              region.call_pending_flags_entry_offsets);
    merge_map(internal_labels, region.internal_labels);
    merge_map(counted_entry_labels, region.counted_entry_labels);

    const auto append = [](auto& target, auto& source) {
        target.insert(target.end(),
                      std::make_move_iterator(source.begin()),
                      std::make_move_iterator(source.end()));
        source.clear();
    };
    append(hot_coalesce_slots, region.hot_coalesce_slots);
    append(pending_direct_link_sites,
           region.pending_direct_link_sites);
    append(pending_return_sites, region.pending_return_sites);
    append(flags_merge_sites, region.flags_merge_sites);
    append(cycle_reason_sites, region.cycle_reason_sites);
}

u64 JitContext::GetPairCallTrampoline() const {
    return reinterpret_cast<u64>(
            module->GetAddressSpace().GetTrampolines().GetPairCall());
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

void JitContext::RecordHotCounter(HotCoalesceCounter counter, u32 amount) {
    if (!hot_counter_storage_enabled ||
        hot_coalesce_slot == kHotCoalesceInvalidSlot ||
        amount == 0) {
        return;
    }
    ASSERT(amount < 4096);
    const u32 begin = CurrentBufferSize();
    // Do not reserve ip0/ip1 in the allocator: doing so changes the spill
    // shape this check is meant to observe.  The check is deliberately slow
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
        hot_check_ranges.push_back({begin, CurrentBufferSize()});
    }
}

void JitContext::RecordFlagsRegsAudit(FlagsRegsAuditMergeCause cause,
                                      FlagsRegsAuditEdgeKind edge,
                                      FlagsRegsAuditCost cost,
                                      u32 amount,
                                      bool site) {
    if (!flags_regs_audit_enabled || (amount == 0 && !site)) return;
    auto it = std::find_if(
            hot_shape.flags_regs_audit.begin(),
            hot_shape.flags_regs_audit.end(),
            [cause, edge](const auto& record) {
                return record.cause == cause && record.edge == edge;
            });
    if (it == hot_shape.flags_regs_audit.end()) {
        hot_shape.flags_regs_audit.push_back({.cause = cause, .edge = edge});
        it = std::prev(hot_shape.flags_regs_audit.end());
    }
    if (site) ++it->sites;
    it->cost[static_cast<size_t>(cost)] += amount;
}

std::optional<JitContext::DeferredFlagsRegsAudit>
JitContext::DeferFlagsRegsAudit() {
    if (!flags_regs_audit_enabled ||
        flags_regs_audit_finished_slot == kHotCoalesceInvalidSlot) {
        return std::nullopt;
    }
    DeferredFlagsRegsAudit audit{
            flags_regs_audit_finished_slot,
            std::move(hot_shape),
    };
    flags_regs_audit_finished_slot = kHotCoalesceInvalidSlot;
    hot_shape = {};
    return audit;
}

void JitContext::ResumeFlagsRegsAudit(DeferredFlagsRegsAudit audit) {
    ASSERT(flags_regs_audit_enabled);
    ASSERT(flags_regs_audit_finished_slot == kHotCoalesceInvalidSlot);
    flags_regs_audit_finished_slot = audit.slot;
    hot_shape = std::move(audit.shape);
}

void JitContext::FinishDeferredFlagsRegsAudit() {
    CommitFlagsRegsAudit();
    flags_regs_audit_finished_slot = kHotCoalesceInvalidSlot;
    hot_shape = {};
}

void JitContext::CommitFlagsRegsAudit() {
    if (!flags_regs_audit_enabled ||
        flags_regs_audit_finished_slot == kHotCoalesceInvalidSlot) {
        return;
    }
    HotCoalesceUpdateUnit(flags_regs_audit_finished_slot, hot_shape);
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

Register JitContext::RForWrite(const ir::Value& value) {
    if (reg_alloc.ValueType(value) == RegAlloc::MEM) {
        return SpillGPR(value, true);
    }
    return R(value);
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

Register JitContext::SpillGPR(const ir::Value& value, bool definition) {
    const auto slot = reg_alloc.ValueMem(value);
    ASSERT_MSG(slot.offset < kMaxSpillSlots, "spill slot beyond reserved area");
    const u32 offset = state_offset_spill_area + slot.offset * sizeof(u64);
    if (cur_inst) {
        if (const auto* reload = reg_alloc.SpillReloadAt(value, cur_inst->Id())) {
            auto resident = XRegister(reload->reg);
            if (active_spill_reload_regions.size() <= reload->region) {
                active_spill_reload_regions.resize(reload->region + 1);
                slot_backed_spill_reload_regions.resize(reload->region + 1);
            }
            if (definition || (value.Defined() && value.Def() == cur_inst)) {
                active_spill_reload_regions[reload->region] = true;
                slot_backed_spill_reload_regions[reload->region] = false;
                if (reload->fault_backed &&
                    !spill_def_scratch.contains(value.Id())) {
                    spill_def_scratch.emplace(value.Id(), reload->reg);
                    pending_spill_writes.push_back(
                            {value.Id(), slot.offset,
                             static_cast<u8>(reload->reg), false, true});
                }
                return resident;
            }
            if (!active_spill_reload_regions[reload->region]) {
                active_spill_reload_regions[reload->region] = true;
                slot_backed_spill_reload_regions[reload->region] = true;
                __ Ldr(resident, MemOperand(state, offset));
                if (RAShapeProfEnabled()) ++reg_alloc.RAShape().spill_loads;
                RecordHotSpillReload();
            } else if (slot_backed_spill_reload_regions[reload->region]) {
                // The establishing reload may have been emitted on a
                // conditional arm that does not dominate this use (e.g. an
                // unaligned-atomic path); reload again so the register is
                // valid on the path reaching here.
                __ Ldr(resident, MemOperand(state, offset));
                if (RAShapeProfEnabled()) ++reg_alloc.RAShape().spill_loads;
                RecordHotSpillReload();
            }
            return resident;
        }
    }
    if (definition || (value.Defined() && value.Def() == cur_inst)) {
        // Def access: nothing to reload yet. Hand out (or reuse) the
        // scratch register the emitter will compute into and queue the
        // deferred write-back (flushed at the next TickIR / block exit).
        if (auto it = spill_def_scratch.find(value.Id()); it != spill_def_scratch.end()) {
            return XRegister(it->second);
        }
        auto tmp = GetSpillTmpX();
        spill_def_scratch.emplace(value.Id(), static_cast<u8>(tmp.GetCode()));
        pending_spill_writes.push_back(
                {value.Id(), slot.offset, static_cast<u8>(tmp.GetCode()), false});
        return tmp;
    }
    // Use access: reload from the spill slot. Any write-back of a value
    // defined by an earlier instruction has already been flushed at this
    // instruction's TickIR, so the slot is current.
    //
    // One reload register per (instruction, value): a second access within
    // the same instruction reuses the register the first reload landed in.
    // The slot cannot change under us mid-instruction (only TickIR flushes
    // writes), and emitters never write through a source register, so the
    // reuse is exact -- and it is what makes the reload demand of an
    // instruction bounded by the number of distinct values it names.
    //
    // The first access may have been emitted inside a conditional arm that
    // does not dominate this use (e.g. an unaligned-atomic path skipped by a
    // forward branch), so reload the slot again -- unless the entry is a
    // forwarded def, whose register-only value the slot does not hold.
    if (auto it = spill_use_scratch.find(value.Id()); it != spill_use_scratch.end()) {
        const auto resident = XRegister(it->second);
        if (!forwarded_spill_use_scratch.contains(value.Id())) {
            __ Ldr(resident, MemOperand(state, offset));
            if (RAShapeProfEnabled()) ++reg_alloc.RAShape().spill_loads;
            RecordHotSpillReload();
        }
        return resident;
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
        pending_spill_writes.push_back(
                {value.Id(), slot.offset, static_cast<u8>(tmp.GetCode()), true});
        return tmp;
    }
    // See SpillGPR: one reload register per (instruction, value), but the
    // load is re-emitted per access so a use on a path that did not run the
    // first reload still reads the slot. FPR entries are never forwarded.
    if (auto it = spill_use_scratch.find(value.Id()); it != spill_use_scratch.end()) {
        const auto resident = VRegister::GetVRegFromCode(it->second);
        __ Ldr(resident.Q(), MemOperand(state, offset));
        if (RAShapeProfEnabled()) ++reg_alloc.RAShape().spill_loads;
        RecordHotSpillReload();
        return resident;
    }
    auto tmp = GetSpillTmpV();
    __ Ldr(tmp.Q(), MemOperand(state, offset));
    if (RAShapeProfEnabled()) ++reg_alloc.RAShape().spill_loads;
    RecordHotSpillReload();
    spill_use_scratch.emplace(value.Id(), static_cast<u8>(tmp.GetCode()));
    return tmp;
}

void JitContext::FlushSpillWrites() {
    (void)FlushSpillWrites(nullptr);
}

std::optional<u32> JitContext::PendingScalarSpillValue() const {
    std::optional<u32> value;
    for (const auto& write : pending_spill_writes) {
        if (write.is_fpr) {
            continue;
        }
        if (value) {
            return std::nullopt;
        }
        value = write.value;
    }
    return value;
}

bool JitContext::AdoptPendingSpillWrite(
        const PendingSpillWrite& write, ir::Inst* consumer) {
    if (!consumer || write.is_fpr) {
        return false;
    }
    const auto* reload = reg_alloc.SpillReloadAt(
            write.value, consumer->Id());
    if (!reload || !reload->owns_all_uses) {
        return false;
    }
    if (active_spill_reload_regions.size() <= reload->region) {
        active_spill_reload_regions.resize(reload->region + 1);
        slot_backed_spill_reload_regions.resize(reload->region + 1);
    }
    auto resident = XRegister(reload->reg);
    if (resident.GetCode() != write.reg) {
        __ Mov(resident, XRegister(write.reg));
    }
    active_spill_reload_regions[reload->region] = true;
    slot_backed_spill_reload_regions[reload->region] = false;
    return true;
}

void JitContext::EmitSpillWriteback(const PendingSpillWrite& write) {
    const u32 offset = state_offset_spill_area + write.slot * sizeof(u64);
    if (write.is_fpr) {
        __ Str(VRegister::GetVRegFromCode(write.reg).Q(),
               MemOperand(state, offset));
    } else {
        __ Str(XRegister(write.reg), MemOperand(state, offset));
    }
    if (RAShapeProfEnabled()) ++reg_alloc.RAShape().spill_stores;
    RecordHotSpillWriteback();
}

std::optional<u8> JitContext::FlushSpillWrites(
        ir::Inst* consumer,
        bool forward_spilled_width_input,
        bool adopt_pending_spill_write,
        std::optional<u32> forward_spilled_memory_input,
        bool consumer_deferred) {
    std::optional<u8> forwarded;
    std::optional<PendingSpillWrite> retained;
    const bool scratch_only = consumer &&
            backend::X86PinExtScratchOnlyEnabled(reg_alloc.GetGprs(), features);
    const u32 fixed = consumer
            ? backend::FixedGPRClobbers(*consumer, features, scratch_only)
            : 0;
    for (auto& write : pending_spill_writes) {
        if (write.required_backing) {
            EmitSpillWriteback(write);
            continue;
        }
        bool fixed_forward = false;
#if defined(__linux__) && !defined(__ANDROID__)
        fixed_forward = write.reg == spill_scratch.GetCode();
#endif
        u32 direct_uses = 0;
        ir::Inst* definition = nullptr;
        if (consumer && !write.is_fpr) {
            for (const auto& value : consumer->GetValues()) {
                if (value.Defined() && value.Id() == write.value) {
                    definition = value.Def();
                    ++direct_uses;
                }
            }
        }
        if (definition && IsFlagsOnlySpillConsumer(consumer->GetOp()) &&
            definition->GetUses(false) == direct_uses) {
            continue;
        }
        if (definition && adopt_pending_spill_write &&
            AdoptPendingSpillWrite(write, consumer)) {
            continue;
        }
        const bool forward_memory_input =
                forward_spilled_memory_input == write.value;
        // A deferred consumer (e.g. a zext fused into a later pinned
        // publication) emits no operand read at this instruction — the value is
        // consumed later by the publication. Forwarding its spill scratch here
        // would elide the write-back while the deferred read still reloads the
        // slot, so emit the backing store instead.
        if (consumer && !consumer_deferred && !forwarded && !write.is_fpr &&
            !reg_alloc.HasSpillReload(write.value, consumer->Id()) &&
            (fixed_forward ||
             IsPortableSpillForwardConsumer(consumer->GetOp()) ||
             forward_spilled_width_input || forward_memory_input) &&
            !reg_alloc.DirtyGPR(consumer->Id()).Get(write.reg) &&
            !(fixed & (1u << write.reg)) &&
            (!IsSpillForwardBarrier(consumer->GetOp()) ||
             forward_memory_input)) {
            if (definition &&
                (definition->GetUses(false) == direct_uses || fixed_forward)) {
                spill_use_scratch.emplace(write.value, write.reg);
                forwarded_spill_use_scratch.insert(write.value);
                if (definition->GetUses(false) != direct_uses) {
                    retained = write;
                }
                forwarded = write.reg;
                continue;
            }
        }
        EmitSpillWriteback(write);
    }
    pending_spill_writes.clear();
    if (retained) {
        pending_spill_writes.push_back(*retained);
    }
    return forwarded;
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
    if (!cur_dirty_gprs.Get(spill_scratch.GetCode())) {
        cur_dirty_gprs.Mark(spill_scratch.GetCode());
        spill_tmp_gprs++;
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

JitContext::StaticForwardResult
JitContext::ForwardStatic(ir::Location location,
                          Label* cycle_exit,
                          LinkSiteKind direct_link_kind,
                          DirectLinkFlagsBypass flags_bypass) {
    // Same-module only, like the BlockLink path in Forward(): a slot filled by
    // another module outlives this module's view of it. The lookup also keeps
    // dispatch slots (a finite shared table) from being reserved for addresses
    // no module owns -- a computed jmp into unmapped memory must not consume
    // one.
    if (!CanBypassDispatcher(location)) {
        return {};
    }
    auto target_module = module;
    // The Ret this replaces leaves the translator without touching JitContext,
    // so a spilled def from the block's last instruction would never reach its
    // slot; branching straight to the next unit makes that visible.
    FlushSpillWrites();
    std::optional<FaultRange> poll_fault;
    if (cycle_exit) {
        const u32 begin = CurrentBufferSize();
        __ Ldr(wzr, MemOperand(state, state_offset_interrupt_poll));
        poll_fault = FaultRange{begin, CurrentBufferSize()};
    }
    if (EmitDirectLink(location, direct_link_kind, flags_bypass)) {
        return {true, poll_fault};
    }
    const u32 dispatcher_index = target_module->GetDispatchIndex(location);
    Label empty_slot;
    __ Mov(ipw, dispatcher_index);
    RecordFlagsRegsAudit(FlagsRegsAuditMergeCause::TerminalDispatcher,
                         FlagsRegsAuditEdgeKind::Dispatcher,
                         FlagsRegsAuditCost::CacheBaseReloadInstructions,
                         1,
                         true);
    __ Ldr(ip, MemOperand(cache, ip, LSL, 3));
    __ Cbz(ip, &empty_slot);
    RecordExecCounter(exec_offset_link_hit);
    __ Br(ip);
    __ Bind(&empty_slot);
    RecordExecCounter(exec_offset_link_miss);
    __ Mov(ip, location.Value());
    __ Str(ip, MemOperand(state, state_offset_current_loc));
    ReturnHost();
    return {true, poll_fault};
}

std::optional<JitContext::FaultRange>
JitContext::Forward(ir::Location location,
                    Label* backedge_exit,
                    Label* self_target,
                    LinkSiteKind direct_link_kind,
                    DirectLinkFlagsBypass flags_bypass) {
    ASSERT(cur_block);
    // Block exit: land any pending spill write-back before the transfer
    // (a spilled value defined by the block's last instruction may be live
    // into the target block in function mode).
    FlushSpillWrites();
    std::optional<FaultRange> poll_fault;
    if (backedge_exit) {
        const u32 begin = CurrentBufferSize();
        __ Ldr(wzr, MemOperand(state, state_offset_interrupt_poll));
        poll_fault = FaultRange{begin, CurrentBufferSize()};
    }
    auto self_forward = location == cur_block->GetStartLocation();
    if (!self_forward && cur_function) {
        self_forward = location == cur_function->GetStartLocation();
    }
    if (self_forward) {
        auto self_label = self_target ? self_target : GetLabel(location.Value());
        __ B(self_label);
    } else {
        auto target_module = module->GetAddressSpace().GetModule(location.Value());
        if (!target_module) {
            // Module miss
            __ Mov(ipw, static_cast<u32>(HaltReason::ModuleMiss));
            __ Str(ipw, MemOperand(state, state_offset_halt_reason));
            ReturnHost();
            return poll_fault;
        }

        const bool self_module_forward{module == target_module};
        const ModuleConfig& module_config{module->GetModuleConfig()};
        if (EmitDirectLink(location, direct_link_kind, flags_bypass)) {
            return poll_fault;
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
            RecordFlagsRegsAudit(FlagsRegsAuditMergeCause::TerminalDispatcher,
                                 FlagsRegsAuditEdgeKind::Dispatcher,
                                 FlagsRegsAuditCost::CacheBaseReloadInstructions,
                                 1,
                                 true);
            __ Ldr(ip, MemOperand(cache, ip, LSL, 3));
            __ Cbz(ip, &empty_slot);
            RecordExecCounter(exec_offset_link_hit);
            __ Br(ip);
            // empty slot -> back to the dispatcher for the target location.
            __ Bind(&empty_slot);
            RecordExecCounter(exec_offset_link_miss);
            __ Mov(ip, location.Value());
            __ Str(ip, MemOperand(state, state_offset_current_loc));
            ReturnHost();
        } else {
            // do not link
            __ Mov(ip, location.Value());
            __ Str(ip, MemOperand(state, state_offset_current_loc));
            ReturnHost();
        }
    }
    return poll_fault;
}

bool JitContext::EmitDirectLink(ir::Location location,
                                LinkSiteKind kind,
                                DirectLinkFlagsBypass flags_bypass) {
    if (!CanEmitDirectLink(location)) {
        return false;
    }
    u32 flags_bypass_instruction{};
    if (flags_bypass.Valid()) {
        ASSERT(flags_bypass.code_offset + sizeof(u32) <=
               flags_bypass.resume_offset);
        ASSERT(flags_bypass.resume_offset <= CurrentBufferSize());
        std::memcpy(&flags_bypass_instruction,
                    masm.GetBuffer()->GetStartAddress<const u8*>() +
                            flags_bypass.code_offset,
                    sizeof(flags_bypass_instruction));
    }
    pending_direct_link_sites.push_back({CurrentBufferSize(),
                                         location.Value(),
                                         kind,
                                         flags_bypass,
                                         flags_bypass_instruction});
    RecordFlagsRegsAudit(FlagsRegsAuditMergeCause::TerminalInternal,
                         FlagsRegsAuditEdgeKind::DirectSlow,
                         FlagsRegsAuditCost::PackInstructions,
                         2,
                         true);
    RecordFlagsRegsAudit(FlagsRegsAuditMergeCause::TerminalInternal,
                         FlagsRegsAuditEdgeKind::DirectSlow,
                         FlagsRegsAuditCost::UnpackInstructions,
                         2);
    __ dc32(*EncodeBL(0));
    return true;
}

void JitContext::EmitFlagsMergeBranch(FlagsMergeTrampolineKind kind, u8 mask) {
    const u32 offset = CurrentBufferSize();
    flags_merge_sites.push_back({offset, kind, mask});
    __ dc32(*EncodeB(0));
}

std::optional<FlagsMergeTrampolineKind>
JitContext::TakeFlagsMergeBranch(u32 code_offset) {
    if (flags_merge_sites.empty() ||
        flags_merge_sites.back().code_offset != code_offset) {
        return std::nullopt;
    }
    const auto kind = flags_merge_sites.back().kind;
    flags_merge_sites.pop_back();
    return kind;
}

void JitContext::EmitCycleReasonBranch() {
    cycle_reason_sites.push_back(
            {CurrentBufferSize(), CycleReasonTrampolineKind::Basic});
    __ dc32(*EncodeB(0));
}

void JitContext::EmitCycleFlagsMergeBranch(bool token) {
    cycle_reason_sites.push_back(
            {CurrentBufferSize(),
             token ? CycleReasonTrampolineKind::NZCVToken
                   : CycleReasonTrampolineKind::NZCV});
    __ dc32(*EncodeB(0));
}

void JitContext::EmitReturnFlagsMergeBranch(bool token) {
    ASSERT(ContinuationActive());
    pending_return_sites.push_back(
            {CurrentBufferSize(),
             token ? ReturnTrampolineKind::NZCVToken
                   : ReturnTrampolineKind::NZCV});
    __ dc32(*EncodeB(0));
}

bool JitContext::CanBypassDispatcher(ir::Location location) const {
    if (!module->GetModuleConfig().HasOpt(Optimizations::BlockLink)) {
        return false;
    }
    return module->GetAddressSpace().GetModule(location.Value()) == module;
}

std::optional<JitContext::FaultRange>
JitContext::ForwardLocal(ir::Location location,
                         Label* cycle_exit,
                         bool fallthrough,
                         Label* local_target) {
    ASSERT(cur_block);
    FlushSpillWrites();
    std::optional<FaultRange> poll_fault;
    if (cycle_exit) {
        const u32 begin = CurrentBufferSize();
        __ Ldr(wzr, MemOperand(state, state_offset_interrupt_poll));
        poll_fault = FaultRange{begin, CurrentBufferSize()};
    }
    if (!fallthrough) {
        __ B(local_target ? local_target : GetInternalLabel(location.Value()));
    }
    return poll_fault;
}

std::optional<JitContext::FaultRange>
JitContext::ForwardPublishedEntry(ir::Location location, Label* cycle_exit) {
    ASSERT(cur_block);
    FlushSpillWrites();
    std::optional<FaultRange> poll_fault;
    if (cycle_exit) {
        const u32 begin = CurrentBufferSize();
        __ Ldr(wzr, MemOperand(state, state_offset_interrupt_poll));
        poll_fault = FaultRange{begin, CurrentBufferSize()};
    }
    __ B(GetLabel(location.Value()));
    return poll_fault;
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
    ReturnHost();
}

void JitContext::ReturnHost() {
    if (!ContinuationActive()) {
        __ Ret();
        return;
    }
    pending_return_sites.push_back(
            {CurrentBufferSize(), ReturnTrampolineKind::Basic});
    __ dc32(*EncodeB(0));
}

JitContext::FaultRange
JitContext::ForwardIndirectL1(const Register& location, Label* miss) {
    ReserveTmpX(XRegister{location.GetCode()});
    const auto index = GetTmpX();
    const auto entry = GetTmpX();

    __ Ldr(entry, MemOperand(state, state_offset_indirect_l1_code_cache));
    __ Bfi(entry, location, 4, L1_CODE_CACHE_BITS);
    const u32 fault_begin = CurrentBufferSize();
    __ Ldp(index, entry, MemOperand(entry));
    const u32 fault_end = CurrentBufferSize();
    if (indirect_l1_prof_enabled) {
        __ Cmp(index, location);
        Label miss;
        // The profiling Runtime keeps invalid values at zero so this arm can
        // distinguish SMC fallback from a real hit.
        __ Ccmp(entry, xzr, ZFlag, eq);
        __ B(&miss, eq);
        RecordHotCounter(HotCoalesceCounter::IndirectL1Hit);
        if (FlagsRegsEnabled()) {
            __ Msr(NZCV, flags);
        }
        __ Br(entry);
        __ Bind(&miss);
        RecordHotCounter(HotCoalesceCounter::IndirectL1Miss);
        ReturnHost();
        return {fault_begin, fault_end};
    }

    // Production publishes value before key and invalidates a key hit to the
    // nonzero miss trampoline, so a matching key is already branch-safe.
    __ Cmp(index, location);
    if (miss) {
        __ B(miss, ne);
    } else if (ContinuationActive()) {
        Label hit;
        __ B(&hit, eq);
        ReturnHost();
        __ Bind(&hit);
    } else {
        __ Csel(entry, entry, x30, eq);
    }
    if (FlagsRegsEnabled()) {
        __ Msr(NZCV, flags);
    }
    __ Br(entry);
    return {fault_begin, fault_end};
}

JitContext::FaultRange
JitContext::ForwardContinuation(const Register& location, Label* miss, Label* null_target) {
    ASSERT(miss && null_target);
    ReserveTmpX(XRegister{location.GetCode()});
    const auto predicted = GetTmpX();
    const auto continuation = GetTmpX();
    ContinuationContract::ConsumeFrame(masm, predicted, continuation);
    __ Cmp(predicted, location);
    __ B(miss, ne);
    // External call-miss frames carry a null continuation by design: a matching
    // guest key must still not blr zero. The frame is already consumed, so fall
    // back to the shared L1 dispatch rather than the miss path, which would
    // discard valid lower frames.
    __ Cbz(continuation, null_target);
    const u32 fault_begin = CurrentBufferSize();
    __ Blr(continuation);
    return {fault_begin, CurrentBufferSize()};
}

JitContext::IndirectCallForwardResult JitContext::ForwardIndirectCall(const Register& location,
                                                                      Label* miss,
                                                                      bool pending_flags) {
    ASSERT(miss);
    ReserveTmpX(XRegister{location.GetCode()});
    const auto index = GetTmpX();
    const auto entry = GetTmpX();
    if (pending_flags) {
        __ Ldr(entry,
               MemOperand(state, state_offset_pending_call_l1_code_cache));
    } else {
        __ Ldr(entry,
               MemOperand(state, state_offset_indirect_call_l1_code_cache));
    }
    __ Bfi(entry, location, 4, L1_CODE_CACHE_BITS);
    const u32 fault_begin = CurrentBufferSize();
    __ Ldp(index, entry, MemOperand(entry));
    const u32 fault_end = CurrentBufferSize();
    __ Cmp(index, location);
    __ B(miss, ne);
    const u32 target_fault_begin = CurrentBufferSize();
    __ Blr(entry);
    return {
            .lookup_fault = {fault_begin, fault_end},
            .target_fault = {target_fault_begin, CurrentBufferSize()},
    };
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
        const auto slot = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip1;
        // Keep the 16-byte frame ABI. Both words carry the stable L2 value-slot
        // index; an OFF pop seeing this frame merely fails its guest-PC compare
        // and takes the safe dispatcher path.
        __ Mov(slot, static_cast<u64>(dispatch_index));
        __ Stp(slot, slot, MemOperand(rsb_ptr, -16, PreIndex));
        return;
    }
    const auto guest = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip0;
    const auto slot = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip1;
    // ip0 (x16) = guest return address, ip1 (x17) = dispatch table slot.
    __ Mov(guest, guest_return_addr);
    __ Mov(slot, static_cast<u64>(dispatch_index));
    // Pre-decrement push: rsb_ptr -= 16, then store the pair.
    __ Stp(guest, slot, MemOperand(rsb_ptr, -16, PreIndex));
}

void JitContext::EmitRSBPop(std::optional<XRegister> actual_target) {
    if (features.shadow_lean) {
        Label rsb_miss;
        const auto predicted = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip0;
        const auto slot = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip1;
        // Pop first: a mismatched prediction is consumed exactly like the old
        // path. The slot points at the L2 value word; the preceding word is
        // its immutable guest key. SMC clears the value word before reclaim.
        __ Ldp(predicted, slot, MemOperand(rsb_ptr, 16, PostIndex));
        __ Cbz(slot, &rsb_miss);
        RecordFlagsRegsAudit(FlagsRegsAuditMergeCause::TerminalInternal,
                             FlagsRegsAuditEdgeKind::RSBHit,
                             FlagsRegsAuditCost::CacheBaseReloadInstructions,
                             1,
                             true);
        __ Add(slot, cache, Operand(slot, LSL, 3));
        __ Ldp(predicted, slot, MemOperand(slot, -8));
        if (actual_target) {
            __ Cmp(predicted, *actual_target);
        } else {
            __ Ldr(ip, MemOperand(state, state_offset_current_loc));
            __ Cmp(predicted, ip);
        }
        __ B(&rsb_miss, ne);
        __ Cbz(slot, &rsb_miss);
        RecordExecCounter(exec_offset_rsb_hit);
        if (FlagsRegsEnabled()) {
            __ Msr(NZCV, flags);
        }
        __ Br(slot);
        __ Bind(&rsb_miss);
        RecordExecCounter(exec_offset_rsb_miss);
        ReturnHost();
        return;
    }
    Label rsb_miss;
    const auto predicted = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip0;
    __ Ldp(predicted, ip, MemOperand(rsb_ptr));
    __ Cbz(ip, &rsb_miss);
    if (actual_target) {
        __ Cmp(predicted, *actual_target);
    } else {
        const auto actual = backend::ScratchXPoolEnabled(features) ? GetTmpX() : ip1;
        __ Ldr(actual, MemOperand(state, state_offset_current_loc));
        __ Cmp(predicted, actual);
    }
    __ B(&rsb_miss, ne);
    // Prediction hit: load the L2 dispatch-table slot index and look up the
    // compiled code pointer.  cache (x27) holds the L2 table base at all
    // times (loaded once at runtime entry).  dispatch_index == 2*entry+1
    // points straight at the entry's value word (an 8-byte code pointer).
    RecordFlagsRegsAudit(FlagsRegsAuditMergeCause::TerminalInternal,
                         FlagsRegsAuditEdgeKind::RSBHit,
                         FlagsRegsAuditCost::CacheBaseReloadInstructions,
                         1,
                         true);
    __ Ldr(ip2, MemOperand(cache, ip, LSL, 3));  // ip2 (x14) = code ptr
    __ Cbz(ip2, &rsb_miss);               // empty slot → fallback
    // Commit the pop and jump directly to the target's compiled code.
    __ Add(rsb_ptr, rsb_ptr, 16);
    RecordExecCounter(exec_offset_rsb_hit);
    if (FlagsRegsEnabled()) {
        __ Msr(NZCV, flags);
    }
    __ Br(ip2);
    __ Bind(&rsb_miss);
    __ Add(rsb_ptr, rsb_ptr, 16);
    RecordExecCounter(exec_offset_rsb_miss);
    ReturnHost();
}

u32 JitContext::GetDispatchIndex(u64 guest_addr) {
    return module->GetDispatchIndex(ir::Location{guest_addr});
}

void JitContext::Finish() {
    vixl::svm_vixl_prof::JitScope prof{features.vixl_fast};
    __ FinalizeCode();
    MaybeDumpHostBytes();
    if (RAShapeProfEnabled() && !ra_shape_submitted) {
        reg_alloc.RAShape().host_bytes = CurrentBufferSize();
        RAShapeSubmitUnit(reg_alloc.RAShape());
        ra_shape_submitted = true;
    }
}

u32 JitContext::HotCheckBytesInRange(u32 begin, u32 end) const {
    u32 bytes = 0;
    for (const auto& range : hot_check_ranges) {
        const u32 overlap_begin = std::max(begin, range.begin);
        const u32 overlap_end = std::min(end, range.end);
        if (overlap_end > overlap_begin) {
            bytes += overlap_end - overlap_begin;
        }
    }
    return bytes;
}

void JitContext::FinishHotCoalesceBlock() {
    if (!hot_counter_storage_enabled ||
        hot_coalesce_slot == kHotCoalesceInvalidSlot) {
        return;
    }
    if (!hot_static_shape_enabled) {
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
    u32 check_instructions = 0;
    for (const auto& range : hot_check_ranges) {
        ASSERT(range.end >= range.begin);
        check_instructions +=
                (range.end - range.begin) / vixl::aarch64::kInstructionSize;
    }
    hot_shape.host_instructions =
            (end - hot_code_start) / vixl::aarch64::kInstructionSize -
            check_instructions;

    for (const auto& helper : reg_alloc.RAShape().helpers) {
        ASSERT(helper.calls <= UINT32_MAX);
        ASSERT(helper.capture_instructions <= UINT32_MAX);
        ASSERT(helper.capture_code_bytes <= UINT32_MAX);
        ASSERT(helper.capture_memory_bytes <= UINT32_MAX);
        hot_shape.helper_calls += static_cast<u32>(helper.calls);
        hot_shape.helper_capture_instructions +=
                static_cast<u32>(helper.capture_instructions);
        hot_shape.helper_capture_code_bytes +=
                static_cast<u32>(helper.capture_code_bytes);
        hot_shape.helper_capture_memory_bytes +=
                static_cast<u32>(helper.capture_memory_bytes);
    }

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    const auto* bytes = masm.GetBuffer()->GetStartAddress<const u8*>();
    if (hot_coalesce_enabled) {
        for (u32 offset = hot_code_start; offset < end;
             offset += vixl::aarch64::kInstructionSize) {
            if (in_range(offset, hot_check_ranges) ||
                in_range(offset, hot_nan_ranges)) {
                continue;
            }
            const auto* instruction =
                    reinterpret_cast<const vixl::aarch64::Instruction*>(bytes + offset);
            decoder.Decode(instruction);
            if (HotCoalesceIsMoveBridge(disassembler.GetOutput())) {
                ++hot_shape.move_bridges;
            }
        }
    }
    HotCoalesceUpdateUnit(hot_coalesce_slot, hot_shape);
    flags_regs_audit_finished_slot = flags_regs_audit_enabled
            ? hot_coalesce_slot
            : kHotCoalesceInvalidSlot;
    hot_coalesce_slot = kHotCoalesceInvalidSlot;
    hot_check_ranges.clear();
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
        SVM_DIAG_PRINT(Codegen, "[svm-host-map] pc=0x%llx exec=%p size=%u\n",
                     static_cast<unsigned long long>(unit_start),
                     static_cast<void*>(code_cache.exec_data),
                     CurrentBufferSize());
    }
    if (!flags_merge_sites.empty()) {
        auto* cache = module->GetCodeCache(code_cache.exec_data);
        ASSERT(cache);
        const auto& region = cache->GetRegion();
        auto* emitted = masm.GetBuffer()->GetStartAddress<u8*>();
        for (const auto& site : flags_merge_sites) {
            auto* trampoline = static_cast<u8*>([&] {
                switch (site.kind) {
                    case FlagsMergeTrampolineKind::NZCV:
                        return cache->GetFlagsMergeRegionTrampoline();
                    case FlagsMergeTrampolineKind::NZCVToken:
                        return cache->GetFlagsMergeTokenRegionTrampoline();
                    case FlagsMergeTrampolineKind::NZCVMask:
                        return cache->GetFlagsMaskMergeRegionTrampoline(site.mask);
                    case FlagsMergeTrampolineKind::NZCVMaskToken:
                        return cache->GetFlagsMaskMergeTokenRegionTrampoline(site.mask);
                }
                UNREACHABLE();
            }());
            ASSERT(trampoline && region.ContainsRx(trampoline));
            auto* rx_site = code_cache.exec_data + site.code_offset;
            const auto branch = EncodeB(trampoline - rx_site);
            ASSERT(branch);
            std::memcpy(emitted + site.code_offset, &*branch, sizeof(*branch));
        }
    }
    if (!cycle_reason_sites.empty()) {
        auto* cache = module->GetCodeCache(code_cache.exec_data);
        ASSERT(cache);
        auto* emitted = masm.GetBuffer()->GetStartAddress<u8*>();
        for (const auto& site : cycle_reason_sites) {
            auto* trampoline = static_cast<u8*>([&] {
                switch (site.kind) {
                    case CycleReasonTrampolineKind::Basic:
                        return cache->GetCycleReasonRegionTrampoline();
                    case CycleReasonTrampolineKind::NZCV:
                        return cache->GetCycleFlagsMergeRegionTrampoline();
                    case CycleReasonTrampolineKind::NZCVToken:
                        return cache->GetCycleFlagsMergeTokenRegionTrampoline();
                }
                PANIC();
            }());
            ASSERT(trampoline && cache->GetRegion().ContainsRx(trampoline));
            auto* rx_site = code_cache.exec_data + site.code_offset;
            const auto branch = EncodeB(trampoline - rx_site);
            ASSERT(branch);
            std::memcpy(emitted + site.code_offset, &*branch, sizeof(*branch));
        }
    }
    if (!pending_return_sites.empty()) {
        auto* cache = module->GetCodeCache(code_cache.exec_data);
        ASSERT(cache);
        auto* emitted = masm.GetBuffer()->GetStartAddress<u8*>();
        for (const auto& site : pending_return_sites) {
            auto* trampoline = static_cast<u8*>([&] {
                switch (site.kind) {
                    case ReturnTrampolineKind::Basic:
                        return cache->GetReturnRegionTrampoline();
                    case ReturnTrampolineKind::NZCV:
                        return cache->GetReturnFlagsMergeRegionTrampoline();
                    case ReturnTrampolineKind::NZCVToken:
                        return cache->GetReturnFlagsMergeTokenRegionTrampoline();
                }
                PANIC();
            }());
            ASSERT(trampoline && cache->GetRegion().ContainsRx(trampoline));
            auto* rx_site = code_cache.exec_data + site.code_offset;
            const auto branch = EncodeB(trampoline - rx_site);
            ASSERT(branch);
            std::memcpy(emitted + site.code_offset, &*branch, sizeof(*branch));
        }
    }
    if (!pending_direct_link_sites.empty()) {
        auto* cache = module->GetCodeCache(code_cache.exec_data);
        ASSERT(cache);
        const auto& region = cache->GetRegion();
        auto* canonical_trampoline =
                static_cast<u8*>(cache->GetRegionTrampoline());
        auto* pending_flags_trampoline =
                static_cast<u8*>(cache->GetPendingFlagsRegionTrampoline());
        auto* flags_merge_trampoline =
                static_cast<u8*>(cache->GetFlagsMergeRegionTrampoline());
        ASSERT(canonical_trampoline &&
               region.ContainsRx(canonical_trampoline));
        ASSERT(pending_flags_trampoline &&
               region.ContainsRx(pending_flags_trampoline));
        ASSERT(flags_merge_trampoline &&
               region.ContainsRx(flags_merge_trampoline));
        const LinkSourceOwner owner{module.get(), code_cache.exec_data};
        auto* emitted = masm.GetBuffer()->GetStartAddress<u8*>();
        for (auto& pending : pending_direct_link_sites) {
            ASSERT(static_cast<size_t>(pending.code_offset) + sizeof(u32) <= code_cache.size);
            auto* rx_site = code_cache.exec_data + pending.code_offset;
            auto* trampoline = pending.flags_bypass.Valid()
                    ? pending_flags_trampoline
                    : canonical_trampoline;
            const auto branch = EncodeBL(trampoline - rx_site);
            ASSERT(branch);
            std::memcpy(emitted + pending.code_offset, &*branch, sizeof(*branch));
            LinkFlagsBypassPatch flags_bypass_patch{};
            if (pending.flags_bypass.Valid()) {
                if (pending.flags_bypass.merge_branch_offset != UINT32_MAX) {
                    ASSERT(static_cast<size_t>(
                                   pending.flags_bypass.merge_branch_offset) +
                                   sizeof(u32) <= code_cache.size);
                    auto* rx_merge_branch = code_cache.exec_data +
                            pending.flags_bypass.merge_branch_offset;
                    const auto merge_branch = EncodeB(
                            flags_merge_trampoline - rx_merge_branch);
                    ASSERT(merge_branch);
                    std::memcpy(emitted +
                                        pending.flags_bypass.merge_branch_offset,
                                &*merge_branch,
                                sizeof(*merge_branch));
                }
                if (pending.flags_bypass.linked_instruction) {
                    std::memcpy(&pending.flags_bypass_instruction,
                                emitted + pending.flags_bypass.code_offset,
                                sizeof(pending.flags_bypass_instruction));
                }
                u32 linked_instruction = pending.flags_bypass.linked_instruction;
                if (!linked_instruction) {
                    const auto linked_branch = EncodeB(
                            static_cast<intptr_t>(pending.flags_bypass.resume_offset) -
                            static_cast<intptr_t>(pending.flags_bypass.code_offset));
                    ASSERT(linked_branch);
                    linked_instruction = *linked_branch;
                }
                flags_bypass_patch = {
                        .rx_site = code_cache.exec_data +
                                pending.flags_bypass.code_offset,
                        .rw_site = code_cache.rw_data +
                                pending.flags_bypass.code_offset,
                        .unlinked_instruction = pending.flags_bypass_instruction,
                        .linked_instruction = linked_instruction,
                };
                std::memcpy(emitted + pending.flags_bypass.code_offset,
                            &linked_instruction,
                            sizeof(linked_instruction));
            }
            const LinkSiteKey key{
                    region.id,
                    code_cache.offset + pending.code_offset,
            };
            const LinkSignalPatchSite signal_patch{
                    .region = region,
                    .rx_site = rx_site,
                    .rw_site = code_cache.rw_data + pending.code_offset,
                    .unlinked_bl = *branch,
                    .flags_bypass = flags_bypass_patch,
            };
            ASSERT(module->GetAddressSpace().GetLinkManager().RegisterSite(
                    key,
                    pending.guest_target,
                    owner,
                    &signal_patch,
                    pending.kind,
                    pending.flags_bypass.edge_flags));
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

ptrdiff_t JitContext::GetDirectLinkCodeOffset(LocationDescriptor location) const {
    if (const auto it = direct_link_entry_offsets.find(location);
        it != direct_link_entry_offsets.end()) {
        return it->second;
    }
    return GetCodeOffset(location);
}

ptrdiff_t JitContext::GetPendingFlagsCodeOffset(
        LocationDescriptor location) const {
    if (const auto it = pending_flags_entry_offsets.find(location);
        it != pending_flags_entry_offsets.end()) {
        return it->second;
    }
    return -1;
}

EdgeFlagsTargetContract JitContext::GetPendingFlagsTargetContract(
        LocationDescriptor location) const {
    if (const auto it = pending_flags_target_contracts.find(location);
        it != pending_flags_target_contracts.end()) {
        return it->second;
    }
    return {};
}

ptrdiff_t JitContext::GetCallCodeOffset(LocationDescriptor location) const {
    if (const auto it = call_entry_offsets.find(location);
        it != call_entry_offsets.end()) {
        return it->second;
    }
    return -1;
}

ptrdiff_t JitContext::GetCallPendingFlagsCodeOffset(
        LocationDescriptor location) const {
    if (const auto it = call_pending_flags_entry_offsets.find(location);
        it != call_pending_flags_entry_offsets.end()) {
        return it->second;
    }
    return -1;
}

void JitContext::RecordDirectLinkEntry(LocationDescriptor location) {
    auto* entry = GetCountedEntryLabel(location);
    ASSERT(entry->IsBound());
    direct_link_entry_offsets.insert_or_assign(
            location, static_cast<u32>(entry->GetLocation()));
}

void JitContext::RecordPendingFlagsEntry(
        LocationDescriptor location,
        EdgeFlagsTargetContract contract) {
    ASSERT(contract.CanPublishPendingEntry());
    auto* entry = GetCountedEntryLabel(location);
    ASSERT(entry->IsBound());
    pending_flags_entry_offsets.insert_or_assign(
            location, static_cast<u32>(entry->GetLocation()));
    pending_flags_target_contracts.insert_or_assign(location, contract);
}

void JitContext::EmitPendingFlagsCallEntry(LocationDescriptor location) {
    const auto contract = pending_flags_target_contracts.find(location);
    const auto full_nzcv = EdgeFlagsState::Pending(
            kEdgeNZCVMask,
            EdgeCarryPolarity::Unknown,
            EdgeFlagsProducer::Restore);
    if (!ContinuationActive() ||
        !pending_flags_entry_offsets.contains(location) ||
        contract == pending_flags_target_contracts.end() ||
        !contract->second.Accepts(full_nzcv)) {
        return;
    }
    call_pending_flags_entry_offsets.emplace(location, CurrentBufferSize());
    ContinuationContract::PublishFrame(masm);
    __ B(GetCountedEntryLabel(location));
}

bool JitContext::IsUniform(const Register& reg) {
    auto &uniform_info = module->GetAddressSpace().GetUniformInfo();
    if (reg.IsV()) {
        return uniform_info.uni_fprs.Get(reg.GetCode());
    } else {
        return uniform_info.uni_gprs.Get(reg.GetCode());
    }
}

void JitContext::SetCurrent(ir::Block* block, bool split_backedge_entry,
                            bool defer_published_entry) {
    cur_block = block;
    if (!unit_start_set) {
        unit_start = block->GetStartLocation().Value();
        unit_start_set = true;
    }
    auto label = GetLabel(block->GetStartLocation().Value());
    if (!defer_published_entry && !label->IsBound()) {
        __ Bind(label);
    } else if (defer_published_entry) {
        if (!split_backedge_entry && !label->IsBound()) {
            __ Bind(label);
        }
        auto* counted = GetCountedEntryLabel(block->GetStartLocation().Value());
        if (!counted->IsBound()) {
            __ Bind(counted);
        }
    }
    if (hot_counter_storage_enabled) {
        if (hot_coalesce_enabled) ASSERT(!hot_collecting);
        hot_coalesce_slot =
                HotCoalesceRegisterUnit(block->GetStartLocation().Value());
    }
    if (hot_static_shape_enabled) {
        flags_regs_audit_finished_slot = kHotCoalesceInvalidSlot;
        hot_shape = {};
        hot_shape.guest_entry = block->GetStartLocation().Value();
        hot_shape.host_offset = CurrentBufferSize();
        if (hot_coalesce_enabled) {
            hot_shape.uniform = HotCoalesceAnalyzeUniformSequences(block);
            HotCoalesceAnalyzeLinkTargets(block, hot_shape);
        }
        if (hot_coalesce_slot != kHotCoalesceInvalidSlot) {
            hot_coalesce_slots.push_back(hot_coalesce_slot);
        }
        hot_check_ranges.clear();
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
    // Taken internal edges skip the entry counter. L2 veneers land on the
    // counted-entry label bound before BeginBackedgeBody.
    __ Bind(label);
}

void JitContext::BeginBackedgeBody() {
    if (hot_static_shape_enabled) {
        if (hot_coalesce_enabled) {
            RecordHotCounter(HotCoalesceCounter::Entries);
        }
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
    if (ContinuationActive()) {
        call_entry_offsets.emplace(function->GetStartLocation().Value(),
                                   CurrentBufferSize());
        ContinuationContract::PublishFrame(masm);
    }
}

void JitContext::TickIR(ir::Inst* instr,
                        bool forward_spilled_width_input,
                        bool adopt_pending_spill_write,
                        std::optional<u32> forward_spilled_memory_input,
                        bool consumer_deferred) {
    EndVixlScratch();
    spill_def_scratch.clear();
    spill_use_scratch.clear();
    forwarded_spill_use_scratch.clear();
    const auto forwarded_spill = FlushSpillWrites(
            instr, forward_spilled_width_input, adopt_pending_spill_write,
            forward_spilled_memory_input, consumer_deferred);
    cur_inst = instr;
    reg_alloc.SetCurrent(instr);
    cur_dirty_gprs = reg_alloc.GetDirtyGPR();
    cur_dirty_fprs = reg_alloc.GetDirtyFPR();
    if (forwarded_spill) {
        cur_dirty_gprs.Mark(*forwarded_spill);
    }
    // Baseline for the per-instruction scratch budget (see GetTmpX).
    tick_dirty_gprs = cur_dirty_gprs;
    tick_dirty_fprs = cur_dirty_fprs;
    spill_tmp_gprs = 0;
    spill_tmp_fprs = 0;
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
        // FLAGS_REGS keeps x12 as last_result; it is never a scratch lease.
        for (u32 code : {12u, 13u}) {
            if (code == 12 && GetSvmConfig().flags_regs) {
                continue;
            }
            if (!(fixed & (1u << code)) &&
                (!forwarded_spill || *forwarded_spill != code)) {
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
                                                  instr->GetOp()) &&
        (!forwarded_spill || *forwarded_spill != 10)) {
        cur_dirty_gprs.Clear(10);
        tick_dirty_gprs.Clear(10);
    }
    BeginVixlScratch(true);
}

void JitContext::BeginVixlScratch(bool allow_pool) {
    ASSERT(!vixl_scratch_contract_active);
    ASSERT(!vixl_scratch_scope);
    const bool audit_gpr = backend::ScratchXPoolEnabled(features);
    u32 allowed = audit_gpr
            ? 0
            : static_cast<u32>(masm.GetScratchRegisterList()->GetList());
    if (audit_gpr && allow_pool) {
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
    // VIXL defaults fptmp_list_ to d31. SwiftVM never budgets a hidden V
    // scratch register, so keep its allow-list empty and fail before a macro
    // can silently overwrite a resident guest value.
    masm.SvmBeginScratchContract(allowed, 0);
    vixl_scratch_contract_active = true;
    if (!audit_gpr) {
        return;
    }
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
    if (vixl_scratch_contract_active) {
        const bool audit_gpr = static_cast<bool>(vixl_scratch_scope);
        vixl_scratch_scope.reset();
        const auto acquired = masm.SvmEndScratchContract();
        vixl_scratch_contract_active = false;
        ASSERT_MSG(acquired.vreg == 0,
                   "VIXL acquired an unbudgeted scratch V register mask 0x{:x}",
                   acquired.vreg);
        if (!audit_gpr) {
            return;
        }
        const u32 explicit_used =
                static_cast<u32>(cur_dirty_gprs.GetMarkedCount() -
                                 tick_dirty_gprs.GetMarkedCount()) -
                spill_tmp_gprs;
        // A VIXL register already declared as a fixed opcode clobber consumes
        // no dynamic scratch headroom; RA excluded it independently.
        const u32 fixed = cur_inst ? backend::FixedGPRClobbers(*cur_inst, features) : 0;
        const u32 vixl_used = static_cast<u32>(
                __builtin_popcountll(acquired.gpr & ~fixed));
        last_instruction_scratch_gpr = explicit_used + vixl_used;
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
        if (!GetSvmConfig().flags_regs) {
            cur_dirty_gprs.Clear(12);
            tick_dirty_gprs.Clear(12);
        }
        cur_dirty_gprs.Clear(13);
        tick_dirty_gprs.Clear(13);
    }
    BeginVixlScratch(true);
}

void JitContext::EndTerminalScratch() {
    EndVixlScratch();
    auxiliary_scratch = false;
}

void JitContext::ReserveLevel3IndirectTarget(const ir::Value& value) {
    if (backend::X86PinExtLevel3Enabled(reg_alloc.GetGprs())) {
        ReserveTmpX(X(value));
    }
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
    SVM_DIAG_PRINT(Codegen, "[svm-host] pc=0x%llx size=%zu hash=%016llx bytes=",
                 static_cast<unsigned long long>(unit_start), size, hash);
    for (size_t i = 0; i < size; ++i) {
        SVM_DIAG_PRINT(Codegen, "%02x", static_cast<unsigned>(bytes[i]));
    }
    SVM_DIAG_PRINT(Codegen, "%c", '\n');
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

vixl::aarch64::Label* JitContext::GetCountedEntryLabel(
        LocationDescriptor location) {
    if (auto itr = counted_entry_labels.find(location);
        itr != counted_entry_labels.end()) {
        return &itr->second;
    }
    return &counted_entry_labels.try_emplace(location).first->second;
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
