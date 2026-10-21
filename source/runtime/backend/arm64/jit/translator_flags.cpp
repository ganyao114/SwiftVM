#include "translator.h"

#include <algorithm>
#include <bit>
#include <optional>

#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/common/svm_config.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::RecordPFAFDensity(PFAFDensityKind kind, u32 begin) {
    const u32 bytes = context.CurrentBufferSize() - begin;
    if (context.FlagsRegsAuditEnabled()) {
        if (!flags_audit_cold) {
            FlagsRegsAuditCost cost = FlagsRegsAuditCost::PFSavedInstructions;
            u32 saved = 0;
            switch (kind) {
                case PFAFDensityKind::PFRead:
                    saved = 1;
                    break;
                case PFAFDensityKind::AFWrite:
                    cost = FlagsRegsAuditCost::AFSavedInstructions;
                    // Only the four-instruction computational AF form has the
                    // mechanical 4 -> 2 raw-xor saving. Clear/FCmp writes and
                    // shorter immediate forms receive no credit.
                    saved = bytes == 4 * sizeof(u32) ? 2 : 0;
                    break;
                case PFAFDensityKind::AFRead:
                    cost = FlagsRegsAuditCost::AFSavedInstructions;
                    saved = 1;
                    break;
                case PFAFDensityKind::PFWrite:
                case PFAFDensityKind::SharedPack:
                case PFAFDensityKind::WholeFlags:
                case PFAFDensityKind::Count:
                    break;
            }
            context.RecordFlagsRegsAudit(
                    FlagsRegsAuditMergeCause::ClearOrPartialWrite,
                    flags_audit_block_edge,
                    cost,
                    saved,
                    true);
        }
    }
    if (!context.DensityProfileEnabled()) return;
    // 单块的某个 PF/AF 子桶不会接近 64 KiB。高 16 位保存
    // 翻译期的 site 数，不增大 JitTranslator，也不改发码。
    statistics.pfaf_density_bytes[static_cast<size_t>(kind)] +=
            (1u << 16) | bytes;
}

void JitTranslator::BeginFlagsTokenProducer(const PseudoFlags& pseudo) {
    if (pseudo.branch_only) {
        return;
    }
    if (FlagsRegsEnabled()) {
        const auto overwritten = pseudo.set | pseudo.clear;
        if (flag_state.flags_token_valid) {
            if (True(overwritten & ir::Flags::Parity)) {
                InvalidateFlagsToken();
            } else {
                PublishFlagsToken();
            }
        }
        if (flag_state.nzcv_dirty) {
            const auto overwritten_nzcv =
                    GuestNZCVToHost(overwritten & ir::Flags::NZCV);
            if (True(flag_state.nzcv_requested & ~overwritten_nzcv)) {
                MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
                          flags_audit_block_edge);
            } else {
                flag_state.nzcv_dirty = false;
                flag_state.nzcv_requested = {};
            }
        }
        return;
    }
    MergeNZCV();
}

Register JitTranslator::FlagsResultRegister(
        ir::Inst* inst, const PseudoFlags& pseudo) {
    if (const auto pinned = context.PlannedGPR<XRegister>(ir::Value{inst}, cur_instr)) {
        return ir::GetValueSizeByte(inst->ReturnType()) > sizeof(u32)
                ? Register{*pinned}
                : Register{pinned->W()};
    }
    if (FlagsRegsEnabled() && !pseudo.Null() && !pseudo.branch_only &&
        !context.InstructionHasValueUses()) {
        // AF/NZCV-only producers reuse x12, but don't create a new PF token.
        // Publish the previous PF before its backing register is overwritten.
        if (!True(pseudo.set & ir::Flags::Parity) && flag_state.flags_token_valid &&
            flag_state.flags_token_result_code == atomic_scratch.GetCode()) {
            PublishFlagsToken();
            InvalidateFlagsToken();
        }
        return ir::GetValueSizeByte(inst->ReturnType()) > sizeof(u32)
                ? atomic_scratch
                : atomic_scratch.W();
    }
    return context.R(ir::Value{inst});
}

bool JitTranslator::CanRetainFlagsTokenResult(
        ir::Inst* producer, const Register& result) {
    return gpr_plan && gpr_plan->flags_tokens && cur_instr &&
           gpr_plan->flags_tokens->Retains(producer, result.GetCode(), cur_instr->Id());
}

XRegister JitTranslator::FlagsTokenResult() const {
    return XRegister{flag_state.flags_token_result_code};
}

void JitTranslator::MaterializeFlagsTokenResult() {
    if (!flag_state.flags_token_valid ||
        flag_state.flags_token_result_code == atomic_scratch.GetCode()) {
        return;
    }
    __ Mov(atomic_scratch, FlagsTokenResult());
    if (!flag_state.flags_token_keep) {
        flag_state.flags_token_result_code = atomic_scratch.GetCode();
    }
}

void JitTranslator::InvalidateFlagsToken() {
    flag_state.flags_token_valid = false;
    flag_state.flags_token_result_code = atomic_scratch.GetCode();
}

void JitTranslator::CaptureFlagsToken(const Register& result,
                                      ir::ValueType type,
                                      ir::Inst* producer) {
    if (!FlagsRegsEnabled()) {
        return;
    }
    const bool wide = ir::GetValueSizeByte(type) > 4;
    if (result.GetCode() != atomic_scratch.GetCode() &&
        !CanRetainFlagsTokenResult(producer, result)) {
        if (wide) {
            __ Mov(atomic_scratch, result.X());
        } else {
            __ Mov(atomic_scratch.W(), result.W());
        }
        flag_state.flags_token_result_code = atomic_scratch.GetCode();
    } else {
        flag_state.flags_token_result_code = result.GetCode();
    }
    flag_state.flags_token_valid = true;
}

void JitTranslator::FinishFlagsTokenProducer(const Register& result,
                                             ir::ValueType type,
                                             const PseudoFlags& pseudo,
                                             ir::Inst* producer) {
    if (!FlagsRegsEnabled() || pseudo.branch_only ||
        !True(pseudo.set & ir::Flags::Parity)) {
        return;
    }
    // A producer that defines PF always supersedes the previous token. Most
    // paths invalidate it in BeginFlagsTokenProducer, but helpers such as
    // MUL and split pseudo flag sequences can arrive here after a publish
    // without that invalidation. Retaining the old token in that case makes
    // LAHF/JP observe stale parity and can also retain a dead result register.
    CaptureFlagsToken(result, type, producer);
}

void JitTranslator::EmitSplitFlagsPublish() {
    MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
              flags_audit_block_edge);
}

bool JitTranslator::TryEmitCycleExitFlags() {
    const auto requested = PendingNZCVMergeMask(
            FlagsRegsAuditMergeCause::ClearOrPartialWrite);
    if (!FlagsRegsEnabled() || !context.CanUseRegionTrampoline() ||
        !requested || *requested != static_cast<u64>(HostFlags::NZCV)) {
        return false;
    }
    const bool token = flag_state.flags_token_valid;
    if (token) {
        MaterializeFlagsTokenResult();
    }
    context.EmitCycleFlagsMergeBranch(token);
    if (!flag_state.flags_token_keep) {
        flag_state.nzcv_dirty = false;
        flag_state.nzcv_requested = {};
        InvalidateFlagsToken();
    }
    return true;
}

