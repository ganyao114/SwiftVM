# Existing cases below execute ARM64 machine code and use its register ABI.
add_executable(swift_ir_test ir_contract_test.cpp)
target_link_libraries(swift_ir_test PRIVATE swift_translator swift::runtime Catch2::Catch2WithMain)
add_test(NAME swift_ir_contracts COMMAND swift_ir_test)
set_tests_properties(swift_ir_contracts PROPERTIES LABELS "unit;ir" TIMEOUT 30)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
    add_executable(swift_entry_persistence_test entry_persistence_test.cpp)
    target_link_libraries(swift_entry_persistence_test PRIVATE swift_aot)
    foreach(mode disk aot reject native aot-links native-links)
        add_test(NAME swift_entry_persistence_${mode} COMMAND swift_entry_persistence_test ${mode})
        set_tests_properties(swift_entry_persistence_${mode} PROPERTIES LABELS "unit;serialization;entry" TIMEOUT 30)
    endforeach()
    add_executable(swift_address_space_shutdown_test address_space_shutdown_test.cpp)
    target_link_libraries(swift_address_space_shutdown_test PRIVATE swift_aot)
    target_link_options(swift_address_space_shutdown_test PRIVATE "-Wl,--wrap=munmap"
            "-Wl,--wrap=mprotect" "-Wl,--wrap=close" "-Wl,--wrap=destroy_mspace"
            "-Wl,--wrap=mmap" "-Wl,--wrap=ftruncate" "-Wl,--wrap=rename"
            "-Wl,--wrap=create_mspace_with_base" "-Wl,--wrap=mspace_free" "-Wl,--wrap=mspace_memalign"
            "-Wl,--wrap=_ZN5swift7runtime7backend6Module4PushEPNS0_2ir11AddressNodeE"
            "-Wl,--wrap=_ZN5swift7runtime7backend6Module14AllocCodeCacheEjb"
            "-Wl,--wrap=_ZNK5swift7runtime7backend11LinkManager12QueryTargetsENS1_15LinkSourceOwnerE"
            "-Wl,--wrap=_ZN5swift7runtime7backend11LinkManager11PurgeSourceENS1_15LinkSourceOwnerE"
            "-Wl,--wrap=_ZN5swift7runtime7backend8ReadUnitERNS1_10BlobReaderERNS1_10SerialUnitE"
            "-Wl,--wrap=sched_yield")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
        target_link_options(swift_address_space_shutdown_test PRIVATE "-Wl,--wrap=process_vm_readv")
    endif()
    foreach(mode gates tokens running construction retained save owners readers retry-rx retry-rw smc close aot aot-dispatch disk-dispatch disk-revival disk-revival-cleanup disk-revival-cleanup-terminal disk-revival-oom symbols symbols-smc symbols-range symbols-write symbols-latest symbols-publication symbols-rollback symbols-dirty symbols-compound multi-install multi-install-cleanup multi-install-cleanup-terminal multi-call-install multi-call-install-cleanup multi-call-install-cleanup-terminal symbols-reader-node symbols-reader-range symbols-child-node symbols-child-range symbols-child-write instruction-pin-terminal instruction-pin-reclaim instruction-pin-failure retire-oom-node retire-oom-range retire-oom-write range-unprotect range-unprotect-terminal range-republish range-republish-protect-fail cycles resource-cycles)
        add_test(NAME swift_address_space_shutdown_${mode} COMMAND swift_address_space_shutdown_test ${mode})
        set_tests_properties(swift_address_space_shutdown_${mode} PROPERTIES
                LABELS "unit;shutdown;mapping;lifetime" TIMEOUT 30)
    endforeach()
    foreach(mode disk-readability aot-readability guest-hash aot-entry disk-entry
            aot-entry-node disk-entry-node aot-entry-range disk-entry-range aot-entry-write disk-entry-write
            aot-reader-node disk-reader-node aot-reader-range disk-reader-range aot-reader-write disk-reader-write
            aot-linked-oom disk-linked-oom aot-linked-protect disk-linked-protect
            aot-linked-cleanup disk-linked-cleanup aot-linked-terminal disk-linked-terminal
            aot-linked-multi-oom disk-linked-multi-oom aot-linked-multi-protect disk-linked-multi-protect
            aot-linked-multi-cleanup disk-linked-multi-cleanup aot-linked-multi-terminal disk-linked-multi-terminal
            aot-linked-private-oom disk-linked-private-oom
            aot-linked-chain-oom disk-linked-chain-oom aot-linked-chain-protect disk-linked-chain-protect
            aot-linked-chain-cleanup disk-linked-chain-cleanup aot-linked-chain-terminal disk-linked-chain-terminal
            aot-linked-race-source disk-linked-race-source aot-linked-race-target disk-linked-race-target aot-linked-race-duplicate
            aot-linked-reuse disk-linked-reuse
            aot-linked-partial-rx disk-linked-partial-rx aot-linked-partial-rw disk-linked-partial-rw
            linked-executor-retire aot-linked-executor-retire disk-linked-executor-retire
            aot-linked-dual-executor-retire disk-linked-dual-executor-retire
            aot-linked-target-executor disk-linked-target-executor
            aot-linked-concurrent-install disk-linked-concurrent-install
            aot-linked-concurrent-compile disk-linked-concurrent-compile
            aot-linked-call-oom disk-linked-call-oom aot-linked-call-protect disk-linked-call-protect
            aot-linked-call-cleanup disk-linked-call-cleanup aot-linked-call-terminal disk-linked-call-terminal
            aot-linked-call-executor disk-linked-call-executor
            aot-linked-double-retire disk-linked-double-retire
            aot-linked-install-retire disk-linked-install-retire
            aot-linked-call-invalidate disk-linked-call-invalidate
            aot-linked-compile-retire disk-linked-compile-retire
            aot-linked-install-failure disk-linked-install-failure)
        add_test(NAME swift_address_space_shutdown_${mode} COMMAND swift_address_space_shutdown_test ${mode})
        set_tests_properties(swift_address_space_shutdown_${mode} PROPERTIES
                LABELS "unit;shutdown;mapping;lifetime;guest-read" TIMEOUT 30)
    endforeach()
    set_tests_properties(swift_address_space_shutdown_save PROPERTIES
            ENVIRONMENT "SVM_FLAGS_REGS=0;SVM_BACKEDGE_FLAGS=0;SVM_FLAGS_LOOP_LAZY=0")
    add_executable(swift_terminal_revoke_test terminal_revoke_test.cpp)
    target_link_libraries(swift_terminal_revoke_test PRIVATE swift_translator swift::runtime)
    target_link_options(swift_terminal_revoke_test PRIVATE "-Wl,--wrap=mprotect")
    foreach(mode catalog links signals smc disable reclaim)
        add_test(NAME swift_terminal_revoke_${mode} COMMAND swift_terminal_revoke_test ${mode})
        set_tests_properties(swift_terminal_revoke_${mode} PROPERTIES
                LABELS "unit;shutdown;signal;lifetime" TIMEOUT 30)
    endforeach()
    add_executable(swift_shutdown_owner_test shutdown_owner_test.cpp)
    target_link_libraries(swift_shutdown_owner_test PRIVATE swift_translator swift::runtime)
    foreach(mode control runtime slots signals destruction)
        add_test(NAME swift_shutdown_owner_${mode} COMMAND swift_shutdown_owner_test ${mode})
        set_tests_properties(swift_shutdown_owner_${mode} PROPERTIES
                LABELS "unit;shutdown;signal;lifetime" TIMEOUT 30)
    endforeach()
    add_executable(swift_mapping_release_test mapping_release_test.cpp)
    target_link_libraries(swift_mapping_release_test PRIVATE swift::runtime)
    target_link_options(swift_mapping_release_test PRIVATE
            "-Wl,--wrap=mmap" "-Wl,--wrap=munmap" "-Wl,--wrap=close"
            "-Wl,--wrap=mremap" "-Wl,--wrap=create_mspace_with_base" "-Wl,--wrap=destroy_mspace"
            "-Wl,--wrap=_Znwm")
    foreach(mode mapping retry-rx retry-rw close registration cache cache-retry data bounded construction retained)
        add_test(NAME swift_mapping_release_${mode} COMMAND swift_mapping_release_test ${mode})
        set_tests_properties(swift_mapping_release_${mode} PROPERTIES
                LABELS "unit;mapping;lifetime" TIMEOUT 30)
    endforeach()
    add_executable(swift_retirement_control_test retirement_control_test.cpp)
    target_link_libraries(swift_retirement_control_test PRIVATE swift_translator swift::runtime)
    foreach(mode control concurrent runtime retained persistence link-publication links requests)
        add_test(NAME swift_retirement_control_${mode} COMMAND swift_retirement_control_test ${mode})
        set_tests_properties(swift_retirement_control_${mode} PROPERTIES
                LABELS "unit;admission;lifetime" TIMEOUT 30)
    endforeach()
    add_executable(swift_retirement_cycle_test retirement_cycle_test.cpp)
    target_link_libraries(swift_retirement_cycle_test PRIVATE swift_translator swift::runtime)
    foreach(shape two-unit self region)
        add_test(NAME swift_retirement_cycle_${shape} COMMAND swift_retirement_cycle_test ${shape})
        set_tests_properties(swift_retirement_cycle_${shape} PROPERTIES
                LABELS "unit;retirement;lifetime" TIMEOUT 30
                ENVIRONMENT "SVM_BACKEDGE_LATCH=0")
    endforeach()
    add_executable(swift_execution_scope_test execution_scope_test.cpp)
    target_link_libraries(swift_execution_scope_test PRIVATE swift_translator swift::runtime)
    foreach(mode epochs runtime retired unregister)
        add_test(NAME swift_execution_scope_${mode} COMMAND swift_execution_scope_test ${mode})
        set_tests_properties(swift_execution_scope_${mode} PROPERTIES
                LABELS "unit;execution;lifetime" TIMEOUT 30)
    endforeach()
    add_executable(swift_fault_catalog_test fault_catalog_test.cpp)
    target_link_libraries(swift_fault_catalog_test PRIVATE swift::runtime)
    foreach(mode snapshots signal concurrent integration)
        add_test(NAME swift_fault_catalog_${mode} COMMAND swift_fault_catalog_test ${mode})
        set_tests_properties(swift_fault_catalog_${mode} PROPERTIES
                LABELS "unit;signal;fault;lifetime" TIMEOUT 30)
    endforeach()
