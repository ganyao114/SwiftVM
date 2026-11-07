set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

# Prefer GCC 13 while retaining the distribution's versioned cross-toolchain
# layout. Explicit CMAKE_*_COMPILER arguments keep their normal precedence.
find_program(CMAKE_C_COMPILER NAMES riscv64-linux-gnu-gcc-13 riscv64-linux-gnu-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES riscv64-linux-gnu-g++-13 riscv64-linux-gnu-g++ REQUIRED)
set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}" CACHE FILEPATH "RV64 assembler driver")
set(CMAKE_C_FLAGS_INIT "-march=rv64g -mabi=lp64d")
set(CMAKE_CXX_FLAGS_INIT "-march=rv64g -mabi=lp64d")
set(CMAKE_ASM_FLAGS_INIT "-march=rv64g -mabi=lp64d")

# The distribution compiler already knows its build sysroot. This path is the
# runtime loader prefix used by QEMU, not a second compiler --sysroot.
set(SVM_RISCV_SYSROOT "/usr/riscv64-linux-gnu" CACHE PATH "RV64 runtime sysroot for QEMU")
set(SVM_RISCV_QEMU_CPU "rv64,v=false,zba=false,zbb=false,zbs=false,zbc=false"
    CACHE STRING "RV64 functional-test CPU; C remains available to the target libc")
find_program(SVM_RISCV_QEMU_EXECUTABLE NAMES qemu-riscv64
    DOC "RV64 user emulator; JIT cache tests require working flush_icache semantics")
if(NOT DEFINED CMAKE_CROSSCOMPILING_EMULATOR AND SVM_RISCV_QEMU_EXECUTABLE)
    # Keep this derived value out of the cache: selecting another QEMU on a
    # later configure must take effect. A user-supplied emulator list wins.
    set(CMAKE_CROSSCOMPILING_EMULATOR
        "${SVM_RISCV_QEMU_EXECUTABLE};-L;${SVM_RISCV_SYSROOT};-cpu;${SVM_RISCV_QEMU_CPU}")
endif()
