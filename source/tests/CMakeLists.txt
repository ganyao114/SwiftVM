add_executable(swift_test
        main_case.cpp)

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

add_test(swift_test swift_test --durations yes)

add_executable(swift_vm main.cpp)
target_link_libraries(swift_vm PUBLIC fronted_arm64 fronted_x86 swift::runtime slang_compiler)