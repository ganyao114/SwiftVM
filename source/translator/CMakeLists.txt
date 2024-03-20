add_library(translator STATIC
        interface.cpp
        interface.h
        arm64/cpu.h
        arm64/translator.cpp
        arm64/translator.h)

target_link_libraries(translator PUBLIC swift::base swift::runtime fronted_arm64)