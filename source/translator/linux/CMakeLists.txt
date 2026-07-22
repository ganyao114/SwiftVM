add_subdirectory(externals/elfio)

set(SVM_LINUX_SOURCES
        loader.cpp
        loader.h
        main.cpp
        guest_memory.cpp
        guest_memory.h
        syscalls.cpp
        syscalls.h
        )

if (NOT APPLE)
    # WIP dynamic-linking support (host shadow libs); not needed for static
    # guests and currently glibc-oriented, so keep it out of the macOS build.
    list(APPEND SVM_LINUX_SOURCES
            linker/linker_bridge.cpp
            linker/linker_bridge.h
            linker/ldso/dynconfig.h
            linker/linker.cpp
            linker/linker.h
            )
endif ()

add_executable(svm_translator_linux ${SVM_LINUX_SOURCES})

target_link_libraries(svm_translator_linux PUBLIC swift_translator fronted_arm64 fronted_x86 elfio::elfio)

target_compile_definitions(svm_translator_linux PRIVATE
        SVM_DEFAULT_GUEST_ELF="${CMAKE_CURRENT_SOURCE_DIR}/tests/hello_aarch64")

if (NOT APPLE)
    add_subdirectory(libs)
endif ()
