//
// Created by 甘尧 on 2023/10/13.
//

#pragma once

#include "runtime/common/common_funcs.h"
#include "runtime/common/types.h"
#include "runtime/ir/block.h"
#include "runtime/ir/host_reg.h"

namespace swift::runtime::backend {

class RegAlloc {
public:

    ir::HostGPR ValueGPR(const ir::Value &value);
    ir::HostFPR ValueFPR(const ir::Value &value);

private:
};

}
