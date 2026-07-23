//
// Created by 甘尧 on 2023/9/8.
//

#pragma once

#include <map>
#include <mutex>
#include "runtime/backend/module.h"
#include "runtime/backend/smc_tracker.h"
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

    // Searches every module's JIT fault table for the compiled unit whose
    // host PC range contains host_pc (host signal handler path).
    [[nodiscard]] bool LookupFault(const u8* host_pc, FaultEntry& out);

    // Self-modifying code tracker: write-protects guest pages holding
    // translated code, claims the resulting write faults from the host
    // signal handler chain and drives invalidation of stale translations.
    [[nodiscard]] SmcTracker& GetSmcTracker() { return smc_tracker; }
    [[nodiscard]] SmcTracker& GetSmcTracker() const { return smc_tracker; }

    // Syscall-layer hook (Phase 4): the guest is changing permissions or
    // unmapping/remapping [guest_start, guest_end) — synchronously
    // invalidate every tracked block overlapping the range and drop the
    // write protection we installed. WIRING NOTE: the syscall layer
    // (translator/linux/syscalls.cpp) should call this from its
    // mprotect/mmap/munmap emulation paths whenever the range may contain
    // translated guest code; it must also make every Runtime flush its
    // per-runtime L1 dispatch cache (the L2 table is zeroed here).
    void InvalidateCodeRange(VAddr guest_start, VAddr guest_end);

    // The address-space wide (L2) translate table backing PushCodeCache /
    // GetCodeCache; the JIT dispatcher reads it directly from generated code.
    [[nodiscard]] TranslateTable& GetCodeCacheTable() { return code_cache; }

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
    // mutable: SMC page records are tracking metadata, mutated through the
    // signal-handler / const-access paths (same pattern as GetTrampolines).
    mutable SmcTracker smc_tracker;
    std::unique_ptr<ir::UniformInfo> uniform_info{};
};

}  // namespace swift::runtime::backend
