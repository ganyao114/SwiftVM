//
// Created by mac on 2023/9/7.
//

#include "function.h"

#include <limits>
#include <type_traits>

namespace swift::runtime::ir {

void AddressNode::CheckRangeMutable() const {
    switch (node_type) {
        case AddressNode::Block:
            static_cast<const ir::Block*>(this)->CheckMutable();
            break;
        case AddressNode::Function:
            static_cast<const ir::Function*>(this)->CheckMutable();
            break;
        default:
            break;
    }
}

ir::Block* Function::EntryBlock() { return FindBlock(GetStartLocation()); }
const ir::Block* Function::EntryBlock() const { return FindBlock(GetStartLocation()); }

void Function::AcquireCompilation() {
    auto regions = compiling_regions.load(std::memory_order_relaxed);
    do {
        ASSERT(regions != std::numeric_limits<u32>::max());
    } while (!compiling_regions.compare_exchange_weak(
            regions, regions + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
}

void Function::ReleaseCompilation() {
    const auto regions = compiling_regions.fetch_sub(1, std::memory_order_release);
    ASSERT(regions != 0);
}

void Function::AcquireIRReadOnly() {
    auto readers = read_only_regions.load(std::memory_order_relaxed);
    do {
        ASSERT(readers != std::numeric_limits<u32>::max());
    } while (!read_only_regions.compare_exchange_weak(
            readers, readers + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
}

void Function::ReleaseIRReadOnly() {
    const auto readers = read_only_regions.fetch_sub(1, std::memory_order_release);
    ASSERT(readers != 0);
}

void Function::AddBlock(ir::Block* block) {
    CheckOwnershipMutable();
    ASSERT(block);
    block->CheckMutable();
    ASSERT(!block->owning_function && !block->instruction_owner.compiling);
    auto inserted = blocks.insert(*block);
    ASSERT(inserted.operator->() == block);
    block->owning_function = this;
}

bool Function::DetachBlock(ir::Block* block) {
    CheckOwnershipMutable();
    if (!block) return false;
    auto it = blocks.find(*block);
    if (it == blocks.end() || it.operator->() != block) return false;
    block->CheckMutable();
    blocks.erase(it);
    block->owning_function = nullptr;
    return true;
}

bool Function::TakeBlocksFrom(Function& region) {
    CheckOwnershipMutable();
    region.CheckOwnershipMutable();
    auto* source = &region;
    return TransferBlocksFrom({&source, 1});
}

bool Function::TransferBlocksFrom(std::span<Function* const> regions) {
    CheckMutable();
    // Validate every source before moving any intrusive node. The normal
    // group has two regions, so reuse their trees rather than building a map.
    for (size_t i = 0; i < regions.size(); ++i) {
        auto* region = regions[i];
        if (!region || region == this) return false;
        region->CheckMutable();
        for (size_t j = 0; j < i; ++j)
            if (regions[j] == region) return false;
        for (auto& node : region->blocks) {
            static_cast<ir::Block&>(node).CheckMutable();
            if (FindBlock(node.GetStartLocation())) return false;
            for (size_t j = 0; j < i; ++j)
                if (regions[j]->FindBlock(node.GetStartLocation())) return false;
        }
    }
    for (auto* region : regions) {
        while (!region->blocks.empty()) {
            auto* block = static_cast<ir::Block*>(&*region->blocks.begin());
            region->blocks.erase(*block);
            blocks.insert(*block);
            block->owning_function = this;
        }
    }
    return true;
}

namespace {
template <typename Map> using BlockFor =
        std::conditional_t<std::is_const_v<Map>, const ir::Block, ir::Block>;

template <typename Map>
BlockFor<Map>* FindBlockIn(Map& blocks, ir::Location loc, bool block_start) {
    if (block_start) {
        if (auto itr = blocks.find(ir::Block{loc}); itr != blocks.end()) {
            ASSERT(itr->GetNodeType() == AddressNode::Block);
            return static_cast<BlockFor<Map>*>(itr.operator->());
        } else {
            return {};
        }
    } else {
        auto start_itr = blocks.lower_bound(ir::AddressNode{loc});
        auto end_itr = blocks.upper_bound(ir::AddressNode{loc});
        for (auto itr = start_itr; itr != end_itr; itr++) {
            if (itr->Overlap(loc.Value(), loc.Value() + 1)) {
                return static_cast<BlockFor<Map>*>(itr.operator->());
            }
        }
        return {};
    }
}
}  // namespace

ir::Block* Function::FindBlock(ir::Location location, bool block_start) {
    return FindBlockIn(blocks, location, block_start);
}

const ir::Block* Function::FindBlock(ir::Location location, bool block_start) const {
    return FindBlockIn(blocks, location, block_start);
}

void Function::DestroyInstrs() {
    CheckOwnershipMutable();
    for (const auto& node : blocks) static_cast<const ir::Block&>(node).CheckMutable();
    // Block-at-a-time is safe *because* Block::DestroyInstrs is the bulk path.
    //
    // The version that ran Inst::DestroyArgs was not: DestroyArgs -> UnUse
    // writes through the defining instruction of each argument value, so a
    // function whose value graph crosses a block boundary would write through
    // a dangling Inst* as soon as the defining block had been freed -- which
    // is exactly what ~Function did, one block at a time. It would not fault:
    // the Inst arena never returns memory, so the write lands inside a live
    // chunk and silently decrements a use count on whichever instruction has
    // since been handed the slot. See Inst::ReleaseArgs.
    for (auto& node : blocks) {
        static_cast<ir::Block&>(node).DestroyInstrs();
    }
}

Function::~Function() {
    CheckOwnershipMutable();
    for (const auto& node : blocks) static_cast<const ir::Block&>(node).CheckMutable();
    while (!blocks.empty()) {
        auto* block = static_cast<ir::Block*>(&*blocks.begin());
        blocks.erase(*block);
        block->owning_function = nullptr;
        // ~Block -> Block::DestroyInstrs, i.e. the same bulk teardown.
        delete block;
    }
}

}  // namespace swift::runtime::ir
