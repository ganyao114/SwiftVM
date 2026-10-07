cmake_minimum_required(VERSION 3.21)

# This directory also builds on its own, without the SwiftVM runtime or its
# host-specific dependencies. Only the native emitter links against Biscuit.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(SwiftVMRiscvTests LANGUAGES CXX)
    enable_testing()
    set(BUILD_TESTING OFF)
    add_subdirectory(../../runtime/externals/biscuit biscuit)
    unset(BUILD_TESTING)
endif()

add_executable(swift_riscv_emit emit.cpp)
target_link_libraries(swift_riscv_emit PRIVATE biscuit::biscuit)
target_compile_features(swift_riscv_emit PRIVATE cxx_std_20)

option(SVM_RISCV_QEMU_TESTS "Enable RV64 execution tests (requires Python, Clang, LLD and qemu-riscv64)" OFF)
if(SVM_RISCV_QEMU_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(SVM_RISCV_CLANG NAMES clang REQUIRED)
    find_program(SVM_RISCV_QEMU NAMES qemu-riscv64 REQUIRED)
    add_test(NAME swift_riscv_smoke
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/run_riscv_smoke.py
            --emitter $<TARGET_FILE:swift_riscv_emit>
            --clang ${SVM_RISCV_CLANG} --qemu ${SVM_RISCV_QEMU}
            --artifact-root ${CMAKE_CURRENT_BINARY_DIR}/artifacts)
    set_tests_properties(swift_riscv_smoke PROPERTIES LABELS "riscv;execution" TIMEOUT 120)
else()
    message(STATUS "RV64 execution tests disabled; enable SVM_RISCV_QEMU_TESTS to register swift_riscv_smoke")
endif()
