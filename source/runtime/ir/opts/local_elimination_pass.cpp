//
// Created by 甘尧 on 2023/9/18.
//

#include "local_elimination_pass.h"

namespace swift::runtime::ir {

void LocalEliminationPass::Run(HIRBuilder* hir_builder) {
    for (auto& hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func);
    }
}

void LocalEliminationPass::Run(HIRFunction* hir_function) {
    auto& hir_blocks = hir_function->GetHIRBlocks();
    Vector<HIRLocal> current_locals(hir_function->MaxLocalId() + 1);
    for (auto block : hir_blocks) {
        for (auto& inst : block->GetInstList()) {
            switch (inst.GetOp()) {
                case OpCode::LoadLocal: {
                    auto local = inst.GetArg<Local>(0);
                    if (auto local_value = current_locals[local.id].current_value;
                        local_value && local_value->block == block) {
                        if (auto hir_value = hir_function->GetHIRValue(Value{&inst}); hir_value) {
                            for (auto& use : hir_value->uses) {
                                if (use.inst->ArgAt(use.arg_idx).IsValue()) {
                                    use.inst->SetArg(use.arg_idx, local_value->value);
                                } else if (use.inst->ArgAt(use.arg_idx).IsLambda()) {
                                    use.inst->SetArg(use.arg_idx, Lambda{local_value->value});
                                }
                            }
                            hir_function->DestroyHIRValue(hir_value);
                        }
                    }
                    break;
                }
                case OpCode::StoreLocal: {
                    auto local = inst.GetArg<Local>(0);
                    auto value = hir_function->GetHIRValue(inst.GetArg<Value>(1));
                    current_locals[local.id] = {local, value};
                    break;
                }
                default:
                    break;
            }
        }
    }
}

}  // namespace swift::runtime::ir
