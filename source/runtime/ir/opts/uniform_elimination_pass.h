//
// Created by 甘尧 on 2024/6/20.
//

#pragma once

#include "runtime/ir/hir_builder.h"
#include "runtime/common/range_map.h"
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::ir {

struct UniformRegister {
    Uniform uniform{};
    HostReg host_reg{};

    [[nodiscard]] bool Null() const {
        return uniform.GetType() == ValueType::VOID;
    }

    bool operator==(const UniformRegister& rhs) const { return uniform == rhs.uniform; }
    bool operator!=(const UniformRegister& rhs) const { return !(rhs == *this); }
};

struct UniformInfo {
    u32 uniform_size{};
    backend::GPRSMask uni_gprs{};
    backend::FPRSMask uni_fprs{};
    RangeMap<u32, UniformRegister> uniform_regs_map{};
};

class UniformEliminationPass {
public:
    static void Run(HIRBuilder *hir_builder, const UniformInfo &info, bool to_regs);
    static void Run(HIRFunction *hir_function, const UniformInfo &info, bool to_regs);
    static void Run(Block *block, const UniformInfo &config, HIRFunction *hir_function = nullptr);
    // Explicit selector for byte-for-byte A/B tests in one process. Production
    // callers use the overload above, which reads SVM_UNIFORM_FAST once.
    static void Run(Block *block, const UniformInfo &config, bool fast_path,
                    HIRFunction *hir_function = nullptr);
};

}  // namespace swift::runtime::ir
