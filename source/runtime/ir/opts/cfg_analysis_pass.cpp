//
// Created by 甘尧 on 2023/9/16.
//

#include "cfg_analysis_pass.h"

namespace swift::runtime::ir {

constexpr static auto kDefaultWorklistSize = 8;

void CFGAnalysisPass::Run(HIRBuilder* hir_builder) {
    for (auto& hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func);
    }
}

void CFGAnalysisPass::Run(HIRFunction* hir_function) {
    auto& blocks = hir_function->GetHIRBlocks();
    ASSERT_MSG(!blocks.empty(), "No block?");
    auto max_block_counts = hir_function->MaxBlockCount();
    BitVector visited{max_block_counts};

    // Make repeated analysis deterministic. The old implementation appended
    // another RPO/back-edge/frontier set and reused stale immediate dominators.
    hir_function->GetHIRBlocksRPO().clear();
    for (auto* block : blocks) {
        block->SetDominator(nullptr);
        block->GetBackEdges().clear();
        block->GetDomFrontier().clear();
    }

    // Mark Edge Dominates
    FindDominateEdges(hir_function);

    // Build back edges
    FindBackEdges(hir_function, visited);

    // Build Reverse Post Order & Build Dom Tree
    ComputeDominanceInformation(hir_function);
}

void CFGAnalysisPass::FindDominateEdges(HIRFunction* hir_function) {
    // Mark Edge Dominates
    for (auto block : hir_function->GetHIRBlocks()) {
        auto& incoming_edges = block->GetIncomingEdges();
        for (auto& edge : incoming_edges) {
            edge.flags &= ~Edge::DOMINATES;
        }
        if (incoming_edges.size() == 1) {
            incoming_edges.begin()->flags |= Edge::DOMINATES;
        }
    }
}

void CFGAnalysisPass::FindBackEdges(HIRFunction* hir_function, BitVector& visited) {
    auto max_block_counts = hir_function->MaxBlockCount();
    auto& blocks = hir_function->GetHIRBlocks();
    auto entry_block = blocks[0];

    // Nodes that we're currently visiting, indexed by block id.
    BitVector visiting{max_block_counts};
    // Number of successors visited from a given node, indexed by block id.
    StackVector<u16, 32> successors_visited{};
    successors_visited.resize(max_block_counts);
    StackVector<HIRBlock*, 32> worklist{};
    // Stack of nodes that we're currently visiting (same as marked in "visiting" above).
    worklist.reserve(kDefaultWorklistSize);
    visited.set(entry_block->GetOrderId());
    visiting.set(entry_block->GetOrderId());
    worklist.push_back(entry_block);

    while (!worklist.empty()) {
        auto current = worklist.back();
        auto current_id = current->GetOrderId();
        if (successors_visited[current_id] == current->GetSuccessors().size()) {
            visiting.reset(current_id);
            worklist.pop_back();
        } else {
            auto successor = current->GetSuccessors()[successors_visited[current_id]++];
            auto successor_id = successor->GetOrderId();
            if (visiting.test(successor_id)) {
                ASSERT(ContainsElement(worklist, successor));
                successor->AddBackEdge(current);
            } else if (!visited.test(successor_id)) {
                visited.set(successor_id);
                visiting.set(successor_id);
                worklist.push_back(successor);
            }
        }
    }
}

