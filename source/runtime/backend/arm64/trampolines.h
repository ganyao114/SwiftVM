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
    explicit TrampolinesArm64(const Config& config);

    bool LinkBlock(u8* source, u8* target, u8* source_rw, bool pic) override;

    std::optional<CallHost> GetCallHost(HostFunction* func, ISA frontend) override;

private:
    void Build();

    void BuildRuntimeEntry(MacroAssembler &assembler);

    void BuildSaveHostCallee(MacroAssembler &assembler);

    void BuildRestoreHostCallee(MacroAssembler &assembler);

    void BuildSaveStaticUniform(MacroAssembler &assembler);

    void BuildRestoreStaticUniform(MacroAssembler &assembler);

    void *BuildFunctionTrampoline(MacroAssembler &assembler, HostFunction *func, ISA frontend_isa);

    static void CallHostTrampoline(TrampolinesArm64 *thiz, State *ctx);

    Label label_runtime_entry;
    Label label_code_dispatcher;
    Label label_return_host;
    Label label_call_host;
    std::unordered_map<LocationDescriptor, CallHost> call_host_trampolines{};
    std::unordered_map<u64, void *> signature_trampolines{};
};

}  // namespace swift::runtime::backend::arm64
