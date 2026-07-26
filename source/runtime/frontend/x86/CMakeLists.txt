add_library(fronted_x86 STATIC decoder.cc
    decoder_alu.cc
    decoder_avx.cc
    decoder_control.cc
    decoder_misc.cc
    decoder_sse.cc
    decoder_sse4.cc
    decoder_string.cc
    decoder_x87.cc
    decoder_avx_fp.cc
    decoder_avx_int.cc
    decoder_avx_mul.cc
    decoder_avx_hadd.cc
    decoder_avx_blend.cc
    decoder_avx_gather.cc
    decoder_avx_misc.cc
    decoder_avx_fma.cc
    decoder_bmi.cc
    decoder_xsave.cc
    vex_decoder.cc
    x87.cpp
    x87.h
    xsave.h
    decoder_internal.h
    decoder.h
    vex_decoder.h
    cpu.h)

target_link_libraries(fronted_x86 PUBLIC swift_runtime distorm softfloat_3e)
add_library(swift::runtime::fronted_x86 ALIAS fronted_x86)
