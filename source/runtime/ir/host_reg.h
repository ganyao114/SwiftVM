//
// Created by 甘尧 on 2023/10/13.
//

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
#pragma pack(pop)

}