void JitTranslator::MergeNZCV() {
    (void)MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
                    flags_audit_block_edge);
}

void JitTranslator::EmitNZCVMerge(u64 requested,
                                  const Register& scratch,
                                  const Register* mask_scratch) {
    const u64 nzcv = static_cast<u64>(HostFlags::NZCV);
    ASSERT(requested && !(requested & ~nzcv));
    const u32 lsb = std::countr_zero(requested);
    const u64 adjusted = requested >> lsb;
    if (requested != nzcv && !(adjusted & (adjusted + 1))) {
        const u32 width = std::bit_width(adjusted);
        __ Mrs(scratch, NZCV);
        __ Ubfx(scratch, scratch, lsb, width);
        __ Bfi(flags, scratch, lsb, width);
        return;
    }

    u64 keep = ~requested;
    __ Mrs(scratch, NZCV);
    if (mask_scratch) {
        ASSERT(*mask_scratch != scratch);
        __ Mov(*mask_scratch, keep);
        __ And(flags, flags, *mask_scratch);
    } else {
        __ And(flags, flags, ForceCast<s64>(keep));
    }
    if (requested != nzcv) {
        if (mask_scratch) {
            __ Mov(*mask_scratch, requested);
            __ And(scratch, scratch, *mask_scratch);
        } else {
            __ And(scratch, scratch, static_cast<u32>(requested));
        }
    }
    __ Orr(flags, flags, scratch);
}

DirectLinkFlagsBypass JitTranslator::EmitDeferredNZCVMerge(
        const XRegister& scratch,
        const XRegister& token) {
    ASSERT(scratch != token);
    auto stub = std::find_if(
            deferred_nzcv_merge_stubs.begin(),
            deferred_nzcv_merge_stubs.end(),
            [&](const auto& candidate) {
                return candidate.scratch == scratch && candidate.token == token;
            });
    if (stub == deferred_nzcv_merge_stubs.end()) {
        deferred_nzcv_merge_stubs.push_back(
                {scratch, token, std::make_unique<Label>()});
        stub = std::prev(deferred_nzcv_merge_stubs.end());
    }

    const u32 begin = context.CurrentBufferSize();
    __ Bl(stub->entry.get());
    alignas(u32) u32 linked_instruction{};
    Assembler encoder{reinterpret_cast<vixl::byte*>(&linked_instruction),
                      sizeof(linked_instruction)};
    encoder.bfi(flags, token, HostFlagsBit::ParityByte, 8);
    encoder.FinalizeCode();
    return {begin, context.CurrentBufferSize(), linked_instruction};
}

DirectLinkFlagsBypass JitTranslator::EmitOutlinedNZCVMerge() {
    vixl::EmissionCheckScope emission{&masm, 2 * kInstructionSize};
    Label resume;
    const u32 begin = context.CurrentBufferSize();
    __ Adr(ip1, &resume);
    const u32 merge_branch = context.CurrentBufferSize();
    __ dc32(*EncodeB(0));
    __ Bind(&resume);
    return {begin, context.CurrentBufferSize(), 0, merge_branch};
}

DirectLinkFlagsBypass JitTranslator::EmitOutlinedNZCVMergeResume(bool token) {
    if (token) {
        MaterializeFlagsTokenResult();
    }
    vixl::EmissionCheckScope emission{&masm, 2 * kInstructionSize};
    Label resume;
    const u32 begin = context.CurrentBufferSize();
    __ Adr(ip1, &resume);
    const u32 merge_branch = context.CurrentBufferSize();
    context.EmitFlagsMergeBranch(
            token ? FlagsMergeTrampolineKind::NZCVToken
                  : FlagsMergeTrampolineKind::NZCV);
    __ Bind(&resume);
    return {begin, context.CurrentBufferSize(), 0, merge_branch};
}

void JitTranslator::EmitDeferredNZCVMergeStubs() {
    if (deferred_nzcv_merge_stubs.empty()) {
        return;
    }
    context.BeginColdScratch();
    for (auto& stub : deferred_nzcv_merge_stubs) {
        __ Bind(stub.entry.get());
        EmitNZCVMerge(static_cast<u64>(HostFlags::NZCV), stub.scratch);
        __ Bfi(flags, stub.token, HostFlagsBit::ParityByte, 8);
        __ Ret();
    }
    context.EndColdScratch();
    deferred_nzcv_merge_stubs.clear();
}

bool JitTranslator::TryEmitReturnFlagsBypass(
        const DirectLinkFlagsBypass& flags_bypass) {
    if (!FlagsRegsEnabled() || !context.ContinuationActive() ||
        !context.CanUseRegionTrampoline() || !flags_bypass.Valid() ||
        flags_bypass.edge_flags.valid_nzcv_mask != kEdgeNZCVMask ||
        flags_bypass.resume_offset != context.CurrentBufferSize() ||
        flags_bypass.linked_instruction != 0) {
        return false;
    }
    const bool outlined_merge =
            flags_bypass.merge_branch_offset ==
                    flags_bypass.code_offset + sizeof(u32) &&
            flags_bypass.resume_offset ==
                    flags_bypass.code_offset + 2 * sizeof(u32);
    const bool inline_merge =
            flags_bypass.merge_branch_offset == UINT32_MAX &&
            flags_bypass.resume_offset ==
                    flags_bypass.code_offset + 3 * sizeof(u32);
    if (!outlined_merge && !inline_merge) {
        return false;
    }
    const auto merge_kind = outlined_merge
            ? context.TakeFlagsMergeBranch(flags_bypass.merge_branch_offset)
            : std::nullopt;
    masm.GetBuffer()->Rewind(flags_bypass.code_offset);
    context.EmitReturnFlagsMergeBranch(
            merge_kind == FlagsMergeTrampolineKind::NZCVToken);
    return true;
}

std::optional<u64> JitTranslator::PendingNZCVMergeMask(
        FlagsRegsAuditMergeCause cause) const {
    const bool force_ret_pstate =
            FlagsRegsEnabled() && !flag_state.nzcv_dirty && !True(flag_state.nzcv_requested) &&
            BlockIsFlagsTransparent(cur_block) &&
            (cause == FlagsRegsAuditMergeCause::HostExit ||
             (cause == FlagsRegsAuditMergeCause::TerminalDispatcher &&
              RegionEdgesActive()));
    if (!(flag_state.save_in_nzcv && flag_state.nzcv_dirty) && !force_ret_pstate) {
        return std::nullopt;
    }
    return force_ret_pstate ? static_cast<u64>(HostFlags::NZCV)
                            : static_cast<u64>(flag_state.nzcv_requested);
}

