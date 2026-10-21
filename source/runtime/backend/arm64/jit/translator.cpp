#include "base/logging.h"
#include "placement_experiment.h"
#pragma once

#include "translator.h"
#include "runtime/backend/arm64/alloc/edge_flags_analysis.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <string_view>
#include <type_traits>
#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/function_entry_contract.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/svm_config.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {

namespace {

constexpr u32 kCycleTailShareMinStubs = 2;

ir::Value ResolveBitCastValue(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<ir::Value>(0);
    }
    return value;
}

bool IsScalarFPRBinary(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::VecFAddScalar32:
        case O::VecFSubScalar32:
        case O::VecFMulScalar32:
        case O::VecFDivScalar32:
        case O::VecFAddScalar64:
        case O::VecFSubScalar64:
        case O::VecFMulScalar64:
        case O::VecFDivScalar64:
            return true;
        default:
            return false;
    }
}

struct AluSplitAudit {
    const char* role = "none";
    u32 value_uses = 0;
    u32 flags = 0;
    u32 pack_b = 0;
    u32 alu_b = 0;
    u32 addr_b = 0;
};

AluSplitAudit ClassifyAluSplit(const AluSplitFacts* facts,
                               u32 emitted,
                               bool has_flags,
                               u32 flags_mask) {
    AluSplitAudit out;
    if (!facts || emitted == 0) return out;
    out.value_uses = facts->value_uses;
    out.flags = flags_mask;
    const bool saw_addr = facts->saw_address, saw_value = facts->saw_value;
    const u32 found = facts->external_users;
    if (FlagsRegsEnabled()) {
        // Token ABI: PF/AF/Merge no longer sit on this IR. Leftover bytes
        // are last_result capture, not x26 pack. Counting them as pack_b
        // would hide the gate and treat token transport as the old ABI tax.
        if (saw_addr && !saw_value && out.value_uses > 0 &&
            out.value_uses == found) {
            out.role = "addr";
            out.addr_b = emitted;
        } else {
            out.role = has_flags && out.value_uses > 0 ? "mixed" : "alu";
            out.alu_b = emitted;
        }
        return out;
    }
    if (has_flags && out.value_uses == 0) {
        out.role = "pack";
        out.pack_b = emitted;
    } else if (saw_addr && !saw_value && out.value_uses > 0 &&
               out.value_uses == found) {
        out.role = "addr";
        out.addr_b = emitted;
    } else if (has_flags && (saw_value || out.value_uses > 0)) {
        out.role = "mixed";
        const u32 alu = emitted >= 4 ? 4u : emitted;
        out.alu_b = alu;
        out.pack_b = emitted - alu;
    } else {
        out.role = "alu";
        out.alu_b = emitted;
    }
    return out;
}

// Audit-only, mutually exclusive IR taxonomy. SVM_DENSITY_PROF is default OFF;
// the emitter-window accounting below does not add instructions to guest code.
DensityCategory DensityClass(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::GetFlags:
        case O::SaveFlags:
        case O::BranchOnlyFlags:
        case O::TestFlags:
        case O::TestNotFlags:
        case O::ClearFlags:
        case O::SetCarry:
        case O::SetOverflow:
        case O::InvertCarry:
        case O::PublishFCmpFlags:
        case O::PublishSse42StrFlags:
        case O::LocalCondSet:
        case O::LocalParitySet:
        case O::FCmpCondSet:
            return DensityCategory::Flags;
        case O::LoadUniform:
        case O::StoreUniform:
        case O::GetUniformAddress:
        case O::UniformBarrier:
            return DensityCategory::Uniform;
        case O::DefineLocal:
        case O::LoadLocal:
        case O::StoreLocal:
        case O::GetHostGPR:
        case O::GetHostFPR:
        case O::SetHostGPR:
        case O::SetHostFPR:
        case O::BitCast:
        case O::GetOperand:
        case O::GetResult:
        case O::LoadImm:
        case O::Zero:
        case O::ZeroExtend32:
        case O::ZeroExtend32To64:
        case O::ZeroExtend64:
        case O::SignExtend:
        case O::VecLoadConst:
        case O::VecSharedZero:
        case O::VecShuffle32:
        case O::VecShuffle32TwoSrc:
        case O::VecShuffle32Indexed:
        case O::VecShuffle16:
        case O::VecZip:
        case O::VecUnzip:
        case O::VecDupPairs32:
        case O::VecDup64:
        case O::VecExtract64:
        case O::VecExtract16:
        case O::VecInsert16:
        case O::VecTableLookup8:
            return DensityCategory::MoveWidth;
        case O::VecFAddScalar32:
        case O::VecFSubScalar32:
        case O::VecFMulScalar32:
        case O::VecFDivScalar32:
        case O::VecFAddScalar64:
        case O::VecFSubScalar64:
        case O::VecFMulScalar64:
        case O::VecFDivScalar64:
        case O::VecFAdd:
        case O::VecFSub:
        case O::VecFMul:
        case O::VecFDiv:
        case O::VecFUnary:
            return DensityCategory::NaN;
        case O::Goto:
        case O::NotGoto:
        case O::BindLabel:
        case O::Nop:
        case O::AdvancePC:
        case O::SetLocation:
        case O::GetLocation:
        case O::PushRSB:
        case O::PopRSB:
        case O::AddPhi:
        case O::BranchOnlyEdges:
            return DensityCategory::Boundary;
        default:
            return DensityCategory::Work;
    }
}

