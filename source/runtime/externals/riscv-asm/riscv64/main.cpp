//
// Created by 甘尧 on 2023/12/8.
//

#include "assembler_riscv64.h"

int main (int argc, char * argv[]) {
    using namespace swift;
    riscv64::Riscv64Label label{};
    riscv64::ArenaAllocator allocator{};
    riscv64::Riscv64Assembler assembler{&allocator};
    assembler.Add(riscv64::A1, riscv64::A1, riscv64::A1);
    assembler.Bind(&label);
    assembler.Add(riscv64::A1, riscv64::A1, riscv64::A1);
    assembler.Add(riscv64::A1, riscv64::A1, riscv64::A1);
    assembler.Bne(riscv64::A1, riscv64::A2, &label);
    assembler.FinalizeCode();
    return 0;
}
