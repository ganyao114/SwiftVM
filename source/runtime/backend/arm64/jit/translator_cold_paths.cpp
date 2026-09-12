#include "translator.h"

#include <algorithm>

namespace swift::runtime::backend::arm64 {

JitTranslator::BlockColdPathRecipe JitTranslator::CaptureBlockColdPathRecipe(
        ir::Block* block,
        bool density,
        std::span<const u32> density_ops,
        std::span<const u32> density_bytes,
        u32 density_scalar_fp_ops,
        const ir::LoopHoistMetadata& loop_hoist,
        u32 loop_hoist_prefix_ops) {
    BlockColdPathRecipe recipe;
    recipe.block = block;
    recipe.density = density;
    ASSERT(density_ops.size() == recipe.density_ops.size());
    ASSERT(density_bytes.size() == recipe.density_bytes.size());
    std::copy(density_ops.begin(), density_ops.end(), recipe.density_ops.begin());
    std::copy(density_bytes.begin(), density_bytes.end(),
              recipe.density_bytes.begin());
    recipe.density_scalar_fp_ops = density_scalar_fp_ops;
    recipe.loop_hoist = &loop_hoist;
    recipe.loop_hoist_prefix_ops = loop_hoist_prefix_ops;
    recipe.pfaf_density_bytes = statistics.pfaf_density_bytes;
    statistics.pfaf_density_bytes.fill(0);
    recipe.boundary_density_enabled = statistics.boundary_density_enabled;
    recipe.boundary_terminal_link_bytes = statistics.boundary_terminal_link_bytes;
    recipe.boundary_density_bytes = statistics.boundary_density_bytes;
    recipe.boundary_density_mnemonics =
            std::move(statistics.boundary_density_mnemonics);
    recipe.boundary_terminal_link_mnemonics =
            std::move(statistics.boundary_terminal_link_mnemonics);
    recipe.boundary_terminal_link_ranges =
            std::move(statistics.boundary_terminal_link_ranges);
    recipe.save_in_nzcv = flag_state.save_in_nzcv;
    recipe.nzcv_dirty = flag_state.nzcv_dirty;
    recipe.nzcv_requested = flag_state.nzcv_requested;
    recipe.flags_token_valid = flag_state.flags_token_valid;
    recipe.flags_token_result_code = flag_state.flags_token_result_code;
    recipe.flags_token_keep = flag_state.flags_token_keep;
    recipe.flags_audit_block_edge = flags_audit_block_edge;
    recipe.flags_audit_strict_advance = flags_audit_strict_advance;
    recipe.backedge_exit_label = std::move(backedge_exit_label);
    recipe.backedge_exit_referenced = backedge_exit_referenced;
    recipe.direct_cycle_exits = std::move(direct_cycle_exits);
    recipe.direct_cycle_cut_edges = direct_cycle_cut_edges;
    recipe.backedge_flags_recipe = std::move(backedge_flags_recipe);
    recipe.loop_hoist_body_entry = std::move(loop_hoist_body_entry);
    recipe.backedge_host_begin = backedge_host_begin;
    recipe.backedge_host_end = backedge_host_end;
    recipe.region_block_edges = statistics.region_block_edges;
    recipe.region_block_cycles = statistics.region_block_cycles;
    recipe.region_block_fallthroughs = statistics.region_block_fallthroughs;
    recipe.region_block_local_branch_bytes = statistics.region_block_local_branch_bytes;
    recipe.pending_exit_poll_faults = std::move(memory_state.pending_exit_poll_faults);
    recipe.vec_nan_cold_sites = std::move(vec_nan_cold_sites);
    recipe.flags_audit = context.DeferFlagsRegsAudit();

    statistics.boundary_density_enabled = false;
    statistics.boundary_terminal_link_bytes = 0;
    statistics.boundary_density_bytes.fill(0);
    flag_state.save_in_nzcv = true;
    flag_state.nzcv_dirty = false;
    flag_state.nzcv_requested = {};
    flag_state.flags_token_keep = false;
    InvalidateFlagsToken();
    flags_audit_strict_advance = false;
    backedge_exit_referenced = false;
    direct_cycle_cut_edges = 0;
    backedge_host_begin = 0;
    backedge_host_end = 0;
    statistics.region_block_edges = 0;
    statistics.region_block_cycles = 0;
    statistics.region_block_fallthroughs = 0;
    statistics.region_block_local_branch_bytes = 0;
    return recipe;
}

void JitTranslator::EmitBlockColdPathRecipe(BlockColdPathRecipe recipe) {
    ASSERT(recipe.block);
    ASSERT(recipe.loop_hoist);
    ASSERT(!backedge_exit_label);
    ASSERT(direct_cycle_exits.empty());
    ASSERT(!backedge_flags_recipe);
    ASSERT(!loop_hoist_body_entry);
    ASSERT(memory_state.pending_exit_poll_faults.empty());
    ASSERT(vec_nan_cold_sites.empty());

    cur_block = recipe.block;
    statistics.pfaf_density_bytes = recipe.pfaf_density_bytes;
    statistics.boundary_density_enabled = recipe.boundary_density_enabled;
    statistics.boundary_terminal_link_bytes = recipe.boundary_terminal_link_bytes;
    statistics.boundary_density_bytes = recipe.boundary_density_bytes;
    statistics.boundary_density_mnemonics =
            std::move(recipe.boundary_density_mnemonics);
    statistics.boundary_terminal_link_mnemonics =
            std::move(recipe.boundary_terminal_link_mnemonics);
    statistics.boundary_terminal_link_ranges =
            std::move(recipe.boundary_terminal_link_ranges);
    flag_state.save_in_nzcv = recipe.save_in_nzcv;
    flag_state.nzcv_dirty = recipe.nzcv_dirty;
    flag_state.nzcv_requested = recipe.nzcv_requested;
    flag_state.flags_token_valid = recipe.flags_token_valid;
    flag_state.flags_token_result_code = recipe.flags_token_result_code;
    flag_state.flags_token_keep = recipe.flags_token_keep;
    flags_audit_block_edge = recipe.flags_audit_block_edge;
    flags_audit_strict_advance = recipe.flags_audit_strict_advance;
    backedge_exit_label = std::move(recipe.backedge_exit_label);
    backedge_exit_referenced = recipe.backedge_exit_referenced;
    direct_cycle_exits = std::move(recipe.direct_cycle_exits);
    direct_cycle_cut_edges = recipe.direct_cycle_cut_edges;
    backedge_flags_recipe = std::move(recipe.backedge_flags_recipe);
    loop_hoist_body_entry = std::move(recipe.loop_hoist_body_entry);
    backedge_host_begin = recipe.backedge_host_begin;
    backedge_host_end = recipe.backedge_host_end;
    statistics.region_block_edges = recipe.region_block_edges;
    statistics.region_block_cycles = recipe.region_block_cycles;
    statistics.region_block_fallthroughs = recipe.region_block_fallthroughs;
    statistics.region_block_local_branch_bytes = recipe.region_block_local_branch_bytes;
    memory_state.pending_exit_poll_faults = std::move(recipe.pending_exit_poll_faults);
    vec_nan_cold_sites = std::move(recipe.vec_nan_cold_sites);
    if (recipe.flags_audit) {
        context.ResumeFlagsRegsAudit(std::move(*recipe.flags_audit));
    }

    context.BeginColdScratch();
    const u32 boundary_cold_before =
            recipe.density ? context.CurrentBufferSize() : 0;
    const u32 flags_audit_cold_begin = context.FlagsRegsAuditEnabled()
            ? context.CurrentBufferSize()
            : 0;
    flags_audit_cold = context.FlagsRegsAuditEnabled();
    EmitBackedgeExitStub();
    flag_state.flags_token_keep = false;
    InvalidateFlagsToken();
    EmitBackedgeColdPaths();
    if (backedge_exit_label) {
        ResolveExitPollFaults(backedge_exit_label.get(),
                              recipe.block->GetStartLocation());
    }
    backedge_exit_label.reset();
    backedge_exit_referenced = false;
    EmitDirectCycleExitStubs();
    if (recipe.density) {
        RecordBoundaryRange(BoundarySubsequence::ColdTail,
                            boundary_cold_before,
                            context.CurrentBufferSize());
        recipe.density_bytes[static_cast<size_t>(DensityCategory::Boundary)] +=
                context.CurrentBufferSize() - boundary_cold_before;
    }
    const u32 nan_cold_before =
            recipe.density ? context.CurrentBufferSize() : 0;
    EmitVecNaNColdPaths();
    if (recipe.density) {
        recipe.density_bytes[static_cast<size_t>(DensityCategory::NaN)] +=
                context.CurrentBufferSize() - nan_cold_before;
    }
    context.EndColdScratch();
    flags_audit_cold = false;
    if (context.FlagsRegsAuditEnabled()) {
        const u32 cold_bytes =
                context.CurrentBufferSize() - flags_audit_cold_begin;
        context.RecordFlagsRegsAudit(
                FlagsRegsAuditMergeCause::FaultVeneer,
                FlagsRegsAuditEdgeKind::Host,
                FlagsRegsAuditCost::RecoveryColdBytes,
                cold_bytes,
                cold_bytes != 0);
        context.FinishDeferredFlagsRegsAudit();
    }

    PrintBlockDensity(recipe.block,
                      recipe.density,
                      recipe.density_ops,
                      recipe.density_bytes,
                      recipe.density_scalar_fp_ops,
                      *recipe.loop_hoist,
                      recipe.loop_hoist_prefix_ops);
    ASSERT(memory_state.pending_exit_poll_faults.empty());
    ASSERT(vec_nan_cold_sites.empty());
    flag_state.save_in_nzcv = true;
    flag_state.nzcv_dirty = false;
    flag_state.nzcv_requested = {};
    flag_state.flags_token_keep = false;
    InvalidateFlagsToken();
}

}  // namespace swift::runtime::backend::arm64