bool DensityScalarFP(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::VecFAddScalar32:
        case O::VecFSubScalar32:
        case O::VecFMulScalar32:
        case O::VecFDivScalar32:
        case O::VecFAddScalar64:
        case O::VecFSubScalar64:
        case O::VecFMulScalar64:
        case O::VecFDivScalar64:
            return true;
        default:
            return false;
    }
}

}  // namespace

#define __ masm.

JitTranslator::JitTranslator(JitContext& ctx)
        : context(ctx), masm(ctx.GetMasm()) {
    auto& config = ctx.GetConfig();
    memory_state.use_memory_base = config.memory_base != nullptr || config.page_table != nullptr;
    memory_state.guest_addr_mask = config.guest_addr_mask;
    memory_state.window_uxtw = memory_state.guest_addr_mask == 0xFFFFFFFFull;
    memory_state.mem_hostbase_fold = config.mem_hostbase_fold;
    sse_scalar_insert = config.sse_scalar_insert;
    sse_afp_nan = config.sse_afp_nan;
    const auto& features = ctx.GetFeatures();
    sse_scalar_tie = features.sse_scalar_tie;
    sse_shufps_imm = features.sse_shufps_imm;
    sse_afp_minmax = features.sse_afp_minmax && sse_afp_nan;
    shift_imm_fast = features.shift_imm_fast;
    memory_state.mem_narrow_fuse = features.mem_narrow_fuse;
    memory_state.addr_ea_tie = features.addr_ea_tie;
    memory_state.abs_const_mat = features.abs_const_mat;
    sse_nan_coldpath = features.sse_nan_coldpath;
    // Retirement must cut static cycles independently of optional SMC polls.
    direct_cycle_latch = true;
    backedge_latch = true;
    execution_trace_enabled = context.ExecutionTraceEnabled();
    if (execution_trace_enabled) {
        for (const auto& desc : config.buffers_static_alloc) {
            if (!desc.is_float && desc.size == sizeof(u64) &&
                desc.offset == offsetof(swift::x86::ThreadContext64, rsp)) {
                execution_trace_rsp_reg = desc.reg;
                break;
            }
        }
        // 当前探针只定义了 x86/x86-64 的 RSP 语义；其他前端不发探针码。
        execution_trace_enabled = execution_trace_rsp_reg >= 0;
    }
}

void JitTranslator::EmitExecutionTrace(u64 guest_rip) {
    if (!execution_trace_enabled) return;
    context.RecordExecutionTrace(guest_rip,
                                 XRegister(execution_trace_rsp_reg));
}



FlagsRegsAuditEdgeKind JitTranslator::ClassifyFlagsAuditEdge(
        const ir::Terminal& terminal) const {
    auto rank = [](FlagsRegsAuditEdgeKind edge) {
        switch (edge) {
            case FlagsRegsAuditEdgeKind::Host: return 6;
            case FlagsRegsAuditEdgeKind::RSBMiss: return 5;
            case FlagsRegsAuditEdgeKind::Dispatcher: return 4;
            case FlagsRegsAuditEdgeKind::DirectSlow: return 3;
            case FlagsRegsAuditEdgeKind::PatchedDirect: return 2;
            case FlagsRegsAuditEdgeKind::RSBHit: return 1;
            case FlagsRegsAuditEdgeKind::RegionInternal: return 0;
            case FlagsRegsAuditEdgeKind::Count: return 7;
        }
        return 7;
    };
    std::function<FlagsRegsAuditEdgeKind(const ir::Terminal&)> classify;
    classify = [&](const ir::Terminal& item) {
        return VisitVariant<FlagsRegsAuditEdgeKind>(item, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost>) {
                return FlagsRegsAuditEdgeKind::Host;
            } else if constexpr (std::is_same_v<T, ir::terminal::PopRSBHint>) {
                // Miss is the conservative representative: it includes the
                // dispatcher continuation and therefore never understates a
                // future unpack/cache-base cost.
                return FlagsRegsAuditEdgeKind::RSBMiss;
            } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                                 std::is_same_v<T, ir::terminal::LinkBlockFast>) {
                if (IsRegionInternalEdge(term.next)) {
                    return FlagsRegsAuditEdgeKind::RegionInternal;
                }
                if (context.CanEmitDirectLink(term.next)) {
                    return FlagsRegsAuditEdgeKind::PatchedDirect;
                }
                return FlagsRegsAuditEdgeKind::Dispatcher;
            } else if constexpr (std::is_same_v<T, ir::terminal::If> ||
                                 std::is_same_v<T, ir::terminal::Condition>) {
                const auto then_edge = classify(term.then_);
                const auto else_edge = classify(term.else_);
                return rank(then_edge) >= rank(else_edge) ? then_edge : else_edge;
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                auto result = FlagsRegsAuditEdgeKind::Dispatcher;
                for (const auto& case_ : term.cases) {
                    const auto edge = classify(case_.then);
                    if (rank(edge) > rank(result)) result = edge;
                }
                return result;
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
                return FlagsRegsAuditEdgeKind::Host;
            } else {
                return FlagsRegsAuditEdgeKind::Dispatcher;
            }
        });
    };
    return classify(terminal);
}