endif()
add_executable(swift_hir_lifetime_test hir_lifetime_test.cpp)
target_link_libraries(swift_hir_lifetime_test PRIVATE swift_translator swift::runtime)
foreach(mode unfinished published construction finish-retry retained thread-exit region-owner preparing prepared
             prepared-blocks prepared-regions constant-window optimized-block optimized-function state-sink-failure)
    foreach(fast 0 1)
        add_test(NAME swift_hir_lifetime_${mode}_fast${fast}
                 COMMAND swift_hir_lifetime_test ${mode})
        set_tests_properties(swift_hir_lifetime_${mode}_fast${fast} PROPERTIES
                LABELS "unit;ir;hir;lifetime" TIMEOUT 30
                ENVIRONMENT "SVM_IR_FAST=${fast};SVM_IR_DETAIL=0")
    endforeach()
endforeach()
if("arm64" IN_LIST ARCHITECTURE)
    add_executable(swift_arm64_call_persistence_test arm64/call_persistence_test.cpp)
    target_link_libraries(swift_arm64_call_persistence_test PRIVATE swift_aot)
    foreach(mode static indirect aot-static aot-indirect aot-producer aot-reject)
        add_test(NAME swift_arm64_call_persistence_${mode} COMMAND swift_arm64_call_persistence_test ${mode})
        set_tests_properties(swift_arm64_call_persistence_${mode} PROPERTIES LABELS "unit;arm64;cache;call"
                TIMEOUT 60 ENVIRONMENT "SVM_FLAGS_REGS=1;SVM_BACKEDGE_LATCH=0;SVM_BACKEDGE_FLAGS=0;SVM_FLAGS_LOOP_LAZY=0")
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
        add_executable(swift_arm64_rsb_disabled_test arm64/rsb_disabled_test.cpp)
        target_link_libraries(swift_arm64_rsb_disabled_test PRIVATE swift_translator swift::runtime)
        foreach(flags 0 1)
            add_test(NAME swift_arm64_rsb_disabled_flags${flags} COMMAND swift_arm64_rsb_disabled_test body)
            set_tests_properties(swift_arm64_rsb_disabled_flags${flags} PROPERTIES
                    LABELS "unit;arm64;rsb;abi" TIMEOUT 30
                    ENVIRONMENT "SVM_FLAGS_REGS=${flags};SVM_BACKEDGE_LATCH=0")
        endforeach()
        add_executable(swift_arm64_static_latch_cache_version_test arm64/static_latch_cache_version_test.cpp)
        target_link_libraries(swift_arm64_static_latch_cache_version_test PRIVATE swift_aot)
        add_test(NAME swift_arm64_static_latch_cache_version COMMAND swift_arm64_static_latch_cache_version_test)
        set_tests_properties(swift_arm64_static_latch_cache_version PROPERTIES
                LABELS "unit;arm64;aot;cache;retirement" TIMEOUT 30
                ENVIRONMENT "SVM_FLAGS_REGS=0;SVM_BACKEDGE_LATCH=1")
        add_executable(swift_arm64_static_call_latch_fault_test arm64/static_call_latch_fault_test.cpp)
        target_link_libraries(swift_arm64_static_call_latch_fault_test PRIVATE swift_aot)
        foreach(mode call branch mixed-call mixed-branch indirect-call rsb-call internal-call published-call indirect-internal-call
                     request-call request-branch request-mixed-call request-mixed-branch request-indirect-call request-rsb-call request-internal-call request-published-call request-indirect-internal-call
                     aot-call aot-branch aot-mixed-call aot-mixed-branch aot-indirect-call aot-rsb-call aot-internal-call aot-published-call aot-indirect-internal-call
                     aot-request-call aot-request-branch aot-request-mixed-call aot-request-mixed-branch aot-request-indirect-call aot-request-rsb-call aot-request-internal-call aot-request-published-call aot-request-indirect-internal-call
                     disk-call disk-branch disk-mixed-call disk-mixed-branch disk-indirect-call disk-rsb-call disk-internal-call disk-published-call disk-indirect-internal-call
                     disk-request-call disk-request-branch disk-request-mixed-call disk-request-mixed-branch disk-request-indirect-call disk-request-rsb-call disk-request-internal-call disk-request-published-call disk-request-indirect-internal-call)
            add_test(NAME swift_arm64_static_latch_${mode} COMMAND swift_arm64_static_call_latch_fault_test ${mode})
            set_tests_properties(swift_arm64_static_latch_${mode} PROPERTIES
                    LABELS "unit;arm64;signal;retirement;lifetime" TIMEOUT 30
                    ENVIRONMENT "SVM_FLAGS_REGS=1;SVM_BACKEDGE_LATCH=1")
        endforeach()
        add_executable(swift_arm64_retirement_static_test arm64/retirement_static_test.cpp)
        target_link_libraries(swift_arm64_retirement_static_test PRIVATE swift_translator swift::runtime)
        foreach(mode direct dispatcher if condition condition-false region region-dispatcher published
                     region-if-taken region-if-fall region-condition-taken region-condition-fall
                    call call-pending if-pending condition-pending condition-pending-false)
            add_test(NAME swift_arm64_retirement_static_${mode} COMMAND swift_arm64_retirement_static_test ${mode})
            set_tests_properties(swift_arm64_retirement_static_${mode} PROPERTIES
                    LABELS "unit;arm64;retirement;lifetime" TIMEOUT 30
                    ENVIRONMENT "SVM_FLAGS_REGS=1;SVM_BACKEDGE_LATCH=0")
        endforeach()
        add_executable(swift_arm64_retirement_dynamic_test arm64/retirement_dynamic_test.cpp)
        target_link_libraries(swift_arm64_retirement_dynamic_test PRIVATE swift_translator swift::runtime)
        foreach(mode l1 rsb rsb-lean continuation call call-pending)
            add_test(NAME swift_arm64_retirement_dynamic_${mode} COMMAND swift_arm64_retirement_dynamic_test ${mode})
            set_tests_properties(swift_arm64_retirement_dynamic_${mode} PROPERTIES
                    LABELS "unit;arm64;retirement;lifetime" TIMEOUT 30
                    ENVIRONMENT "SVM_FLAGS_REGS=1;SVM_BACKEDGE_LATCH=0")
        endforeach()
        add_executable(swift_arm64_cache_lifetime_test arm64/cache_lifetime_test.cpp)
        target_link_libraries(swift_arm64_cache_lifetime_test PRIVATE swift::runtime)
        foreach(mode mapping cache exceptions fd-zero)
            add_test(NAME swift_arm64_cache_lifetime_${mode}
                     COMMAND swift_arm64_cache_lifetime_test ${mode})
            set_tests_properties(swift_arm64_cache_lifetime_${mode} PROPERTIES
                    LABELS "unit;arm64;cache;lifetime" TIMEOUT 30)
        endforeach()
    endif()
    add_executable(swift_narrow_carry_test narrow_adc_sbb_test.cpp)
    target_link_libraries(swift_narrow_carry_test PRIVATE swift_translator Catch2::Catch2WithMain)
    add_test(NAME swift_narrow_carry COMMAND swift_narrow_carry_test)
    add_test(NAME swift_narrow_carry_eager COMMAND swift_narrow_carry_test)
    set_tests_properties(swift_narrow_carry_eager PROPERTIES
            LABELS "unit;ir;flags" TIMEOUT 60 ENVIRONMENT "SVM_FLAGS_REGS=0")
    set_tests_properties(swift_narrow_carry PROPERTIES LABELS "unit;ir;flags" TIMEOUT 60)
