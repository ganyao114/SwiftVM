#pragma once

#include <optional>

#include "runtime/common/types.h"

namespace swift::runtime::ir {
class Block;
class HIRFunction;
}  // namespace swift::runtime::ir

namespace swift::runtime::compile {

class SemanticFacts;

// Semantic demand on explicit GetHostGPR captures. Width belongs to the raw
// operand view, before BitCast resolution; no physical high bits or homes are
// inferred. Block and whole-Function RPO inputs retain their separate scope.
class RegisterReadUses final {
public:
    struct Demand {
        u16 last_use{};
        u8 max_width{};
    };
    [[nodiscard]] std::optional<Demand> Find(u32 value) const;
    RegisterReadUses(const RegisterReadUses&) = delete;
    RegisterReadUses& operator=(const RegisterReadUses&) = delete;

private:
    friend class SemanticFacts;
    RegisterReadUses(const ir::Block*, const ir::HIRFunction*);
    struct Entry {
        u16 value{};
        Demand demand;
        bool seen{};
    };
    StackVector<Entry, 8> reads;
};

}  // namespace swift::runtime::compile
