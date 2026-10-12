add_library(fronted_arm64 STATIC
        arm64_frontend.cpp
        arm64_frontend_branch.cpp
        arm64_frontend_dataproc.cpp
        arm64_frontend_crypto.cpp
        arm64_frontend_sha.cpp
        arm64_frontend_internal.h
        arm64_frontend_ls.cpp
        arm64_frontend_base_memory_extensions.cpp
        arm64_frontend_rcpc.cpp
        arm64_frontend_neon.cpp
        arm64_frontend_simd.cpp
        arm64_frontend_indexed.cpp
        arm64_frontend_dot.cpp
        arm64_frontend_scalar.cpp
        arm64_frontend_scalar_misc.cpp
        arm64_frontend_scalar_shift.cpp
        arm64_frontend_vector_sat_shift.cpp
        arm64_frontend_scalar_multiply.cpp
        arm64_frontend_scalar_mulhigh.cpp
        arm64_frontend_scalar_pairwise_fp.cpp
        arm64_frontend_fp16_unary.cpp
        arm64_frontend_fp16_binary.cpp
        arm64_frontend_vector_fp_binary.cpp
        arm64_frontend_vector_fp_unary.cpp
        arm64_frontend_fp_compare.cpp
        arm64_frontend_fp_integer.cpp
        arm64_frontend_fp_fixed_simd.cpp
        arm64_frontend_frint_range.cpp
        arm64_frontend_polynomial.cpp
        arm64_frontend_mixed_sat.cpp
        arm64_frontend_pairwise_widen.cpp
        arm64_frontend_high_narrow.cpp
        arm64_frontend_absolute_long.cpp
        arm64_frontend_absolute_narrow.cpp
        arm64_frontend_halving.cpp
        arm64_frontend_fp_precision.cpp
        arm64_frontend_fp_estimate.cpp
        arm64_frontend_unsigned_estimate.cpp
        arm64_frontend_fp_reduction.cpp
        arm64_frontend_scalar_fp16_misc.cpp
        arm64_frontend_vector_fma.cpp
        arm64_frontend_reciprocal_step.cpp
        arm64_frontend_indexed_fp.cpp
        arm64_frontend_vector_doubling_long.cpp
        fp_helpers.cpp
        single_instruction_admission.cpp
        single_instruction_admission.h
        arm64_frontend.h)

target_link_libraries(fronted_arm64 PUBLIC swift_runtime vixl softfloat_3e)

add_executable(arm64_decode_smoke arm64_decode_smoke.cpp)
target_link_libraries(arm64_decode_smoke PRIVATE fronted_arm64)