void CFGAnalysisPass::ComputeDominanceInformation(HIRFunction* hir_function) {
    auto max_block_counts = hir_function->MaxBlockCount();
    auto& blocks = hir_function->GetHIRBlocks();
    auto entry_block = blocks[0];

    // Build a true reverse post order with an iterative DFS. The previous walk
    // scheduled a block after all non-DFS-back predecessors had been seen, then
    // assigned its dominator exactly once. That is not a fixed-point algorithm:
    // an irreducible loop can revise an already-processed block's dominator
    // through its second entry, without propagating the revision to successors.
    BitVector visited{max_block_counts};
    // Number of successors visited from a given node, indexed by block id.
    StackVector<u16, 32> successors_visited{};
    successors_visited.resize(max_block_counts);
    StackVector<HIRBlock*, 32> worklist{};
    worklist.reserve(kDefaultWorklistSize);
    StackVector<HIRBlock*, 32> post_order{};

    visited.set(entry_block->GetOrderId());
    worklist.push_back(entry_block);
    while (!worklist.empty()) {
        auto current = worklist.back();
        auto current_id = current->GetOrderId();
        if (successors_visited[current_id] == current->GetSuccessors().size()) {
            post_order.push_back(current);
            worklist.pop_back();
        } else {
            auto successor = current->GetSuccessors()[successors_visited[current_id]++];
            auto successor_id = successor->GetOrderId();
            if (!visited.test(successor_id)) {
                visited.set(successor_id);
                worklist.push_back(successor);
            }
        }
    }

    StackVector<HIRBlock*, 32> rpo{};
    rpo.reserve(post_order.size());
    auto& reverse_post_order = hir_function->GetHIRBlocksRPO();
    for (auto it = post_order.rbegin(); it != post_order.rend(); ++it) {
        rpo.push_back(*it);
        reverse_post_order.push_back(**it);
    }

    // Cooper-Harvey-Kennedy: intersect immediate-dominator chains in RPO until
    // no block changes. Every update moves toward entry in the finite RPO tree,
    // so this terminates for reducible and irreducible CFGs alike.
    const auto unreachable_index = max_block_counts;
    StackVector<u16, 32> rpo_index{};
    rpo_index.resize(max_block_counts, unreachable_index);
    for (u16 i = 0; i < rpo.size(); ++i) {
        rpo_index[rpo[i]->GetOrderId()] = i;
    }

    entry_block->SetDominator(entry_block);
    auto intersect = [&rpo_index, unreachable_index](HIRBlock* left, HIRBlock* right) {
        while (left != right) {
            auto left_index = rpo_index[left->GetOrderId()];
            auto right_index = rpo_index[right->GetOrderId()];
            ASSERT(left_index != unreachable_index);
            ASSERT(right_index != unreachable_index);
            while (left_index > right_index) {
                left = left->GetDominator();
                ASSERT(left != nullptr);
                left_index = rpo_index[left->GetOrderId()];
            }
            while (right_index > left_index) {
                right = right->GetDominator();
                ASSERT(right != nullptr);
                right_index = rpo_index[right->GetOrderId()];
            }
        }
        return left;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (u16 i = 1; i < rpo.size(); ++i) {
            auto* block = rpo[i];
            HIRBlock* new_dominator = nullptr;
            for (auto* predecessor : block->GetPredecessors()) {
                if (predecessor->GetDominator() == nullptr) {
                    continue;
                }
                new_dominator = new_dominator == nullptr
                                      ? predecessor
                                      : intersect(predecessor, new_dominator);
            }
            ASSERT(new_dominator != nullptr);
            if (block->GetDominator() != new_dominator) {
                block->SetDominator(new_dominator);
                changed = true;
            }
        }
    }

    // Dominance Frontier
    for (auto* block : rpo) {
        auto& predecessors = block->GetPredecessors();
        if (predecessors.size() > 1) {
            auto dom = block->GetDominator();
            for (auto predecessor : predecessors) {
                // An unreachable predecessor has no dominator tree. It cannot
                // contribute to the reachable block's dominance frontier.
                if (predecessor->GetDominator() == nullptr) {
                    continue;
                }
                auto runner = predecessor;
                while (runner != dom) {
                    runner->PushDominance(block);
                    runner = runner->GetDominator();
                    ASSERT(runner != nullptr);
                }
            }
        }
    }
}

// ComputeLoopInformation used to run here. It built a bogus HIRLoop for every
// natural loop it found and then threw the result away.
//
// THE DEFECT. It collected the loop's member blocks into a local StackVector
// and called `HIRLoop::Create(hir_function, *loop.begin(), loop.size())`, whose
// second parameter is typed `HIRBlock*`. `*loop.begin()` is the vector's FIRST
// ELEMENT -- one HIRBlock pointer -- not a pointer to the vector's storage, and
// HIRLoop::HIRLoop (ir/hir_builder.cpp) then does
//     std::memcpy(loop.data(), (void*)header, sizeof(HIRBlock*) * length);
// So it copies `length` pointers out of a single HIRBlock OBJECT: the "loop
// members" it produced were that block's own fields reinterpreted as
// HIRBlock*, and it read past the end of the object once
// `length > sizeof(HIRBlock)/8` (21 on this build, sizeof(HIRBlock) == 168).
//
// MEASURED, before deleting it, with a probe on the reconstructed call:
// across the whole swift_test suite the body ran EXACTLY ONCE, for a 3-block
// loop -- 24 bytes read from a 168-byte object, i.e. type-confused garbage
// that stayed inside the object -- and it ran ZERO times over eight guest
// binaries (func_tests, func_tests_musl, real_busy, real_hello, loop,
// basic_coverage_smoke, x87_bench, vec_float_nan_pressure). So the read is
// unambiguously wrong on every execution, but on the workloads available here
// it was never observed to actually leave the object; that needs a natural
// loop of 22+ blocks. Reported as a latent overrun, not as one caught in the
// act.
//
// WHY DELETED RATHER THAN REPAIRED. The product is unreachable twice over.
// HIRFunction::AddLoop only appends to `loops`, and HIRFunction::GetHIRLoop()
// -- its only accessor -- has no callers anywhere in the tree. And
// CFGAnalysisPass::Run itself is called only from source/tests/main_case.cpp
// (unit tests only); PassPipeline never registers it, so this pass does not
// run in the production translation pipeline at all. Repairing the memcpy
// means changing HIRLoop's constructor in ir/hir_builder.cpp to take the block
// array it actually wants -- worth doing when somebody needs loop information,
// together with a consumer that proves it correct. HIRLoop, AddLoop and
// GetHIRLoop are left in place (another owner's file) and now have no callers.

}  // namespace swift::runtime::ir
