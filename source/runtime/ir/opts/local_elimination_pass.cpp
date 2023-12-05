//
// Created by 甘尧 on 2023/9/18.
//

#include <list>
#include "local_elimination_pass.h"

namespace swift::runtime::ir {

void LocalEliminationPass::Run(HIRBuilder* hir_builder) {
    for (auto& hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func);
    }
}

void LocalEliminationPass::Run(HIRFunction* hir_function) {
    auto& hir_blocks = hir_function->GetHIRBlocksRPO();
    auto block_count = hir_function->MaxBlockCount();
    auto local_count = hir_function->MaxLocalCount();

    StackVector<HIRBlock*, 8> local_def_block{local_count};
    StackVector<Local, 8> locals{local_count};
    StackVector<StackVector<HIRBlock*, 8>, 8> local_loads{local_count};
    StackVector<StackVector<HIRLocal, 8>, 8> block_current_locals{block_count};

    StackVector<StackVector<StackVector<HIRValue*, 8>, 8>, 8> block_local_loads{block_count};

    // Same block elimination
    for (auto& block : hir_blocks) {
        auto& current_locals = block_current_locals[block.GetOrderId()];
        auto& loads = block_local_loads[block.GetOrderId()];
        BitVector load_set{local_count};
        current_locals.resize(local_count);
        loads.resize(local_count);
        StackVector<HIRValue*, 4> value_be_destroy{};
        for (auto& inst : block.GetInstList()) {
            switch (inst.GetOp()) {
                case OpCode::DefineLocal: {
                    auto local = inst.GetArg<Local>(0);
                    local_def_block[local.id] = &block;
                    locals[local.id] = local;
                    break;
                }
                case OpCode::LoadLocal: {
                    auto local = inst.GetArg<Local>(0);
                    bool load_inst = true;
                    auto hir_value = hir_function->GetHIRValue(Value{&inst});
                    ASSERT(hir_value);
                    if (auto local_value = current_locals[local.id].current_value) {
                        auto use_count = hir_value->uses.size();
                        StackVector<HIRUse*, 4> uses_be_destroyed{};
                        for (auto use = hir_value->uses.begin(); use != hir_value->uses.end();) {
                            if (use->IsPhi()) {
                                // Do nothing
                                use++;
                            } else if (use->IsFuncCall()) {
                                // TODO
                                use++;
                            } else if (use->inst->ArgAt(use->arg_idx).IsValue()) {
                                use->inst->SetArg(use->arg_idx, local_value->value);
                                use_count--;
                                use = hir_value->uses.erase(use);
                            } else if (use->inst->ArgAt(use->arg_idx).IsLambda()) {
                                use->inst->SetArg(use->arg_idx, Lambda{local_value->value});
                                use_count--;
                                use = hir_value->uses.erase(use);
                            } else {
                                use++;
                            }
                            if (use_count == 0) {
                                value_be_destroy.push_back(hir_value);
                                load_inst = false;
                            } else {
                                loads[local.id].push_back(hir_value);
                            }
                        }
                    } else {
                        loads[local.id].push_back(hir_value);
                    }
                    if (load_inst && !load_set.test(local.id)) {
                        local_loads[local.id].push_back(&block);
                        load_set.set(local.id, true);
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
        for (auto& value : value_be_destroy) {
            hir_function->DestroyHIRValue(value);
        }
    }

    struct Phi {
        Local local;
        std::list<HIRValue*> nodes{};

        explicit Phi(const Local& local) : local(local) {}
    };

    // Mem 2 Reg
    StackVector<StackVector<Phi, 8>, 8> block_phi_map{block_count};
    // Step 1: Insert Phi Node
    for (u32 local_id = 0; local_id < local_count; local_id++) {
        auto &loads = local_loads[local_id];
        if (loads.empty()) {
            continue;
        }
        StackVector<Phi*, 8> blocks_phi{block_count};
        StackVector<HIRBlock*, 8> worklist{};
        for (auto load_block : loads) {
            worklist.push_back(load_block);
        }
        while (!worklist.empty()) {
            auto block = worklist.back();
            worklist.pop_back();
            for (auto &dom : block->GetDomFrontier()) {
                auto dom_id = dom.block->GetOrderId();
                if (!blocks_phi[dom_id]) {
                    auto &phi_map = block_phi_map[dom_id];
                    phi_map.push_back(Phi{locals[local_id]});
                    blocks_phi[dom_id] = &phi_map.back();
                    if (std::find(worklist.begin(), worklist.end(), dom.block) == worklist.end()) {
                        worklist.push_back(dom.block);
                    }
                }
                auto local_value = block_current_locals[block->GetOrderId()][local_id].current_value;
                if (local_value) {
                    blocks_phi[dom_id]->nodes.push_back(local_value);
                }
            }
        }
    }

    // Step 2: Rename incoming value
    for (u32 block_id = 0; block_id < block_count; ++block_id) {
        auto &block_phis = block_phi_map[block_id];
        if (block_phis.empty()) {
            continue;
        }
        for (auto &phi_desc : block_phis) {
            if (phi_desc.nodes.empty()) {
                continue;
            }
            Params params{};
            for (auto value : phi_desc.nodes) {
                params.Push(value->value);
            }
            auto block = hir_function->GetHIRBlocks()[block_id];
            auto phi = block->CreateInst(OpCode::AddPhi, params);
            auto phi_value = block->InsertFront(phi);
            // TODO
        }
    }
}

}  // namespace swift::runtime::ir
