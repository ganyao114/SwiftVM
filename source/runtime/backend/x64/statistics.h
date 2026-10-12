#pragma once

#include <array>
#include <cstdint>

namespace swift::runtime::backend::x64 {

// A partial, static census of explicitly emitted transfer instructions.
// These are code-generation sites, never dynamic access counts or cycles.
enum class TransferRegion : unsigned {
    CanonicalSSA, ProducerMetadata, AddressScratch, PhiScratch, Local,
    Uniform, StateFlags, StateOther, FrameOther, IndexedFrame, OtherAddress,
    Count
};
struct TransferCount {
    std::uint64_t scalar_load_sites{}, scalar_store_sites{};
    std::uint64_t vector_load_sites{}, vector_store_sites{};
    std::uint64_t load_bytes{}, store_bytes{};
};
struct EmitStatistics {
    // Canonical home stores are not allocator spills. The scalar cache does
    // not provide a general register-allocation or spill census.
    static constexpr bool complete_memory_census = false;
    static constexpr const char* observed_transfers =
        "mov,movzx,movsx,movsxd,movd,movq,movdqu,movdqa,movss,movsd,pop(memory)";
    static constexpr const char* unobserved_memory =
        "ALU/SIMD memory operands,cmp,bt,RMW,push/implicit stack,rep,helper internals";
    std::array<TransferCount, static_cast<unsigned>(TransferRegion::Count)> transfers{};
    std::uint64_t helper_call_sites{}; // Explicit native indirect calls only.
    std::uint64_t flags_snapshot_sites{}; // pop into producer's flags metadata.
    std::uint64_t flags_state_load_sites{}, flags_state_store_sites{};
    // ADC/SBC memory/register BT reads are tracked separately from MOV;
    // they do not extend the partial transfer census above.
    std::uint64_t flags_state_carry_read_sites{}, flags_resident_carry_read_sites{};
    // Actual two-producer CF forwards / omitted BT and first packed NZCV
    // merges. These are static codegen sites, not dynamic execution counts.
    std::uint64_t native_cf_forward_sites{}, native_cf_merge_omissions{};
    std::uint64_t native_cf_state_bt_omissions{}, native_cf_resident_bt_omissions{};
    std::uint64_t native_cf_polarity_cmc_sites{};
    std::uint64_t flags_resident_read_sites{}, flags_resident_write_sites{};
    // Direct 64-bit AND/OR SaveFlags merges; read/write sites count MOV only.
    std::uint64_t flags_resident_merge_sites{};
    // Other direct resident publication/mutation groups; no-effect requests
    // do not count. SaveFlags remains exclusively in merge_sites above.
    std::uint64_t flags_resident_update_sites{};
    // Flag publication/mutation IR requests reaching Emit (including empty
    // requested masks), never native op counts. RMW/helper transfers excluded.
    std::uint64_t flags_materialization_ir_sites{};
    std::uint64_t phi_value_copy_sites{}, phi_immediate_copy_sites{};
    // Scalar Phi copies written directly to distinct homes without scratch.
    std::uint64_t direct_phi_copy_sites{};
    std::uint64_t vector_forward_write_sites{}, vector_forward_read_sites{};
    // Actual full-container cache copies and dedicated frame helper traffic.
    // Canonical homes/stores are retained; this is not a vector spill reduction.
    bool vector_cfg_requested{}, vector_cfg_plan_enabled{};
    std::uint32_t vector_cfg_rejection_reason{}, vector_cfg_budget_limit{};
    std::uint64_t vector_cfg_analysis_nodes{}, vector_cfg_analysis_work{}, vector_cfg_pair_work{};
    std::uint64_t vector_cfg_interference_storage_entries{}, vector_cfg_fixed_point_passes{};
    std::uint64_t vector_cfg_cache_read_sites{}, vector_cfg_cache_write_sites{};
    std::uint64_t vector_cfg_helper_save_sites{}, vector_cfg_helper_restore_sites{};
    std::uint64_t vector_cfg_phi_publication_sites{};
    // Canonical stores restored where legacy XMM2 forwarding had omitted them.
    std::uint64_t vector_cfg_legacy_home_retentions{};
    // Actual cache-to-scratch / scratch-to-cache copy MOV sites; masks and
    // direct ALU reads/writes of cache registers are not cache copy sites.
    std::uint64_t scalar_cache_read_sites{}, scalar_cache_write_sites{};
    // Audited binary sites computing directly in a dead lhs cache slot.
    std::uint64_t scalar_coalesced_binary_sites{};
    // Unary groups executing in a proved dead input's assigned register,
    // eliminating the two cache-copy MOV sites. Includes no-op groups.
    std::uint64_t scalar_coalesced_unary_sites{};
    // Actual 8/16/32-bit consumer-mask instructions omitted through
    // canonical publication and full CFG dead-input reuse proofs.
    std::uint64_t scalar_unary_mask_omissions{};
    // Fixed shift groups that avoid RCX count capture/masking. Zero shifts
    // retain input and output normalization but emit no shift instruction.
    std::uint64_t scalar_shift_immediate_sites{}, scalar_shift_zero_sites{};
    // Proven-unread high qwords omitted while retaining a scalar home store.
    std::uint64_t scalar_high_word_store_omissions{};
    // Actual read-site MOV reconstruction and omitted LoadImm publication
    // groups. These are static sites, not dynamic execution counters.
    std::uint64_t scalar_rematerialized_read_sites{}, scalar_rematerialized_definition_sites{};
    // ALU immediate sites replacing RHS scratch materialization; the second
    // counter is the subset whose RHS was an approved rematerialized value.
    std::uint64_t scalar_binary_immediate_sites{}, scalar_binary_rematerialized_immediate_sites{};
    std::uint64_t emitted_code_bytes{}, frame_bytes{};
    std::uint64_t unresolved_transfer_width_sites{};
    // Without Config, uniform classification uses only State's buffer lower bound.
    bool uniform_extent_known{};
    const TransferCount& At(TransferRegion region) const noexcept {
        return transfers[static_cast<unsigned>(region)];
    }
};

} // namespace swift::runtime::backend::x64