bool JitTranslator::IsStrictInternalAdvancePC(ir::Block* block,
                                              ir::Inst* advance) const {
    if (!context.FlagsRegsAuditEnabled() || !RegionEdgesActive() || !block ||
        !advance) {
        return false;
    }
    bool after = false;
    for (auto& inst : block->GetInstList()) {
        if (&inst == advance) {
            after = true;
            continue;
        }
        if (after && MayFaultOrObserve(inst)) {
            return false;
        }
    }
    if (!after) return false;
    std::vector<u64> targets;
    CollectRegionTargets(block->GetTerminal(), targets);
    if (targets.size() != 2) return false;
    return std::all_of(targets.begin(), targets.end(), [&](u64 target) {
        return RegionPlan().ContainsInternalBlock(target);
    });
}

JitTranslator::BlockTranslateState
JitTranslator::PrepareBlockState(ir::Block* block) {
    const u32 preparation_begin = context.CurrentBufferSize();
    cur_block = block;
    const auto transparent = RegionPlan().BlockTransparency(block);
    ASSERT(transparent);
    current_flags_transparent = *transparent;
    memory_state.spilled_memory_operands.clear();
    memory_state.home_alignment_mod8.clear();
    const auto& loop_hoist = block->GetLoopHoistMetadata();
    loop_hoist_body_entry = loop_hoist.prefix_end
            ? std::make_unique<Label>()
            : nullptr;
    u32 loop_hoist_prefix_begin = 0;
    u32 loop_hoist_prefix_ops = 0;
    ASSERT_MSG(!loop_hoist_body_entry || HasSelfEdge(block->GetTerminal()),
               "loop-hoist prefix without a self edge at {:#x}",
               block->GetStartLocation().Value());
    ASSERT(direct_cycle_exits.empty());
    direct_cycle_cut_edges = 0;
    statistics.region_block_edges = 0;
    statistics.region_block_cycles = 0;
    statistics.region_block_fallthroughs = 0;
    statistics.region_block_local_branch_bytes = 0;
    cur_block_is_call = false;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() == ir::OpCode::PushRSB) {
            cur_block_is_call = true;
            break;
        }
    }
    static_next_loc.reset();
    dynamic_next_loc.reset();
    dynamic_location_miss = nullptr;
    call_return_value.reset();
    call_return_pc.reset();
    flag_state.compound_logical_clear_pending = false;
    flag_state.compound_logical_zero_pending = false;
    // Function mode keeps one function-sized suppression bitmap. Grow it
    // before the per-block backedge proof marks the two sunk IR instructions.
    disable_instructions.resize(
            std::max<size_t>(disable_instructions.size(), block->MaxInstrId()));
    ImportGPRBlock(block);
    if (gpr_plan->memory)
        for (auto* covered : gpr_plan->memory->covered) disable_instructions.set(covered->Id());
    // Both allocation plans already own these nodes. Make their coverage
    // visible to later integer/bit/funnel selection before those plans build.
    PrepareScalarFPRPublications(block);
    ImportIntegerBranchPlan();
    ImportIntegerFlagPreparation();
    if (gpr_plan->boolean_selects)
        for (auto* covered : gpr_plan->boolean_selects->covered) disable_instructions.set(covered->Id());
    for (auto* inst : ScalarPreparation().resident.Discarded())
        disable_instructions.set(inst->Id());
    flag_state.raw_carry_pending = nullptr;
    backedge_flags_recipe = ImportBackedgeFlagsRecipe();
    if (backedge_flags_recipe) {
        if (backedge_flags_recipe->plan.polarity_load) {
            disable_instructions.set(backedge_flags_recipe->plan.polarity_load->Id());
        }
        if (backedge_flags_recipe->plan.polarity_store) {
            disable_instructions.set(backedge_flags_recipe->plan.polarity_store->Id());
        }
    }
    backedge_exit_referenced = false;
    backedge_exit_label =
            backedge_latch &&
                            (HasSelfEdge(block->GetTerminal()) ||
                             HasRegionCycleEdgeFromCurrent() ||
                             (backedge_flags_recipe &&
                              backedge_flags_recipe->plan.dead_successor &&
                              (IsDirectCycleCutEdge(
                                       backedge_flags_recipe->plan.self_target) ||
                               IsDirectCycleCutEdge(
                                       backedge_flags_recipe->plan.cold_target))))
                    ? std::make_unique<Label>()
                    : nullptr;
    const bool split_flags_entry = backedge_flags_recipe &&
                                   !backedge_flags_recipe->plan.dead_successor;
    ASSERT_MSG(context.CurrentBufferSize() == preparation_begin,
               "block preparation materialized an operand before plan freeze");
    context.SetCurrent(block, split_flags_entry,
                       FlagsRegsEnabled() && RegionEdgesActive());
    if (FlagsRegsEnabled() && RegionEdgesActive() && !split_flags_entry &&
        !BlockIsFlagsTransparent(block)) {
        context.RecordDirectLinkEntry(block->GetStartLocation().Value());
        const auto target_contract = AnalyzeEdgeFlagsTarget(
                block->GetStartLocation());
        if (target_contract.CanPublishPendingEntry()) {
            context.RecordPendingFlagsEntry(block->GetStartLocation().Value(),
                                            target_contract);
        }
    }
    flags_audit_block_edge = ClassifyFlagsAuditEdge(block->GetTerminal());
    if (RegionEdgesActive()) {
        context.BindInternalEntry(block->GetStartLocation().Value());
    }
    return {&loop_hoist,
            loop_hoist_prefix_begin,
            loop_hoist_prefix_ops,
            split_flags_entry};
}

