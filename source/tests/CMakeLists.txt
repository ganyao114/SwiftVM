add_executable(swift_test
        main_case.cpp)

target_link_libraries(swift_test PUBLIC swift_translator swift::runtime Catch2::Catch2WithMain slang_compiler)
add_test(swift_test swift_test --durations yes)

add_executable(swift_vm main.cpp)
target_link_libraries(swift_vm PUBLIC fronted_arm64 fronted_x86 swift::runtime slang_compiler)