add_executable(arm64_frontend_semantics_test arm64_frontend_semantics_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE ir_value_size_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_atomic_extrema_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_indexed_scalar_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_indexed_fp_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_crypto_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_crc_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_sha_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_scalar_misc_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_scalar_shift_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_vector_sat_shift_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_scalar_multiply_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_scalar_mulhigh_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_scalar_sign_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_dot_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_saturating_multiply_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_vector_doubling_long_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_scalar_pairwise_fp_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_fp16_unary_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_vector_fp_unary_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_fp_compare_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_fp_integer_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_fp_fixed_simd_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_frint_range_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_polynomial_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_mixed_sat_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_pairwise_widen_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_high_narrow_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_absolute_long_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_absolute_narrow_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_halving_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_integer_reserved_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_scalar_variable_shift_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_fp_precision_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_fp_estimate_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_fp_reduction_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_fp16_binary_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_vector_fp_binary_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_scalar_fp16_misc_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_vector_fma_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_fmulx_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_reciprocal_step_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_casp_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_dataproc_decode_regression_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_memory_commit_test.cpp)
target_sources(arm64_frontend_semantics_test PRIVATE arm64_base_memory_extensions_test.cpp)
target_link_libraries(arm64_frontend_semantics_test PRIVATE
        swift_translator fronted_arm64 swift::runtime Catch2::Catch2WithMain)
add_test(NAME arm64_frontend_semantics_test COMMAND arm64_frontend_semantics_test)
set_tests_properties(arm64_frontend_semantics_test PROPERTIES LABELS "unit;arm64;frontend" TIMEOUT 120)

add_executable(arm64_single_instruction_test arm64_single_instruction_test.cpp)
target_link_libraries(arm64_single_instruction_test PRIVATE
        swift_translator fronted_arm64 swift::runtime Catch2::Catch2WithMain)
add_test(NAME arm64_single_instruction_test COMMAND arm64_single_instruction_test)
set_tests_properties(arm64_single_instruction_test PROPERTIES
        LABELS "unit;arm64;frontend;single-instruction" TIMEOUT 60)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(swift_guest_code_fetch_test guest_code_fetch_test.cpp
            ../translator/linux/guest_memory.cpp)
    target_link_libraries(swift_guest_code_fetch_test PRIVATE swift_translator swift::runtime Catch2::Catch2WithMain)
    add_test(NAME swift_guest_code_fetch_test COMMAND swift_guest_code_fetch_test)
    set_tests_properties(swift_guest_code_fetch_test PROPERTIES
            LABELS "unit;memory;code-fetch;lease" TIMEOUT 45)

    add_executable(swift_interp_atomic_range_test interp_atomic_range_test.cpp)
    target_link_libraries(swift_interp_atomic_range_test PRIVATE swift_translator swift::runtime Catch2::Catch2WithMain)
    add_test(NAME swift_interp_atomic_range_test COMMAND swift_interp_atomic_range_test)
    set_tests_properties(swift_interp_atomic_range_test PROPERTIES LABELS "unit;memory;atomic;interpreter" TIMEOUT 30)
endif()

find_package(Threads REQUIRED)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND "x86_64" IN_LIST ARCHITECTURE)
    add_executable(swift_runtime_interpreter_single_instruction_test runtime_interpreter_single_instruction_test.cpp)
    target_link_libraries(swift_runtime_interpreter_single_instruction_test PRIVATE
            swift_translator fronted_arm64 swift::runtime Catch2::Catch2WithMain Threads::Threads)
    add_test(NAME swift_runtime_interpreter_single_instruction_test COMMAND swift_runtime_interpreter_single_instruction_test)
    set_tests_properties(swift_runtime_interpreter_single_instruction_test PROPERTIES
            LABELS "unit;runtime;interpreter;single-instruction;gate-e1" TIMEOUT 120 ENVIRONMENT "SVM_ENABLE_JIT=0;SVM_FUNC_BASE=0")
    add_executable(swift_runtime_execution_owner_test runtime_execution_owner_test.cpp)
    target_link_libraries(swift_runtime_execution_owner_test PRIVATE
            swift_translator swift::runtime Catch2::Catch2WithMain Threads::Threads)
    add_test(NAME swift_runtime_execution_owner_test COMMAND swift_runtime_execution_owner_test)
    set_tests_properties(swift_runtime_execution_owner_test PROPERTIES
            LABELS "unit;runtime;ownership;step" TIMEOUT 60)
endif()
if(TARGET swift_guest_code_fetch_test)
    target_link_libraries(swift_guest_code_fetch_test PRIVATE Threads::Threads)
endif()
add_executable(swift_exclusive_monitor_test exclusive_monitor_test.cpp
        ../runtime/common/exclusive_monitor.cpp)