void JitTranslator::PlacementPoint(const char* kind, u64 guest_pc) {
    const auto ops = GetPlacementExperiment().Padding(
            kind, placement_unit_pc, guest_pc, context.CurrentBufferSize());
    for (u32 i = 0; i < ops; ++i) __ Nop();
}


void JitTranslator::TranslateBlockInstructions(
        ir::Block* block,
        const ir::LoopHoistMetadata& loop_hoist,
        bool density,
        bool gap_audit,
        std::span<u32> density_ops,
        std::span<u32> density_bytes,
        u32& density_scalar_fp_ops,
        u32& loop_hoist_prefix_begin,
        u32& loop_hoist_prefix_ops) {
    loop_hoist_prefix_begin = context.CurrentBufferSize();
    terminal_body_inst = nullptr;
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() >= disable_instructions.size() ||
            !disable_instructions.test(inst.Id())) {
            // The decoder places CallReturn after SetLocation. It is terminal
            // metadata and emits no guest work; keep that static location
            // deferred so the physical Call path can consume it.
            if (inst.GetOp() == ir::OpCode::CallReturn) continue;
            terminal_body_inst = &inst;
        }
    }
    VAddr audit_guest_pc = block->GetStartLocation().Value();
    if (gap_audit) {
        u32 guest_insts = 0;
        for (auto& inst : block->GetInstList()) {
            if (inst.GetOp() == ir::OpCode::AdvancePC) ++guest_insts;
        }
        SVM_DIAG_PRINT(Codegen,
                     "[svm-gap-block] unit=0x%llx block=0x%llx bytes=%u "
                     "insts=%u\n",
                     static_cast<unsigned long long>(placement_unit_pc),
                     static_cast<unsigned long long>(
                             block->GetStartLocation().Value()),
                     static_cast<unsigned>(
                             block->GetEndLocation().Value() -
                             block->GetStartLocation().Value()),
                     guest_insts);
    }
    for (auto& inst : block->GetInstList()) {
        auto category = DensityCategory::Work;
        if (density) {
            category = DensityClass(inst.GetOp());
            ++density_ops[static_cast<size_t>(category)];
            density_scalar_fp_ops += DensityScalarFP(inst.GetOp());
        }
        cur_instr = &inst;
        if (inst.Id() < disable_instructions.size() && disable_instructions.test(inst.Id())) {
            continue;
        }
        const u32 before = density ? context.CurrentBufferSize() : 0;
        const u32 nan_before = density ? context.DensityNaNBytes() : 0;
        const bool audit_advance = (gap_audit || context.FlagsRegsAuditEnabled()) &&
                inst.GetOp() == ir::OpCode::AdvancePC;
        const bool audit_nzcv_dirty = audit_advance && flag_state.save_in_nzcv && flag_state.nzcv_dirty;
        const u64 audit_nzcv_requested = audit_advance
                ? static_cast<u64>(flag_state.nzcv_requested)
                : 0;
        flags_audit_strict_advance = audit_nzcv_dirty &&
                IsStrictInternalAdvancePC(block, &inst);
        Translate(&inst);
        flags_audit_strict_advance = false;
        if (loop_hoist_body_entry && &inst == loop_hoist.prefix_end) {
            loop_hoist_prefix_ops =
                    (context.CurrentBufferSize() - loop_hoist_prefix_begin) /
                    sizeof(u32);
            PlacementPoint("body", block->GetStartLocation().Value());
            __ Bind(loop_hoist_body_entry.get());
        }
        if (density) {
            const u32 emitted = context.CurrentBufferSize() - before;
            const u32 nan_emitted = context.DensityNaNBytes() - nan_before;
            density_bytes[static_cast<size_t>(DensityCategory::NaN)] += nan_emitted;
            const auto emitted_category =
                    category == DensityCategory::NaN
                            ? DensityCategory::Work
                            : (inst.GetOp() == ir::OpCode::AdvancePC
                                       ? DensityCategory::Flags
                                       : category);
            density_bytes[static_cast<size_t>(emitted_category)] +=
                    emitted - nan_emitted;
            if (category == DensityCategory::Boundary &&
                inst.GetOp() != ir::OpCode::AdvancePC) {
                RecordBoundaryRange(inst.GetOp() == ir::OpCode::PushRSB
                                            ? BoundarySubsequence::LinkTail
                                            : BoundarySubsequence::TerminalMain,
                                    before, context.CurrentBufferSize());
            }
            if (gap_audit) {
                const u32 production_emitted =
                        emitted - context.HotCheckBytesInRange(before,
                                                               context.CurrentBufferSize());
                const bool scalar_binary = IsScalarFPRBinary(inst.GetOp());
                const bool scalar_tied = scalar_binary && context.SharesFPR(
                        ir::Value{&inst}, inst.GetArg<ir::Value>(0));
                const bool shufps = inst.GetOp() == ir::OpCode::VecShuffle32TwoSrc;
                const u32 shufps_imm = shufps ? inst.GetArg<ir::Imm>(2).Get() & 0xffu : 0;
                const bool shufps_alias = shufps &&
                        inst.GetArg<ir::Value>(0).Id() == inst.GetArg<ir::Value>(1).Id();
                const bool shufps_left_tied = shufps && context.SharesFPR(
                        ir::Value{&inst}, inst.GetArg<ir::Value>(0));
                const bool shufps_left_fixed = shufps && context.IsHostReadCoalesced(
                        ResolveBitCastValue(inst.GetArg<ir::Value>(0)).Id());
                const auto pf = GetPseudoFlags(&inst);
                const AluSplitFacts* alu_facts = nullptr;
                if (gpr_plan->consumers) {
                    const auto found = gpr_plan->consumers->alu_split.find(&inst);
                    if (found != gpr_plan->consumers->alu_split.end()) alu_facts = &found->second;
                }
                const auto alu_split = ClassifyAluSplit(
                        alu_facts,
                        production_emitted,
                        !pf.Null(),
                        static_cast<u32>(pf.set));
                SVM_DIAG_PRINT(Codegen,
                             "[svm-gap-op] unit=0x%llx block=0x%llx guest_pc=0x%llx id=%u "
                             "op=%s bytes=%u host_offset=%u scalar_binary=%u scalar_tied=%u "
                             "shufps=%u shufps_imm=%u shufps_alias=%u "
                             "shufps_left_tied=%u shufps_left_fixed=%u "
                             "advpc_nzcv_dirty=%u advpc_nzcv_requested=0x%llx "
                             "alu_role=%s value_uses=%u flags=%u pack_b=%u alu_b=%u addr_b=%u\n",
                             static_cast<unsigned long long>(placement_unit_pc),
                             static_cast<unsigned long long>(
                                     block->GetStartLocation().Value()),
                             static_cast<unsigned long long>(audit_guest_pc),
                             inst.Id(), ir::GetIRMetaInfo(inst.GetOp()).name,
                             production_emitted, before,
                             scalar_binary ? 1u : 0u, scalar_tied ? 1u : 0u,
                             shufps ? 1u : 0u, shufps_imm,
                             shufps_alias ? 1u : 0u, shufps_left_tied ? 1u : 0u,
                             shufps_left_fixed ? 1u : 0u,
                             audit_nzcv_dirty ? 1u : 0u,
                             static_cast<unsigned long long>(audit_nzcv_requested),
                             alu_split.role, alu_split.value_uses, alu_split.flags,
                             alu_split.pack_b, alu_split.alu_b, alu_split.addr_b);
            }
        }
        if (inst.GetOp() == ir::OpCode::AdvancePC) {
            audit_guest_pc += inst.GetArg<ir::Imm>(0).Get();
        }
    }
    ASSERT(!flag_state.raw_carry_pending);
    terminal_body_inst = nullptr;
}

