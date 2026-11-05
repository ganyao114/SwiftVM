# Exact ext80/F64 dependency subset of the preserved SoftFloat-3e tree.
add_library(svm_softfloat_ext80 STATIC
    src/extF80_add.c
    src/extF80_mul.c
    src/extF80_div.c
    src/extF80_sqrt.c
    src/f64_to_extF80.c
    src/extF80_to_f64.c
    src/s_addMagsExtF80.c
    src/s_subMagsExtF80.c
    src/s_normRoundPackToExtF80.c
    src/s_roundPackToExtF80.c
    src/s_normSubnormalExtF80Sig.c
    src/s_propagateNaNExtF80UI.c
    src/s_f64UIToCommonNaN.c
    src/s_commonNaNToExtF80UI.c
    src/s_extF80UIToCommonNaN.c
    src/s_commonNaNToF64UI.c
    src/s_normSubnormalF64Sig.c
    src/s_roundPackToF64.c
    src/s_approxRecipSqrt32_1.c
    src/s_approxRecipSqrt_1Ks.c
    src/s_shiftRightJam128.c
    src/softfloat_raiseFlags.c
)
target_include_directories(svm_softfloat_ext80 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/SoftFloat-3e)
target_compile_definitions(svm_softfloat_ext80 PRIVATE
    LITTLEENDIAN=1 SOFTFLOAT_FAST_INT64=1 SOFTFLOAT_BUILTIN_CLZ=1
    FEXCORE_PRESERVE_ALL_ATTR= FEXCORE_HAS_PRESERVE_ALL_ATTR=0)
target_compile_definitions(svm_softfloat_ext80 PRIVATE
    "INLINE=static inline" INLINE_LEVEL=4
    SOFTFLOAT_FAST_DIV32TO16=1 SOFTFLOAT_FAST_DIV64TO32=1)
