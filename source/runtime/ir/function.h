//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <atomic>
#include "runtime/ir/block.h"

namespace swift::runtime::ir {

class Function : public SlabObject<Function, true>,
                 public IntrusiveRefCounter<Function>,
                 public AddressNode {
public:
    using ReadLock = std::shared_lock<RwSpinLock>;
    using WriteLock = std::unique_lock<RwSpinLock>;

    explicit Function() : AddressNode(Location{0}, AddressNode::Function) {}

    explicit Function(const Location& location) : AddressNode(location, AddressNode::Function) {}

    ~Function();

    ir::Block* EntryBlock();
    const ir::Block* EntryBlock() const;
    ir::Block* FindBlock(ir::Location location, bool block_start = true);
    const ir::Block* FindBlock(ir::Location location, bool block_start = true) const;
    void AddBlock(ir::Block* block);
    // Detach only this exact node, relinquishing Function's deletion ownership.
    bool DetachBlock(ir::Block* block);
    bool TakeBlocksFrom(Function& region);
    // Frees every instruction of every block, leaving the blocks (and their
    // guest ranges, jit_cache and dispatch indices) in place. Idempotent.
    void DestroyInstrs();
    [[nodiscard]] const BlockMap& GetBlocks() const { return blocks; }
    [[nodiscard]] bool IsIRReadOnly() const {
        return read_only_regions.load(std::memory_order_acquire) != 0;
    }

    [[nodiscard]] ReadLock LockRead() { return std::shared_lock{func_lock}; }

    [[nodiscard]] WriteLock LockWrite() { return std::unique_lock{func_lock}; }

    [[nodiscard]] backend::JitCache& GetJitCache() { return jit_cache; }

    [[nodiscard]] u32 GetDispatchIndex() const { return dispatch_index; }

private:
    friend struct AddressNode;
    friend class HIRFunction;
    friend class compile::CompileUnit;
    void CheckMutable() const { ASSERT(!IsIRReadOnly()); }
    void CheckOwnershipMutable() const {
        CheckMutable();
        ASSERT(compiling_regions.load(std::memory_order_acquire) == 0);
    }
    void AcquireCompilation();
    void ReleaseCompilation();
    void AcquireIRReadOnly();
    void ReleaseIRReadOnly();
    bool TransferBlocksFrom(std::span<Function* const> regions);
    union {
        u32 id{};
        u32 dispatch_index;
    };
    BlockMap blocks{};
    RwSpinLock func_lock{};
    // Disjoint prepared regions can share this actual persistent parent.
    // Releasing one region must preserve the other consumers' structure seal.
    std::atomic<u32> read_only_regions{};
    std::atomic<u32> compiling_regions{};
    backend::JitCache jit_cache{};
};

using FunctionList = AddressNode::List;
using FunctionMap = AddressNode::Map;

// AddressNode is not itself reference counted -- Block and Function each carry
// their own IntrusiveRefCounter base -- so a holder of the erased
// AddressNode* has to dispatch on node_type. Every owner of a node pointer
// (the module's address map, SmcTracker's page records) must use these; a raw
// AddressNode* borrowed from another owner is a use-after-free waiting for the
// owner to drop it.
inline void AddressNodeAddRef(AddressNode* node) {
    if (!node) {
        return;
    }
    switch (node->GetNodeType()) {
        case AddressNode::Function:
            IntrusivePtrAddRef(static_cast<Function*>(node));
            break;
        case AddressNode::Block:
            IntrusivePtrAddRef(static_cast<Block*>(node));
            break;
        default:
            break;
    }
}

inline void AddressNodeRelease(AddressNode* node) {
    if (!node) {
        return;
    }
    switch (node->GetNodeType()) {
        case AddressNode::Function:
            IntrusivePtrRelease(static_cast<Function*>(node));
            break;
        case AddressNode::Block:
            IntrusivePtrRelease(static_cast<Block*>(node));
            break;
        default:
            break;
    }
}

// Owning handle for an ir::AddressNode of either concrete type.
class NodeRef {
public:
    NodeRef() = default;

    explicit NodeRef(AddressNode* node) : node_(node) { AddressNodeAddRef(node_); }

    NodeRef(const NodeRef& other) : node_(other.node_) { AddressNodeAddRef(node_); }

    NodeRef(NodeRef&& other) noexcept : node_(other.node_) { other.node_ = nullptr; }

    NodeRef& operator=(const NodeRef& other) {
        if (this != &other) {
            AddressNodeAddRef(other.node_);
            AddressNodeRelease(node_);
            node_ = other.node_;
        }
        return *this;
    }

    NodeRef& operator=(NodeRef&& other) noexcept {
        if (this != &other) {
            AddressNodeRelease(node_);
            node_ = other.node_;
            other.node_ = nullptr;
        }
        return *this;
    }

    ~NodeRef() { AddressNodeRelease(node_); }

    [[nodiscard]] AddressNode* Get() const { return node_; }

    explicit operator bool() const { return node_ != nullptr; }

private:
    AddressNode* node_{};
};

}  // namespace swift::runtime::ir