void JitTranslator::EmitBlockTerminal(
        ir::Block* block,
        bool density,
        std::span<u32> density_bytes) {
    ASSERT_MSG(gpr_plan->pinned->MatchesFrozenIR(), "IR changed during GPR plan emission");
    // Publish the last instruction's deferred definitions before scratch or
    // any conditional terminal arm can reuse their registers.
    context.FlushSpillWrites();
    context.BeginTerminalScratch();
    if (context.ContinuationActive() && call_return_value &&
        context.IsGPRMappedTo(*call_return_value, 14)) {
        // The IR use ends before terminal flag merges; the call frame still
        // needs this return address on either transfer or cancellation.
        context.ReserveTmpX(context.X(*call_return_value));
    }
    if (dynamic_next_loc) {
        // SetLocation has consumed this SSA value, so linear scan may consider
        // its register dead at the terminal. FLAGS_REGS (and any other late
        // MergeNZCV) takes GetSharedTmpX here; keep the target live so the
        // scratch lease cannot alias it before the indirect branch.
        context.ReserveTmpX(context.X(*dynamic_next_loc));
    }
    const u32 flags_before = density ? context.CurrentBufferSize() : 0;
    FlushFlags();
    if (density) {
        density_bytes[static_cast<size_t>(DensityCategory::Flags)] +=
                context.CurrentBufferSize() - flags_before;
    }
    const u32 terminal_before = density ? context.CurrentBufferSize() : 0;
    statistics.boundary_terminal_open = density;
    flag_state.flags_token_keep = true;
    if (!EmitBackedgeFlagsTerminal(block->GetTerminal())) {
        EmitTerminal(block->GetTerminal());
    }
    statistics.boundary_terminal_open = false;
    context.EndTerminalScratch();
    if (density) {
        RecordBoundaryRange(BoundarySubsequence::TerminalMain, terminal_before,
                            context.CurrentBufferSize());
        density_bytes[static_cast<size_t>(DensityCategory::Boundary)] +=
                context.CurrentBufferSize() - terminal_before;
    }
    if (backedge_flags_recipe) {
        backedge_host_end = context.CurrentBufferSize();
    }
    context.FinishHotCoalesceBlock();
}


