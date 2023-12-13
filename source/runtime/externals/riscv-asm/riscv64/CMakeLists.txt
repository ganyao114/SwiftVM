add_library(riscv64_asm STATIC assembler_riscv64.cc
        assembler_riscv64.h)
target_include_directories(riscv64_asm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(riscv64_test main.cpp)
target_link_libraries(riscv64_test PRIVATE riscv64_asm)