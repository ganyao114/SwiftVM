//
// Created by 甘尧 on 2023/9/16.
//

#include "cfg_analysis_pass.h"

namespace swift::runtime::ir {

constexpr static auto kDefaultWorklistSize = 8;

// Helper class for finding common dominators of two or more blocks in a graph.
// The domination information of a graph must not be modified while there is
// a CommonDominator object as it's internal state could become invalid.
class CommonDominator {
public:
    // Convenience function to find the common dominator of 2 blocks.
    static HIRBlock* ForPair(HIRBlock* block1, HIRBlock* block2) {
        CommonDominator finder(block1);
        finder.Update(block2);
        return finder.Get();
    }

    // Create a finder starting with a given block.
    explicit CommonDominator(HIRBlock* block)
            : dominator(block), chain_length_(ChainLength(block)) {}

    // Update the common dominator with another block.
    void Update(HIRBlock* block) {
        ASSERT(block != nullptr);
        if (dominator == nullptr) {
            dominator = block;
            chain_length_ = ChainLength(block);
            return;
        }
        HIRBlock* block2 = dominator;
        ASSERT(block2 != nullptr);
        if (block == block2) {
            return;
        }
        size_t chain_length = ChainLength(block);
        size_t chain_length2 = chain_length_;
        // Equalize the chain lengths
        for (; chain_length > chain_length2; --chain_length) {
            block = block->GetDominator();
            ASSERT(block != nullptr);
        }
        for (; chain_length2 > chain_length; --chain_length2) {
            block2 = block2->GetDominator();
            ASSERT(block2 != nullptr);
        }
        // Now run up the chain until we hit the common dominator.
        while (block != block2) {
            --chain_length;
            block = block->GetDominator();
            ASSERT(block != nullptr);
            block2 = block2->GetDominator();
            ASSERT(block2 != nullptr);
        }
        dominator = block;
        chain_length_ = chain_length;
    }

    [[nodiscard]] HIRBlock* Get() const { return dominator; }

private:
    static size_t ChainLength(HIRBlock* block) {
        size_t result = 0;
        while (block != nullptr) {
            ++result;
            block = block->GetDominator();
        }
        return result;
    }

    HIRBlock* dominator;
    size_t chain_length_;
};

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

    // Mark Edge Dominates
    FindDominateEdges(hir_function);

    // Build back edges
    FindBackEdges(hir_function, visited);

    // Build Reverse Post Order & Build Dom Tree
    ComputeDominanceInformation(hir_function);
}

bool CFGAnalysisPass::UpdateDominatorOfSuccessor(HIRBlock* block, HIRBlock* successor) {
    ASSERT(ContainsElement(block->GetSuccessors(), successor));

    auto old_dominator = successor->GetDominator();
    auto new_dominator =
            (old_dominator == nullptr) ? block : CommonDominator::ForPair(old_dominator, block);

    if (old_dominator == new_dominator) {
        return false;
    } else {
        successor->SetDominator(new_dominator);
        return true;
    }
}

void CFGAnalysisPass::FindDominateEdges(HIRFunction* hir_function) {
    // Mark Edge Dominates
    for (auto block : hir_function->GetHIRBlocks()) {
        auto& incoming_edges = block->GetIncomingEdges();
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
    // Build Reverse Post Order
    // Number of visits of a given node, indexed by block id
    StackVector<u16, 32> visits{};
    visits.resize(max_block_counts);
    // Number of successors visited from a given node, indexed by block id.
    StackVector<u16, 32> successors_visited{};
    successors_visited.resize(max_block_counts);
    // Nodes for which we need to visit successors.
    StackVector<HIRBlock*, 32> worklist{};
    worklist.reserve(kDefaultWorklistSize);
    worklist.push_back(entry_block);
    // RPO List
    auto& reverse_post_order = hir_function->GetHIRBlocksRPO();
    reverse_post_order.push_back(*entry_block);

    while (!worklist.empty()) {
        auto current = worklist.back();
        auto current_id = current->GetOrderId();
        if (successors_visited[current_id] == current->GetSuccessors().size()) {
            worklist.pop_back();
        } else {
            auto successor = current->GetSuccessors()[successors_visited[current_id]++];
            UpdateDominatorOfSuccessor(current, successor);

            // Once all the forward edges have been visited, we know the immediate
            // dominator of the block. We can then start visiting its successors.
            if (++visits[successor->GetOrderId()] ==
                successor->GetPredecessors().size() - successor->GetBackEdges().size()) {
                reverse_post_order.push_back(*successor);
                worklist.push_back(successor);
            }
        }
    }
    // Entry Dom self
    entry_block->SetDominator(entry_block);

    // Dominance Frontier
    for (auto& block : reverse_post_order) {
        auto& predecessors = block.GetPredecessors();
        if (predecessors.size() > 1) {
            auto dom = block.GetDominator();
            for (auto predecessor : predecessors) {
                auto runner = predecessor;
                while (runner != dom) {
                    runner->PushDominance(&block);
                    runner = runner->GetDominator();
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
// (three unit tests); PassPipeline never registers it, so this pass does not
// run in the production translation pipeline at all. Repairing the memcpy
// means changing HIRLoop's constructor in ir/hir_builder.cpp to take the block
// array it actually wants -- worth doing when somebody needs loop information,
// together with a consumer that proves it correct. HIRLoop, AddLoop and
// GetHIRLoop are left in place (another owner's file) and now have no callers.

}  // namespace swift::runtime::ir
