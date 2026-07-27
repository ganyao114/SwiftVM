//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "runtime/ir/block.h"

namespace swift::runtime::ir {

class Function : public SlabObject<Function, true>,
                 public IntrusiveRefCounter<Function>,
                 public AddressNode {
public:
    using ReadLock = std::shared_lock<RwSpinLock>;
    using WriteLock = std::unique_lock<RwSpinLock>;

    explicit Function() = default;

    explicit Function(const Location& location) : AddressNode(location, AddressNode::Function) {}

    ~Function();

    ir::Block* EntryBlock();
    ir::Block* FindBlock(ir::Location location, bool block_start = true);
    void AddBlock(ir::Block* block);
    // Frees every instruction of every block, leaving the blocks (and their
    // guest ranges, jit_cache and dispatch indices) in place. Idempotent.
    void DestroyInstrs();
    [[nodiscard]] BlockMap& GetBlocks() { return blocks; }
    [[nodiscard]] const BlockMap& GetBlocks() const { return blocks; }

    [[nodiscard]] ReadLock LockRead() { return std::shared_lock{func_lock}; }

    [[nodiscard]] WriteLock LockWrite() { return std::unique_lock{func_lock}; }

    [[nodiscard]] backend::JitCache& GetJitCache() { return jit_cache; }

    [[nodiscard]] u32 GetDispatchIndex() const { return dispatch_index; }

private:
    union {
        u32 id{};
        u32 dispatch_index;
    };
    BlockMap blocks{};
    RwSpinLock func_lock{};
    u16 v_stack{};
    backend::JitCache jit_cache{};
};

using FunctionList = IntrusiveList<&Function::list_node>;
using FunctionMap = IntrusiveMap<&Function::map_node>;

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
    switch (node->node_type) {
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
    switch (node->node_type) {
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
