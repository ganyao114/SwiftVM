add_library(fronted_x86 STATIC decoder.cc
    decoder.h
    cpu.h)

target_link_libraries(fronted_x86 PUBLIC swift_runtime distorm)
add_library(swift::runtime::fronted_x86 ALIAS fronted_x86)