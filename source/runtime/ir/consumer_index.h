#pragma once

#include <unordered_map>
#include "runtime/ir/block.h"

namespace swift::runtime::ir {

// Block-local operand edges, including multiplicity, Operand, Lambda and
// Params. Terminal and out-of-block edges remain accounted for by GetUses().
class SingleConsumerIndex {
public:
    explicit SingleConsumerIndex(Block* block) {
        for (auto& inst : block->GetInstList())
            for (auto value : inst.GetValues()) Add(value.Def(), &inst);
    }

    Inst* Find(Inst* definition) const {
        const auto it = uses.find(definition);
        return it != uses.end() && it->second.count == 1 ? it->second.consumer : nullptr;
    }

    void Transfer(Inst* from, Inst* to, Inst* consumer) {
        auto it = uses.find(from);
        ASSERT(it != uses.end() && it->second.count == 1 && it->second.consumer == consumer);
        uses.erase(it);
        Add(to, consumer);
    }

private:
    struct Uses { u32 count{}; Inst* consumer{}; };
    std::unordered_map<Inst*, Uses> uses;

    void Add(Inst* definition, Inst* consumer) {
        auto& entry = uses[definition];
        ++entry.count;
        entry.consumer = entry.count == 1 ? consumer : nullptr;
    }
};
}  // namespace swift::runtime::ir