endif()

# RV64 has its own execution suite; do not register ARM64 cases as RV64 tests.
if("riscv" IN_LIST ARCHITECTURE OR "riscv64" IN_LIST ARCHITECTURE)
    add_subdirectory(riscv)
    return()
endif()

# Keep focused cases reusable without compiling the integration/fuzz suite.
add_library(swift_ra_test_support STATIC support/register_alloc_test_support.cpp support/allocation_fixture.h)
target_link_libraries(swift_ra_test_support PUBLIC swift::runtime riscv64_asm)

set(SWIFT_CASE_GROUPS config runtime uniform decode integer gpr fpr fault allocation fp_environment fp_shapes pressure flags coverage code_buffer)
foreach(group IN LISTS SWIFT_CASE_GROUPS)
    add_library(swift_cases_${group} OBJECT cases/${group}.cpp)
    target_link_libraries(swift_cases_${group} PUBLIC swift_translator swift_ra_test_support
            swift::runtime Catch2::Catch2 slang_compiler)
endforeach()
add_executable(swift_cases_test)
target_link_libraries(swift_cases_test PRIVATE Catch2::Catch2WithMain swift_linux_test_support)
foreach(group IN LISTS SWIFT_CASE_GROUPS)
    target_link_libraries(swift_cases_test PRIVATE swift_cases_${group})
