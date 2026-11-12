#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "base/error.h"
#include "runtime/arch/arm32/profile.h"
#include "runtime/exception.h"
#include "runtime/frontend/source.h"
#include "runtime/ir/types.h"
#include "timing.h"

namespace swift::runtime::execution {

// Mixed interprets cold entry fragments, then compiles a hot entry into the
// requested allocation scope. Unsupported native entries keep interpreting.
enum class ExecutionMode : std::uint8_t { JitOnly, InterpretOnly, Mixed };

struct RuntimeOptions {
    ExecutionMode mode{ExecutionMode::JitOnly};
    ir::Scope scope{ir::Scope::Block};
    bool optimize{true};
    // Mixed defers this many entry visits; 0 requests compilation immediately.
    std::uint16_t hot_threshold{8};
    frontend::DecodeLimits function_limits{};
    arm32::Profile arm32_profile{};
    // UINT8_MAX keeps the backend default; otherwise the effective count is
    // clamped to the backend capability.
    std::uint8_t pinned_slots{UINT8_MAX};
    bool decode_timing{};
    // Additional request-level clocks when decode_timing is enabled.
    bool compile_details{};
    // Only the standalone launcher forwards guest job-control/default fatal
    // actions to its host process. Embedders retain ownership of that policy.
    bool forward_process_signals{};
    // A cold Runtime callback may inspect/edit the borrowed context and resume
    // via dispatch. Stop returns an owning snapshot; GuestOS uses Linux policy.
    GuestExceptionHandler exception_handler{};
    void* exception_opaque{};
    // Empty keeps persistence entirely disabled.
    std::string cache_directory;
    std::string artifact_path;
};

struct RuntimeStats {
    // Diagnostic-only actual native dispatcher lookups/ready hits. Ordinary
    // execution does not count. Probe steps count actual directory slot reads;
    // null-directory exits count no probes. Resolved links bypass all three
    // diagnostic counts; these fields never infer how many guest units ran.
    std::uint64_t dispatches{}, cache_hits{}, directory_probe_steps{};
    // Completed direct execution steps, including individual REP iterations.
    std::uint64_t interpreted_steps{};
    std::uint64_t syscalls{}, compile_attempts{}, function_attempts{}, block_attempts{},
            fallbacks{};
    std::uint64_t compile_request_ns{}, frontend_ns{}, optimize_ns{}, backend_ns{},
            publication_ns{};
    std::uint64_t code_release_ns{}, fault_service_ns{};
    std::uint64_t cache_load_ns{}, cache_write_ns{};
    std::uint64_t fp_domains_created{}, fp_domain_switches{}, fp_domain_switch_ns{};
    std::uint64_t disk_cache_hits{}, disk_cache_misses{}, disk_cache_rejects{}, disk_cache_writes{},
            aot_units_loaded{};
    frontend::DecodeTiming
            compile_decode;  // Opt-in diagnostic; ordinary runs read no per-instruction clocks.
    PublicationTiming publication_detail;
    std::uint64_t compile_without_byte_decode_ns{};
    // O(1) existing-result observations, enabled with decode_timing. IR sizes
    // cover returned owners, not a rejected frontend's already-freed prefix.
    std::uint64_t ir_nodes{}, operand_words{}, optimized_erased{}, optimized_uniform_forwarded{},
            optimized_folded{}, optimized_copy_forwarded{}, optimized_final_walks_skipped{};
    std::uint64_t spill_slots{}, max_frame_bytes{};
    std::uint64_t emitted_instructions{}, emitted_guest_bytes{}, native_bytes{},
            cache_invalidations{};
    std::uint64_t peak_code_bytes{}, peak_slabs{}, peak_dual_slabs{};
    std::uint64_t peak_cache_entries{}, peak_cache_occupied{}, peak_cache_slots{},
            cache_table_allocations{};
    std::uint64_t code_units_published{}, static_links{}, patched_links{}, pending_links{},
            pending_targets{};
    std::uint64_t link_relocation_rows{}, link_relocation_capacity{};
    std::uint64_t thread_l1_bytes{}, dispatch_interrupt_bytes{}, dispatch_interrupts{};
    std::uint64_t smc_write_faults{}, smc_function_single_block_attempts{};
    std::uint64_t smc_file_write_invalidations{};
    std::uint64_t fault_sites{}, unaligned_recoveries{}, unaligned_patches{};
    std::uint64_t run_lifetime_ns{}, unsupported_syscalls{};
    std::uint32_t pinned_uniform_slots{};
    bool thread_dispatch_cache{};
    bool code_wx_fallback{}, rcpc_loads{}, rcpc_imm9{}, fault_locations{}, recover_unaligned{},
            private_uniforms{}, native_nzcv{}, native_eflags{};
    // frontend_ns includes formation, actual byte decoding, buffer management
    // and IR construction. Only decode_timing runs expose a perturbed
    // compile_without_byte_decode_ns; formation/buffer work stays charged.
};

struct RunResult {
    int exit_code;
    RuntimeStats stats;
    std::string error;
    swift::Failure failure;  // Host/runtime failure, independent of a legitimate guest exit code.
    GuestStopReason reason{GuestStopReason::GuestExit};
    int termination_signal{};
    std::unique_ptr<GuestExceptionSnapshot> exception;
    swift::Failure cleanup_failure;

    explicit operator bool() const noexcept {
        return reason == GuestStopReason::GuestExit && !failure && !cleanup_failure;
    }
};

RunResult RunGuest(const std::string& executable,
                   std::span<const std::string> argv,
                   std::span<const std::string> environment,
                   RuntimeOptions = {});

}  // namespace swift::runtime::execution
