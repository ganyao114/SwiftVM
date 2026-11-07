#include "runtime/execution/runtime.h"
#include "runtime/execution/dispatch.h"

#include <charconv>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern char** environ;

int main(int argc, char** argv) {
    using namespace swift::runtime::execution;
    try {
        RuntimeOptions options;
        int argument = 1;
        for (; argument < argc; ++argument) {
            const std::string_view option{argv[argument]};
            if (option == "--") {
                ++argument;
                break;
            }
            if (option == "--help") {
                std::puts(
                        "svm_rebuild_linux [--scope block|function] [--engine "
                        "jit|interpreter|mixed] [--hot-threshold N] [--no-opt] [--decode-timing] "
                        "[--] guest [args...]\n"
                        "Static x86-64 ELF, direct guest==host memory, one guest thread.\n"
                        "PT_INTERP and unknown instructions are rejected. Unimplemented syscalls "
                        "return ENOSYS.\n"
                        "Default engine: jit; compile each requested entry immediately.\n"
                        "With --engine mixed, interpret cold blocks and compile after N entry "
                        "visits (--hot-threshold, default 8).\n"
                        "--decode-timing adds perturbed diagnostic clocks around the byte decoder "
                        "only.");
                return 0;
            }
            if (option == "--scope") {
                if (++argument >= argc)
                    throw std::runtime_error("--scope requires block or function");
                const std::string_view scope{argv[argument]};
                if (scope != "block" && scope != "function")
                    throw std::runtime_error("invalid --scope");
                options.scope = scope == "function" ? swift::runtime::ir::Scope::Function
                                                    : swift::runtime::ir::Scope::Block;
            } else if (option == "--engine") {
                if (++argument >= argc)
                    throw std::runtime_error("--engine requires jit, interpreter or mixed");
                const std::string_view engine{argv[argument]};
                if (engine == "jit")
                    options.mode = ExecutionMode::JitOnly;
                else if (engine == "interpreter")
                    options.mode = ExecutionMode::InterpretOnly;
                else if (engine == "mixed")
                    options.mode = ExecutionMode::Mixed;
                else
                    throw std::runtime_error("invalid --engine");
            } else if (option == "--hot-threshold") {
                if (++argument >= argc)
                    throw std::runtime_error("--hot-threshold requires an integer in 0..65535");
                const std::string_view count{argv[argument]};
                const auto parsed = std::from_chars(
                        count.data(), count.data() + count.size(), options.hot_threshold);
                if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size())
                    throw std::runtime_error("--hot-threshold requires an integer in 0..65535");
            } else if (option == "--no-opt")
                options.optimize = false;
            else if (option == "--decode-timing")
                options.decode_timing = true;
            else if (option.starts_with('-'))
                throw std::runtime_error("unknown runner option");
            else
                break;
        }
        if (argument >= argc) throw std::runtime_error("a guest ELF path is required");
        std::vector<std::string> arguments, environment;
        for (int i = argument; i < argc; ++i) arguments.emplace_back(argv[i]);
        for (char** variable = environ; variable && *variable; ++variable)
            environment.emplace_back(*variable);
        const auto result = RunGuest(arguments.front(), arguments, environment, options);
        if (!result) std::fprintf(stderr, "guest execution stopped: %s\n", result.error.c_str());
        const auto& s = result.stats;
        char dispatches[32]{"null"}, cache_hits[32]{"null"}, directory_probe_steps[32]{"null"};
        if (options.decode_timing) {
            std::snprintf(dispatches,
                          sizeof dispatches,
                          "%llu",
                          static_cast<unsigned long long>(s.dispatches));
            std::snprintf(cache_hits,
                          sizeof cache_hits,
                          "%llu",
                          static_cast<unsigned long long>(s.cache_hits));
            std::snprintf(directory_probe_steps,
                          sizeof directory_probe_steps,
                          "%llu",
                          static_cast<unsigned long long>(s.directory_probe_steps));
        }
        std::fprintf(
                stderr,
                "{\"measurement_kind\":\"application_runtime\",\"scope\":\"%s\",\"optimize\":%s,"
                "\"runtime_completed\":%s,\"exit_code\":%d,"
                "\"engine\":\"%s\",\"hot_threshold\":%u,\"interpreted_steps\":%llu,"
                "\"compile_ns\":%llu,\"compile_includes_byte_decode\":true,\"decode_excluded_"
                "available\":%s,"
                "\"decode_timing_diagnostic\":%s,\"byte_decoder_timing_perturbs_compile\":%s,"
                "\"frontend_ns\":%llu,\"optimize_ns\":%llu,\"backend_ns\":%llu,\"publication_ns\":%"
                "llu,\"code_release_ns\":%llu,\"fault_service_ns\":%llu,\"run_lifetime_ns\":%llu,"
                "\"compile_attempts\":%llu,\"function_attempts\":%llu,\"block_attempts\":%llu,"
                "\"fallbacks\":%llu,"
                "\"dispatches\":%s,\"cache_hits\":%s,\"directory_probe_steps\":%s,\"syscalls\":%"
                "llu,\"unsupported_syscalls\":%llu,"
                "\"emitted_guest_instructions\":%llu,\"emitted_guest_bytes\":%llu,\"native_bytes\":"
                "%llu,"
                "\"peak_code_virtual_bytes\":%llu,\"peak_slabs\":%llu,\"peak_dual_slabs\":%llu,"
                "\"wx_fallback_used\":%s,\"cache_invalidations\":%llu,"
                "\"dispatch_cache\":\"native_directory_with_local_target_cache\",\"cache_slot_"
                "bytes\":%zu,\"native_code_bytes\":%zu,"
                "\"cache_allocation_kind\":\"pc_slots_and_ready_code_rows\","
                "\"dispatch_count_kind\":\"diagnostic_native_local_cache_and_directory_attempts\","
                "\"dispatch_counts_available\":%s,\"resolved_links_bypass_dispatch_count\":true,"
                "\"peak_cache_entries\":%llu,\"peak_cache_occupied\":%llu,\"peak_cache_slots\":%"
                "llu,\"cache_table_allocations\":%llu,"
                "\"code_units_published\":%llu,\"static_links\":%llu,\"patched_links\":%llu,"
                "\"pending_links\":%llu,\"pending_targets\":%llu,"
                "\"link_relocation_rows\":%llu,\"link_relocation_capacity\":%llu,\"private_"
                "uniforms\":%s,\"native_nzcv\":%s,\"pinned_uniform_slots\":%u,"
                "\"smc_write_faults\":%llu,\"smc_function_single_block_attempts\":%llu,\"smc_file_"
                "write_invalidations\":%llu",
                options.scope == swift::runtime::ir::Scope::Function ? "function" : "block",
                options.optimize ? "true" : "false",
                result ? "true" : "false",
                result.exit_code,
                options.mode == ExecutionMode::JitOnly         ? "jit"
                : options.mode == ExecutionMode::InterpretOnly ? "interpreter"
                                                               : "mixed",
                static_cast<unsigned>(options.hot_threshold),
                static_cast<unsigned long long>(s.interpreted_steps),
                static_cast<unsigned long long>(s.compile_request_ns),
                options.decode_timing ? "true" : "false",
                options.decode_timing ? "true" : "false",
                options.decode_timing ? "true" : "false",
                static_cast<unsigned long long>(s.frontend_ns),
                static_cast<unsigned long long>(s.optimize_ns),
                static_cast<unsigned long long>(s.backend_ns),
                static_cast<unsigned long long>(s.publication_ns),
                static_cast<unsigned long long>(s.code_release_ns),
                static_cast<unsigned long long>(s.fault_service_ns),
                static_cast<unsigned long long>(s.run_lifetime_ns),
                static_cast<unsigned long long>(s.compile_attempts),
                static_cast<unsigned long long>(s.function_attempts),
                static_cast<unsigned long long>(s.block_attempts),
                static_cast<unsigned long long>(s.fallbacks),
                dispatches,
                cache_hits,
                directory_probe_steps,
                static_cast<unsigned long long>(s.syscalls),
                static_cast<unsigned long long>(s.unsupported_syscalls),
                static_cast<unsigned long long>(s.emitted_instructions),
                static_cast<unsigned long long>(s.emitted_guest_bytes),
                static_cast<unsigned long long>(s.native_bytes),
                static_cast<unsigned long long>(s.peak_code_bytes),
                static_cast<unsigned long long>(s.peak_slabs),
                static_cast<unsigned long long>(s.peak_dual_slabs),
                s.code_wx_fallback ? "true" : "false",
                static_cast<unsigned long long>(s.cache_invalidations),
                sizeof(DispatchSlot),
                sizeof(NativeCode),
                options.decode_timing ? "true" : "false",
                static_cast<unsigned long long>(s.peak_cache_entries),
                static_cast<unsigned long long>(s.peak_cache_occupied),
                static_cast<unsigned long long>(s.peak_cache_slots),
                static_cast<unsigned long long>(s.cache_table_allocations),
                static_cast<unsigned long long>(s.code_units_published),
                static_cast<unsigned long long>(s.static_links),
                static_cast<unsigned long long>(s.patched_links),
                static_cast<unsigned long long>(s.pending_links),
                static_cast<unsigned long long>(s.pending_targets),
                static_cast<unsigned long long>(s.link_relocation_rows),
                static_cast<unsigned long long>(s.link_relocation_capacity),
                s.private_uniforms ? "true" : "false",
                s.native_nzcv ? "true" : "false",
                s.pinned_uniform_slots,
                static_cast<unsigned long long>(s.smc_write_faults),
                static_cast<unsigned long long>(s.smc_function_single_block_attempts),
                static_cast<unsigned long long>(s.smc_file_write_invalidations));
        if (options.decode_timing) {
            std::fprintf(stderr,
                         ",\"compile_decode_ns\":%llu,\"compile_decode_calls\":%llu,\"compile_"
                         "without_byte_decode_ns\":%llu,"
                         "\"ir_nodes\":%llu,\"operand_words\":%llu,\"ir_work_counts_include_"
                         "rejected_prefixes\":false,"
                         "\"optimized_erased\":%llu,\"optimized_uniform_forwarded\":%llu,"
                         "\"optimized_folded\":%llu,\"optimized_copy_forwarded\":%llu,"
                         "\"optimized_final_walks_skipped\":%llu,"
                         "\"spill_slots\":%llu,\"max_frame_bytes\":%llu,"
                         "\"guest_pc_policy\":\"%s\",\"packed_flags_policy\":\"%s\","
                         "\"rcpc_loads\":%s,\"rcpc_imm9\":%s,\"fault_locations\":%s,\"recover_"
                         "unaligned\":%s,"
                         "\"fault_sites\":%llu,\"unaligned_recoveries\":%llu,\"unaligned_patches\":"
                         "%llu}",
                         static_cast<unsigned long long>(s.compile_decode.nanoseconds),
                         static_cast<unsigned long long>(s.compile_decode.calls),
                         static_cast<unsigned long long>(s.compile_without_byte_decode_ns),
                         static_cast<unsigned long long>(s.ir_nodes),
                         static_cast<unsigned long long>(s.operand_words),
                         static_cast<unsigned long long>(s.optimized_erased),
                         static_cast<unsigned long long>(s.optimized_uniform_forwarded),
                         static_cast<unsigned long long>(s.optimized_folded),
                         static_cast<unsigned long long>(s.optimized_copy_forwarded),
                         static_cast<unsigned long long>(s.optimized_final_walks_skipped),
                         static_cast<unsigned long long>(s.spill_slots),
                         static_cast<unsigned long long>(s.max_frame_bytes),
                         s.fault_locations ? "native_fault_sites" : "explicit_guest_pc",
                         s.native_nzcv        ? "native_pstate_nzcv_inverted_c"
                         : s.private_uniforms ? "native_live_shadow"
                                              : "fault_boundary_shadow",
                         s.rcpc_loads ? "true" : "false",
                         s.rcpc_imm9 ? "true" : "false",
                         s.fault_locations ? "true" : "false",
                         s.recover_unaligned ? "true" : "false",
                         static_cast<unsigned long long>(s.fault_sites),
                         static_cast<unsigned long long>(s.unaligned_recoveries),
                         static_cast<unsigned long long>(s.unaligned_patches));
        } else
            std::fputs(
                    ",\"compile_decode_ns\":null,\"compile_decode_calls\":null,\"compile_without_"
                    "byte_decode_ns\":null}",
                    stderr);
        std::fputc('\n', stderr);
        return result.exit_code;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "runner error: %s\n", error.what());
        return 125;
    }
}
