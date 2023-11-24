add_executable(swift_test
        main_case.cpp)

target_link_libraries(swift_test PUBLIC swift::runtime Catch2::Catch2WithMain slang_compiler)
add_test(swift_test swift_test --durations yes)