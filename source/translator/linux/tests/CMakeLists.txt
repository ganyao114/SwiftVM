add_executable(test main.c)

# ---------------------------------------------------------------------------
# End-to-end AVX qualification: run a real -mavx2 -O2 program under
# svm_translator_linux and compare it, bit for bit, against golden output
# recorded from an actual x86-64 execution of the same kernels.
#
# The guests are static Linux x86-64 ELFs checked in next to this file, exactly
# like the other real_* guests here; they are not compiled by this CMakeLists.
# Regenerate them and their goldens with build_avx_real_tests.sh, which also
# documents the oracle's known defects.  run_avx_real_tests.sh is the actual
# test driver and is runnable on its own:
#
#     bash source/translator/linux/tests/run_avx_real_tests.sh \
#          <build>/source/translator/linux/svm_translator_linux
#
# NOTE: this directory is not currently reached by the build (nothing does
# add_subdirectory(tests), and the project never calls enable_testing(), so
# `ctest -N` reports zero tests today).  The registration below is written so
# that it becomes live the moment either changes, and the shell script above is
# the entrypoint until then -- the same arrangement build_real_tests.sh uses.
#
# The driver reports unimplemented instructions as EXPECTED-GAP rather than
# failing, so the suite stays green while the gap list shrinks; a WRONG answer
# or an undocumented new gap fails hard.
# ---------------------------------------------------------------------------
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/avx_real_x86_64 AND
    EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/avx_crosspage_x86_64)
    add_test(NAME avx_real_program
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_avx_real_tests.sh
                     $<TARGET_FILE:svm_translator_linux>)
    set_tests_properties(avx_real_program PROPERTIES
                         LABELS "avx;guest;e2e" TIMEOUT 600)
endif ()
