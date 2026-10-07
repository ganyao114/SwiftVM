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

if(TARGET swift_runtime)
    add_executable(swift_riscv_backend_test backend_test.cpp native_scalar_test.cpp native_flags_test.cpp native_atomic_test.cpp native_vector_test.cpp native_vector_shuffle_test.cpp native_local_test.cpp native_vector_float_test.cpp native_call_test.cpp native_memory_test.cpp native_crypto_test.cpp native_structure_test.cpp native_wide_test.cpp memory_protocol_test.cpp persistence_test.cpp)
    target_link_libraries(swift_riscv_backend_test PRIVATE swift_translator)
    if("riscv" IN_LIST ARCHITECTURE OR "riscv64" IN_LIST ARCHITECTURE)
        target_sources(swift_riscv_backend_test PRIVATE check_abi.S)
        add_test(NAME swift_riscv_backend COMMAND swift_riscv_backend_test)
        add_test(NAME swift_riscv_backend_blocks COMMAND swift_riscv_backend_test)
        add_test(NAME swift_riscv_backend_cache_reuse COMMAND swift_riscv_backend_test --cache-reuse)
        add_test(NAME swift_riscv_backend_persistence COMMAND swift_riscv_backend_test --persistence)
        set_tests_properties(swift_riscv_backend swift_riscv_backend_blocks swift_riscv_backend_cache_reuse swift_riscv_backend_persistence PROPERTIES
            LABELS "riscv;backend;execution" TIMEOUT 180)
        set_tests_properties(swift_riscv_backend_blocks PROPERTIES ENVIRONMENT "SVM_FUNC_BASE=0")
        if(CMAKE_CROSSCOMPILING AND SVM_RISCV_QEMU_EXECUTABLE)
            add_test(NAME swift_riscv_backend_zbb
                    COMMAND ${SVM_RISCV_QEMU_EXECUTABLE} -L ${SVM_RISCV_SYSROOT}
                    -cpu rv64,v=false,zba=false,zbb=true,zbs=false,zbc=false
                    $<TARGET_FILE:swift_riscv_backend_test> --zbb)
            add_test(NAME swift_riscv_backend_scalar_crypto
                    COMMAND ${SVM_RISCV_QEMU_EXECUTABLE} -L ${SVM_RISCV_SYSROOT}
                    -cpu rv64,v=false,zba=false,zbb=true,zbs=false,zbc=true,zkne=true,zknd=true,zknh=true
                    $<TARGET_FILE:swift_riscv_backend_test> --scalar-crypto)
            set_tests_properties(swift_riscv_backend_scalar_crypto PROPERTIES
                    LABELS "riscv;backend;execution;crypto" TIMEOUT 180)
            foreach(vlen IN ITEMS 128 256)
                add_test(NAME swift_riscv_backend_vector_${vlen}
                        COMMAND ${SVM_RISCV_QEMU_EXECUTABLE} -L ${SVM_RISCV_SYSROOT}
                        -cpu rv64,v=true,vlen=${vlen},elen=64,zba=false,zbb=false,zbs=false,zbc=false
                        $<TARGET_FILE:swift_riscv_backend_test> --rvv)
                add_test(NAME swift_riscv_backend_persistence_vector_${vlen}
                        COMMAND ${SVM_RISCV_QEMU_EXECUTABLE} -L ${SVM_RISCV_SYSROOT}
                        -cpu rv64,v=true,vlen=${vlen},elen=64,zba=false,zbb=false,zbs=false,zbc=false
                        $<TARGET_FILE:swift_riscv_backend_test> --persistence)
                set_tests_properties(swift_riscv_backend_vector_${vlen} PROPERTIES
                        LABELS "riscv;backend;execution;vector" TIMEOUT 180)
                set_tests_properties(swift_riscv_backend_persistence_vector_${vlen} PROPERTIES
                        LABELS "riscv;backend;execution;persistence;vector" TIMEOUT 180)
            endforeach()
            set_tests_properties(swift_riscv_backend_zbb PROPERTIES
                    LABELS "riscv;backend;execution;extensions" TIMEOUT 180)
        endif()
        set(SVM_RISCV_QEMU_CRYPTO_EXECUTABLE "" CACHE FILEPATH "QEMU with Zvkned/Zvknha/Zvbc and Zacas for extension qualification")
        if(CMAKE_CROSSCOMPILING AND SVM_RISCV_QEMU_CRYPTO_EXECUTABLE)
            foreach(vlen IN ITEMS 128 256)
                add_test(NAME swift_riscv_backend_vector_crypto_${vlen}
                        COMMAND ${SVM_RISCV_QEMU_CRYPTO_EXECUTABLE} -L ${SVM_RISCV_SYSROOT}
                        -cpu rv64,v=true,vlen=${vlen},elen=64,zvkned=true,zvknha=true,zvbc=true
                        $<TARGET_FILE:swift_riscv_backend_test> --vector-crypto)
                set_tests_properties(swift_riscv_backend_vector_crypto_${vlen} PROPERTIES
                        LABELS "riscv;backend;execution;crypto" TIMEOUT 180)
            endforeach()
            add_test(NAME swift_riscv_backend_zacas
                    COMMAND ${SVM_RISCV_QEMU_CRYPTO_EXECUTABLE} -L ${SVM_RISCV_SYSROOT}
                    -cpu rv64,v=false,zacas=true $<TARGET_FILE:swift_riscv_backend_test> --zacas)
            set_tests_properties(swift_riscv_backend_zacas PROPERTIES
                    LABELS "riscv;backend;execution;atomics" TIMEOUT 180)
        endif()
    endif()
endif()

option(SVM_RISCV_QEMU_TESTS "Enable RV64 execution tests (requires Python, Clang, LLD and qemu-riscv64)" OFF)
if(SVM_RISCV_QEMU_TESTS)
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "swift_riscv_smoke needs a native emitter; build source/tests/riscv separately or transfer a native --bundle")
    endif()
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
