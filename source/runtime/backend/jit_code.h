//
// Created by 甘尧 on 2023/10/18.
//

#pragma once

#include "runtime/common/types.h"
#include "runtime/common/bit_fields.h"

namespace swift::runtime::backend {

struct JitCache {
    using CacheID = BitField<0, 8, u32>;
    using Offset = BitField<9, 32, u32>;
    using Size = BitField<41, 24, u32>;
    CacheID cache_id;
    Offset offset_in;
};

}
