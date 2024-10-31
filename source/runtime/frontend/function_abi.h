//
// Created by 甘尧 on 2024/7/23.
//

#pragma once

#include <span>
#include "base/types.h"

namespace swift::runtime::frontend {

struct ABIRegUniform {
    u16 offset;
    u16 size;
};

struct ABIDescriptor {
    std::span<ABIRegUniform> general_params;
    std::span<ABIRegUniform> float_params;
    std::span<ABIRegUniform> general_return;
    std::span<ABIRegUniform> float_return;
};

}