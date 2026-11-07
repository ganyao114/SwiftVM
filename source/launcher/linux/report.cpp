#include "report.h"
#include "base/format.h"
#include "runtime/execution/dispatch.h"

#include <iterator>

namespace swift::launcher {
namespace {
// Text values in this fixed report are closed protocol constants. Guest
// paths and diagnostic messages use the logger instead.
class Report {
public:
    Report() { buffer.push_back('{'); }

    template <class T> void Number(std::string_view key, T value) {
        Key(key);
        FormatTo(std::back_inserter(buffer), "{}", value);
    }

    void Text(std::string_view key, std::string_view value) {
        Key(key);
        FormatTo(std::back_inserter(buffer), "\"{}\"", value);
    }

    void Null(std::string_view key) {
        Key(key);
        FormatTo(std::back_inserter(buffer), "null");
    }

    void Optional(std::string_view key, std::uint64_t value, bool available) {
        if (available)
            Number(key, value);
        else
            Null(key);
    }

    void Write(std::FILE* stream) {
        buffer.push_back('}');
        buffer.push_back('\n');
        std::fwrite(buffer.data(), 1, buffer.size(), stream);
    }

private:
    void Key(std::string_view key) {
        if (buffer.size() != 1) buffer.push_back(',');
        FormatTo(std::back_inserter(buffer), "\"{}\":", key);
    }

    fmt::memory_buffer buffer;
};
}  // namespace

void WriteReport(std::FILE* stream,
                 const runtime::execution::RunResult& result,
                 const runtime::execution::RuntimeOptions& options) {
    using namespace runtime::execution;
    const auto& s = result.stats;
    Report r;
    r.Text("measurement_kind", "application_runtime");
    r.Text("scope", options.scope == runtime::ir::Scope::Function ? "function" : "block");
    r.Number("optimize", options.optimize);
    r.Number("runtime_completed", bool(result));
    r.Number("exit_code", result.exit_code);
    r.Text("engine",
           options.mode == ExecutionMode::JitOnly         ? "jit"
           : options.mode == ExecutionMode::InterpretOnly ? "interpreter"
                                                          : "mixed");
    r.Number("hot_threshold", options.hot_threshold);
    r.Number("interpreted_steps", s.interpreted_steps);
    r.Number("compile_ns", s.compile_request_ns);
    r.Number("compile_includes_byte_decode", true);
    r.Number("decode_excluded_available", options.decode_timing);
    r.Number("decode_timing_diagnostic", options.decode_timing);
    r.Number("byte_decoder_timing_perturbs_compile", options.decode_timing);
    r.Number("frontend_ns", s.frontend_ns);
    r.Number("optimize_ns", s.optimize_ns);
    r.Number("backend_ns", s.backend_ns);
    r.Number("publication_ns", s.publication_ns);
    r.Number("code_release_ns", s.code_release_ns);
    r.Number("fault_service_ns", s.fault_service_ns);
    r.Number("run_lifetime_ns", s.run_lifetime_ns);
    r.Number("compile_attempts", s.compile_attempts);
    r.Number("function_attempts", s.function_attempts);
    r.Number("block_attempts", s.block_attempts);
    r.Number("fallbacks", s.fallbacks);
    r.Optional("dispatches", s.dispatches, options.decode_timing);
    r.Optional("cache_hits", s.cache_hits, options.decode_timing);
    r.Optional("directory_probe_steps", s.directory_probe_steps, options.decode_timing);
    r.Number("syscalls", s.syscalls);
    r.Number("unsupported_syscalls", s.unsupported_syscalls);
    r.Number("emitted_guest_instructions", s.emitted_instructions);
    r.Number("emitted_guest_bytes", s.emitted_guest_bytes);
    r.Number("native_bytes", s.native_bytes);
    r.Number("peak_code_virtual_bytes", s.peak_code_bytes);
    r.Number("peak_slabs", s.peak_slabs);
    r.Number("peak_dual_slabs", s.peak_dual_slabs);
    r.Number("wx_fallback_used", s.code_wx_fallback);
    r.Number("cache_invalidations", s.cache_invalidations);
    r.Text("dispatch_cache",
           s.native_local_cache ? "native_directory_with_local_target_cache" : "native_directory");
    r.Number("cache_slot_bytes", sizeof(DispatchSlot));
    r.Number("native_code_bytes", sizeof(NativeCode));
    r.Text("cache_allocation_kind", "pc_slots_and_ready_code_rows");
    r.Text("dispatch_count_kind",
           s.native_local_cache ? "diagnostic_native_local_cache_and_directory_attempts"
                                : "diagnostic_native_directory_attempts");
    r.Number("dispatch_counts_available", options.decode_timing);
    r.Number("resolved_links_bypass_dispatch_count", true);
    r.Number("peak_cache_entries", s.peak_cache_entries);
    r.Number("peak_cache_occupied", s.peak_cache_occupied);
    r.Number("peak_cache_slots", s.peak_cache_slots);
    r.Number("cache_table_allocations", s.cache_table_allocations);
    r.Number("code_units_published", s.code_units_published);
    r.Number("static_links", s.static_links);
    r.Number("patched_links", s.patched_links);
    r.Number("pending_links", s.pending_links);
    r.Number("pending_targets", s.pending_targets);
    r.Number("link_relocation_rows", s.link_relocation_rows);
    r.Number("link_relocation_capacity", s.link_relocation_capacity);
    r.Number("private_uniforms", s.private_uniforms);
    r.Number("native_nzcv", s.native_nzcv);
    r.Number("pinned_uniform_slots", s.pinned_uniform_slots);
    r.Number("smc_write_faults", s.smc_write_faults);
    r.Number("smc_function_single_block_attempts", s.smc_function_single_block_attempts);
    r.Number("smc_file_write_invalidations", s.smc_file_write_invalidations);
    r.Optional("compile_decode_ns", s.compile_decode.nanoseconds, options.decode_timing);
    r.Optional("compile_decode_calls", s.compile_decode.calls, options.decode_timing);
    r.Optional("compile_without_byte_decode_ns",
               s.compile_without_byte_decode_ns,
               options.decode_timing);
    if (options.decode_timing) {
        r.Number("ir_nodes", s.ir_nodes);
        r.Number("operand_words", s.operand_words);
        r.Number("ir_work_counts_include_rejected_prefixes", false);
        r.Number("optimized_erased", s.optimized_erased);
        r.Number("optimized_uniform_forwarded", s.optimized_uniform_forwarded);
        r.Number("optimized_folded", s.optimized_folded);
        r.Number("optimized_copy_forwarded", s.optimized_copy_forwarded);
        r.Number("optimized_final_walks_skipped", s.optimized_final_walks_skipped);
        r.Number("spill_slots", s.spill_slots);
        r.Number("max_frame_bytes", s.max_frame_bytes);
        r.Text("guest_pc_policy", s.fault_locations ? "native_fault_sites" : "explicit_guest_pc");
        r.Text("packed_flags_policy",
               s.native_nzcv        ? "native_pstate_nzcv_inverted_c"
               : s.private_uniforms ? "native_live_shadow"
                                    : "fault_boundary_shadow");
        r.Number("rcpc_loads", s.rcpc_loads);
        r.Number("rcpc_imm9", s.rcpc_imm9);
        r.Number("fault_locations", s.fault_locations);
        r.Number("recover_unaligned", s.recover_unaligned);
        r.Number("fault_sites", s.fault_sites);
        r.Number("unaligned_recoveries", s.unaligned_recoveries);
        r.Number("unaligned_patches", s.unaligned_patches);
    }
    r.Write(stream);
}
}  // namespace swift::launcher