endforeach()
add_test(NAME swift_cases_flags COMMAND swift_cases_test "SaveCV*,CondSet*,Flag elimination keeps live pseudo masks*")
add_test(NAME swift_cases_registers COMMAND swift_cases_test "guest GPR coalescing keeps publication*,resident XMM ABI maps*,scalar FPR fixed-home tie*,Spill eviction*")
add_test(NAME swift_cases_width COMMAND swift_cases_test "unit-local width chains*,integer width ties require*,pinned W-view inputs hand off*,integer width chains keep*")
add_test(NAME swift_cases_state COMMAND swift_cases_test "Runtime preserves an interrupt*,Uniform elimination does not propagate*,Uniform helper effect sets*")
add_test(NAME swift_cases_configuration COMMAND swift_cases_test "FeatureSet*,config hash*,module feature binding parser*,SVM_FLAGS_REGS*")
add_test(NAME swift_cases_coverage COMMAND swift_cases_test "bounded byte-table*,bounded streaming*,bounded integer arithmetic*")
add_test(NAME swift_cases_instances COMMAND swift_cases_test "guest instances*,guest helper address*")
set_tests_properties(swift_cases_instances PROPERTIES LABELS "unit;review" TIMEOUT 15)
add_test(NAME swift_cases_reachability COMMAND swift_cases_test "Unreachable blocks keep unique IDs*")
add_test(NAME swift_cases_code_buffer COMMAND swift_cases_test "Vector emission grows a full code buffer*")
set_tests_properties(swift_cases_code_buffer PROPERTIES LABELS "unit;review" TIMEOUT 5)
add_test(NAME swift_cases_fcmp COMMAND swift_cases_test "bounded floating comparisons*")
add_test(NAME swift_cases_fcmp_legacy_carry COMMAND swift_cases_test "bounded floating comparisons*")
add_test(NAME swift_cases_scalar_live COMMAND swift_cases_test
        "bounded scalar arithmetic*,bounded scalar copy*,bounded scalar minmax*,AFP scalar arithmetic may reuse*,AFP scalar unary*,AFP scalar MIN MAX*,AFP guest FPCR*,AFP translated SSE arithmetic*")
