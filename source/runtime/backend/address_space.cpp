//
// Created by 甘尧 on 2023/9/8.
//

#include "address_space.h"
#include "runtime/backend/arm64/trampolines.h"
#include "runtime/backend/riscv64/trampolines.h"

namespace swift::runtime::backend {

AddressSpace::AddressSpace(const Config& config) : config(config) {
    default_module = std::make_shared<Module>(config, *this, config.loc_start, config.loc_end, false);
    auto [ignore, tramp_buf] = default_module->AllocCodeCache(0x1000);
    switch (config.backend_isa) {
        case kArm64:
            trampolines = std::make_unique<arm64::TrampolinesArm64>(config, tramp_buf);
            break;
        case kRiscv64:
            trampolines = std::make_unique<riscv64::TrampolinesRiscv64>(config, tramp_buf);
        default:
            PANIC();
    }
    trampolines->Build();
}

std::shared_ptr<Module> AddressSpace::MapModule(LocationDescriptor start,
                                                LocationDescriptor end,
                                                bool read_only) {
    std::unique_lock guard(lock);
    auto module = std::make_shared<Module>(config, *this, start, end, read_only);
    modules.Map(start, end, module);
    return module;
}

std::shared_ptr<Module> AddressSpace::GetModule(LocationDescriptor location) {
    std::shared_lock guard(lock);
    return modules.GetValueAt(location);
}

std::shared_ptr<Module> AddressSpace::GetDefaultModule() {
    return default_module;
}

void AddressSpace::UnmapModule(LocationDescriptor start, LocationDescriptor end) {
    std::unique_lock guard(lock);
    modules.Unmap(start, end);
}

void AddressSpace::PushCodeCache(ir::Location location, void* cache) {
    code_cache.Put(location.Value(), reinterpret_cast<size_t>(cache));
}

void* AddressSpace::GetCodeCache(ir::Location location) {
    if (auto cache = code_cache.Lookup(location.Value()); cache) {
        return reinterpret_cast<void*>(cache);
    }
    auto module = GetModule(location.Value());
    if (module) {
        return nullptr;
    }
    return module->GetJitCache(location);
}

Trampolines& AddressSpace::GetTrampolines() { return *trampolines; }

const Config& AddressSpace::GetConfig() {
    return config;
}

const Config& AddressSpace::GetConfig() const {
    return config;
}

}  // namespace swift::runtime::backend
