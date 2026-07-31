add_subdirectory(externals/elfio)

set(SVM_LINUX_SOURCES
        loader.cpp
        loader.h
        main.cpp
        guest_memory.cpp
        guest_memory.h
        path_utils.h
        syscalls.cpp
        syscalls.h
        )

option(SWIFT_ENABLE_SHADOW_LINKER
        "Build the experimental host shadow-linker into the Linux launcher" OFF)
if (NOT APPLE AND SWIFT_ENABLE_SHADOW_LINKER)
    # WIP dynamic-linking support (host shadow libs); static guests do not use
    # it. Keep it opt-in until the bridge owns/moves ShadowLib correctly.
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

# Linux identity mapping needs the translator itself out of the conventional
# ET_EXEC guest range (typically 0x400000). Make that invariant explicit
# instead of relying on a distribution compiler's default-PIE specs. macOS is
# deliberately untouched.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckPIESupported)
    check_pie_supported()
    set_property(TARGET svm_translator_linux PROPERTY POSITION_INDEPENDENT_CODE TRUE)
endif ()

target_compile_definitions(svm_translator_linux PRIVATE
        SVM_DEFAULT_GUEST_ELF="${CMAKE_CURRENT_SOURCE_DIR}/tests/hello_aarch64")

# Unbounded guest address space (SVM_GUEST_BITS=0): restores the pre-isolation
# behaviour in which a guest can read and write arbitrary HOST memory. It is
# not a diagnostic knob that happens to be dangerous -- it is the defect the
# isolation work removed, kept only so run_isolation_tests.sh can demonstrate
# it. A runtime warning is not enough of a barrier for that, so the code is not
# compiled in unless this option is turned on explicitly:
#
#   cmake -DSWIFT_ALLOW_UNBOUNDED_GUEST=ON ...   # demo build only, never ship
#
# Without it, SVM_GUEST_BITS=0 is refused with a message naming this flag.
option(SWIFT_ALLOW_UNBOUNDED_GUEST
        "Compile in the unbounded (non-isolating) guest address mode; DIAGNOSTIC ONLY" OFF)
if (SWIFT_ALLOW_UNBOUNDED_GUEST)
    message(WARNING
            "SWIFT_ALLOW_UNBOUNDED_GUEST=ON: this build accepts SVM_GUEST_BITS=0, "
            "under which the guest can read and write host memory. Do not ship it.")
    target_compile_definitions(svm_translator_linux PRIVATE SWIFT_ALLOW_UNBOUNDED_GUEST=1)
endif ()

if (NOT APPLE)
    add_subdirectory(libs)
endif ()