bool JitTranslator::CanDeferFullNZCVMerge(
        FlagsRegsAuditMergeCause cause) const {
    const auto requested = PendingNZCVMergeMask(cause);
    return FlagsRegsEnabled() && RegionEdgesActive() && requested &&
           *requested == static_cast<u64>(HostFlags::NZCV);
}

DirectLinkFlagsBypass JitTranslator::MergeNZCV(
        FlagsRegsAuditMergeCause cause,
        FlagsRegsAuditEdgeKind edge,
        bool outline_direct_link) {
    DirectLinkFlagsBypass flags_bypass{};
    bool deferred_merge{};
    const auto requested = PendingNZCVMergeMask(cause);
    if (requested) {
        const u32 begin = context.CurrentBufferSize();
        // Only merge the NZCV bits that SaveFlags actually requested.
        // Bits NOT requested (e.g. C/V when only SF/ZF were saved) keep
        // their existing value in the flags register, so a ClearFlags(CF)
        // between two flag-setting instructions is not overwritten.
        const u64 req = *requested;
        const bool full_nzcv = req == static_cast<u64>(HostFlags::NZCV);
        const bool cold_outline = context.ColdScratchActive() &&
                                  context.CanUseRegionTrampoline() &&
                                  !outline_direct_link && full_nzcv;
        deferred_merge = (outline_direct_link && flag_state.flags_token_keep && full_nzcv) ||
                         cold_outline;
        if (deferred_merge) {
            if (cold_outline) {
                flags_bypass = EmitOutlinedNZCVMergeResume(flag_state.flags_token_valid);
            } else if (flag_state.flags_token_valid) {
                const auto scratch = context.GetSharedTmpX();
                flags_bypass = EmitDeferredNZCVMerge(scratch, FlagsTokenResult());
            } else {
                flags_bypass = EmitOutlinedNZCVMerge();
            }
        } else {
            const auto scratch = context.GetSharedTmpX();
            if (context.ColdScratchActive()) {
                const Register mask_scratch = scratch == ip0 ? ip1 : ip0;
                EmitNZCVMerge(req, scratch, &mask_scratch);
            } else {
                EmitNZCVMerge(req, scratch);
            }
        }
        const u32 merge_end = context.CurrentBufferSize();
        if (!deferred_merge && FlagsRegsEnabled() && RegionEdgesActive()) {
            const auto edge_flags = PendingEdgeFlagsState(
                    static_cast<HostFlags>(req),
                    flag_state.save_in_nzcv && flag_state.nzcv_dirty
                            ? EdgeFlagsProducer::Arithmetic
                            : EdgeFlagsProducer::Restore);
            if (edge_flags.HasPendingPState()) {
                flags_bypass = {begin, merge_end};
            }
        }
        if (flags_bypass.code_offset != UINT32_MAX) {
            flags_bypass.edge_flags = PendingEdgeFlagsState(
                    static_cast<HostFlags>(req),
                    flag_state.save_in_nzcv && flag_state.nzcv_dirty
                            ? EdgeFlagsProducer::Arithmetic
                            : EdgeFlagsProducer::Restore);
            ASSERT(flags_bypass.Valid());
        }
        if (!flag_state.flags_token_keep) {
            flag_state.nzcv_dirty = false;
            flag_state.nzcv_requested = {};
        }
        const u32 instructions =
                (context.CurrentBufferSize() - begin) / sizeof(u32);
        if (flags_audit_cold) {
            cause = FlagsRegsAuditMergeCause::FaultVeneer;
            edge = FlagsRegsAuditEdgeKind::Host;
        }
        const u32 saved = cause == FlagsRegsAuditMergeCause::AdvancePC &&
                                  flags_audit_strict_advance &&
                                  instructions == 4
                ? instructions
                : 0;
        context.RecordFlagsRegsAudit(cause,
                                     edge,
                                     FlagsRegsAuditCost::MergeSavedInstructions,
                                     saved,
                                     true);
        // Incremental deltas against the existing four-instruction MergeNZCV:
        // split PF/AF needs one additional AF insert to pack canonical State;
        // restoring resident PSTATE after a helper/clobber needs the listed
        // extra unpack operations. These are conservative B0 model costs, not
        // emitted instructions.
        if (cause == FlagsRegsAuditMergeCause::Helper ||
            cause == FlagsRegsAuditMergeCause::HostExit ||
            cause == FlagsRegsAuditMergeCause::PStateClobber) {
            context.RecordFlagsRegsAudit(cause,
                                         edge,
                                         FlagsRegsAuditCost::PackInstructions,
                                         1);
        }
        if (cause == FlagsRegsAuditMergeCause::Helper) {
            context.RecordFlagsRegsAudit(cause,
                                         edge,
                                         FlagsRegsAuditCost::UnpackInstructions,
                                         3);
        } else if (cause == FlagsRegsAuditMergeCause::PStateClobber) {
            context.RecordFlagsRegsAudit(cause,
                                         edge,
                                         FlagsRegsAuditCost::UnpackInstructions,
                                         2);
        }
    }
    if (!deferred_merge) {
        PublishFlagsToken();
    }
    return flags_bypass;
}

void JitTranslator::PublishFlagsToken() {
    if (!flag_state.flags_token_valid) {
        return;
    }
    __ Bfi(flags, FlagsTokenResult(), HostFlagsBit::ParityByte, 8);
    if (!flag_state.flags_token_keep) {
        InvalidateFlagsToken();
    }
}

void JitTranslator::LoadNZCVFromFlags() {
    __ Msr(NZCV, flags);
}

ir::Flags JitTranslator::FlagsForCondition(ir::Cond cond) {
    switch (cond) {
        case ir::Cond::EQ:
        case ir::Cond::NE:
            return ir::Flags::Zero;
        case ir::Cond::CS:
        case ir::Cond::CC:
            return ir::Flags::Carry;
        case ir::Cond::MI:
        case ir::Cond::PL:
            return ir::Flags::Negate;
        case ir::Cond::VS:
        case ir::Cond::VC:
            return ir::Flags::Overflow;
        case ir::Cond::HI:
        case ir::Cond::LS:
            return ir::Flags::Carry | ir::Flags::Zero;
        case ir::Cond::GE:
        case ir::Cond::LT:
            return ir::Flags::Negate | ir::Flags::Overflow;
        case ir::Cond::GT:
        case ir::Cond::LE:
            return ir::Flags::Negate | ir::Flags::Zero | ir::Flags::Overflow;
        case ir::Cond::AL:
        case ir::Cond::NV:
        default:
            return ir::Flags::None;
    }
}

bool JitTranslator::PendingNZCVCovers(ir::Flags guest_flags) const {
    if (!flag_state.save_in_nzcv || !flag_state.nzcv_dirty) {
        return false;
    }
    const auto requested = GuestNZCVToHost(guest_flags & ir::Flags::NZCV);
    return static_cast<u64>(requested) != 0 &&
           (static_cast<u64>(flag_state.nzcv_requested) &
            static_cast<u64>(requested)) == static_cast<u64>(requested);
}

