//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <shared_mutex>
#include "runtime/ir/instr.h"
#include "runtime/ir/terminal.h"
#include "runtime/common/cast_utils.h"
#include "runtime/common/spin_lock.h"
#include "runtime/backend/jit_code.h"

namespace swift::runtime::ir {

class Block final : public SlabObject<Block, true>, public IntrusiveRefCounter<Block> {
public:
    using ReadLock = std::shared_lock<RwSpinLock>;
    using WriteLock = std::unique_lock<RwSpinLock>;

    explicit Block() = default;

    explicit Block(const Location &location) : location(location) {}

    explicit Block(u32 id, Location location) : id(id), location(location) {}

    [[nodiscard]] Terminal GetTerminal() const;
    void SetTerminal(Terminal term);
    [[nodiscard]] bool HasTerminal() const;

    template <typename... Args> Inst* AppendInst(OpCode op, const Args&... args) {
        auto inst = new Inst(op);
        inst->SetArgs(args...);
        AppendInst(inst);
        return inst;
    }

    void AppendInst(Inst* inst);
    void InsertBefore(Inst* inst, Inst* before);
    void InsertAfter(Inst* inst, Inst* after);
    void RemoveInst(Inst* inst);
    void DestroyInst(Inst* inst);
    void DestroyInsts();

    void SetEndLocation(Location location);
    Location GetStartLocation();

    InstList &GetInstList();
    InstList::iterator GetBeginInst();

    [[nodiscard]] ReadLock LockRead() {
        return std::shared_lock{block_lock};
    }

    [[nodiscard]] WriteLock LockWrite() {
        return std::unique_lock{block_lock};
    }

    [[nodiscard]] u32 GetVStackSize() const {
        return v_stack * 8;
    }

    [[nodiscard]] backend::JitCache &GetJitCache() {
        return jit_cache;
    }

    [[nodiscard]] u32 GetDispatchIndex() const {
        return dispatch_index;
    }

#define INST(name, ret, ...)                                                                      \
    template <typename... Args> ret name(const Args&... args) {                                   \
        return ret{AppendInst(OpCode::name, args...)};                                            \
    }
#include "ir.inc"
#undef INST

    template<typename Lambda, typename... Args>
    Value CallHost(Lambda l, const Args&... args) {
        constexpr static auto MAX_ARG = 3;
        auto arg_count = sizeof...(args);
        ASSERT(arg_count <= MAX_ARG);
        return CallLambda(FptrCast(l), std::forward<const Args&>(args)...);
    }

    bool operator<(const Block& rhs) const { return location < rhs.location; }

    bool operator>(const Block& rhs) const { return location > rhs.location; }

    bool operator==(const Block& rhs) const { return location == rhs.location; }

    // for rbtree compare
    static NOINLINE int Compare(const Block &lhs, const Block &rhs) {
        if (rhs.location > lhs.location) {
            return 1;
        } else if (rhs.location < lhs.location) {
            return -1;
        } else {
            return 0;
        }
    }

    ~Block();

    union {
        NonTriviallyDummy dummy{};
        IntrusiveMapNode map_node;
        IntrusiveListNode list_node;
    };

private:
    union {
        u32 id{};
        u32 dispatch_index;
    };
    Location location{0};
    Location end{0};
    InstList inst_list{};
    Terminal block_term{};
    RwSpinLock block_lock{};
    u16 v_stack{};
    backend::JitCache jit_cache;
};

using BlockList = IntrusiveList<&Block::list_node>;
using BlockMap = IntrusiveMap<&Block::map_node>;

}  // namespace swift::runtime::ir