set_tests_properties(swift_cases_scalar_live PROPERTIES LABELS "unit;review" TIMEOUT 5
        ENVIRONMENT "SVM_SSE_SCALAR_INSERT=1")
set_tests_properties(swift_cases_fcmp swift_cases_fcmp_legacy_carry PROPERTIES
        LABELS "unit;review" TIMEOUT 10)
set_tests_properties(swift_cases_fcmp PROPERTIES
        ENVIRONMENT "SVM_FLAGS_FCMP_FUSE=1;SVM_FLAGS_FCMP_COMPACT=1;SVM_FLAGS_CFINV=1")
set_tests_properties(swift_cases_fcmp_legacy_carry PROPERTIES
        ENVIRONMENT "SVM_FLAGS_FCMP_FUSE=1;SVM_FLAGS_FCMP_COMPACT=1;SVM_FLAGS_CFINV=0")
set_tests_properties(swift_cases_coverage swift_cases_reachability PROPERTIES LABELS "unit;review" TIMEOUT 5)
set_tests_properties(swift_cases_flags swift_cases_registers swift_cases_width swift_cases_state swift_cases_configuration
        PROPERTIES LABELS "unit;review" TIMEOUT 20)
add_library(swift_codegen_cases OBJECT
        operand_checks_test.cpp
        block_analysis_index_test.cpp
        alu_split_plan_test.cpp
        atomic_fallback_plan_test.cpp
        cycle_exit_plan_test.cpp
        coalescing_contract_test.cpp
        pinned_gpr_transfer_test.cpp
        pinned_load_update_test.cpp
        pinned_spill_publication_test.cpp
        pinned_gpr_plan_test.cpp
        fpr_plan_metrics_test.cpp
        memory_plan_test.cpp
          flags_token_plan_test.cpp
          block_static_lowering_plan_test.cpp
          narrow_integer_flags_plan_test.cpp
          consumer_pstate_plan_test.cpp
          backedge_flags_plan_test.cpp
          zero_store_verifier_test.cpp
          lowering_coverage_test.cpp
          edge_flags_analysis_test.cpp
          region_compile_metrics_test.cpp
          pseudo_flags_plan_test.cpp
          pseudo_flags_metrics_test.cpp
          token_demand_metrics_test.cpp
          pinned_memory_index_test.cpp
          scalar_publication_metrics_test.cpp
          scalar_publication_plan_test.cpp
          shuffle_plan_metrics_test.cpp
          shuffle_plan_test.cpp
          memory_plan_metrics_test.cpp
          boolean_select_metrics_test.cpp
        register_alloc_copy_test.cpp)
