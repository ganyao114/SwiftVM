//
// Created by 甘尧 on 2024/4/10.
//

#include "runtime/backend/riscv64/trampolines.h"
#include "runtime/backend/riscv64/defines.h"

#define __ assembler.
namespace swift::runtime::backend::riscv64 {

using namespace riscv64;

TrampolinesRiscv64::TrampolinesRiscv64(const Config& config, const CodeBuffer& buffer)
        : Trampolines(config, buffer) {}

void TrampolinesRiscv64::Build() {
    BuildRuntimeEntry();
    __ FinalizeCode();
    __ GetBuffer()->CopyInstructions({code_buffer.rw_data, code_buffer.size});
    code_buffer.Flush();
    runtime_entry = reinterpret_cast<RuntimeEntry>(code_buffer.exec_data);
    return_host =
            reinterpret_cast<ReturnHost>((ptrdiff_t)runtime_entry + label_return_host.Position());
}

void TrampolinesRiscv64::BuildRuntimeEntry() {

}

void TrampolinesRiscv64::BuildSaveHostCallee() {
    for (int i = 0; i <= 11; ++i) {
        __ Sd(XRegister(i), SP, -8 * i);
    }
    __ Addi(SP, SP, 12 * 8);
}

void TrampolinesRiscv64::BuildRestoreHostCallee() {
    for (int i = 11; i >= 0; --i) {
        __ Ld(XRegister(i), SP, 8 * i);
    }
    __ Addi(SP, SP, -12 * 8);
}

void TrampolinesRiscv64::BuildSaveStaticUniform() {

}

void TrampolinesRiscv64::BuildRestoreStaticUniform() {}

}
#undef __
