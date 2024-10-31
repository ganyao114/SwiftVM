add_library(swift_translator STATIC
        interface.cpp
        runtime.h
        arm64/cpu.h
        arm64/translator.cpp
        arm64/translator.h
        x86/cpu.h
        x86/translator.cpp
        x86/translator.h)

target_link_libraries(swift_translator PUBLIC swift::base swift::runtime fronted_x86)

if (UNIX AND NOT APPLE)
    add_subdirectory(linux)
endif ()