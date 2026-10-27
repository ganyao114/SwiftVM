#pragma once

#include <array>
#include <span>

#include "runtime/common/types.h"

namespace swift::runtime::compile {

// Optional immutable tables owned by one AllocationInputs request. The target
// describes semantic choices; the common core checks actual physical homes.
struct FPRInputs final {
    enum class MergeRule : u8 { None, RejectIfMapped, ReadIfMapped, Mapped, ReadMapped };
    struct Tie {
        u16 value, source;
        bool insert_only;
    };
    struct WriteRange {
        u32 mask;
        u16 begin, end;
    };
    struct ValueEnd {
        u16 value, end;
    };
    struct Node {
        u32 first_input;
        u16 value, input_count;
    };
    struct Publication {
        u32 first_node;
        u16 node_count, store, home, head, end;
        std::array<MergeRule, 2> merge{};
        u8 modes{3};
        bool recolor_safe{};
    };
    struct BlockRange {
        u32 first_value, value_count, first_publication, publication_count;
    };

    [[nodiscard]] u16 TieSource(u32 value, bool scalar_insert) const;
    [[nodiscard]] bool ReplacesHome(u32 instruction, u16 home) const;
    [[nodiscard]] std::span<const Node> Nodes(const Publication&) const;
    [[nodiscard]] std::span<const u16> Inputs(const Node&) const;
    [[nodiscard]] std::span<const ValueEnd> Values(const BlockRange&) const;
    [[nodiscard]] std::span<const Publication> Publications(const BlockRange&) const;

    u32 resident_homes{};
    Vector<Tie> ties;
    Vector<WriteRange> writes;
    Vector<ValueEnd> values;
    Vector<Node> nodes;
    Vector<u16> inputs;
    Vector<Publication> publications;
    Vector<BlockRange> blocks;
};

}  // namespace swift::runtime::compile