target_compile_definitions(swift_exclusive_monitor_test PRIVATE SWIFT_EXCLUSIVE_MONITOR_STANDALONE)
target_link_libraries(swift_exclusive_monitor_test PRIVATE Threads::Threads)
add_test(NAME swift_exclusive_monitor_test COMMAND swift_exclusive_monitor_test)
set_tests_properties(swift_exclusive_monitor_test PROPERTIES LABELS "unit;exclusive;registry" TIMEOUT 30)

add_executable(swift_memory_lease_test memory_lease_test.cpp
        ../runtime/common/memory_lease.cpp)
target_include_directories(swift_memory_lease_test PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_link_libraries(swift_memory_lease_test PRIVATE Threads::Threads)
add_test(NAME swift_memory_lease_test COMMAND swift_memory_lease_test)
set_tests_properties(swift_memory_lease_test PROPERTIES LABELS "unit;memory;lease" TIMEOUT 45)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME swift_tools_test COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tools/tests -p "test_*.py")
    set_tests_properties(swift_tools_test PROPERTIES LABELS "unit;review" TIMEOUT 120)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND "x86_64" IN_LIST ARCHITECTURE)
    target_include_directories(arm64_frontend_semantics_test PRIVATE ${XBYAK_INCLUDE_DIR})
    add_subdirectory(x64)
    # This case only allocates and emits ARM64 bytes; it never executes them.
    add_executable(swift_arm64_satmul_codegen_test register_alloc_copy_test.cpp)
    target_link_libraries(swift_arm64_satmul_codegen_test PRIVATE
            swift_translator swift::runtime Catch2::Catch2WithMain)
    add_test(NAME swift_arm64_satmul_codegen_test COMMAND swift_arm64_satmul_codegen_test
            "saturating multiply flat-block QC is reserved at its producer")
    set_tests_properties(swift_arm64_satmul_codegen_test PROPERTIES
            LABELS "unit;arm64;codegen" TIMEOUT 30)
    add_test(NAME swift_arm64_satshift_codegen_test COMMAND swift_arm64_satmul_codegen_test
            "saturating shift flat-block QC is reserved at its producer,saturating shift spilled input and result retain independent FPR leases")
    set_tests_properties(swift_arm64_satshift_codegen_test PROPERTIES
            LABELS "unit;arm64;codegen" TIMEOUT 30)
    add_test(NAME swift_arm64_unsigned_estimate_codegen_test COMMAND swift_arm64_satmul_codegen_test
            "unsigned estimates emit native ARM64 under aliases and FPR pressure,unsigned estimates reject noncanonical ARM64 IR before emission")
    set_tests_properties(swift_arm64_unsigned_estimate_codegen_test PROPERTIES
            LABELS "unit;arm64;codegen" TIMEOUT 30)
    add_test(NAME swift_arm64_sataddsub_codegen_test COMMAND swift_arm64_satmul_codegen_test
            "saturating add subtract emit ARM64 dword qword aliases and spills,saturating add subtract reject malformed ARM64 whole blocks")
    set_tests_properties(swift_arm64_sataddsub_codegen_test PROPERTIES
            LABELS "unit;arm64;codegen" TIMEOUT 30)
    add_executable(swift_arm64_crc_codegen_test scalar_bit_codegen_test.cpp)
    target_link_libraries(swift_arm64_crc_codegen_test PRIVATE
            swift_translator swift::runtime Catch2::Catch2WithMain)
    add_test(NAME swift_arm64_crc_codegen_test COMMAND swift_arm64_crc_codegen_test
            "IEEE CRC scalar views emit hardware snapshots and bounded software scratch")
    set_tests_properties(swift_arm64_crc_codegen_test PROPERTIES
            LABELS "unit;arm64;codegen" TIMEOUT 30)
    return()
endif()

# Existing cases below execute ARM64 machine code and use its register ABI.
# RV64 has its own execution suite; do not register ARM64 cases as RV64 tests.
if("riscv" IN_LIST ARCHITECTURE OR "riscv64" IN_LIST ARCHITECTURE)
    add_subdirectory(riscv)
    return()
endif()

# Keep focused cases reusable without compiling the integration/fuzz suite.
add_library(swift_ra_test_support STATIC support/register_alloc_test_support.cpp)
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
        coalescing_contract_test.cpp
        pinned_gpr_transfer_test.cpp
        pinned_load_update_test.cpp
        pinned_spill_publication_test.cpp
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
        funnel_shift_codegen_test.cpp
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

if(Python3_Interpreter_FOUND)
    add_test(NAME swift_wording_check COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/check_wording.py)
    set_tests_properties(swift_wording_check PROPERTIES LABELS "unit;review" TIMEOUT 20)
endif()

add_subdirectory(riscv)
add_subdirectory(loongarch64)