target_link_libraries(swift_codegen_cases PUBLIC swift_translator swift_ra_test_support swift::runtime Catch2::Catch2)
add_executable(swift_codegen_test)
target_link_libraries(swift_codegen_test PRIVATE swift_codegen_cases Catch2::Catch2WithMain)
add_test(NAME swift_codegen_test COMMAND swift_codegen_test)
set_tests_properties(swift_codegen_test PROPERTIES LABELS "unit;codegen" TIMEOUT 30)

add_executable(swift_table_test translate_table_test.cpp smc_tracker_test.cpp)
target_link_libraries(swift_table_test PRIVATE swift_translator swift::runtime Catch2::Catch2WithMain)
add_test(NAME swift_table_test COMMAND swift_table_test
        "translation tables*,SMC range invalidation redirects*,SMC owner retirement*")
set_tests_properties(swift_table_test PROPERTIES LABELS "unit;memory" TIMEOUT 5)

add_library(swift_linux_test_support OBJECT
        ../translator/linux/guest_memory.cpp
        ../translator/linux/syscalls.cpp)
target_link_libraries(swift_linux_test_support PUBLIC swift_translator swift::runtime)
add_library(swift_diagnostic_cases OBJECT linux_diagnostics_test.cpp)
target_link_libraries(swift_diagnostic_cases PUBLIC swift_translator swift::runtime Catch2::Catch2)
add_executable(swift_diagnostics_test)
target_link_libraries(swift_diagnostics_test PRIVATE swift_diagnostic_cases
        swift_linux_test_support Catch2::Catch2WithMain)
