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

# ---------------------------------------------------------------------------
# Isolation: a malformed guest instruction stream must kill only the guest.
#
# Unlike the suites above this one asserts nothing about results -- it asserts
# that the HOST process survives.  The guests are generated on the fly by
# gen_malformed_guest_x86_64.py (no checked-in binaries), so the corpus is
# cheap to extend when a new host-kill route turns up.
#
# Same caveat as the note above: nothing does add_subdirectory(tests) and the
# project never calls enable_testing(), so this registration is dormant until
# either changes; run_malformed_guest_tests.sh is runnable on its own until
# then, and takes --audit for the exhaustive opcode-space sweep.
# ---------------------------------------------------------------------------
add_test(NAME malformed_guest_isolation
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_malformed_guest_tests.sh
                 $<TARGET_FILE:svm_translator_linux>)
set_tests_properties(malformed_guest_isolation PROPERTIES
                     LABELS "isolation;guest;e2e" TIMEOUT 300)

# ---------------------------------------------------------------------------
# Guest/host address-space isolation.
#
# The bounded guest window: every guest address is truncated into one host
# reservation before the bias is added, so no guest address can name host
# memory.  The suite asserts that `base` and `base + 2^k` alias (i.e. the
# access stayed inside the window) and that wild pointers never kill the host.
# It fails on the unbounded build -- run it with SVM_ISOLATION_EXPECT=broken
# to see the defect it locks down.
#
# Same dormancy caveat as the registrations above: nothing calls
# enable_testing(), so run_isolation_tests.sh is runnable on its own for now.
# ---------------------------------------------------------------------------
add_test(NAME guest_address_space_isolation
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_isolation_tests.sh
                 $<TARGET_FILE:svm_translator_linux>)
set_tests_properties(guest_address_space_isolation PROPERTIES
                     LABELS "isolation;guest;e2e" TIMEOUT 300)
