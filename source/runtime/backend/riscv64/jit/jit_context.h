#pragma once

#include <biscuit/assembler.hpp>
#include "runtime/backend/code_cache.h"
#include "runtime/backend/riscv64/defines.h"
#include "runtime/ir/args.h"

namespace swift::runtime::backend::riscv64 {

struct ScalarValueStats {
    u32 loads{};
    u32 stores{};
    u32 cache_hits{};
    u32 spills{};
};

// Each SSA value has a canonical 16-byte home. Direct scalar lowering defers
// writes while a value resides in a callee-saved GPR; semantic helpers and
// control-flow joins require canonical homes. No ARM64 register rewrite runs.
class JitContext {
public:
    explicit JitContext(bool cache_scalars = true) : cache_scalars(cache_scalars) {}
    biscuit::Assembler& GetMasm() { return masm; }
    void EnsureSpace();
    void Address(biscuit::GPR result, biscuit::GPR base, s64 offset);
    void Load(biscuit::GPR result, biscuit::GPR base, s64 offset, u32 size = 8);
    void Store(biscuit::GPR value, biscuit::GPR base, s64 offset, u32 size = 8);
    void Read(biscuit::GPR result, ir::Value value);
    void Data(biscuit::GPR result, const ir::DataClass& data);
    void Operand(biscuit::GPR result, const ir::Operand& operand);
    void Mask(biscuit::GPR value, u32 bits);
    void SignExtend(biscuit::GPR value, u32 bits);
    // Reserve/evict before any operands are read; Write publishes the result.
    biscuit::GPR ResultRegister(ir::Inst* inst);
    void Write(ir::Inst* inst, biscuit::GPR value);
    void FlushValues();
    void DiscardValues();
    [[nodiscard]] const ScalarValueStats& ValueStats() const { return value_stats; }
    void Jump(biscuit::Label& label);
    void BranchZero(biscuit::GPR value, biscuit::Label& label, bool zero = true);
    void Condition(biscuit::GPR result, ir::Cond condition);
    [[nodiscard]] u32 CurrentBufferSize();
    void Flush(const CodeBuffer& buffer);

private:
    biscuit::Assembler masm{};
    const bool cache_scalars;
    std::array<ir::Inst*, scalar_registers.size()> cached_values{};
    size_t next_register{};
    ScalarValueStats value_stats{};
};

}  // namespace swift::runtime::backend::riscv64