bool JitTranslator::Translate(ir::Block* block) {
    vixl::svm_vixl_prof::JitScope vixl_prof{context.GetFeatures().vixl_fast};
    ASSERT(vec_nan_cold_sites.empty());
    ASSERT(!True(terminal_bypass_nzcv));
    {
        PerfScope2 perf_plan{GetPerfStats2().codegen_plan};
        if (!context.RequireGPRPlan(block)) return false;
    }
    if (!translating_function && !ImportRegionPreparation(block, nullptr)) return false;
    if (!translating_function) {
        placement_unit_pc = block->GetStartLocation().Value();
        ASSERT(memory_state.unaligned_atomic_fallbacks.empty());
    }
    // Each block is a dual-entry direct. Do not inherit a compile-time
    // token/dirty from a sibling that is not a runtime predecessor.
    InvalidateFlagsToken();
    flag_state.flags_token_keep = false;
    flag_state.nzcv_dirty = false;
    flag_state.nzcv_requested = {};
    flag_state.edge_carry_source.Reset();
    // Keep entry padding outside the block density/hot accounting window.  It
    // is reached only on the first fallthrough; every self edge targets the
    // label bound after it.
    PlacementPoint("block", block->GetStartLocation().Value());
    const bool density = context.DensityProfileEnabled();
    const bool gap_audit = density && GetSvmConfig().ra_hot_coalesce_all;
    ResetBoundaryDensity();
    const u32 density_start = density ? context.CurrentBufferSize() : 0;
    std::array<u32, static_cast<size_t>(DensityCategory::Count)> density_ops{};
    std::array<u32, static_cast<size_t>(DensityCategory::Count)> density_bytes{};
    if (density) {
        statistics.pfaf_density_bytes.fill(0);
    }
    u32 density_scalar_fp_ops = 0;
    PerfScope2 perf_prologue{GetPerfStats2().codegen_prologue};
    auto block_state = PrepareBlockState(block);
    const auto& loop_hoist = *block_state.loop_hoist;
    u32& loop_hoist_prefix_begin = block_state.loop_hoist_prefix_begin;
    u32& loop_hoist_prefix_ops = block_state.loop_hoist_prefix_ops;
    const bool split_flags_entry = block_state.split_flags_entry;
    // Do not force dirty at region entry. TestFlags Tst x26 after a packed
    // predecessor. Skip-pack only when the successor SaveFlags first.
    if (split_flags_entry) {
        // Every published/external entry takes the cold initializer below;
        // only the self edge targets local_entry. This makes host NZCV valid
        // before a pre-producer guest fault without charging the steady loop.
        __ B(backedge_flags_recipe->external_entry.get());
        PlacementPoint("flags", block->GetStartLocation().Value());
        __ Bind(backedge_flags_recipe->local_entry.get());
        context.BeginBackedgeBody();
        backedge_host_begin = context.CurrentBufferSize();
    } else if (backedge_flags_recipe) {
        backedge_host_begin = context.CurrentBufferSize();
    }
    EmitExecutionTrace(block->GetStartLocation().Value());
    const auto uniform_density = CollectUniformDensity(block, density);
    const u32 gpr_uniform_accesses =
            uniform_density.gpr_uniform_accesses;
    const u32 xmm_uniform_accesses =
            uniform_density.xmm_uniform_accesses;
    context.RecordExecCounter(exec_offset_gpr_uniform_accesses,
                              gpr_uniform_accesses);
    context.RecordExecCounter(exec_offset_xmm_uniform_accesses,
                              xmm_uniform_accesses);
    if (density) {
        RecordBoundaryRange(BoundarySubsequence::Prologue, density_start,
                            context.CurrentBufferSize());
        density_bytes[static_cast<size_t>(DensityCategory::Boundary)] +=
                context.CurrentBufferSize() - density_start;
    }
    perf_prologue.Stop();
    PerfScope2 perf_body{GetPerfStats2().codegen_body};
    TranslateBlockInstructions(block,
                               loop_hoist,
                               density,
                               gap_audit,
                               density_ops,
                               density_bytes,
                               density_scalar_fp_ops,
                               loop_hoist_prefix_begin,
                               loop_hoist_prefix_ops);
    perf_body.Stop();

    PerfScope2 perf_terminal{GetPerfStats2().codegen_terminal};
    EmitBlockTerminal(block, density, density_bytes);
    auto cold_recipe = CaptureBlockColdPathRecipe(block,
                                              density,
                                              density_ops,
                                              density_bytes,
                                              density_scalar_fp_ops,
                                              loop_hoist,
                                              loop_hoist_prefix_ops);
    if (translating_function) {
        block_cold_path_recipes.push_back(std::move(cold_recipe));
    } else {
        EmitBlockColdPathRecipe(std::move(cold_recipe));
    }
    if (!translating_function) {
        EmitUnalignedAtomicFallbacks();
        EmitDeferredNZCVMergeStubs();
        PlacementPoint("unit", block->GetStartLocation().Value());
    }
    return true;
}

bool JitTranslator::Translate(ir::HIRFunction* function) {
    if (!ImportRegionPreparation(nullptr, function)) return false;
    return EmitPreparedFunction(function);
}

