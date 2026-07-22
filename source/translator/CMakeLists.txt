add_library(swift_translator STATIC
        interface.cpp
        runtime.h
        arm64/cpu.h
        arm64/translator.cpp
        arm64/translator.h
        x86/cpu.h
        x86/translator.cpp
        x86/translator.h
        )

target_link_libraries(swift_translator PUBLIC swift::base swift::runtime fronted_arm64 fronted_x86)
target_link_libraries(swift_translator PRIVATE distorm)
target_include_directories(swift_translator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/externals/distorm)

add_subdirectory(linux)