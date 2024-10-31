//
// Created by 甘尧 on 2023/9/8.
//

#pragma once

#include <map>
#include <mutex>
#include "runtime/backend/module.h"
#include "runtime/backend/translate_table.h"
#include "runtime/backend/trampolines.h"
#include "runtime/common/range_map.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"
#include "runtime/ir/opts/uniform_elimination_pass.h"

namespace swift::runtime::backend {

constexpr static auto code_cache_bits = 23;

class AddressSpace : public runtime::Instance {
public:
    explicit AddressSpace(const Config& config);

    ~AddressSpace();

    std::shared_ptr<Module> MapModule(LocationDescriptor start,
                                      LocationDescriptor end,
                                      const ModuleConfig &m_config);

    [[nodiscard]] std::shared_ptr<Module> GetModule(LocationDescriptor loc);

    [[nodiscard]] std::shared_ptr<Module> GetDefaultModule();

    void UnmapModule(LocationDescriptor start, LocationDescriptor end);

    u32 PushCodeCache(ir::Location location, void* cache);

    u32 GetCodeCacheIndex(ir::Location location);

    [[nodiscard]] void* GetCodeCache(ir::Location location);

    [[nodiscard]] Trampolines &GetTrampolines();
    [[nodiscard]] Trampolines &GetTrampolines() const;

    [[nodiscard]] const Config &GetConfig();
    [[nodiscard]] const Config &GetConfig() const;

    [[nodiscard]] const ir::UniformInfo &GetUniformInfo();
    [[nodiscard]] const ir::UniformInfo &GetUniformInfo() const;

private:
    void Init();

    const Config config;
    std::shared_mutex lock;
    RangeMap<LocationDescriptor, std::shared_ptr<Module>> modules{};
    std::shared_ptr<Module> default_module;
    std::unique_ptr<Trampolines> trampolines;
    TranslateTable code_cache{code_cache_bits};
    std::unique_ptr<ir::UniformInfo> uniform_info{};
};

}  // namespace swift::runtime::backend
