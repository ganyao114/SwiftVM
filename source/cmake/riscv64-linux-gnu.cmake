set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)
set(CMAKE_C_COMPILER riscv64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER riscv64-linux-gnu-g++)
set(CMAKE_ASM_COMPILER riscv64-linux-gnu-gcc)
set(CMAKE_C_FLAGS_INIT "-march=rv64g -mabi=lp64d")
set(CMAKE_CXX_FLAGS_INIT "-march=rv64g -mabi=lp64d")
set(CMAKE_ASM_FLAGS_INIT "-march=rv64g -mabi=lp64d")

set(SVM_RISCV_SYSROOT "/usr/riscv64-linux-gnu" CACHE PATH "RV64 runtime sysroot for QEMU")
find_program(SVM_RISCV_QEMU_EXECUTABLE qemu-riscv64)
if(SVM_RISCV_QEMU_EXECUTABLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR
        "${SVM_RISCV_QEMU_EXECUTABLE};-L;${SVM_RISCV_SYSROOT};-cpu;rv64,v=false,zba=false,zbb=false,zbs=false,zbc=false"
        CACHE STRING "RV64 test emulator")
endif()
