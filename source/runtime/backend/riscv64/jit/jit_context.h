#pragma once

#include <biscuit/assembler.hpp>
#include "runtime/backend/code_cache.h"
#include "runtime/ir/args.h"

namespace swift::runtime::backend::riscv64 {

// Each SSA value has a canonical 16-byte home, shared by direct lowering and
// semantic helpers. The ARM64 host-register rewrite pass is not used here.
class JitContext {
public:
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
    void Write(ir::Inst* inst, biscuit::GPR value);
    void Jump(biscuit::Label& label);
    void BranchZero(biscuit::GPR value, biscuit::Label& label, bool zero = true);
    void Condition(biscuit::GPR result, ir::Cond condition);
    [[nodiscard]] u32 CurrentBufferSize();
    void Flush(const CodeBuffer& buffer);

private:
    biscuit::Assembler masm{};
};

}  // namespace swift::runtime::backend::riscv64