bool JitTranslator::EmitPreparedFunction(ir::HIRFunction* function) {
    vixl::svm_vixl_prof::JitScope vixl_prof{context.GetFeatures().vixl_fast};
    ASSERT(function);
    ASSERT(region_plan_owner && region_plan_owner->function == function &&
           region_plan_owner->generation == context.GetAllocation().GPRLocationGeneration() &&
           region_plan_owner->allocation_attempt == context.GetAllocation().AllocationAttempt() &&
           region_plan_owner->options == CaptureRegionPreparationOptions(context.GetConfig(), context.GetFeatures()));
    ASSERT(block_cold_path_recipes.empty());
    placement_unit_pc = function->GetFunction()->GetStartLocation().Value();
    context.SetCurrent(function->GetFunction());
    disable_instructions.resize(function->MaxInstrCount());
    region_flags_canonical_stubs.clear();
    ReportRegionEdges();
    std::vector<ir::Block*> emitted_blocks;
    for (auto& hir_block : function->GetHIRBlocksRPO()) {
        auto* block = hir_block.GetBlock();
        if (block->GetInstList().empty() && !block->HasTerminal()) {
            continue;
        }
        emitted_blocks.push_back(block);
    }
    PrepareHostCallThunks(emitted_blocks);
    ASSERT(memory_state.unaligned_atomic_fallbacks.empty());
    terminal_location_publication.Prepare(emitted_blocks, context);
    share_cycle_exit_reason =
            CountCycleExitCandidates() >=
            kCycleTailShareMinStubs;
    translating_function = true;
    InvalidateFlagsToken();
    flag_state.flags_token_keep = false;
    for (size_t i = 0; i < emitted_blocks.size(); ++i) {
        // Undecoded successor left behind by lazy region compilation (and by
        // the pre-existing 128-block cap): no instructions and no terminal.
        // Emitting it would bind a label nobody branches to and then fall into
        // terminal::Invalid -> Ret without setting current_loc, which is a
        // dispatcher loop if it were ever entered.  Its guest address is
        // deliberately never published (TranslateIR skips empty blocks), so the
        // only way in is JitContext::Forward, which routes to it through the L2
        // dispatch slot instead.
        auto* block = emitted_blocks[i];
        next_region_block = i + 1 < emitted_blocks.size()
                ? std::optional<u64>{emitted_blocks[i + 1]
                                             ->GetStartLocation()
                                             .Value()}
                : std::nullopt;
        Translate(block);
    }
    PerfScope2 perf_unit_tail{GetPerfStats2().codegen_unit_tail};
    if (FlagsRegsEnabled() && RegionEdgesActive()) {
        for (auto* block : emitted_blocks) {
            EmitFlagsPublishedVeneer(block);
        }
    }
    context.EmitPendingFlagsCallEntry(
            function->GetFunction()->GetStartLocation().Value());
    for (auto& candidate : block_cold_path_recipes) {
        EmitBlockColdPathRecipe(std::move(candidate));
    }
    block_cold_path_recipes.clear();
    context.BeginColdScratch();
    EmitRegionFlagsCanonicalStubs();
    if (cycle_exit_reason) {
        EmitCycleExitReasonTail(cycle_exit_reason.get());
        cycle_exit_reason.reset();
    }
    EmitIndirectExitColdPaths();
    ASSERT(memory_state.pending_deferred_faults.empty());
    const auto recovery_offsets = terminal_location_publication.EmitColdPaths(context);
    EmitHostCallThunks();
    EmitUnalignedAtomicFallbacks();
    context.EndColdScratch();
    EmitDeferredNZCVMergeStubs();
    for (auto& fault : memory_state.fault_metadata) {
        if (fault.recovery_reg == UINT32_MAX) {
            continue;
        }
        fault.recovery_offset = recovery_offsets[fault.recovery_reg];
        ASSERT(fault.recovery_offset != 0);
        fault.recovery_reg = UINT32_MAX;
    }
    perf_unit_tail.Stop();
    translating_function = false;
    share_cycle_exit_reason = false;
    PlacementPoint("unit", placement_unit_pc);
    next_region_block.reset();
    canonical_entries_active = false;
    return true;
}


Label* JitTranslator::GetLocalLabel(ir::Inst* inst) {
    if (auto itr = local_labels.find(inst); itr != local_labels.end()) {
        return &itr->second;
    }
    return &local_labels.try_emplace(inst).first->second;
}

HostFlags JitTranslator::GuestNZCVToHost(ir::Flags guest) {
    HostFlags host{};
    if (True(guest & ir::Flags::Negate)) {
        host |= HostFlags::N;
    }
    if (True(guest & ir::Flags::Zero)) {
        host |= HostFlags::Z;
    }
    if (True(guest & ir::Flags::Carry)) {
        host |= HostFlags::C;
    }
    if (True(guest & ir::Flags::Overflow)) {
        host |= HostFlags::V;
    }
    return host;
}

Register JitTranslator::MaterializeOperand(const Operand& operand, ir::ValueType type) {
    auto tmp = context.GetTmpGPR(type);
    __ Mov(tmp, operand);
    return tmp;
}

