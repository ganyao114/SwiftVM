//
// Created by 甘尧 on 2024/4/10.
//

#pragma once

#include "aarch64/macro-assembler-aarch64.h"
#include "runtime/backend/trampolines.h"

namespace swift::runtime::backend::arm64 {

using namespace vixl::aarch64;

class TrampolinesArm64 : public Trampolines {
public:
    explicit TrampolinesArm64(const Config& config, const CodeBuffer& buffer);

    void Build() override;

    bool LinkBlock(u8* source, u8* target, u8* source_rw, bool pic) override;

private:
    void BuildRuntimeEntry();

    void BuildSaveHostCallee();

    void BuildRestoreHostCallee();

    void BuildSaveStaticUniform();

    void BuildRestoreStaticUniform();

    MacroAssembler assembler{};
    Label label_runtime_entry;
    Label label_code_dispatcher;
    Label label_return_host;
};

}  // namespace swift::runtime::backend::arm64
