//
// Created by 甘尧 on 2023/10/18.
//

#pragma once

#include "runtime/common/types.h"
#include "runtime/common/bit_fields.h"

namespace swift::runtime::backend {

enum class JitState {
    None = 0,
    Queue,
    Cached,
    Deprecated
};

struct JitCache {
    using State = BitField<0, 2, u64>;
    using CacheID = BitField<3, 6, u64>;
    using Offset = BitField<10, 32, u64>;
    using Size = BitField<42, 22, u64>;
    State jit_state;
    CacheID cache_id;
    Offset offset_in;
    Size cache_size;
};

}