void JitTranslator::Translate(ir::Inst* inst) {
    ASSERT(inst);
    ASSERT_MSG((gpr_plan->pinned && context.GetAllocation().GPRLocationsFrozen()) && context.GetAllocation().GPRLocationsFrozen() &&
                       gpr_plan->pinned->Generation() == context.GetAllocation().GPRLocationGeneration(),
               "instruction emission requires the current frozen GPR plan");
    const bool preservation = !context.GetAllocation().GPRPreservationsBefore(inst->Id()).empty();
    const bool forward_spilled_width_input = !preservation &&
            context.HasPendingSpillWrites() &&
            CanConsumeForwardedWidthSpill(inst);
    const auto forward_spilled_memory_input =
            !preservation && context.HasPendingSpillWrites()
            ? ForwardedMemorySpillInput(inst)
            : std::nullopt;
    const bool adopt_pending_spill_write = !preservation &&
            context.HasPendingSpillWrites() &&
            CanAdoptPendingSpillWrite(
                    inst, forward_spilled_memory_input.has_value());
    context.TickIR(inst, forward_spilled_width_input,
                   adopt_pending_spill_write,
                   forward_spilled_memory_input,
                   IsGPRZeroExtendElided(inst));
    context.ExecuteGPRPreservations(inst->Id());
    TrackHomeAlignment(inst);
    if (inst->GetOp() != ir::OpCode::SetLocation &&
        inst->GetOp() != ir::OpCode::CallReturn) {
        static_next_loc.reset();
        if (inst->GetOp() != ir::OpCode::PopRSB) {
            dynamic_next_loc.reset();
            dynamic_location_miss = nullptr;
        }
    }

#define INST(name, ...)                                                                            \
    case ir::OpCode::name:                                                                         \
        Emit##name(inst);                                                                          \
        break;

    switch (inst->GetOp()) {
#include "runtime/ir/ir.inc"
        default:
            ASSERT_MSG(false, "Instr unk op: {}", inst->GetOp());
    }

#undef INST
    context.EndInstructionScratch();
}

bool JitTranslator::MatchMemoryOffsetCase(ir::Inst* inst) { return false; }
































Operand JitTranslator::EmitOperand(ir::Operand& ir_op) {
    if (ir_op.GetRight().Null()) {
        if (ir_op.GetLeft().IsImm()) {
            auto imm = ir_op.GetLeft().imm.Get();
            auto imm_signed = ir_op.GetLeft().imm.GetSigned();
            bool can_imm = __ IsImmAddSub(imm_signed);
            if (can_imm) {
                return Operand{imm_signed};
            } else {
                auto tmp = context.GetTmpX();
                __ Mov(tmp, imm);
                return Operand{tmp};
            }
        } else {
            return Operand{context.R(ir_op.GetLeft().value, true)};
        }
    } else {
        Register left_reg;
        ir::ValueType left_type{ir::ValueType::U64};
        if (ir_op.GetLeft().IsImm()) {
            // Materialize an immediate left side (constant-based composite
            // operand) into a scratch register first.
            auto tmp = context.GetTmpX();
            __ Mov(tmp, ir_op.GetLeft().imm.Get());
            left_reg = tmp;
        } else {
            auto left_value = ir_op.GetLeft().value;
            left_type = left_value.Type();
            left_reg = context.R(left_value, true);
        }
        auto right = ir_op.GetRight();
        if (right.IsImm()) {
            auto imm = right.imm.GetSigned();
            auto is_lsl = ir_op.GetOp() == ir::OperandOp::LSL;
            auto is_lsr = ir_op.GetOp() == ir::OperandOp::LSR;
            if (is_lsl || is_lsr) {
                if ((left_reg.Is64Bits() || (imm < kWRegSize)) || (left_reg.Is32Bits() || (imm < kXRegSize))) {
                    return Operand{left_reg, is_lsl ? LSL : LSR, static_cast<u8>(imm)};
                } else {
                    PANIC();
                }
            } else if (ir_op.GetOp() == ir::OperandOp::Plus) {
                auto tmp = context.GetTmpGPR(left_type);
                bool can_imm = __ IsImmAddSub(imm);
                if (can_imm) {
                    __ Add(tmp, left_reg, imm);
                } else {
                    __ Mov(tmp, imm);
                    __ Add(tmp, left_reg, tmp);
                }
                return Operand{tmp};
            } else {
                PANIC();
            }
        } else {
            auto right_reg = context.R(right.value, true);
            auto tmp = context.GetTmpGPR(left_type);
            if (ir_op.GetOp() == ir::OperandOp::Plus) {
                __ Add(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                __ Lsl(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                __ Lsr(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::PlusExt) {
                auto shift_amount = ir_op.GetOp().shift_ext;
                ASSERT(right_reg.Is64Bits() || (shift_amount < kWRegSize));
                ASSERT(right_reg.Is32Bits() || (shift_amount < kXRegSize));
                __ Add(tmp, left_reg, Operand{right_reg, LSL, shift_amount});
                return Operand{tmp};
            } else {
                PANIC();
            }
        }
        return {};
    }
}





























































#undef masm

}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {
void JitTranslator::ImportGPRBlock(ir::Block* block) {
    ASSERT(context.RequireGPRPlan(block));
    gpr_plan_owner = context.GetAllocation().GPRPlanFor(block);
    gpr_plan = gpr_plan_owner.get();
    // The complete unit's exact IR/CFG certificate was checked once before
    // any entry code. Import consumes that proof instead of rescanning blocks.
    ASSERT(gpr_plan && gpr_plan->region == region_plan_owner &&
           gpr_plan->pinned->MatchesAllocation());
    context.BindInstructionScratchPlan(block, gpr_plan->pinned);
}
} // namespace swift::runtime::backend::arm64
