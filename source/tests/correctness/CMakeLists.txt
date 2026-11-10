add_library(svm_correctness_core STATIC
    case_io.cpp
    catalog.cpp
    generate.cpp
    compare.cpp
    reduce.cpp
    arch/state.cpp
    execute.cpp
    compile.cpp)
target_link_libraries(svm_correctness_core PUBLIC svm_base
    PRIVATE svm_x86_ir svm_arm64_ir svm_arm32_ir svm_ir_optimize
        svm_arm64 svm_x64 svm_riscv64 svm_loongarch64)

add_executable(svm_correctness main.cpp execute_vm.cpp)
target_link_libraries(svm_correctness PRIVATE svm_correctness_core)
set_target_properties(svm_correctness PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(TARGET svm_execution)
    # A single source list and implementation, with test-only cold stops. The
    # production archive is never linked into this executable alongside it.
    get_target_property(svm_correctness_runtime_sources svm_execution SOURCES)
    list(TRANSFORM svm_correctness_runtime_sources PREPEND "${PROJECT_SOURCE_DIR}/")
    add_library(svm_correctness_execution STATIC ${svm_correctness_runtime_sources})
    get_target_property(svm_correctness_runtime_links svm_execution LINK_LIBRARIES)
    target_link_libraries(svm_correctness_execution PUBLIC ${svm_correctness_runtime_links})
    target_compile_definitions(svm_correctness_execution PUBLIC SVM_TEST_BOUNDARY=1)
    target_link_libraries(svm_correctness PRIVATE svm_correctness_execution)
endif()

if(BUILD_TESTING)
    add_test(NAME correctness_protocol COMMAND svm_correctness selftest)
    set_tests_properties(correctness_protocol PROPERTIES LABELS "correctness;portable" TIMEOUT 30)
endif()