bool JitTranslator::TryEmitCondSetFromFlags(ir::Inst* inst, ir::Cond cond) {
    u32 bit;
    switch (cond) {
        case ir::Cond::EQ:
        case ir::Cond::NE:
            bit = HostFlagsBit::Z;
            break;
        case ir::Cond::CS:
        case ir::Cond::CC:
            bit = HostFlagsBit::C;
            break;
        case ir::Cond::MI:
        case ir::Cond::PL:
            bit = HostFlagsBit::N;
            break;
        case ir::Cond::VS:
        case ir::Cond::VC:
            bit = HostFlagsBit::V;
            break;
        default:
            return false;
    }

    const auto result = context.X(ir::Value{inst});
    __ Ubfx(result, flags, bit, 1);
    if ((static_cast<u8>(cond) & 1) != 0) {
        __ Eor(result, result, 1);
    }
    return true;
}

void JitTranslator::MergeLogicalFlagsNZ(ir::Flags requested) {
    // Logical flag producers may have only an N or only a Z SaveFlags pseudo
    // (SAHF deliberately writes them independently).  Commit exactly that
    // requested subset: merging both bits lets a later SF-only zero result
    // resurrect an earlier ZF that SAHF already cleared.
    const u64 requested_nz =
            static_cast<u64>(GuestNZCVToHost(requested & ir::Flags::NZ));
    if (!requested_nz) {
        return;
    }
    EmitNZCVMerge(requested_nz, context.GetSharedTmpX());
    flag_state.nzcv_dirty = false;
    flag_state.nzcv_requested = {};
}

void JitTranslator::SaveLogicalResultFlags(Register& result,
                                           ir::ValueType type,
                                           const PseudoFlags& pseudo) {
    const bool needs_nz = True(pseudo.set & ir::Flags::NZ);
    const bool needs_parity_value =
            !FlagsRegsEnabled() && True(pseudo.set & ir::Flags::Parity);
    if (needs_nz && (pseudo.branch_only || !needs_parity_value)) {
        EmitLogicalNZFlags(result, type);
    } else if (needs_nz) {
        const auto scratch = context.GetSharedTmpX();
        switch (type) {
            case ir::ValueType::S8:
            case ir::ValueType::U8:
                __ Sxtb(scratch, result.W());
                break;
            case ir::ValueType::S16:
            case ir::ValueType::U16:
                __ Sxth(scratch, result.W());
                break;
            case ir::ValueType::S32:
            case ir::ValueType::U32:
                __ Sxtw(scratch, result.W());
                break;
            case ir::ValueType::S64:
            case ir::ValueType::U64:
                __ Mov(scratch, result);
                break;
            default:
                PANIC();
        }
        __ Tst(scratch, scratch);
    }
    RecordLogicalResultFlags(result, pseudo);
}

void JitTranslator::RecordLogicalResultFlags(Register& result,
                                             const PseudoFlags& pseudo) {
    if (pseudo.branch_only) {
        return;
    }
    if (FlagsRegsEnabled()) {
        const auto requested = GuestNZCVToHost(pseudo.set & ir::Flags::NZ);
        if (True(requested)) {
            flag_state.nzcv_requested |= requested;
            flag_state.nzcv_dirty = true;
        }
    } else {
        MergeLogicalFlagsNZ(pseudo.set);
    }
    if (True(pseudo.set & ir::Flags::Parity)) {
        SaveParity(result);
    }
}

void JitTranslator::EmitLogicalNZFlags(const Register& value,
                                       ir::ValueType type) {
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            // Zero plus the shifted value sets narrow N/Z while leaving C/V clear.
            __ Adds(wzr, wzr, Operand{value.W(), LSL, 24});
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Adds(wzr, wzr, Operand{value.W(), LSL, 16});
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Tst(value.W(), value.W());
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Tst(value.X(), value.X());
            break;
        default:
            PANIC();
    }
}

void JitTranslator::SaveHostFlags(HostFlags host, ir::Flags guest) {
    // To arm64 host
    HostFlags host_need_saved{};
    if (True(guest & ir::Flags::Negate)) {
        host_need_saved |= HostFlags::N;
    }
    if (True(guest & ir::Flags::Zero)) {
        host_need_saved |= HostFlags::Z;
    }
    if (True(guest & ir::Flags::Carry)) {
        host_need_saved |= HostFlags::C;
    }
    if (True(guest & ir::Flags::Overflow)) {
        host_need_saved |= HostFlags::V;
    }
    if (flag_state.save_in_nzcv) {
        // Accumulate which NZCV bits were actually requested by guest
        // SaveFlags. MergeNZCV will only merge these bits, preserving
        // any ClearFlags(CF/OF) that happened between flag-setting
        // instructions.
        flag_state.nzcv_requested |= host_need_saved;
        flag_state.nzcv_dirty = true;
    } else {
        const auto scratch = context.GetSharedTmpX();
        __ Mrs(scratch, NZCV);
        if (host_need_saved != host) {
            __ And(scratch, scratch, static_cast<u32>(host_need_saved));
        }
        __ Orr(flags, flags, scratch);
    }
}

