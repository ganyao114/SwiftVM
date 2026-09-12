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
    if (const auto pinned = ResolvePinnedGPRValue(ir::Value{inst})) {
        return ir::GetValueSizeByte(inst->ReturnType()) > sizeof(u32)
                ? Register{*pinned}
                : Register{pinned->W()};
    }
    if (FlagsRegsEnabled() && !pseudo.Null() && !pseudo.branch_only &&
        inst->GetUses() == 0) {
        return ir::GetValueSizeByte(inst->ReturnType()) > sizeof(u32)
                ? atomic_scratch
                : atomic_scratch.W();
    }
    return context.R(ir::Value{inst});
}

bool JitTranslator::CanRetainFlagsTokenResult(
        ir::Inst* producer, const Register& result) {
    if (!producer) {
        return false;
    }
    const u32 width = ir::GetValueSizeByte(producer->ReturnType());
    if (width != sizeof(u32) && width != sizeof(u64)) {
        return false;
    }
    if (!backend::IsFixedGPRHome(result.GetCode())) {
        return false;
    }
    const ir::Value value{producer};
    auto is_publication_wrapper = [&](ir::Inst* wrapper) {
        if (width != sizeof(u32) || !wrapper ||
            wrapper->GetOp() != ir::OpCode::ZeroExtend32To64) {
            return false;
        }
        auto source = wrapper->GetArg<ir::Value>(0);
        while (source.Defined() && source.Def()->IsBitCastOperation()) {
            source = source.Def()->GetArg<ir::Value>(0);
        }
        return source.Def() == producer &&
               context.SharesGPR(source, ir::Value{wrapper});
    };

    bool after_producer = false;
    bool published = false;
    ir::Inst* publication_wrapper = nullptr;
    for (auto& inst : cur_block->GetInstList()) {
        if (&inst == producer) {
            after_producer = true;
            continue;
        }
        if (!after_producer) {
            continue;
        }
        if (inst.GetOp() == ir::OpCode::SetHostGPR &&
            inst.GetArg<ir::Imm>(1).Get() == result.GetCode()) {
            auto stored = inst.GetArg<ir::Value>(0);
            while (stored.Defined() && stored.Def()->IsBitCastOperation()) {
                stored = stored.Def()->GetArg<ir::Value>(0);
            }
            auto* wrapper = stored.Def();
            bool publishes_producer = wrapper == producer;
            if (!publishes_producer && is_publication_wrapper(wrapper)) {
                publishes_producer = true;
                publication_wrapper = wrapper;
            }
            if (!published && inst.GetArg<ir::Imm>(2).Get() == 0 &&
                context.IsHostWriteCoalesced(inst.Id()) &&
                publishes_producer) {
                published = true;
                continue;
            }
            return false;
        }
        if ((backend::FixedGPRClobbers(inst, context.GetFeatures()) &
             (1u << result.GetCode())) != 0) {
            return false;
        }
        if (inst.HasValue() && !inst.IsBitCastOperation()) {
            if (!publication_wrapper && is_publication_wrapper(&inst)) {
                publication_wrapper = &inst;
                continue;
            }
            if (&inst == publication_wrapper) {
                continue;
            }
            const ir::Value other{&inst};
            if (context.SharesGPR(value, other)) {
                return false;
            }
        }
    }
    return published;
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
    if (!FlagsRegsEnabled() || pseudo.branch_only || flag_state.flags_token_valid ||
        !True(pseudo.set & ir::Flags::Parity)) {
        return;
    }
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
              region_edges_active));
    if (!(flag_state.save_in_nzcv && flag_state.nzcv_dirty) && !force_ret_pstate) {
        return std::nullopt;
    }
    return force_ret_pstate ? static_cast<u64>(HostFlags::NZCV)
                            : static_cast<u64>(flag_state.nzcv_requested);
}

