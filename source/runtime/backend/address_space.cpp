//
// Created by 甘尧 on 2023/9/8.
//

#include "address_space.h"

namespace swift::runtime::backend {

std::shared_ptr<Module> AddressSpace::MapModule(LocationDescriptor start,
                                                LocationDescriptor end,
                                                bool read_only) {
    std::unique_lock guard(lock);
    auto module = std::make_shared<Module>(start, end, read_only);
    modules.Map(start, end, module);
    return module;
}

std::shared_ptr<Module> AddressSpace::GetModule(LocationDescriptor location) {
    std::shared_lock guard(lock);
    return modules.GetValueAt(location);
}

void AddressSpace::UnmapModule(LocationDescriptor start, LocationDescriptor end) {
    std::unique_lock guard(lock);
    modules.Unmap(start, end);
}

void AddressSpace::PushCodeCache(ir::Location location, void* cache) {
    code_cache.Put(location.Value(), reinterpret_cast<size_t>(cache));
}

void* AddressSpace::GetCodeCache(ir::Location location) {
    return reinterpret_cast<void*>(code_cache.Lookup(location.Value()));
}

}  // namespace swift::runtime::backend