void JitTranslator::ClearFlags(ir::Flags guest) {
    const auto cv_af = ir::Flags::CV | ir::Flags::AuxiliaryCarry;
    const bool clear_cv_af = (guest & cv_af) == cv_af;
    const bool compound_logical = flag_state.compound_logical_clear_pending &&
            guest == cv_af && FlagsRegsEnabled() && flag_state.nzcv_dirty &&
            flag_state.nzcv_requested == HostFlags::NZ;
    const bool compound_logical_zero =
            compound_logical && flag_state.compound_logical_zero_pending;
    flag_state.compound_logical_clear_pending = false;
    flag_state.compound_logical_zero_pending = false;
    if (compound_logical) {
        if (compound_logical_zero) {
            __ Mov(flags, u64{1} << HostFlagsBit::Z);
            if (flag_state.flags_token_valid && !flag_state.flags_token_keep) {
                InvalidateFlagsToken();
            }
        } else if (flag_state.flags_token_valid) {
            __ Mrs(flags, NZCV);
            __ Bfi(flags, FlagsTokenResult(), HostFlagsBit::ParityByte, 8);
            if (!flag_state.flags_token_keep) {
                InvalidateFlagsToken();
            }
        } else {
            constexpr u32 width =
                    HostFlagsBit::N - HostFlagsBit::AuxiliaryCarry + 1;
            const auto scratch = context.GetSharedTmpX();
            __ Mrs(scratch, NZCV);
            __ Ubfx(scratch, scratch, HostFlagsBit::AuxiliaryCarry, width);
            __ Bfi(flags, scratch, HostFlagsBit::AuxiliaryCarry, width);
        }
        flag_state.nzcv_dirty = false;
        flag_state.nzcv_requested = {};
        return;
    }
    if (guest == cv_af && FlagsRegsEnabled() && flag_state.nzcv_dirty &&
        flag_state.nzcv_requested == HostFlags::NZ && flag_state.flags_token_valid &&
        !flag_state.flags_token_keep) {
        const u32 begin = context.CurrentBufferSize();
        __ Uxtb(flags.W(), FlagsTokenResult().W());
        RecordPFAFDensity(PFAFDensityKind::AFWrite, begin);
        InvalidateFlagsToken();
        return;
    }
    if (True(guest & ir::Flags::NZCV)) {
        // ClearFlags is an independent IR write, not merely an annotation on
        // the preceding flag producer. Flag elimination can delete a dead
        // SaveFlags and DCE can then delete that producer, while a sibling
        // ClearFlags(C/V) remains live (x86 logical ops followed by ADC/SBB).
        //
        // Eager path: commit a pending lazy producer before clearing the
        // stored bits so a later MergeNZCV cannot resurrect them.
        // FLAGS_REGS must not publish here — FlushFlags runs from AdvancePC,
        // and packing the token onto that IR is the relocating-pack fail.
        // Clear only the committed x26 bits; live PSTATE (Ands already left
        // C=V=0) stays in NZCV until a real observe point.
        if (!FlagsRegsEnabled()) {
            MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
                      flags_audit_block_edge);
        }
        u64 mask{UINT64_MAX};
        if (True(guest & ir::Flags::Negate)) {
            mask &= ~(u64(1) << HostFlagsBit::N);
        }
        if (True(guest & ir::Flags::Zero)) {
            mask &= ~(u64(1) << HostFlagsBit::Z);
        }
        // When CV and AF are cleared together, the AF BFC below clears the
        // contiguous AF..V field in one instruction.  Keep C/V out of this
        // mask so the compact form does not redundantly emit an AND.
        if (!clear_cv_af && True(guest & ir::Flags::Carry)) {
            mask &= ~(u64(1) << HostFlagsBit::C);
        }
        if (!clear_cv_af && True(guest & ir::Flags::Overflow)) {
            mask &= ~(u64(1) << HostFlagsBit::V);
        }
        if (mask != UINT64_MAX) {
            __ And(flags, flags, ForceCast<s64>(mask));
        }
        if (clear_cv_af) {
            // A logical clear invalidates any older lazy arithmetic C/V
            // producer.  Leaving those request bits live lets a later merge
            // repack stale host NZCV over the canonical zeroes.
            flag_state.nzcv_requested &= HostFlags::NZ;
        }
    }
    if (True(guest & ir::Flags::Parity)) {
        const u32 begin = context.CurrentBufferSize();
        // Clear Parity: an odd-parity byte makes TestParityFlag read PF = 0.
        if (FlagsRegsEnabled() && flag_state.flags_token_valid) {
            MaterializeFlagsTokenResult();
        }
        const auto scratch = context.GetSharedTmpX();
        __ Mov(scratch, 1);
        if (FlagsRegsEnabled() && flag_state.flags_token_valid) {
            __ Bfi(atomic_scratch, scratch, HostFlagsBit::ParityByte, 8);
        }
        __ Bfi(flags, scratch, HostFlagsBit::ParityByte, 8);
        RecordPFAFDensity(PFAFDensityKind::PFWrite, begin);
    }
    if (True(guest & ir::Flags::AuxiliaryCarry)) {
        const u32 begin = context.CurrentBufferSize();
        // AF is a single bit (carry into bit 4).
        __ Bfc(flags,
               HostFlagsBit::AuxiliaryCarry,
               clear_cv_af
                       ? HostFlagsBit::C - HostFlagsBit::AuxiliaryCarry + 1
                       : 1);
        RecordPFAFDensity(PFAFDensityKind::AFWrite, begin);
    }
}

void JitTranslator::SaveParity(Register& value) {
    if (FlagsRegsEnabled()) {
        return;
    }
    const u32 begin = context.CurrentBufferSize();
    __ Bfi(flags, value, HostFlagsBit::ParityByte, 8);
    RecordPFAFDensity(PFAFDensityKind::PFWrite, begin);
}

void JitTranslator::SaveNZ(Register& value, ir::ValueType type) {
    const auto scratch = context.GetSharedTmpX();
    switch (type) {
        case ir::ValueType::U8:
            __ Sxtb(scratch, value);
            break;
        case ir::ValueType::U16:
            __ Sxth(scratch, value);
            break;
        case ir::ValueType::U32:
            __ Sxtw(scratch, value);
            break;
        case ir::ValueType::U64:
            __ Mov(scratch, value);
            break;
        default:
            PANIC();
    }
    // Same reasoning as SaveLogicalResultFlags: Tst avoids the vixl x16
    // scratch that `Bics(ip, ip, 0)` would take.
    __ Tst(scratch, scratch);
    if (flag_state.save_in_nzcv) {
        flag_state.nzcv_dirty = true;
    } else {
        __ Mrs(scratch, NZCV);
        __ Orr(flags, flags, scratch);
    }
}

// SaveCV / SaveOF set C+V (resp. V) when `value` does not fit in `type` --
// the x86 mul/imul CF/OF shape, computed from the upper half of a widened
// product.
//
// Both used to have a `flag_state.save_in_nzcv` path that wrote the bits into the HOST
// NZCV register with Msr and then set flag_state.nzcv_dirty = false.  Every one of those
// three spellings was wrong, and they compounded:
//
//   * flag_state.nzcv_dirty = false makes MergeNZCV() a no-op, so bits placed in host
//     NZCV were never copied into `flags` -- CF/OF were silently dropped.
//   * Setting flag_state.nzcv_dirty = true instead would not have helped: the Msr sits
//     inside the Cbz skip, so on the no-overflow path host NZCV still holds
//     the PREVIOUS producer's result and merging it would invent flags.
//   * flag_state.nzcv_requested was never widened to C|V, so MergeNZCV's mask would have
//     filtered the bits out even if the other two had been right.
//
// The lazy-NZCV representation cannot express "conditionally set two bits", so
// do not try: commit any pending producer up front (MergeNZCV is a no-op when
// nothing is pending -- both call sites already invoke it) and then OR the bits
// straight into the `flags` register, which is what the non-lazy branch and
// EmitMul's inline signed-overflow path have always done.  Nothing here touches
// host NZCV afterwards; Lsr, Cbz and a non-flag-setting Orr all leave it alone.
//
// Reachability, as of this commit: the x86 frontend cannot get here.  Its
// mul/imul lowering (MulWithFlags in frontend/x86/decoder_alu.cc) deliberately
// materialises CF/OF through a separate `t = bad << 63; SaveFlags(t + t, C|V)`
// producer rather than hanging SaveFlags(CV) on the Mul itself, and its only
// ir::Div is the RCL/RCR modulus, which carries no flags.  SaveOF has no caller
// at all.  So this is a latent-bug fix, exercised by the codegen-shape test
// "SaveCV commits x86 CF/OF into the flags register" in tests/main_case.cpp
// (which builds the Mul + SaveFlags(C|V) IR the frontend does not currently
// emit), not by any guest program.
void JitTranslator::SaveCV(Register& value, ir::ValueType type) {
    if (type == ir::ValueType::U64) {
        return;
    }
    MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
              flags_audit_block_edge);
    const auto scratch = context.GetSharedTmpX();
    Label pass;
    __ Lsr(scratch, value, ir::GetValueSizeByte(type) * 8);
    __ Cbz(scratch, &pass);
    __ Orr(flags, flags, 3u << HostFlagsBit::V);
    __ Bind(&pass);
}

