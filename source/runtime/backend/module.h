//
// Created by 甘尧 on 2024/3/8.
//

#pragma once

#include <map>
#include <shared_mutex>
#include "runtime/backend/code_cache.h"
#include "runtime/common/range_mutex.h"
#include "runtime/common/types.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"

namespace swift::runtime::backend {

constexpr static auto INVALID_CACHE_ID = UINT16_MAX;
class AddressSpace;

struct ModuleConfig {
    bool read_only{};
    Optimizations optimizations{Optimizations::None};

    [[nodiscard]] bool HasOpt(Optimizations cmp) const {
        return (optimizations & cmp) != Optimizations::None;
    }
};

class Module : DeleteCopyAndMove {
public:
    explicit Module(const Config& config,
                    AddressSpace& space,
                    const ir::Location& start,
                    const ir::Location& end,
                    const ModuleConfig &m_config);

    bool Push(ir::Block* block);

    bool Push(ir::Function* func);

    [[nodiscard]] IntrusivePtr<ir::Function> GetFunction(ir::Location location);

    [[nodiscard]] IntrusivePtr<ir::Block> GetBlock(ir::Location location);

    [[nodiscard]] CodeCache *GetCodeCache(u8 *exe_ptr);

    [[nodiscard]] void *GetJitCache(ir::Location location);

    [[nodiscard]] u32 GetDispatchIndex(ir::Location location);

    [[nodiscard]] void *GetJitCache(const JitCache &jit_cache);

    void RemoveBlock(ir::Block* block);

    void RemoveFunction(ir::Function* function);

    [[nodiscard]] ScopedRangeLock LockAddress(ir::Location start, ir::Location end) {
        return ScopedRangeLock{address_lock, start.Value(), end.Value()};
    }

    [[nodiscard]] const ModuleConfig &GetModuleConfig() const { return module_config; }

    [[nodiscard]] std::pair<u16, CodeBuffer> AllocCodeCache(u32 size);

    [[nodiscard]] const Config& GetConfig() {
        return config;
    }

    [[nodiscard]] AddressSpace& GetAddressSpace() {
        return address_space;
    }

    [[nodiscard]] AddressSpace& GetAddressSpace() const {
        return address_space;
    }

private:

    const Config& config;
    const ModuleConfig module_config;
    AddressSpace &address_space;
    ir::Location module_start;
    ir::Location module_end;
    std::shared_mutex lock;
    RangeMutex address_lock{};
    ir::BlockMap ir_blocks{};
    ir::FunctionMap ir_functions{};
    std::shared_mutex cache_lock;
    std::map<u16, CodeCache> code_caches{};
    u16 current_code_cache{};
};

}  // namespace swift::runtime::backend
