# Catch is a test-only dependency. Keep the frozen upstream version and its
# licence; production libraries/launchers do not link this target.
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CATCH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(externals/catch EXCLUDE_FROM_ALL)
# Catch's compiled stringification support must use the same language feature
# set as its C++20 consumers (not a C++14 library with C++17-only declarations).
target_compile_features(Catch2 PUBLIC cxx_std_20)

add_library(svm_test_support STATIC
    support/test_mode.cpp
    support/native_code.cpp)
target_link_libraries(svm_test_support PUBLIC Catch2::Catch2 svm_base)
target_include_directories(svm_test_support PUBLIC "${PROJECT_SOURCE_DIR}/source")
target_compile_features(svm_test_support PUBLIC cxx_std_20)

function(svm_test_target target)
    cmake_parse_arguments(CASE "" "ISA" "SOURCES;LIBRARIES" ${ARGN})
    set(test_isa None)
    if(CASE_ISA STREQUAL "arm64")
        set(test_isa Arm64)
    elseif(CASE_ISA STREQUAL "arm64_android")
        set(test_isa Arm64Android)
    elseif(CASE_ISA STREQUAL "riscv64")
        set(test_isa Riscv64)
    elseif(CASE_ISA STREQUAL "host")
        set(test_isa Host)
    endif()
    add_executable(${target} support/main.cpp ${CASE_SOURCES})
    target_link_libraries(${target} PRIVATE svm_test_support ${CASE_LIBRARIES})
    target_compile_definitions(${target} PRIVATE SVM_TEST_NATIVE_ISA=${test_isa})
    # Existing device scripts address the historical target paths directly.
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

function(svm_test name target)
    add_test(NAME ${name} COMMAND ${target} --compile-only "[compile]")
    set_tests_properties(${name} PROPERTIES LABELS "unit;portable;compile" TIMEOUT 90
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
endfunction()

function(svm_native_test name target)
    add_test(NAME ${name} COMMAND ${target} --native "[native]")
    set_tests_properties(${name} PROPERTIES LABELS "unit;native" TIMEOUT 90 SKIP_RETURN_CODE 77
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
endfunction()

svm_test_target(svm_ir_construction_test SOURCES rebuild/ir_construction_test.cpp LIBRARIES svm_ir)
svm_test_target(svm_x86_construction_test SOURCES rebuild/x86_construction_test.cpp LIBRARIES svm_x86_ir distorm)
svm_test_target(svm_ir_optimize_test SOURCES rebuild/ir_optimize_test.cpp LIBRARIES svm_ir_optimize)
set(arm64_test_libraries svm_arm64)
if(TARGET svm_execution)
    list(APPEND arm64_test_libraries svm_execution)
endif()
svm_test_target(svm_arm64_compile_test ISA arm64_android SOURCES
    rebuild/arm64_compile_test.cpp
    rebuild/arm64_vector_test.cpp
    rebuild/arm64_link_test.cpp
    rebuild/arm64_fault_test.cpp
    support/arm64_fixture.cpp
    LIBRARIES ${arm64_test_libraries})
svm_test_target(svm_compile_pipeline_test ISA arm64 SOURCES rebuild/compile_pipeline_test.cpp
    LIBRARIES svm_x86_ir svm_ir_optimize svm_arm64 distorm)
svm_test_target(svm_ir_direct_test SOURCES rebuild/ir_direct_test.cpp LIBRARIES svm_ir_direct)
svm_test_target(svm_riscv64_compile_test ISA riscv64 SOURCES rebuild/riscv64_compile_test.cpp
    LIBRARIES svm_riscv64)
svm_test_target(svm_x86_interpreter_test SOURCES rebuild/x86_interpreter_test.cpp LIBRARIES svm_x86_ir)

foreach(coverage IN ITEMS instruction avx x87)
    svm_test_target(svm_${coverage}_coverage_test ISA arm64 SOURCES rebuild/${coverage}_coverage_test.cpp
        LIBRARIES svm_x86_ir svm_ir_optimize ${arm64_test_libraries})
    svm_test(${coverage}_coverage svm_${coverage}_coverage_test)
    svm_native_test(${coverage}_coverage_native svm_${coverage}_coverage_test)
endforeach()

svm_test(ir_construction_contract svm_ir_construction_test)
svm_test(x86_construction_contract svm_x86_construction_test)
svm_test(ir_optimize_contract svm_ir_optimize_test)
svm_test(arm64_compile_contract svm_arm64_compile_test)
svm_test(compile_pipeline_contract svm_compile_pipeline_test)
svm_test(x86_interpreter_contract svm_x86_interpreter_test)
svm_test(ir_direct_contract svm_ir_direct_test)
svm_test(riscv64_compile_contract svm_riscv64_compile_test)
svm_native_test(arm64_native_contract svm_arm64_compile_test)
svm_native_test(pipeline_native_contract svm_compile_pipeline_test)
svm_native_test(riscv64_native_contract svm_riscv64_compile_test)
if(TARGET svm_execution)
    svm_test_target(svm_runtime_engine_test ISA host SOURCES rebuild/runtime_engine_test.cpp LIBRARIES svm_execution)
    svm_test(runtime_engine_contract svm_runtime_engine_test)
    svm_native_test(runtime_engine_native_contract svm_runtime_engine_test)
endif()
if(TARGET svm_ir_format)
    svm_test_target(svm_ir_format_test SOURCES ir/format_test.cpp LIBRARIES svm_ir_format)
    svm_test(ir_format_contract svm_ir_format_test)
endif()

svm_test_target(svm_base_test SOURCES base_test.cpp LIBRARIES svm_base)
svm_test(base_contract svm_base_test)
