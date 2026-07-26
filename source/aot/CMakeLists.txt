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
        ${SVM_AOT_LINUX_SUPPORT})

target_include_directories(swift_aot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../translator/linux)
target_link_libraries(swift_aot PUBLIC swift_translator swift::runtime fronted_x86 elfio::elfio)
add_library(swift::aot ALIAS swift_aot)

add_executable(svm_aot aot_main.cpp)
target_link_libraries(svm_aot PUBLIC swift_aot)
