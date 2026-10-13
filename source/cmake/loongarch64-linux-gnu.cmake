set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR loongarch64)

set(SVM_LOONGARCH_TOOLCHAIN_ROOT
    "/root/swiftvm-loongarch-deps/toolchain/cross-tools" CACHE PATH
    "LoongArch cross-tools installation directory")
set(SVM_LOONGARCH_TARGET_PREFIX "loongarch64-unknown-linux-gnu-" CACHE STRING
    "LoongArch compiler executable prefix")
find_program(CMAKE_C_COMPILER NAMES "${SVM_LOONGARCH_TARGET_PREFIX}gcc"
    HINTS "${SVM_LOONGARCH_TOOLCHAIN_ROOT}/bin" REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES "${SVM_LOONGARCH_TARGET_PREFIX}g++"
    HINTS "${SVM_LOONGARCH_TOOLCHAIN_ROOT}/bin" REQUIRED)
set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")

# Keep the executable and its helpers usable on the baseline LP64D ISA.
# JIT feature-specific paths must perform their own runtime feature checks.
set(SVM_LOONGARCH_BASELINE_FLAGS
    "-march=loongarch64 -mabi=lp64d -mfpu=64 -mno-lsx -mno-lasx" CACHE STRING
    "Baseline LoongArch compile flags")
set(CMAKE_C_FLAGS_INIT "${SVM_LOONGARCH_BASELINE_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${SVM_LOONGARCH_BASELINE_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${SVM_LOONGARCH_BASELINE_FLAGS}")

set(SVM_LOONGARCH_SYSROOT "${SVM_LOONGARCH_TOOLCHAIN_ROOT}/target" CACHE PATH
    "LoongArch runtime sysroot for QEMU")
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libstdc++.so.6
    OUTPUT_VARIABLE task_loongarch_libstdcxx OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(task_loongarch_runtime_libdir "${task_loongarch_libstdcxx}" DIRECTORY)
set(SVM_LOONGARCH_RUNTIME_LIBDIR "${task_loongarch_runtime_libdir}" CACHE PATH
    "LoongArch libstdc++ and libgcc runtime library directory")
find_program(SVM_LOONGARCH_QEMU_EXECUTABLE NAMES qemu-loongarch64)
set(SVM_LOONGARCH_QEMU_CPU "la464" CACHE STRING "QEMU LoongArch CPU model")
if(SVM_LOONGARCH_QEMU_EXECUTABLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR
        "${SVM_LOONGARCH_QEMU_EXECUTABLE};-L;${SVM_LOONGARCH_SYSROOT};-E;LD_LIBRARY_PATH=${SVM_LOONGARCH_RUNTIME_LIBDIR};-cpu;${SVM_LOONGARCH_QEMU_CPU}"
        CACHE STRING "LoongArch test emulator")
endif()

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    SVM_LOONGARCH_TOOLCHAIN_ROOT SVM_LOONGARCH_TARGET_PREFIX
    SVM_LOONGARCH_BASELINE_FLAGS SVM_LOONGARCH_SYSROOT SVM_LOONGARCH_RUNTIME_LIBDIR
    SVM_LOONGARCH_QEMU_EXECUTABLE SVM_LOONGARCH_QEMU_CPU)
