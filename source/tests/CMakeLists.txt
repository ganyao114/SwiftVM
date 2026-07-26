add_executable(swift_test
        main_case.cpp
        # Decoder-level tests: no Unicorn dependency, so they build and run even
        # where the differential fuzz below is skipped.
        fuzz/vex_decoder_test.cpp
        fuzz/avx_fp_test.cpp
        fuzz/avx_fp2_test.cpp
        fuzz/avx_int_test.cpp
        fuzz/avx_mul_test.cpp
        fuzz/avx_hadd_test.cpp
        fuzz/avx_blend_test.cpp
        fuzz/avx_gather_test.cpp
        fuzz/xsave_test.cpp
        fuzz/bmi_test.cpp)

target_link_libraries(swift_test PUBLIC swift_translator swift::runtime Catch2::Catch2WithMain slang_compiler)

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
        ENVIRONMENT "SVM_AVX=1;SVM_BMI=1;SVM_XSAVE=1")

add_executable(swift_vm main.cpp)
target_link_libraries(swift_vm PUBLIC fronted_arm64 fronted_x86 swift::runtime slang_compiler)