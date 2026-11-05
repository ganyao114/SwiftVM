cmake_minimum_required(VERSION 3.22)
project(SwiftVMRebuild LANGUAGES C CXX)

# Deliberately independent of the retired VM middle pipeline.
add_library(svm_ir STATIC
    source/runtime/ir/program.cpp
    source/runtime/ir/builder.cpp)
target_include_directories(svm_ir PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/source")
target_compile_features(svm_ir PUBLIC cxx_std_20)

add_library(svm_compile STATIC source/runtime/compile/optimize.cpp)
target_link_libraries(svm_compile PUBLIC svm_ir)

add_subdirectory(source/runtime/externals/SoftFloat-3e)
add_library(svm_x87 STATIC source/runtime/execution/x87.cpp)
target_include_directories(svm_x87 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/source")
target_compile_features(svm_x87 PUBLIC cxx_std_20)
target_link_libraries(svm_x87 PRIVATE svm_softfloat_ext80)

add_library(svm_arm64 STATIC
    source/runtime/backend/arm64/allocate.cpp
    source/runtime/backend/arm64/helper_calls.cpp
    source/runtime/backend/arm64/emit_state.cpp
    source/runtime/backend/arm64/emit_vector.cpp
    source/runtime/backend/arm64/compile.cpp)
target_link_libraries(svm_arm64 PUBLIC svm_ir PRIVATE svm_x87)

# This private helper closure performs only integer arithmetic. Its bridge
# preserves NZCV/GPRs; vector registers and the FP environment stay untouched.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    target_compile_options(svm_x87 PRIVATE -mgeneral-regs-only -fno-sanitize=all)
    target_compile_options(svm_softfloat_ext80 PRIVATE -mgeneral-regs-only -fno-sanitize=all)
    target_compile_definitions(svm_arm64 PRIVATE SVM_X87_GPR_ONLY=1)
endif()

# Reuse only the byte decoder. No legacy frontend or middle-pipeline target.
add_subdirectory(source/runtime/externals/distorm)
target_compile_definitions(distorm PUBLIC DISTORM_LIGHT)
add_library(svm_x86_ir STATIC
    source/runtime/frontend/x86/build_ir.cpp
    source/runtime/frontend/x86/decode.cpp)
target_link_libraries(svm_x86_ir PUBLIC svm_ir PRIVATE distorm)

# The application service consumes the same frontend/optimizer/backend API.
# ELF loading, executable-memory ownership and dispatch stay outside codegen.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
    add_library(svm_execution STATIC
        source/runtime/execution/guest_cpu.cpp
        source/runtime/execution/dispatch.cpp
        source/runtime/execution/fault.cpp
        source/runtime/execution/memory.cpp
        source/runtime/execution/native_code.cpp
        source/runtime/execution/runtime.cpp)
    target_link_libraries(svm_execution PUBLIC svm_x86_ir svm_compile svm_arm64)
    add_executable(svm_rebuild_linux source/launcher/linux/main.cpp)
    target_link_libraries(svm_rebuild_linux PRIVATE svm_execution)
endif()

add_executable(svm_ir_construction_test source/tests/rebuild/ir_construction_test.cpp)
target_link_libraries(svm_ir_construction_test PRIVATE svm_ir)
add_executable(svm_x86_construction_test source/tests/rebuild/x86_construction_test.cpp)
target_link_libraries(svm_x86_construction_test PRIVATE svm_x86_ir)
add_executable(svm_ir_optimize_test source/tests/rebuild/ir_optimize_test.cpp)
target_link_libraries(svm_ir_optimize_test PRIVATE svm_compile)
add_executable(svm_arm64_compile_test source/tests/rebuild/arm64_compile_test.cpp)
target_link_libraries(svm_arm64_compile_test PRIVATE svm_arm64)
if(TARGET svm_execution)
    target_link_libraries(svm_arm64_compile_test PRIVATE svm_execution)
endif()
add_executable(svm_compile_pipeline_test source/tests/rebuild/compile_pipeline_test.cpp)
target_link_libraries(svm_compile_pipeline_test PRIVATE svm_x86_ir svm_compile svm_arm64)

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
    target_link_libraries(svm_ir_birth_replay_bench PRIVATE svm_compile)
    add_executable(svm_ir_compile_bench benchmarks/ir_compile_bench.cpp)
    target_link_libraries(svm_ir_compile_bench PRIVATE svm_compile svm_arm64)
    add_executable(svm_function_compile_bench benchmarks/function_compile_bench.cpp)
    target_link_libraries(svm_function_compile_bench PRIVATE svm_x86_ir svm_compile svm_arm64)
endif()

enable_testing()
add_test(NAME ir_construction_contract COMMAND svm_ir_construction_test)
add_test(NAME x86_construction_contract COMMAND svm_x86_construction_test)
add_test(NAME ir_optimize_contract COMMAND svm_ir_optimize_test)
add_test(NAME arm64_compile_contract COMMAND svm_arm64_compile_test)
add_test(NAME compile_pipeline_contract COMMAND svm_compile_pipeline_test)

# A recovering UBSan report must fail the contract run instead of being hidden
# behind an otherwise successful process exit. These are inert without ASan/UBSan.
set_tests_properties(ir_construction_contract x86_construction_contract
    ir_optimize_contract arm64_compile_contract compile_pipeline_contract PROPERTIES
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