void JitTranslator::SaveOF(Register& value, ir::ValueType type) {
    if (type == ir::ValueType::U64) {
        return;
    }
    MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
              flags_audit_block_edge);
    const auto scratch = context.GetSharedTmpX();
    Label pass;
    __ Lsr(scratch, value, ir::GetValueSizeByte(type) * 8);
    __ Cbz(scratch, &pass);
    __ Orr(flags, flags, 1u << HostFlagsBit::V);
    __ Bind(&pass);
}

void JitTranslator::SaveAuxiliaryCarry(Register &left, const Operand &right, Register &result) {
    const u32 begin = context.CurrentBufferSize();
    // AF = carry into bit 4 = bit4(left) ^ bit4(right) ^ bit4(result). This holds
    // for add/adc/sub/sbb alike (result already reflects any carry-in). Only the
    // three bit-4s matter, so fold the whole values together and extract bit 4.
    // AF is emitted after the arithmetic result has been written. Some narrow
    // operands are dead according to ordinary SSA liveness, but remain live
    // here as implicit flag inputs. Reserve the right operand before leasing
    // the fold temporary so GetTmpX cannot alias it.
    const auto reserve_gpr = [&](const Register& reg) {
        if (reg.GetCode() < 31) {
            context.ReserveTmpX(XRegister(reg.GetCode()));
        }
    };
    if (!right.IsImmediate()) {
        reserve_gpr(right.GetRegister());
    }
    auto tmp = context.GetTmpX();
    __ Eor(tmp, left.X(), Operand{result.X()});
    if (right.IsImmediate()) {
        if ((right.GetImmediate() >> 4) & 1) {
            __ Eor(tmp, tmp, 1u << 4);
        }
    } else {
        // Basic or shifted register: materialize its effective bits (.X view keeps
        // bit 4 correct for the W-width shift forms too); only bit 4 survives.
        auto reg = right.GetRegister().X();
        auto shift = right.IsShiftedRegister() ? right.GetShift() : LSL;
        auto amount = right.IsShiftedRegister() ? right.GetShiftAmount() : 0;
        __ Eor(tmp, tmp, Operand{reg, shift, amount});
    }
    __ Ubfx(tmp, tmp, 4, 1);
    __ Bfi(flags, tmp, HostFlagsBit::AuxiliaryCarry, 1);
    RecordPFAFDensity(PFAFDensityKind::AFWrite, begin);
}

void JitTranslator::GetParityFlag(const Register& result) {
    if (FlagsRegsEnabled() && flag_state.flags_token_valid) {
        __ Ubfx(result.W(), FlagsTokenResult(), HostFlagsBit::ParityByte, 8);
    } else {
        __ Ubfx(result.W(), flags, HostFlagsBit::ParityByte, 8);
    }
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 4});
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 2});
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 1});
}

void JitTranslator::TestParityFlag(const Register& result) {
    const u32 begin = context.CurrentBufferSize();
    GetParityFlag(result);
    __ And(result.W(), result.W(), 1);
    // x86 PF is set on even parity
    __ Eor(result.W(), result.W(), 1);
    RecordPFAFDensity(PFAFDensityKind::PFRead, begin);
}

void JitTranslator::TestAuxiliaryCarry(const Register& result) {
    const u32 begin = context.CurrentBufferSize();
    // AF is stored as a single bit (the carry into bit 4) at AuxiliaryCarry.
    __ Ubfx(result, flags, HostFlagsBit::AuxiliaryCarry, 1);
    RecordPFAFDensity(PFAFDensityKind::AFRead, begin);
}


bool JitTranslator::RegionBranchPFAFActive(ir::Inst* producer) const {
    return backedge_flags_recipe && backedge_flags_recipe->plan.pfaf &&
           backedge_flags_recipe->plan.pfaf->producer == producer;
}

JitTranslator::PseudoFlags JitTranslator::GetPseudoFlags(ir::Inst* inst) {
    if (IsDeadEdgeIntegerBranchProducer(inst)) {
        return {DeadEdgePlan()->required, ir::Flags::None, true};
    }
    auto demand = gpr_plan->pseudo_flags.For(inst);
    if (!demand.branch_only && RegionBranchPFAFActive(inst)) demand.branch_only = true;
    return demand;
}

void JitTranslator::EmitSaveFlags(ir::Inst* inst) {
    // Multiple SaveFlags may appear in one flush window (e.g. the x86 frontend
    // emits separate PF/AF and NZCV saves for narrow ALU ops); merge them.
    const auto written = inst->GetArg<ir::Flags>(1);
    flag_state.flags_set |= written;
    // A later flag definition supersedes an earlier queued clear. The eager
    // path has already packed the new value at its producer; applying that
    // stale clear at AdvancePC would erase it (ClearFlags(All), then POPCNT ZF).
    flag_state.flags_clear &= ~written;
}

void JitTranslator::EmitBranchOnlyFlags(ir::Inst* inst) {
    // Its producer has already left the exact requested NZCV live.  Recording
    // flag_state.flags_set/flag_state.nzcv_dirty here would make AdvancePC or the terminal flush
    // materialize the very architectural state this marker exists to avoid.
}

void JitTranslator::EmitClearFlags(ir::Inst* inst) {
    // See EmitSaveFlags: merge instead of asserting on a pending window.
    auto* producer = flag_state.flags_clear == ir::Flags::None ? PlannedCompoundProducer(inst) : nullptr;
    flag_state.compound_logical_clear_pending = producer != nullptr;
    flag_state.compound_logical_zero_pending = flag_state.compound_logical_clear_pending &&
            ScalarPreparation().copies.IsSelfXor(producer);
    flag_state.flags_clear |= inst->GetArg<ir::Flags>(0);
}

void JitTranslator::EmitSetCarry(ir::Inst* inst) {
    // Set guest CF directly in the flags register from a computed 0/1 value.
    // Merge pending NZCV first so no later merge clobbers the bit we write
    // (MergeNZCV leaves flag_state.nzcv_dirty=false; Bfi does not set it).
    MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
              flags_audit_block_edge);
    // A preceding ClearFlags may still be queued in the lazy flag window.
    // Apply it before inserting CF, otherwise the next flush clears the bit
    // just written (RDRAND/RDSEED are the minimal reproducer).
    FlushFlags();
    auto bit = context.R(inst->GetArg<ir::Value>(0));
    __ Bfi(flags, bit.X(), HostFlagsBit::C, 1);
}

