//
// Created by 甘尧 on 2023/9/8.
//

#pragma once

#include <map>
#include "runtime/ir/function.h"
#include "runtime/backend/code_cache.h"
#include "runtime/backend/translate_table.h"

namespace swift::runtime::backend {

constexpr static auto l2_cache_bits = 23;

class AddressSpace {
public:

    void Push(ir::Block *block);

    void Push(ir::Function *func);



private:
    ir::BlockMap ir_blocks{};
    ir::FunctionMap ir_functions{};
    TranslateTable l2_cache{l2_cache_bits};
    std::map<u16, CodeCache> code_caches;
};

}
