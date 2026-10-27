#pragma once

#include "constraints.h"

namespace swift::runtime::ir {
class Inst;
}

namespace swift::runtime::compile {

// Semantic write admission for both stages of the width transaction. Capture
// rows describe raw reads, including reads that cannot themselves be elided.
struct GPRWriteInputs final {
    enum Mode : u8 { Baseline = 1, Width = 2, WidthRoot = 4, ZeroExtend = 8 };
    struct Write {
        const ir::Inst* stored;
        const ir::Inst* produced;
        u32 live_end, narrow_load{UINT32_MAX};
        u16 position, home;
        u8 modes, fixed_class_pools;
    };
    struct Capture {
        u16 instruction, home;
    };
    Vector<Write> writes;
    Vector<Capture> captures;
};

}  // namespace swift::runtime::compile
