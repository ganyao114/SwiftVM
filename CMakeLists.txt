cmake_minimum_required(VERSION 3.22)
project(SwiftVMRebuild LANGUAGES C CXX ASM)

# Deliberately independent of the retired VM middle pipeline.
add_library(svm_ir STATIC
    source/runtime/ir/program.cpp
    source/runtime/ir/builder.cpp)
target_include_directories(svm_ir PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/source")
target_compile_features(svm_ir PUBLIC cxx_std_20)

# The portable IR optimizer shares the IR contract, independently of execution
# ownership and any backend. Files divide responsibilities, not extra walks.
add_library(svm_ir_optimize STATIC
    source/runtime/ir/passes/optimize.cpp
    source/runtime/ir/passes/liveness.cpp
    source/runtime/ir/passes/rewrite.cpp
    source/runtime/ir/passes/fold.cpp)
target_link_libraries(svm_ir_optimize PUBLIC svm_ir)

add_subdirectory(source/runtime/externals/SoftFloat-3e)
add_library(svm_cephes STATIC
    source/runtime/externals/cephes/src/128bit/atanll.c
    source/runtime/externals/cephes/src/128bit/constll.c
    source/runtime/externals/cephes/src/128bit/exp2ll.c
    source/runtime/externals/cephes/src/128bit/floorll.c
    source/runtime/externals/cephes/src/128bit/log2ll.c
    source/runtime/externals/cephes/src/128bit/mtherr.c
    source/runtime/externals/cephes/src/128bit/polevll.c
    source/runtime/externals/cephes/src/128bit/sinll.c
    source/runtime/externals/cephes/src/128bit/tanll.c
)
target_include_directories(svm_cephes PRIVATE source/runtime/externals/cephes/include)
target_link_libraries(svm_cephes PUBLIC svm_softfloat_ext80)
target_compile_definitions(svm_cephes PRIVATE LITTLEENDIAN=1 SOFTFLOAT_FAST_INT64=1
    SOFTFLOAT_BUILTIN_CLZ=1 FEXCORE_PRESERVE_ALL_ATTR= FEXCORE_HAS_PRESERVE_ALL_ATTR=0
    "INLINE=static inline" INLINE_LEVEL=4 SOFTFLOAT_FAST_DIV32TO16=1 SOFTFLOAT_FAST_DIV64TO32=1)

add_library(svm_x87 STATIC
    source/runtime/arch/x86/x87.cpp
    source/runtime/arch/x86/x87_convert.cpp
    source/runtime/arch/x86/x87_extended.cpp
    source/runtime/arch/x86/x87_remainder.cpp
    source/runtime/arch/x86/sse42_string.cpp
    source/runtime/arch/x86/vector_crypto.cpp
    source/runtime/arch/x86/x87_transcendental.cpp
    source/runtime/arch/x86/save_state.cpp
    source/runtime/arch/x86/cpu.cpp)
target_include_directories(svm_x87 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/source")
target_compile_features(svm_x87 PUBLIC cxx_std_20)
target_link_libraries(svm_x87 PRIVATE svm_softfloat_ext80 svm_cephes)

add_library(svm_arm64 STATIC
    source/runtime/backend/arm64/allocate.cpp
    source/runtime/backend/arm64/helper_calls.cpp
    source/runtime/backend/arm64/assembler.cpp
    source/runtime/backend/arm64/patch.cpp
    source/runtime/backend/arm64/emit.cpp
    source/runtime/backend/arm64/emit_operand.cpp
    source/runtime/backend/arm64/emit_scalar.cpp
    source/runtime/backend/arm64/emit_memory.cpp
    source/runtime/backend/arm64/emit_atomic.cpp
    source/runtime/backend/arm64/emit_control.cpp
    source/runtime/backend/arm64/emit_state.cpp
    source/runtime/backend/arm64/emit_flags.cpp
    source/runtime/backend/arm64/emit_vector.cpp
    source/runtime/backend/arm64/emit_vector_ext.cpp
    source/runtime/backend/arm64/emit_sse42.cpp
    source/runtime/backend/arm64/emit_bits.cpp
    source/runtime/backend/arm64/emit_crypto.cpp
    source/runtime/backend/arm64/vector_helper.cpp
    source/runtime/backend/arm64/emit_float.cpp
    source/runtime/backend/arm64/compile.cpp)
target_link_libraries(svm_arm64 PUBLIC svm_ir PRIVATE svm_x87)

# The controlled fork exposes exact encoding only: no MacroAssembler, scratch
# allocator, literal/veneer pool, simulator or second code buffer is linked.
add_library(svm_vixl_exact STATIC
    source/runtime/externals/vixl/compiler-intrinsics-vixl.cc
    source/runtime/externals/vixl/cpu-features.cc
    source/runtime/externals/vixl/utils-vixl.cc
    source/runtime/externals/vixl/aarch64/assembler-aarch64.cc
    source/runtime/externals/vixl/aarch64/operands-aarch64.cc
    source/runtime/externals/vixl/aarch64/instructions-aarch64.cc
    source/runtime/externals/vixl/aarch64/cpu-aarch64.cc)
target_include_directories(svm_vixl_exact PUBLIC source/runtime/externals/vixl)
target_compile_features(svm_vixl_exact PUBLIC cxx_std_20)
target_compile_definitions(svm_vixl_exact PUBLIC VIXL_INCLUDE_TARGET_AARCH64)
target_link_libraries(svm_arm64 PRIVATE svm_vixl_exact)

# This private helper closure performs only integer arithmetic. Its bridge
# preserves NZCV/GPRs; vector registers and the FP environment stay untouched.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    target_compile_options(svm_cephes PRIVATE -mgeneral-regs-only -fno-sanitize=all)
    target_compile_options(svm_x87 PRIVATE -mgeneral-regs-only -fno-sanitize=all)
    target_compile_options(svm_softfloat_ext80 PRIVATE -mgeneral-regs-only -fno-sanitize=all)
    target_compile_definitions(svm_arm64 PRIVATE SVM_X87_GPR_ONLY=1)
endif()

# Numerical IR execution is an independent consumer, without a decoder,
# Program construction or x86 register-layout dependency.
add_library(svm_ir_direct STATIC source/runtime/ir/direct/interpreter.cpp)
target_link_libraries(svm_ir_direct PUBLIC svm_ir PRIVATE svm_x87)

# Reuse only the byte decoder. No legacy frontend or middle-pipeline target.
add_subdirectory(source/runtime/externals/distorm)
target_compile_definitions(distorm PUBLIC DISTORM_LIGHT)
add_library(svm_x86_ir STATIC
    source/runtime/frontend/x86/build_ir.cpp
    source/runtime/frontend/x86/detail/vex_decode.cpp
    source/runtime/frontend/x86/detail/extended_decode.cpp
    source/runtime/frontend/x86/decode.cpp
    source/runtime/frontend/x86/region.cpp
    source/runtime/frontend/x86/interpreter.cpp
    source/runtime/platform/system_query.cpp)
target_link_libraries(svm_x86_ir PUBLIC svm_ir PRIVATE distorm svm_x87 svm_ir_direct)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(source/runtime/ir/direct/interpreter.cpp
        PROPERTIES COMPILE_OPTIONS "-frounding-math")
endif()

# The application service consumes the same frontend/optimizer/backend API.
# ELF loading, executable-memory ownership and dispatch stay outside codegen.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
    add_library(svm_execution STATIC
        source/runtime/execution/dispatch.cpp
        source/runtime/execution/fault.cpp
        source/runtime/execution/memory.cpp
        source/runtime/execution/mem_map.cpp
        source/runtime/execution/code_heap.cpp
        source/runtime/execution/code_cache.cpp
        source/runtime/execution/code_link.cpp
        source/runtime/execution/compile_service.cpp
        source/runtime/execution/state_service.cpp
        source/runtime/execution/smc.cpp
        source/runtime/execution/runtime.cpp
        source/runtime/platform/linux/loader.cpp
        source/runtime/platform/linux/syscalls.cpp
        source/runtime/platform/linux/safe_access.cpp
        source/runtime/platform/linux/signals.cpp
        source/runtime/platform/linux/arm64_runtime.cpp
        source/runtime/platform/linux/arm64_entry.S)
    target_link_libraries(svm_execution PUBLIC svm_x86_ir svm_ir_optimize svm_arm64)
    add_executable(svm_rebuild_linux source/launcher/linux/main.cpp)
    target_link_libraries(svm_rebuild_linux PRIVATE svm_execution)
    add_executable(svm_runtime_engine_test source/tests/rebuild/runtime_engine_test.cpp)
    target_link_libraries(svm_runtime_engine_test PRIVATE svm_execution)
endif()

add_executable(svm_ir_construction_test source/tests/rebuild/ir_construction_test.cpp)
target_link_libraries(svm_ir_construction_test PRIVATE svm_ir)
add_executable(svm_x86_construction_test source/tests/rebuild/x86_construction_test.cpp)
target_link_libraries(svm_x86_construction_test PRIVATE svm_x86_ir distorm)
add_executable(svm_ir_optimize_test source/tests/rebuild/ir_optimize_test.cpp)
target_link_libraries(svm_ir_optimize_test PRIVATE svm_ir_optimize)
add_executable(svm_arm64_compile_test source/tests/rebuild/arm64_compile_test.cpp)
target_link_libraries(svm_arm64_compile_test PRIVATE svm_arm64)
if(TARGET svm_execution)
    target_link_libraries(svm_arm64_compile_test PRIVATE svm_execution)
endif()
add_executable(svm_compile_pipeline_test source/tests/rebuild/compile_pipeline_test.cpp)
target_link_libraries(svm_compile_pipeline_test PRIVATE svm_x86_ir svm_ir_optimize svm_arm64 distorm)
add_executable(svm_ir_direct_test source/tests/rebuild/ir_direct_test.cpp)
target_link_libraries(svm_ir_direct_test PRIVATE svm_ir_direct)
add_executable(svm_x86_interpreter_test source/tests/rebuild/x86_interpreter_test.cpp)
target_link_libraries(svm_x86_interpreter_test PRIVATE svm_x86_ir)

option(SVM_BUILD_IR_BENCHMARK "Build the IR construction/submission benchmark" ON)
if(SVM_BUILD_IR_BENCHMARK)
    add_executable(svm_ir_build_submit_bench benchmarks/ir_build_submit_bench.cpp)
    target_link_libraries(svm_ir_build_submit_bench PRIVATE svm_ir)
    add_executable(svm_ir_static_build_submit_bench benchmarks/ir_build_submit_bench.cpp)
    target_compile_definitions(svm_ir_static_build_submit_bench PRIVATE SVM_IR_STATIC_WRITER=1)
    target_link_libraries(svm_ir_static_build_submit_bench PRIVATE svm_ir)
    add_executable(svm_x86_build_submit_bench benchmarks/x86_build_submit_bench.cpp)
    target_link_libraries(svm_x86_build_submit_bench PRIVATE svm_x86_ir)
    add_executable(svm_ir_birth_replay_bench benchmarks/ir_birth_replay_bench.cpp)
    target_link_libraries(svm_ir_birth_replay_bench PRIVATE svm_ir_optimize)
    add_executable(svm_ir_compile_bench benchmarks/ir_compile_bench.cpp)
    target_link_libraries(svm_ir_compile_bench PRIVATE svm_ir_optimize svm_arm64)
    add_executable(svm_function_compile_bench benchmarks/function_compile_bench.cpp)
    target_link_libraries(svm_function_compile_bench PRIVATE svm_x86_ir svm_ir_optimize svm_arm64)
endif()

foreach(svm_coverage IN ITEMS instruction avx x87)
    add_executable(svm_${svm_coverage}_coverage_test source/tests/rebuild/${svm_coverage}_coverage_test.cpp)
    target_link_libraries(svm_${svm_coverage}_coverage_test PRIVATE svm_x86_ir svm_ir_optimize svm_arm64)
    if(TARGET svm_execution)
        target_link_libraries(svm_${svm_coverage}_coverage_test PRIVATE svm_execution)
    endif()
endforeach()

enable_testing()
foreach(svm_coverage IN ITEMS instruction avx x87)
    add_test(NAME ${svm_coverage}_coverage COMMAND svm_${svm_coverage}_coverage_test)
    set_tests_properties(${svm_coverage}_coverage PROPERTIES ENVIRONMENT
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
endforeach()
add_test(NAME ir_construction_contract COMMAND svm_ir_construction_test)
add_test(NAME x86_construction_contract COMMAND svm_x86_construction_test)
add_test(NAME ir_optimize_contract COMMAND svm_ir_optimize_test)
add_test(NAME arm64_compile_contract COMMAND svm_arm64_compile_test)
add_test(NAME compile_pipeline_contract COMMAND svm_compile_pipeline_test)
add_test(NAME x86_interpreter_contract COMMAND svm_x86_interpreter_test)
add_test(NAME ir_direct_contract COMMAND svm_ir_direct_test)
if(TARGET svm_runtime_engine_test)
    add_test(NAME runtime_engine_contract COMMAND svm_runtime_engine_test)
endif()

# A recovering UBSan report must fail the contract run instead of being hidden
# behind an otherwise successful process exit. These are inert without ASan/UBSan.
set_tests_properties(ir_construction_contract x86_construction_contract
    ir_optimize_contract arm64_compile_contract compile_pipeline_contract x86_interpreter_contract
    ir_direct_contract PROPERTIES
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
if(TARGET svm_runtime_engine_test)
    set_tests_properties(runtime_engine_contract PROPERTIES
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
endif()

# Keep source ownership boundaries without imposing them on the JIT's host
# call graph. The ext80 helper closure remains separately compiled with its
# general-register-only ABI; IPO never imports FP code into that closure.
option(SVM_ENABLE_IPO "Optimize Release JIT calls across translation units" ON)
if(SVM_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT svm_ipo_supported OUTPUT svm_ipo_error LANGUAGES C CXX)
    if(svm_ipo_supported)
        get_property(svm_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(svm_target IN LISTS svm_targets)
            if(NOT svm_target STREQUAL "svm_x87" AND NOT svm_target STREQUAL "svm_cephes")
                set_property(TARGET ${svm_target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
                set_property(TARGET ${svm_target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
            endif()
        endforeach()
    else()
        message(STATUS "SVM Release IPO unavailable: ${svm_ipo_error}")
    endif()
endif()
