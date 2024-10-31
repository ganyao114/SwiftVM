//
// Created by 甘尧 on 2023/10/13.
//

#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::ir {

#pragma pack(push, 1)
struct HostGPR {
    static constexpr auto INVALID = u16(-1);
    u16 id{INVALID};
};

struct HostFPR {
    static constexpr auto INVALID = u16(-1);
    u16 id{INVALID};
};

struct SpillSlot {
    static constexpr auto INVALID = u16(-1);
    u16 offset{INVALID};
};

struct Reference {
    static constexpr auto INVALID = u16(-1);
    u16 ref_to{INVALID};
};

struct HostReg {
    union {
        HostGPR gpr;
        HostFPR fpr;
    };
    bool is_fpr{};
};
#pragma pack(pop)

}
