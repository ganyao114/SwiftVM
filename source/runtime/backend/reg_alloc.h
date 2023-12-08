//
// Created by 甘尧 on 2023/10/13.
//

#pragma once

#include "base/common_funcs.h"
#include "runtime/common/types.h"
#include "runtime/ir/block.h"
#include "runtime/ir/host_reg.h"

namespace swift::runtime::backend {

class RegAlloc : DeleteCopyAndMove {
public:

    enum Type : u8 {
        NONE,
        GPR,
        FPR,
        MEM
    };

    struct Map {
        union {
            ir::HostGPR gpr;
            ir::HostFPR fpr;
            ir::SpillSlot spill;
        };
        Type type{NONE};
        u32 dirty_gprs{};
        u32 dirty_fprs{};
    };

    ir::HostGPR ValueGPR(const ir::Value &value);
    ir::HostFPR ValueFPR(const ir::Value &value);

private:
    Vector<Map> alloc_result;
};

}
