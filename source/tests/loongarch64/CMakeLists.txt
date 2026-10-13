cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(SwiftVMLoongArchEncodingTests LANGUAGES CXX)
    enable_testing()
    add_subdirectory(../../runtime/externals/xbyak_loongarch64 xbyak_loongarch64)
endif()

add_executable(swift_loongarch_assembler_test encoding_test.cpp
    ../../runtime/backend/loongarch64/assembler.cpp)
target_include_directories(swift_loongarch_assembler_test PRIVATE ../..)
target_link_libraries(swift_loongarch_assembler_test PRIVATE swift_xbyak_loongarch64)
target_compile_features(swift_loongarch_assembler_test PRIVATE cxx_std_20)
add_test(NAME swift_loongarch_assembler COMMAND swift_loongarch_assembler_test)
set_tests_properties(swift_loongarch_assembler PROPERTIES
    LABELS "loongarch;assembler;encoding" TIMEOUT 30)
add_executable(swift_loongarch_assembler_range_test assembler_test.cpp
    ../../runtime/backend/loongarch64/assembler.cpp)
target_include_directories(swift_loongarch_assembler_range_test PRIVATE ../..)
target_link_libraries(swift_loongarch_assembler_range_test PRIVATE swift_xbyak_loongarch64)
target_compile_features(swift_loongarch_assembler_range_test PRIVATE cxx_std_20)
add_test(NAME swift_loongarch_assembler_range COMMAND swift_loongarch_assembler_range_test)
set_tests_properties(swift_loongarch_assembler_range PROPERTIES
    LABELS "loongarch;assembler;encoding" TIMEOUT 30)
add_executable(swift_loongarch_lbt_encoding_test lbt_encoding_test.cpp
    ../../runtime/backend/loongarch64/assembler.cpp)
target_include_directories(swift_loongarch_lbt_encoding_test PRIVATE ../..)
target_link_libraries(swift_loongarch_lbt_encoding_test PRIVATE swift_xbyak_loongarch64)
target_compile_features(swift_loongarch_lbt_encoding_test PRIVATE cxx_std_20)
add_test(NAME swift_loongarch_lbt_encoding COMMAND swift_loongarch_lbt_encoding_test)
set_tests_properties(swift_loongarch_lbt_encoding PROPERTIES
    LABELS "loongarch;assembler;encoding;lbt" TIMEOUT 30)

if(NOT TARGET swift_runtime)
    return()
endif()
if(NOT "loongarch64" IN_LIST ARCHITECTURE AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^loongarch")
    return()
endif()
enable_language(ASM)

add_executable(swift_loongarch_backend_test
    backend_test.cpp scalar_test.cpp flags_test.cpp memory_test.cpp
    control_test.cpp arm64_guest_test.cpp vector_test.cpp wide_test.cpp float_test.cpp
    register_plan_test.cpp scalar_allocation_plan_test.cpp scalar_allocation_execution_test.cpp
    sparse_homes_plan_test.cpp sparse_home_execution_test.cpp
    flags_plan_test.cpp flags_publication_test.cpp
    runtime_lifecycle_test.cpp runtime_step_test.cpp
    single_instruction_admission_test.cpp single_instruction_encoding_test.cpp single_instruction_code_test.cpp
    compile_contract_test.cpp benchmark.cpp check_abi.S)
# Runtime's portable semantics reference x86 helper objects, while the
# frontend itself uses Runtime. Rescan this static cycle for standalone IR
# tests, which need not reference any x86 decoder to pull those objects in.
target_link_libraries(swift_loongarch_backend_test PRIVATE
    "-Wl,--start-group" swift_translator swift_runtime fronted_arm64 fronted_x86 "-Wl,--end-group")
target_compile_features(swift_loongarch_backend_test PRIVATE cxx_std_20)

foreach(suite IN ITEMS scalar flags control memory guest vector float allocation sparse-homes flags-plan flags-publication lifecycle step single-instruction compile-contracts)
    add_test(NAME swift_loongarch_backend_${suite}
        COMMAND swift_loongarch_backend_test --${suite})
    set_tests_properties(swift_loongarch_backend_${suite} PROPERTIES
        LABELS "loongarch;backend;execution;${suite}" TIMEOUT 180)
endforeach()
add_test(NAME swift_loongarch_backend COMMAND swift_loongarch_backend_test)
set_tests_properties(swift_loongarch_backend PROPERTIES
    LABELS "loongarch;backend;execution" TIMEOUT 240)

option(SVM_LOONGARCH_UNICORN_TESTS "Compare ARM64 guests against Unicorn on the build host" OFF)
set(SVM_LOONGARCH_UNICORN_PYTHONPATH "" CACHE PATH "Host Python package directory with Unicorn and Capstone")
if(SVM_LOONGARCH_UNICORN_TESTS)
    if(NOT CMAKE_CROSSCOMPILING OR NOT SVM_LOONGARCH_QEMU_EXECUTABLE)
        message(FATAL_ERROR "LoongArch Unicorn comparison currently requires the QEMU cross-build")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME swift_loongarch_arm64_unicorn
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_arm64_unicorn.py
            --runner $<TARGET_FILE:svm_translator_linux>
            --qemu ${SVM_LOONGARCH_QEMU_EXECUTABLE}
            --sysroot ${SVM_LOONGARCH_SYSROOT}
            --runtime-libdir ${SVM_LOONGARCH_RUNTIME_LIBDIR}
            --cpu ${SVM_LOONGARCH_QEMU_CPU} --group scalar
            --output ${CMAKE_CURRENT_BINARY_DIR}/arm64-unicorn)
    set_tests_properties(swift_loongarch_arm64_unicorn PROPERTIES
        LABELS "loongarch;arm64;backend;unicorn;execution" TIMEOUT 300)
    if(SVM_LOONGARCH_UNICORN_PYTHONPATH)
        set_tests_properties(swift_loongarch_arm64_unicorn PROPERTIES
            ENVIRONMENT "PYTHONPATH=${SVM_LOONGARCH_UNICORN_PYTHONPATH}")
    endif()
endif()
