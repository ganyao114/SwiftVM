//
// Created by 甘尧 on 2023/10/13.
//

#include "reg_alloc.h"

namespace swift::runtime::backend {

ir::HostFPR RegAlloc::ValueFPR(const ir::Value& value) {
    return {};
}

ir::HostGPR RegAlloc::ValueGPR(const ir::Value& value) {
    return {};
}

}
