add_library(swift_guest_call STATIC
        abi_sysv.h
        guest_ptr.h
        guest_call.h
        guest_call.cpp
        guest_space.h
        guest_space.cpp
        jit_env.h
        jit_env.cpp
        )

target_link_libraries(swift_guest_call PUBLIC swift_translator swift::runtime swift::base)
target_include_directories(swift_guest_call PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Test binary lives here rather than in source/tests so that adding it does not
# touch a CMakeLists other work streams are editing.
add_executable(swift_guest_call_test
        tests/abi_class_test.cpp
        tests/negative_compile_test.cpp
        tests/stub_call_test.cpp
        tests/glibc_call_test.cpp
        tests/test_vm.cpp
        tests/test_vm.h
        )
target_link_libraries(swift_guest_call_test PRIVATE swift_guest_call Catch2::Catch2WithMain)
target_compile_definitions(swift_guest_call_test PRIVATE
        SVM_GUEST_CALL_STUB_ELF="${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_stubs_x86_64"
        SVM_GUEST_CALL_GLIBC_ELF="${CMAKE_CURRENT_SOURCE_DIR}/../linux/tests/func_tests_x86_64")

add_test(swift_guest_call_test swift_guest_call_test --durations yes)
