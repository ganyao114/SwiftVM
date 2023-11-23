//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "runtime/ir/instr.h"
#include "runtime/ir/terminal.h"

namespace swift::runtime::ir {

class Block : public SlabObject<Block, true> {
public:
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
    void DestroyInst(Inst* inst);

    void SetEndLocation(Location location);
    Location GetStartLocation();

    InstList &GetInstList();

#define INST(name, ret, ...)                                                                      \
    template <typename... Args> ret name(const Args&... args) {                                    \
        return ret{AppendInst(OpCode::name, args...)};                                             \
    }
#include "ir.inc"
#undef INST

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

    virtual ~Block();

    union {
        NonTriviallyDummy dummy{};
        IntrusiveMapNode map_node;
        IntrusiveListNode list_node;
    };

private:
    u32 id{};
    Location location{0};
    Location end{0};
    InstList inst_list{};
    Terminal block_term{};
};

using BlockList = IntrusiveList<&Block::list_node>;
using BlockMap = IntrusiveMap<&Block::map_node>;

}  // namespace swift::runtime::ir