bool JitTranslator::CanDeferFullNZCVMerge(
        FlagsRegsAuditMergeCause cause) const {
    const auto requested = PendingNZCVMergeMask(cause);
    return FlagsRegsEnabled() && region_edges_active && requested &&
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
        if (!deferred_merge && FlagsRegsEnabled() && region_edges_active) {
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
        if (!clear_cv_af && True(guest & ir::Flags::Carry)) {
            mask &= ~(u64(1) << HostFlagsBit::C);
        }
        if (!clear_cv_af && True(guest & ir::Flags::Overflow)) {
            mask &= ~(u64(1) << HostFlagsBit::V);
        }
        if (mask != UINT64_MAX) {
            __ And(flags, flags, ForceCast<s64>(mask));
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

bool JitTranslator::RecipeRegionBranchPFAF(BackedgeFlagsRecipe& recipe,
                                        ir::Inst* producer) const {
    if (!recipe.dead_successor || !producer || !recipe.final_save ||
        recipe.final_save->GetArg<ir::Value>(0).Def() != producer ||
        producer->GetOp() != ir::OpCode::Sub ||
        (producer->ReturnType() != ir::ValueType::U8 &&
         producer->ReturnType() != ir::ValueType::U16)) {
        return false;
    }
    const auto requested = recipe.final_save->GetArg<ir::Flags>(1);
    if (!True(requested & ir::Flags::NZCV) ||
        !True(requested & ir::Flags::Parity) ||
        !True(requested & ir::Flags::AuxiliaryCarry)) {
        return false;
    }

    using Deferred = BackedgeFlagsRecipe::DeferredOperand;
    recipe.pfaf_width = static_cast<u8>(ir::GetValueSizeByte(producer->ReturnType()));
    auto describe = [&](ir::Value value) -> Deferred {
        auto* def = value.Def();
        if (!def || ir::GetValueSizeByte(value.Type()) < recipe.pfaf_width) {
            return {};
        }
        if (def->GetOp() == ir::OpCode::LoadImm) {
            return {Deferred::Kind::Imm, def->GetArg<ir::Imm>(0).Get(), 0};
        }
        if (def->GetOp() == ir::OpCode::GetHostGPR) {
            const u64 offset = def->GetArg<ir::Imm>(1).Get();
            if (offset + recipe.pfaf_width > sizeof(u64)) {
                return {};
            }
            return {Deferred::Kind::HostGPR,
                    def->GetArg<ir::Imm>(0).Get(), static_cast<u8>(offset)};
        }
        if (def->GetOp() == ir::OpCode::LoadUniform) {
            const auto uniform = def->GetArg<ir::Uniform>(0);
            if (ir::GetValueSizeByte(uniform.GetType()) < recipe.pfaf_width) {
                return {};
            }
            return {Deferred::Kind::Uniform, uniform.GetOffset(), 0};
        }
        return {};
    };

    recipe.pfaf_left = describe(producer->GetArg<ir::Value>(0));
    const auto right = producer->GetArg<ir::Operand>(1);
    if (!right.GetRight().Null()) {
        return false;
    }
    if (right.GetLeft().IsImm()) {
        recipe.pfaf_right = {Deferred::Kind::Imm, right.GetLeft().imm.Get(), 0};
    } else if (right.GetLeft().IsValue()) {
        recipe.pfaf_right = describe(right.GetLeft().value);
    }
    recipe.defer_pfaf = recipe.pfaf_left.kind != Deferred::Kind::None &&
                      recipe.pfaf_right.kind != Deferred::Kind::None;
    auto overlaps_uniform = [&](const Deferred& operand,
                                const ir::Uniform& uniform) {
        if (operand.kind != Deferred::Kind::Uniform) {
            return false;
        }
        const u64 left_begin = operand.value;
        const u64 left_end = left_begin + recipe.pfaf_width;
        const u64 right_begin = uniform.GetOffset();
        const u64 right_end = right_begin + ir::GetValueSizeByte(uniform.GetType());
        return left_begin < right_end && right_begin < left_end;
    };
    for (const auto& scan : cur_block->GetInstList()) {
        if (scan.Id() <= producer->Id()) {
            continue;
        }
        if (scan.GetOp() == ir::OpCode::StoreUniform) {
            const auto uniform = scan.GetArg<ir::Uniform>(0);
            if (overlaps_uniform(recipe.pfaf_left, uniform) ||
                overlaps_uniform(recipe.pfaf_right, uniform)) {
                recipe.defer_pfaf = false;
            }
        } else if (scan.GetOp() == ir::OpCode::SetHostGPR) {
            const u64 target = scan.GetArg<ir::Imm>(1).Get();
            if ((recipe.pfaf_left.kind == Deferred::Kind::HostGPR &&
                 recipe.pfaf_left.value == target) ||
                (recipe.pfaf_right.kind == Deferred::Kind::HostGPR &&
                 recipe.pfaf_right.value == target)) {
                recipe.defer_pfaf = false;
            }
        }
    }
    return recipe.defer_pfaf;
}

bool JitTranslator::ReproveRegionBranchPFAF() const {
    if (!backedge_flags_recipe || !backedge_flags_recipe->defer_pfaf ||
        !backedge_flags_recipe->final_save) {
        return false;
    }
    BackedgeFlagsRecipe reproved;
    reproved.dead_successor = true;
    reproved.final_save = backedge_flags_recipe->final_save;
    auto* producer = reproved.final_save->GetArg<ir::Value>(0).Def();
    if (!RecipeRegionBranchPFAF(reproved, producer)) {
        return false;
    }
    return reproved.pfaf_width == backedge_flags_recipe->pfaf_width &&
           reproved.pfaf_left == backedge_flags_recipe->pfaf_left &&
           reproved.pfaf_right == backedge_flags_recipe->pfaf_right;
}

bool JitTranslator::RegionBranchPFAFActive(ir::Inst* producer) const {
    return backedge_flags_recipe && backedge_flags_recipe->defer_pfaf &&
           backedge_flags_recipe->final_save &&
           backedge_flags_recipe->final_save->GetArg<ir::Value>(0).Def() == producer;
}

JitTranslator::PseudoFlags JitTranslator::GetPseudoFlags(ir::Inst* inst) {
    if (IsDeadEdgeIntegerBranchProducer(inst)) {
        return {flag_state.dead_edge_integer_branch->required, ir::Flags::None, true};
    }
    ir::Flags result_set{};
    ir::Flags result_clear{};
    bool branch_only = false;
    if (auto pseudos = inst->GetPseudoOperations(); !pseudos.empty()) {
        for (auto& pseudo : pseudos) {
            if (pseudo->GetOp() == ir::OpCode::SaveFlags) {
                auto guest_flags = pseudo->GetArg<ir::Flags>(1);
                result_set |= guest_flags;
            } else if (pseudo->GetOp() == ir::OpCode::BranchOnlyFlags) {
                auto guest_flags = pseudo->GetArg<ir::Flags>(1);
                result_set |= guest_flags;
                branch_only = true;
            } else if (pseudo->GetOp() == ir::OpCode::ClearFlags) {
                auto guest_flags = pseudo->GetArg<ir::Flags>(0);
                result_clear |= guest_flags;
            }
        }
    }
    if (!branch_only && RegionBranchPFAFActive(inst)) {
        branch_only = true;
    }
    return {result_set, result_clear, branch_only};
}

void JitTranslator::EmitSaveFlags(ir::Inst* inst) {
    // Multiple SaveFlags may appear in one flush window (e.g. the x86 frontend
    // emits separate PF/AF and NZCV saves for narrow ALU ops); merge them.
    flag_state.flags_set |= inst->GetArg<ir::Flags>(1);
}

void JitTranslator::EmitBranchOnlyFlags(ir::Inst* inst) {
    // Its producer has already left the exact requested NZCV live.  Recording
    // flag_state.flags_set/flag_state.nzcv_dirty here would make AdvancePC or the terminal flush
    // materialize the very architectural state this marker exists to avoid.
}

void JitTranslator::EmitClearFlags(ir::Inst* inst) {
    // See EmitSaveFlags: merge instead of asserting on a pending window.
    flag_state.compound_logical_clear_pending =
            flag_state.flags_clear == ir::Flags::None && MatchCompoundLogicalClear(inst);
    flag_state.compound_logical_zero_pending = flag_state.compound_logical_clear_pending &&
            MatchCompoundZeroLogicalClear(inst);
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
    if (CanonicalCarryEnabled() &&
        raw_carry_branch_analysis.SuppressesInvert(inst)) {
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
    const bool compact = inst->GetArg<ir::Imm>(1).Get() != 0;
    auto packed = inst->GetArg<ir::Value>(0);
    ASSERT(compact == IsCompactFCmp(packed));

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
    constexpr u64 replaced =
            (u64(1) << HostFlagsBit::N) |
            (u64(1) << HostFlagsBit::Z) |
            (u64(1) << HostFlagsBit::C) |
            (u64(1) << HostFlagsBit::V) |
            (u64(1) << HostFlagsBit::AuxiliaryCarry) |
            (u64(0xff) << HostFlagsBit::ParityByte);
    u64 keep = ~replaced;
    // keep 不是合法 logical immediate，VIXL 要临时占用一枚池寄存器合成掩码。
    // 必须在 GetTmpX() 之前执行：shared_tmp + packed 重载 + bit 已占满
    // reserve=3 的池时，再晚一步 VIXL 就无寄存器可借（L3 direct 实测炸点）。
    u32 begin = context.CurrentBufferSize();
    __ And(flags, flags, ForceCast<s64>(keep));
    RecordPFAFDensity(PFAFDensityKind::SharedPack, begin);

    auto bit = context.GetTmpX();

    // VecFCmp: bit0=CF, bit1=PF, bit2=ZF.  PF's ABI slot stores a raw
    // parity byte, so 0 spells PF=1 and 1 spells PF=0.
    __ Ubfx(bit, packed_reg, 0, 1);
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
    if (!fcmp || !IsCompactFCmp(ir::Value{fcmp}) || !cur_block) {
        return false;
    }

    auto& list = cur_block->GetInstList();
    auto publish = list.iterator_to(*fcmp);
    if (++publish == list.end() ||
        publish->GetOp() != ir::OpCode::PublishFCmpFlags ||
        publish->GetArg<ir::Value>(0).Def() != fcmp ||
        publish->GetArg<ir::Imm>(1).Get() == 0) {
        return false;
    }

    u32 consumers = 0;
    bool saw_condition = false;
    for (auto& user : list) {
        bool consumes = false;
        for (auto value : user.GetValues()) {
            consumes |= value.Def() == fcmp;
        }
        if (!consumes) {
            continue;
        }
        ++consumers;
        if (&user == &*publish) {
            continue;
        }
        if (saw_condition || user.GetOp() != ir::OpCode::FCmpCondSet) {
            return false;
        }
        saw_condition = true;
        auto scan = publish;
        for (++scan; scan != list.end() && &*scan != &user; ++scan) {
            if (scan->GetOp() != ir::OpCode::InvertCarry &&
                !RetainsPendingHostNZCV(*scan)) {
                return false;
            }
        }
        if (scan == list.end()) {
            return false;
        }
    }
    return consumers != 0 && consumers == fcmp->GetUses();
}

ir::Inst* JitTranslator::RawFCmpCondition(ir::Inst* fcmp) const {
    if (!fcmp || fcmp->GetOp() != ir::OpCode::VecFCmp || !cur_block) {
        return nullptr;
    }

    ir::Inst* condition = nullptr;
    for (auto& user : cur_block->GetInstList()) {
        bool consumes = false;
        for (auto value : user.GetValues()) {
            consumes |= value.Def() == fcmp;
        }
        if (!consumes) {
            continue;
        }
        if (condition || user.GetOp() != ir::OpCode::FCmpCondSet) {
            return nullptr;
        }
        condition = &user;
    }
    if (!condition || fcmp->GetUses() != 1) {
        return nullptr;
    }

    auto& list = cur_block->GetInstList();
    auto scan = std::next(list.iterator_to(*fcmp));
    for (; scan != list.end() && &*scan != condition; ++scan) {
        if (!RetainsPendingHostNZCV(*scan) ||
            MayFaultOrObserve(*scan)) {
            return nullptr;
        }
    }
    return scan != list.end() ? condition : nullptr;
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

namespace {
bool OpClobbersPstate(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::TestFlags:
        case ir::OpCode::TestNotFlags:
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::VecFCmp:
        case ir::OpCode::PublishFCmpFlags:
        case ir::OpCode::PublishSse42StrFlags:
        case ir::OpCode::X87Op:
            return true;
        default:
            return false;
    }
}

// Z/N/V have the same host/guest polarity. C does not (see CarryStillInPstate).
bool SimpleFlagStillInPstate(ir::Block* block, ir::Inst* test, ir::Flags flag) {
    bool live = false;
    for (auto& inst : block->GetInstList()) {
        if (&inst == test) {
            return live;
        }
        if (inst.GetOp() == ir::OpCode::SaveFlags ||
            inst.GetOp() == ir::OpCode::BranchOnlyFlags) {
            live = True(inst.GetArg<ir::Flags>(1) & flag);
            continue;
        }
        if (inst.GetOp() == ir::OpCode::ClearFlags &&
            True(inst.GetArg<ir::Flags>(0) & flag)) {
            live = false;
            continue;
        }
        if (flag == ir::Flags::Overflow &&
            inst.GetOp() == ir::OpCode::SetOverflow) {
            live = false;
            continue;
        }
        if (OpClobbersPstate(inst.GetOp())) {
            live = false;
        }
    }
    return false;
}

bool IsAddFamilyCarryProducer(ir::Inst* producer) {
    if (!producer) {
        return false;
    }
    const auto op = producer->GetOp();
    return op == ir::OpCode::Add || op == ir::OpCode::Adc;
}

// Host C matches the bit TestFlags(Carry) reads: either the last Carry
// SaveFlags came from Add/Adc (no later invert), or InvertCarry ran after
// that save and PSTATE was not clobbered. Sub/Sbb/Cmp without invert keep
// ARM not-borrow, which is the stored C bit only until CFINV.
bool CarryStillInPstate(ir::Block* block, ir::Inst* test) {
    bool saw_carry_save = false;
    bool clobbered = false;
    bool invert_live = false;
    bool add_family_live = false;
    for (auto& inst : block->GetInstList()) {
        if (&inst == test) {
            return invert_live || add_family_live;
        }
        if (inst.GetOp() == ir::OpCode::SaveFlags ||
            inst.GetOp() == ir::OpCode::BranchOnlyFlags) {
            if (True(inst.GetArg<ir::Flags>(1) & ir::Flags::Carry)) {
                saw_carry_save = true;
                clobbered = false;
                invert_live = false;
                add_family_live =
                        IsAddFamilyCarryProducer(inst.GetArg<ir::Value>(0).Def());
            }
            continue;
        }
        if (inst.GetOp() == ir::OpCode::InvertCarry) {
            invert_live = saw_carry_save && !clobbered;
            add_family_live = false;
            continue;
        }
        if (inst.GetOp() == ir::OpCode::SetCarry ||
            (inst.GetOp() == ir::OpCode::ClearFlags &&
             True(inst.GetArg<ir::Flags>(0) & ir::Flags::Carry))) {
            saw_carry_save = false;
            invert_live = false;
            add_family_live = false;
            clobbered = true;
            continue;
        }
        if (OpClobbersPstate(inst.GetOp())) {
            clobbered = true;
            invert_live = false;
            add_family_live = false;
        }
    }
    return false;
}
}  // namespace

bool JitTranslator::CarryCanStayInPstate(ir::Inst* test) const {
    return CarryStillInPstate(cur_block, test);
}

ir::Inst* SoleUserInBlock(ir::Block* block, ir::Inst* def) {
    if (!block || !def || def->GetUses() != 1) {
        return nullptr;
    }
    ir::Inst* found = nullptr;
    for (auto& inst : block->GetInstList()) {
        for (auto value : inst.GetValues()) {
            if (value.Def() == def) {
                if (found) {
                    return nullptr;
                }
                found = &inst;
            }
        }
    }
    return found;
}

ir::Inst* OtherAndOrArg(ir::Inst* combine, ir::Inst* known) {
    if (!combine || !known ||
        (combine->GetOp() != ir::OpCode::And && combine->GetOp() != ir::OpCode::Or)) {
        return nullptr;
    }
    if (combine->GetArg<ir::Value>(0).Def() == known) {
        auto rhs = combine->GetArg<ir::Operand>(1).GetLeft();
        return rhs.IsValue() ? rhs.value.Def() : nullptr;
    }
    return combine->GetArg<ir::Value>(0).Def();
}

bool JitTranslator::FoldCcFromCarryTest(ir::Inst* test_flags) {
    if (!FlagsRegsEnabled() || !cur_block || !test_flags) {
        return false;
    }
    if (test_flags->GetArg<ir::Flags>(0) != ir::Flags::Carry) {
        return false;
    }
    const bool carry_in_pstate = CarryStillInPstate(cur_block, test_flags);
    auto* pred = SoleUserInBlock(cur_block, test_flags);
    if (!pred) {
        return false;
    }
    auto* combine = SoleUserInBlock(cur_block, pred);
    if (!combine) {
        return false;
    }
    auto* other = OtherAndOrArg(combine, pred);
    if (!other || other->GetOp() != ir::OpCode::CondSet) {
        return false;
    }
    const auto zcond = other->GetArg<ir::Cond>(0);
    const bool above = pred->GetOp() == ir::OpCode::TestZero &&
                       combine->GetOp() == ir::OpCode::And &&
                       zcond == ir::Cond::NE;
    const bool below_equal = pred->GetOp() == ir::OpCode::TestNotZero &&
                             combine->GetOp() == ir::OpCode::Or &&
                             zcond == ir::Cond::EQ;
    if (!above && !below_equal) {
        return false;
    }
    if (const auto raw_condition =
                raw_carry_branch_analysis.ConditionForTest(test_flags)) {
        ASSERT_MSG(flag_state.raw_carry_pending ==
                           raw_carry_branch_analysis.InvertForTest(test_flags),
                   "raw carry branch state diverged at IR {}", test_flags->Id());
        ASSERT_MSG(RecordLocalCondition(combine, *raw_condition),
                   "raw carry branch consumer diverged at IR {}", test_flags->Id());
        MergeNZCV();
        __ Eor(flags, flags, static_cast<u64>(HostFlags::C));
        flag_state.raw_carry_pending = nullptr;
        return true;
    }
    if (CanonicalCarryEnabled()) {
        if (!RecordLocalCondition(combine,
                                  above ? ir::Cond::EQ : ir::Cond::NE)) {
            return false;
        }
        MergeNZCV();
        const auto mask = static_cast<u64>(GuestNZCVToHost(
                ir::Flags::Carry | ir::Flags::Zero));
        __ Tst(flags, mask);
        return true;
    }
    if (!carry_in_pstate) {
        return false;
    }
    if (above) {
        return RecordLocalCondition(combine, ir::Cond::HI);
    }
    return RecordLocalCondition(combine, ir::Cond::LS);
}

void JitTranslator::EmitTestFlags(ir::Inst* inst) {
    if (FoldCcFromCarryTest(inst)) {
        return;
    }
    auto test = inst->GetArg<ir::Flags>(0);
    if (FlagsRegsEnabled() && test == ir::Flags::Zero &&
        SimpleFlagStillInPstate(cur_block, inst, ir::Flags::Zero) &&
        RecordLocalCondition(inst, ir::Cond::EQ)) {
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Negate &&
        SimpleFlagStillInPstate(cur_block, inst, ir::Flags::Negate) &&
        RecordLocalCondition(inst, ir::Cond::MI)) {
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Overflow &&
        SimpleFlagStillInPstate(cur_block, inst, ir::Flags::Overflow) &&
        RecordLocalCondition(inst, ir::Cond::VS)) {
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Carry &&
        CarryStillInPstate(cur_block, inst) &&
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
        if (flag_state.save_in_nzcv && flag_state.nzcv_dirty) {
            __ Cset(result, host_test->condition);
        } else {
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
        if (flag_state.save_in_nzcv && flag_state.nzcv_dirty) {
            __ Mrs(scratch, NZCV);
            __ Tst(scratch, nzcv_mask);
            __ Cset(result, ne);
            if (LaterNeedsHostPstate(inst)) {
                __ Msr(NZCV, scratch);
            }
        } else {
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
        SimpleFlagStillInPstate(cur_block, inst, ir::Flags::Zero) &&
        RecordLocalCondition(inst, ir::Cond::NE)) {
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Negate &&
        SimpleFlagStillInPstate(cur_block, inst, ir::Flags::Negate) &&
        RecordLocalCondition(inst, ir::Cond::PL)) {
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Overflow &&
        SimpleFlagStillInPstate(cur_block, inst, ir::Flags::Overflow) &&
        RecordLocalCondition(inst, ir::Cond::VC)) {
        return;
    }
    if (FlagsRegsEnabled() && test == ir::Flags::Carry &&
        CarryStillInPstate(cur_block, inst) &&
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
