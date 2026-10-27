#pragma once

#include <span>
#include "constraints.h"

namespace swift::runtime::compile {

// Target semantics for spill ownership. Segments borrow instruction positions
// from the unit's unique block index; no operand/use graph is retained here.
struct SpillInputs final {
    enum Flag : u8 {
        DefinitionTransfer = 1,
        DefinitionConsumer = 2,
        PendingHandoff = 4,
        OwnershipInput = 8,
        OwnershipResult = 16
    };
    struct Site {
        u16 instruction;
        u8 flags;
    };
    struct Segment {
        u32 first, count;
    };
    struct BlockRange {
        const ir::Block* block;
        u32 first_site, site_count, first_segment, segment_count;
    };
    [[nodiscard]] const BlockRange* BlockFor(const ir::Block*) const;
    [[nodiscard]] u8 FlagsFor(const BlockRange&, u32 instruction) const;
    [[nodiscard]] std::span<const Segment> Segments(const BlockRange&) const;
    Vector<Site> sites;
    Vector<Segment> segments;
    Vector<BlockRange> blocks;
};

}  // namespace swift::runtime::compile
