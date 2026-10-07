#pragma once

#include <map>
#include "runtime/backend/riscv64/jit/jit_context.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend::riscv64 {

struct EmissionStats {
    u32 direct{};
    u32 helpers{};
};

class JitTranslator {
public:
    explicit JitTranslator(JitContext& context) : context(context) {}
    void Translate(ir::Block* block);
    [[nodiscard]] u32 RecoveryOffset() const { return recovery_offset; }
    [[nodiscard]] const EmissionStats& Stats() const { return stats; }

private:
    bool EmitScalar(ir::Inst* inst);
    void EmitHelper(ir::Inst* inst);
    void EmitMemory(ir::Inst* inst, bool store, bool ordered, biscuit::GPR result);
    void EmitTerminal(const ir::Terminal& terminal);
    void Poll();
    void Return(HaltReason reason);

    JitContext& context;
    ir::Block* block{};
    u32 slot_count{};
    u32 recovery_offset{};
    EmissionStats stats{};
    biscuit::Label epilogue;
    std::map<ir::Inst*, biscuit::Label> labels;
};

}  // namespace swift::runtime::backend::riscv64