void JitTranslator::EmitSetOverflow(ir::Inst* inst) {
    MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
              flags_audit_block_edge);
    FlushFlags();
    auto bit = context.R(inst->GetArg<ir::Value>(0));
    __ Bfi(flags, bit.X(), HostFlagsBit::V, 1);
}

void JitTranslator::EmitInvertCarry(ir::Inst* inst) {
    // CFINV operates on host NZCV.  A known in-unit producer leaves its C
    // there; otherwise restore the committed ABI word first.  Mark only C as
    // pending so the eventual merge cannot overwrite unrelated guest bits.
    if (FlagsRegsEnabled() && CanonicalCarryEnabled() &&
        ScalarPreparation().raw_carry.SuppressesInvert(inst)) {
        ASSERT(flag_state.save_in_nzcv && flag_state.nzcv_dirty);
        ASSERT(!flag_state.raw_carry_pending);
        flag_state.raw_carry_pending = inst;
        return;
    }
    if (!(flag_state.save_in_nzcv && flag_state.nzcv_dirty)) {
        LoadNZCVFromFlags();
    }
    {
        vixl::CPUFeaturesScope flagm(&masm, vixl::CPUFeatures::kFlagM);
        __ Cfinv();
    }
    flag_state.nzcv_requested |= HostFlags::C;
    flag_state.nzcv_dirty = true;
}

void JitTranslator::EmitPublishFCmpFlags(ir::Inst* inst) {
    auto packed = inst->GetArg<ir::Value>(0);
    ASSERT(packed.Def() && packed.Def()->GetOp() == ir::OpCode::VecFCmp &&
           (inst->GetArg<ir::Imm>(1).Get() != 0) == (packed.Def()->GetArg<ir::Imm>(3).Get() != 0));
    const bool compact = IsCompactFCmp(packed);

    if (compact) {
        // VecFCmp left ARM FP NZCV live and computed ordered (VC) in its
        // result or directly in the flags carrier. AXFLAG transforms it as follows:
        //   less/unordered -> C=0, equal/greater -> C=1  (inverted x86 CF)
        //   equal/unordered -> Z=1                       (x86 ZF)
        //   N=V=0                                           (x86 SF/OF)
        // Keep those four bits lazy in host NZCV. A consumer-proven VecFCmp
        // writes ordered directly to x26 and clears AF with its CSET; the
        // generic result path folds both updates into one bitfield insert.
        InvalidateFlagsToken();
        if (!CanUseCompactFCmpCarrier(packed.Def())) {
            auto ordered = context.R(packed);
            const u32 begin = context.CurrentBufferSize();
            constexpr u32 non_nzcv_width =
                    HostFlagsBit::AuxiliaryCarry - HostFlagsBit::ParityByte + 1;
            __ Bfi(flags, ordered, HostFlagsBit::ParityByte, non_nzcv_width);
            RecordPFAFDensity(PFAFDensityKind::SharedPack, begin);
        }
        {
            vixl::CPUFeaturesScope flagm2(&masm, vixl::CPUFeatures::kAXFlag);
            __ Axflag();
        }
        flag_state.nzcv_requested = HostFlags::NZCV;
        flag_state.nzcv_dirty = true;
        return;
    }

    MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
              flags_audit_block_edge);
    FlushFlags();
    auto packed_reg = context.R(packed);
    // Clear the overwritten fields directly. A spilled relation and the flags
    // merge can already occupy the pool; synthesizing a mask needs another
    // VIXL scratch register before the explicit bit temporary is acquired.
    u32 begin = context.CurrentBufferSize();
    __ Bfc(flags, HostFlagsBit::V, 4);
    __ Bfc(flags, HostFlagsBit::AuxiliaryCarry, 1);
    // The later eight-bit BFI replaces the whole parity byte with 0 or 1.
    RecordPFAFDensity(PFAFDensityKind::SharedPack, begin);

    auto bit = context.GetTmpX();

    // VecFCmp: bit0=CF, bit1=PF, bit2=ZF.  PF's ABI slot stores a raw
    // parity byte, so 0 spells PF=1 and 1 spells PF=0.
    __ Ubfx(bit, packed_reg, 0, 1);
    // The full relation fallback changes storage, not the IR's declared
    // carry polarity. Compact publication promises inverted x86 CF.
    if (inst->GetArg<ir::Imm>(1).Get() != 0) __ Eor(bit, bit, 1);
    __ Bfi(flags, bit, HostFlagsBit::C, 1);
    __ Ubfx(bit, packed_reg, 2, 1);
    __ Bfi(flags, bit, HostFlagsBit::Z, 1);
    begin = context.CurrentBufferSize();
    __ Ubfx(bit, packed_reg, 1, 1);
    __ Eor(bit, bit, 1);
    __ Bfi(flags, bit, HostFlagsBit::ParityByte, 8);
    RecordPFAFDensity(PFAFDensityKind::PFWrite, begin);
}

bool JitTranslator::CanUseCompactFCmpCarrier(ir::Inst* fcmp) const {
    if (!fcmp || !gpr_plan || !gpr_plan->consumers) return false;
    const auto found = gpr_plan->consumers->comparisons.find(fcmp);
    return found != gpr_plan->consumers->comparisons.end() && found->second.compact_carrier;
}

ir::Inst* JitTranslator::RawFCmpCondition(ir::Inst* fcmp, ir::Inst* consumer) const {
    if (!fcmp || !gpr_plan || !gpr_plan->consumers) return nullptr;
    const auto found = gpr_plan->consumers->comparisons.find(fcmp);
    if (found == gpr_plan->consumers->comparisons.end()) return nullptr;
    return consumer && found->second.raw_all_conditions ? consumer : found->second.raw_condition;
}

void JitTranslator::FlushFlags() {
    if (flag_state.flags_clear != ir::Flags::None) {
        ClearFlags(flag_state.flags_clear);
    }

    flag_state.flags_set = ir::Flags::None;
    flag_state.flags_clear = ir::Flags::None;
}

void JitTranslator::EmitTestBit(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto bit = inst->GetArg<ir::Imm>(1).Get();
    if (ir::GetValueSizeByte(value.Type()) == 8) {
        __ Ubfx(context.X(ir::Value{inst}), context.X(value), bit, 1);
    } else {
        __ Ubfx(context.W(ir::Value{inst}), context.W(value), bit, 1);
    }
}

void JitTranslator::EmitGetFlags(ir::Inst* inst) {
    MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
              flags_audit_block_edge);
    const u32 begin = context.CurrentBufferSize();
    __ Mov(context.R(ir::Value{inst}), flags);
    RecordPFAFDensity(PFAFDensityKind::WholeFlags, begin);
}


