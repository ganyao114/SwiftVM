#pragma once

#include "constraints.h"

namespace swift::runtime::ir {
class Inst;
}

namespace swift::runtime::compile {

// Optional width-family witnesses. Targets describe topology before allocation;
// the allocator still selects consecutive long steps after physical filtering.
struct WidthInputs final {
    enum PublicationFlag : u8 { Owner = 1, HighZero = 2, LongStep = 4 };
    struct Publication {
        const ir::Inst* stored;
        const ir::Inst* produced;
        u16 instruction, home;
        u8 flags;
    };
    struct Step {
        u32 publication, first_link, link_count, first_view, view_count;
    };
    Vector<Publication> publications;
    Vector<Step> steps;
    Vector<u32> links, views;
    u16 minimum_long_steps{32};
};

}  // namespace swift::runtime::compile