add_test(NAME swift_diagnostics_test COMMAND swift_diagnostics_test)
set_tests_properties(swift_diagnostics_test PROPERTIES LABELS "unit;diagnostics" TIMEOUT 15
        ENVIRONMENT "SVM_EXEC_TRACE=1")

add_executable(swift_test
        distorm_fast_test.cpp
        link_manager_test.cpp
        region_link_trampoline_test.cpp
        guarded_return_stack_test.cpp
        indirect_target_preservation_test.cpp
        cold_path_layout_test.cpp
        region_edges_test.cpp
        direct_link_production_test.cpp
        double_shift_test.cpp
        function_decode_frontier_test.cpp
        function_entry_contract_test.cpp
        function_region_membership_test.cpp
        helper_call_contract_test.cpp
        helper_effect_codegen_test.cpp
        scalar_bit_codegen_test.cpp
        compile_concurrency_test.cpp
        flags_merge_test.cpp
        jit_cache_direct_link_test.cpp
        integer_width_elimination_test.cpp
        branch_only_flags_test.cpp
        logical_flags_codegen_test.cpp
        narrow_flags_input_test.cpp
        narrow_memory_extension_test.cpp
        pinned_gpr_read_test.cpp
        pinned_gpr_width_fact_test.cpp
        pinned_spilled_ea_test.cpp
        register_alloc_fpr_test.cpp
        scalar_copy_analysis_test.cpp
        scalar_conversion_fpr_fusion_test.cpp
        scalar_load_fpr_fusion_test.cpp
        scalar_memory_ea_test.cpp
        spill_forwarding_test.cpp
        stack_writeback_test.cpp
        zero_store_chain_test.cpp
        smc_tracker_test.cpp
        # Decoder-level tests: no Unicorn dependency, so they build and run even
        # where the differential fuzz below is skipped.
        fuzz/vex_decoder_test.cpp
        fuzz/avx_fp_test.cpp
        fuzz/avx_fp2_test.cpp
        fuzz/avx_cmp_test.cpp
        fuzz/avx_int_test.cpp
        fuzz/avx_mul_test.cpp
        fuzz/avx_hadd_test.cpp
        fuzz/avx_blend_test.cpp
        fuzz/avx_gather_test.cpp
        fuzz/avx_misc_test.cpp
        fuzz/avx_fma_test.cpp
        fuzz/sse4_test.cpp
        fuzz/sse42str_test.cpp
        fuzz/xsave_test.cpp
        fuzz/bmi_test.cpp
        fuzz/userland_ext_test.cpp)

