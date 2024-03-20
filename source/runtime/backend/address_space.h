//
// Created by 甘尧 on 2023/9/8.
//

#pragma once

#include <map>
#include <mutex>
#include "runtime/backend/module.h"
#include "runtime/backend/translate_table.h"
#include "runtime/common/range_map.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"

namespace swift::runtime::backend {

constexpr static auto code_cache_bits = 23;

class AddressSpace {
public:
    std::shared_ptr<Module> MapModule(LocationDescriptor start,
                                      LocationDescriptor end,
                                      bool read_only = false);

    [[nodiscard]] std::shared_ptr<Module> GetModule(LocationDescriptor loc);

    void UnmapModule(LocationDescriptor start, LocationDescriptor end);

    void PushCodeCache(ir::Location location, void* cache);

    [[nodiscard]] void* GetCodeCache(ir::Location location);

private:
    std::shared_mutex lock;
    RangeMap<LocationDescriptor, std::shared_ptr<Module>> modules{};
    TranslateTable code_cache{code_cache_bits};
};

}  // namespace swift::runtime::backend
