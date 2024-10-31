//
// Created by 甘尧 on 2023/9/8.
//

#include "address_space.h"
#include "runtime/backend/arm64/trampolines.h"
#include "runtime/backend/riscv64/trampolines.h"

namespace swift::runtime::backend {

AddressSpace::AddressSpace(const Config& config) : config(config) {
    Init();
}

void AddressSpace::Init() {
    const ModuleConfig default_module_config{.read_only = config.static_program,
                                             .optimizations = config.global_opts};
    default_module = std::make_shared<Module>(
            *this, config.loc_start, config.loc_end, default_module_config);

    // build trampolines
    switch (config.backend_isa) {
        case kArm64:
            trampolines = std::make_unique<arm64::TrampolinesArm64>(config);
            break;
        case kRiscv64:
            trampolines = std::make_unique<riscv64::TrampolinesRiscv64>(config);
        default:
            PANIC();
    }

    // build uniform info
    if (config.uniform_buffer_size) {
        uniform_info = std::make_unique<ir::UniformInfo>();
        uniform_info->uniform_size = config.uniform_buffer_size;
        for (auto& desc : config.buffers_static_alloc) {
            auto type = desc.is_float ? ir::GetVecIRValueType(desc.size)
                                      : ir::GetIRValueType(desc.size);
            ir::UniformRegister reg{.uniform = ir::Uniform{desc.offset, type}};
            reg.host_reg.is_fpr = desc.is_float;
            if (desc.is_float) {
                reg.host_reg.fpr = ir::HostFPR{desc.reg};
                uniform_info->uni_fprs.Mark(desc.reg);
            } else {
                reg.host_reg.gpr = ir::HostGPR{desc.reg};
                uniform_info->uni_gprs.Mark(desc.reg);
            }
            uniform_info->uniform_regs_map.Map(desc.offset, desc.offset + desc.size, reg);
        }
    }
}

std::shared_ptr<Module> AddressSpace::MapModule(LocationDescriptor start,
                                                LocationDescriptor end,
                                                const ModuleConfig& m_config) {
    std::unique_lock guard(lock);
    auto module = std::make_shared<Module>(*this, start, end, m_config);
    modules.Map(start, end, module);
    return module;
}

std::shared_ptr<Module> AddressSpace::GetModule(LocationDescriptor location) {
    std::shared_lock guard(lock);
    if (auto module = modules.GetValueAt(location); module) {
        return module;
    } else {
        return default_module;
    }
}

std::shared_ptr<Module> AddressSpace::GetDefaultModule() { return default_module; }

void AddressSpace::UnmapModule(LocationDescriptor start, LocationDescriptor end) {
    std::unique_lock guard(lock);
    modules.Unmap(start, end);
}

u32 AddressSpace::PushCodeCache(ir::Location location, void* cache) {
    return code_cache.Put(location.Value(), reinterpret_cast<size_t>(cache));
}

u32 AddressSpace::GetCodeCacheIndex(ir::Location location) {
    return code_cache.GetOrPut(location.Value(), 0);
}

void* AddressSpace::GetCodeCache(ir::Location location) {
    if (auto cache = code_cache.Lookup(location.Value()); cache) {
        return reinterpret_cast<void*>(cache);
    }
    auto module = GetModule(location.Value());
    if (!module) {
        return nullptr;
    }
    return module->GetJitCache(location);
}

Trampolines& AddressSpace::GetTrampolines() { return *trampolines; }

Trampolines& AddressSpace::GetTrampolines() const { return *trampolines; }

const Config& AddressSpace::GetConfig() { return config; }

const Config& AddressSpace::GetConfig() const { return config; }

const ir::UniformInfo& AddressSpace::GetUniformInfo() { return *uniform_info; }

const ir::UniformInfo& AddressSpace::GetUniformInfo() const { return *uniform_info; }

AddressSpace::~AddressSpace() {}

}  // namespace swift::runtime::backend