target_link_libraries(swift_test PUBLIC swift_translator swift::runtime Catch2::Catch2WithMain slang_compiler
        swift_codegen_cases swift_diagnostic_cases swift_linux_test_support swift_ra_test_support)
foreach(group IN LISTS SWIFT_CASE_GROUPS)
    target_link_libraries(swift_test PRIVATE swift_cases_${group})
endforeach()

find_library(UNICORN_LIBRARY NAMES unicorn)
find_path(UNICORN_INCLUDE_DIR NAMES unicorn/unicorn.h)

if (UNICORN_LIBRARY AND UNICORN_INCLUDE_DIR)
    message(STATUS "Unicorn found: ${UNICORN_LIBRARY}")
    target_sources(swift_test PRIVATE fuzz/unicorn_interface.cpp fuzz/x86_fuzz.cpp)
    target_link_libraries(swift_test PRIVATE ${UNICORN_LIBRARY})
    target_include_directories(swift_test PRIVATE ${UNICORN_INCLUDE_DIR})
else()
    message(WARNING "Unicorn not found, fuzz tests skipped")
endif()

# The AVX/XSAVE/BMI cases self-skip to a green SUCCEED() when their gate is
# unset, so a ctest run without these would report "all passed" having executed
# almost nothing -- the exact "green but covering nothing" failure mode this
# suite exists to prevent. Run the gated configuration by default; the default
# (all gates off) configuration is still reachable by running the binary
# directly.
add_test(swift_test swift_test --durations yes)
set_tests_properties(swift_test PROPERTIES
        LABELS "integration;fuzz"
        ENVIRONMENT "SVM_AVX=1;SVM_BMI=1;SVM_XSAVE=1;SVM_FSGSBASE=1;SVM_ADX=1")
add_test(NAME swift_memory_ordering COMMAND swift_test
        "x86 memory ordering belongs*,bounded ordered memory*")
set_tests_properties(swift_memory_ordering PROPERTIES
        LABELS "unit;review" TIMEOUT 5 ENVIRONMENT "SVM_AVX=1")

add_executable(swift_vm main.cpp)
target_link_libraries(swift_vm PUBLIC fronted_arm64 fronted_x86 swift::runtime slang_compiler riscv64_asm)

add_executable(shufps_test shufps_test.cpp)
target_link_libraries(shufps_test PUBLIC swift_translator swift::runtime Catch2::Catch2WithMain)
add_test(shufps_test shufps_test)
add_library(swift_support_cases OBJECT diagnostic_helpers_test.cpp)
target_link_libraries(swift_support_cases PUBLIC swift::base Catch2::Catch2)
target_link_libraries(swift_test PRIVATE swift_support_cases)
add_executable(swift_support_test)
target_link_libraries(swift_support_test PRIVATE swift_support_cases Catch2::Catch2WithMain)
add_test(NAME swift_support_test COMMAND swift_support_test)
set_tests_properties(swift_support_test PROPERTIES LABELS "unit;diagnostics" TIMEOUT 15)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME swift_wording_check COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/check_wording.py)
    set_tests_properties(swift_wording_check PROPERTIES LABELS "unit;review" TIMEOUT 20)
    add_test(NAME swift_tools_test COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tools/tests -p "test_*.py")
    set_tests_properties(swift_tools_test PROPERTIES LABELS "unit;review" TIMEOUT 15)
endif()

add_subdirectory(riscv)
