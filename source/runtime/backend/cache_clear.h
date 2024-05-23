//
// Created by 甘尧 on 2024/5/9.
//

#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::backend {
void ClearICache(void *start, size_t size);
void ClearDCache(void *start, size_t size);
}
