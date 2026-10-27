#pragma once

#include "base/types.h"
#include "runtime/common/types.h"

namespace swift::runtime::ir {
class Block;
}

namespace swift::runtime::compile {
struct PhysicalGPRWrite {
    u32 instruction{};
    u16 home{};
    // A verified early writeback consumes this SSA value, replacing its
    // sole remaining covered use at consuming_use.
    u32 consumed_value{UINT32_MAX}, consuming_use{UINT32_MAX};
};

enum class RegisterClass : u8 { GPR, FPR };
struct FixedRegisterRead {
    u16 value, home;
    RegisterClass reg_class;
    bool track_use_end{};
};

// A target-proved publication window. The optional source can reuse the same
// home only after the published value itself passes physical admission.
struct FixedGPRPublication {
    const ir::Block* block;
    u16 value, publication, home;
    u16 source{UINT16_MAX};
};

// A declared SSA residence, shared by immutable inputs and mutable physical
// results. A preserved value borrows the guest home until transfer; its
// ordinary mapping owns the suffix. No mapping or emitter lives in this type.
struct FixedGPRDefinition {
    u16 home{UINT16_MAX};
    u16 transferred_home{UINT16_MAX};
    u32 end{};
    u32 transfer{UINT32_MAX};
    u32 last_use{};
    u32 publication{UINT32_MAX};
    u32 owner{};
    u8 width{};
    u8 bit_offset{};
    bool elided{}, preserved{}, version{};
};

struct FixedGPRInput {
    u32 value;
    FixedGPRDefinition definition;
};

// Explicitly rebuilt from declarations after changes. Initial constraint
// production and physical preparation use the same half-open overlap rules.
// A single sparse buffer replaces per-home interval and prefix containers.
class FixedGPRHomeIndex final {
public:
    void Clear();
    void Add(u32 value, const FixedGPRDefinition& definition);
    void Finish();
    [[nodiscard]] bool Conflicts(u16 home,
                                 u32 begin,
                                 u32 end,
                                 u32 ignored = UINT32_MAX,
                                 u32 ignored_second = UINT32_MAX) const;

private:
    struct Interval {
        u32 owner, begin, end, prefix_end;
        u16 home;
    };
    Vector<Interval> intervals;
};

}  // namespace swift::runtime::compile
