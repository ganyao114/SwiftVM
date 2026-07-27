# AOT pre-compiler (docs/aot-design.md).
#
# The linux loader / guest memory / syscall sources are compiled *into* this
# target rather than linked from svm_translator_linux: that executable owns its
# own main(), and those three files are not packaged as a library. They are
# referenced, never modified.
set(SVM_AOT_LINUX_SUPPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/../translator/linux/loader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../translator/linux/guest_memory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../translator/linux/syscalls.cpp)

add_library(swift_aot STATIC
        aot_format.h
        aot_artifact.h
        aot_artifact.cpp
        aot_compile.h
        aot_compile.cpp
        aot_guest.h
        aot_guest.cpp
        aot_load.h
        aot_load.cpp
        aot_call.h
        aot_call.cpp
        ${SVM_AOT_LINUX_SUPPORT})

target_include_directories(swift_aot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../translator/linux)
# swift_guest_call: the AOT artifact feeds the host->guest call layer
# (aot_call.h). The dependency runs aot -> guest_call and never the other way:
# the call layer must stay usable without an artifact, which is what makes the
# "same call, JIT vs AOT" comparison meaningful.
target_link_libraries(swift_aot PUBLIC swift_translator swift::runtime fronted_x86 elfio::elfio
        swift_guest_call)
add_library(swift::aot ALIAS swift_aot)

add_executable(svm_aot aot_main.cpp)
target_link_libraries(svm_aot PUBLIC swift_aot)

# End-to-end: compile an artifact in THIS process image, install it, then call
# guest functions by name through GuestFn and compare against the same calls
# with no artifact. Compiling in-process is not a convenience: the validity key
# covers the SwiftVM build id, so an artifact from `svm_aot` would (correctly)
# be refused here.
add_executable(swift_aot_call_test tests/aot_call_test.cpp)
target_link_libraries(swift_aot_call_test PRIVATE swift_aot Catch2::Catch2WithMain)
target_compile_definitions(swift_aot_call_test PRIVATE
        SVM_AOT_GLIBC_ELF="${CMAKE_CURRENT_SOURCE_DIR}/../translator/linux/tests/func_tests_x86_64")
add_test(swift_aot_call_test swift_aot_call_test --durations yes)
