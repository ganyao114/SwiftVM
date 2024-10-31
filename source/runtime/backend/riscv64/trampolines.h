//
// Created by 甘尧 on 2024/4/10.
//


#pragma once

#include "runtime/backend/trampolines.h"
#include "assembler_riscv64.h"

namespace swift::runtime::backend::riscv64 {

using namespace swift::riscv64;

class TrampolinesRiscv64 : public Trampolines {
public:
    explicit TrampolinesRiscv64(const Config& config);

    bool LinkBlock(u8* source, u8* target, u8* source_rw, bool pic) override;

private:
    void Build();

    void BuildRuntimeEntry();

    void BuildSaveHostCallee();

    void BuildRestoreHostCallee();

    void BuildSaveStaticUniform();

    void BuildRestoreStaticUniform();

    riscv64::ArenaAllocator allocator{};
    riscv64::Riscv64Assembler assembler{&allocator};
    Label label_runtime_entry;
    Label label_code_dispatcher;
    Label label_return_host;
};

}