bool JitTranslator::FoldCcFromCarryTest(ir::Inst* test_flags) {
    if (!FlagsRegsEnabled() || !CanonicalCarryEnabled() || !cur_block || !test_flags) {
        return false;
    }
    if (test_flags->GetArg<ir::Flags>(0) != ir::Flags::Carry) {
        return false;
    }
    const auto* action = ScalarPreparation().raw_carry.FoldForTest(test_flags);
    if (!action) return false;
    auto* combine = action->combine;
    if (const auto raw_condition =
                ScalarPreparation().raw_carry.ConditionForTest(test_flags)) {
        ASSERT_MSG(flag_state.raw_carry_pending ==
                           ScalarPreparation().raw_carry.InvertForTest(test_flags),
                   "raw carry branch state diverged at IR {}", test_flags->Id());
        ASSERT_MSG(RecordLocalCondition(combine, *raw_condition),
                   "raw carry branch consumer diverged at IR {}", test_flags->Id());
        MergeNZCV();
        __ Eor(flags, flags, static_cast<u64>(HostFlags::C));
        flag_state.raw_carry_pending = nullptr;
        return true;
    }
    if (!RecordLocalCondition(combine, action->packed_condition)) return false;
    MergeNZCV();
    const auto mask = static_cast<u64>(GuestNZCVToHost(ir::Flags::Carry | ir::Flags::Zero));
    __ Tst(flags, mask);
    return true;
}

void JitTranslator::EmitTestFlags(ir::Inst* inst) {
    if (FoldCcFromCarryTest(inst)) {
        return;
    }
    auto test = inst->GetArg<ir::Flags>(0);
    if (FlagsRegsEnabled() && test == ir::Flags::Zero &&
        PstateAvailable(inst, ir::Flags::Zero) &&
        RecordLocalCondition(inst, ir::Cond::EQ)) {
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Negate &&
        PstateAvailable(inst, ir::Flags::Negate) &&
        RecordLocalCondition(inst, ir::Cond::MI)) {
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Overflow &&
        PstateAvailable(inst, ir::Flags::Overflow) &&
        RecordLocalCondition(inst, ir::Cond::VS)) {
        return;
    }
    // ARM C is the inverse of x86 CF for subtraction producers, and logical
    // clears are allowed to update only the canonical carrier.  Reading live
    // PSTATE here can therefore consume a stale carry after TEST/AND/OR/XOR.
    // Use the packed guest bit for FLAGS_REGS; MergeNZCV first when the latest
    // producer is still lazy.
    if (FlagsRegsEnabled() && test == ir::Flags::Carry) {
        auto result = context.W(ir::Value{inst});
        if (flag_state.save_in_nzcv && flag_state.nzcv_dirty) {
            MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
                      flags_audit_block_edge);
        }
        __ Ubfx(result, flags.W(), HostFlagsBit::C, 1);
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Carry &&
        PstateAvailable(inst, ir::Flags::Carry) &&
        RecordLocalCondition(inst, ir::Cond::CS)) {
        return;
    }
    auto result = context.W(ir::Value{inst});
    auto nzcv_mask = static_cast<u32>(GuestNZCVToHost(test));
    struct HostFlagTest {
        u32 bit;
        Condition condition;
    };
    std::optional<HostFlagTest> host_test;
    switch (test) {
        case ir::Flags::Carry:
            host_test = HostFlagTest{HostFlagsBit::C, cs};
            break;
        case ir::Flags::Overflow:
            host_test = HostFlagTest{HostFlagsBit::V, vs};
            break;
        case ir::Flags::Zero:
            host_test = HostFlagTest{HostFlagsBit::Z, eq};
            break;
        case ir::Flags::Negate:
            host_test = HostFlagTest{HostFlagsBit::N, mi};
            break;
        default:
            break;
    }
    if (host_test) {
        if (PendingNZCVCovers(test)) {
            __ Cset(result, host_test->condition);
        } else {
            if (flag_state.save_in_nzcv && flag_state.nzcv_dirty) {
                MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
                          flags_audit_block_edge);
            }
            __ Ubfx(result, flags.W(), host_test->bit, 1);
        }
        return;
    }
    bool first{true};
    const auto scratch = context.GetSharedTmpX();
    // JA/JBE are And(TestFlags(CF), CondSet(NE)). Tst clobbers host NZCV.
    // Restore PSTATE when it still holds the cmp so CondSet sees guest ZF.
    // Do not Merge here: that was moving a 4-insn pack onto every unfused JA.
    if (nzcv_mask) {
        if (PendingNZCVCovers(test)) {
            __ Mrs(scratch, NZCV);
            __ Tst(scratch, nzcv_mask);
            __ Cset(result, ne);
            if (LaterNeedsHostPstate(inst)) {
                __ Msr(NZCV, scratch);
            }
        } else {
            if (flag_state.save_in_nzcv && flag_state.nzcv_dirty) {
                MergeNZCV(FlagsRegsAuditMergeCause::ClearOrPartialWrite,
                          flags_audit_block_edge);
            }
            __ Tst(flags, nzcv_mask);
            __ Cset(result, ne);
        }
        first = false;
    }
    if (True(test & ir::Flags::Parity)) {
        TestParityFlag(scratch);
        if (first) {
            __ Mov(result, scratch.W());
        } else {
            __ And(result, result, scratch.W());
        }
        first = false;
    }
    if (True(test & ir::Flags::AuxiliaryCarry)) {
        TestAuxiliaryCarry(scratch);
        if (first) {
            __ Mov(result, scratch.W());
        } else {
            __ And(result, result, scratch.W());
        }
        first = false;
    }
    if (first) {
        __ Mov(result, 0);
    }
}

void JitTranslator::EmitTestNotFlags(ir::Inst* inst) {
    auto test = inst->GetArg<ir::Flags>(0);
    if (FlagsRegsEnabled() && test == ir::Flags::Zero &&
        PstateAvailable(inst, ir::Flags::Zero) &&
        RecordLocalCondition(inst, ir::Cond::NE)) {
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Negate &&
        PstateAvailable(inst, ir::Flags::Negate) &&
        RecordLocalCondition(inst, ir::Cond::PL)) {
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Overflow &&
        PstateAvailable(inst, ir::Flags::Overflow) &&
        RecordLocalCondition(inst, ir::Cond::VC)) {
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Carry &&
        PstateAvailable(inst, ir::Flags::Carry) &&
        RecordLocalCondition(inst, ir::Cond::CC)) {
        return;
    }
    auto nzcv_mask = static_cast<u32>(GuestNZCVToHost(test));
    if (nzcv_mask && !True(test & (ir::Flags::Parity | ir::Flags::AuxiliaryCarry))) {
        auto result = context.W(ir::Value{inst});
        if (flag_state.save_in_nzcv && flag_state.nzcv_dirty) {
            const auto scratch = context.GetSharedTmpX();
            __ Mrs(scratch, NZCV);
            __ Tst(scratch, nzcv_mask);
            __ Cset(result, eq);
            if (LaterNeedsHostPstate(inst)) {
                __ Msr(NZCV, scratch);
            }
        } else {
            __ Tst(flags, nzcv_mask);
            __ Cset(result, eq);
        }
    } else {
        EmitTestFlags(inst);
        auto result = context.W(ir::Value{inst});
        __ Eor(result, result, 1);
        __ And(result, result, 1);
    }
}

}  // namespace swift::runtime::backend::arm64
