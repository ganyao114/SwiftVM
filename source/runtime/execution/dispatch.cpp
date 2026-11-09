#include "timing.h"
#include "vm.h"

#include <algorithm>
#include <exception>

namespace swift::runtime::execution {
const NativeCode* VM::LookupNative(void* opaque, std::uintptr_t host_pc) noexcept {
    for (const auto& cache : static_cast<VM*>(opaque)->caches)
        if (const auto* code = cache.FindHost(host_pc)) return code;
    return nullptr;
}

bool VM::RecoverCodeWrite(void* opaque, std::uintptr_t address) noexcept {
    return static_cast<VM*>(opaque)->smc.Recover(address);
}

void VM::RefreshNative(NativeRunContext& context) noexcept {
    context.mask = thread.faults.smc_pending ? 0 : Cache().Mask();
    context.slots = thread.faults.smc_pending ? Cache().EmptyData() : Cache().Data();
    context.l1 = thread.faults.smc_pending ? empty_l1.Data() : thread.l1.Data();
    context.anchor = &thread.anchor;
}

void VM::ObserveCache() noexcept {
    std::uint64_t entries{}, occupied{}, slots{}, allocations{};
    for (const auto& cache : caches) {
        entries += cache.Entries();
        occupied += cache.Occupied();
        slots += cache.Slots();
        allocations += cache.Allocations();
    }
    stats.peak_cache_entries = std::max(stats.peak_cache_entries, entries);
    stats.peak_cache_occupied = std::max(stats.peak_cache_occupied, occupied);
    stats.peak_cache_slots = std::max(stats.peak_cache_slots, slots);
    stats.cache_table_allocations = allocations;
}

std::uintptr_t VM::ResolveBridge(void* opaque, NativeRunContext* context) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    try {
        vm.MergeFPStatus();
        return vm.Resolve(*context);
    } catch (const std::exception& error) {
        vm.thread.bridge_failure.Capture(error);
    } catch (...) {
        vm.thread.bridge_failure.Set(ErrorKind::Runtime, "unknown native resolver failure");
    }
    vm.ObserveCache();
    vm.RefreshNative(*context);
    return 0;
}

std::uintptr_t VM::Resolve(NativeRunContext& context) {
    thread.interpret_requested = false;
    thread.bridge_failure.Clear();
    // All native/fault readers have left. SMC retained old ranges and links
    // until this exact point; no allocation is reclaimed in the handler.
    if (thread.faults.smc_pending) Invalidate();
    if (host_call_active && PC() == host_return_pc) {
        host_returned = true;
        RefreshNative(context);
        return 0;
    }
    NativeCode code;
    auto entry = Cache().Find(PC(), &code);
    if (!entry) {
        if (options.mode == ExecutionMode::Mixed &&
            Cache().DeferCompilation(PC(), options.hot_threshold)) {
            thread.interpret_requested = true;
            // Counting a new cold PC may grow the actual directory. Refresh
            // this borrow even though no native code was constructed.
            ObserveCache();
            RefreshNative(context);
            return 0;
        }
        RuntimeTimer total{stats.compile_request_ns};
        if (options.scope == ir::Scope::Function) {
            code = compiler.Compile(ir::Scope::Function);
            if (!code.entry) {
                ++stats.fallbacks;
                code = compiler.Compile(ir::Scope::Block);
            }
        } else
            code = compiler.Compile(ir::Scope::Block);
        if (!code.entry) {
            if (options.mode == ExecutionMode::Mixed &&
                compiler.Failure() == CompileFailure::Unsupported) {
                Cache().DisableCompilation(PC());
                thread.interpret_requested = true;
            }
            ObserveCache();
            RefreshNative(context);
            return 0;
        }
        {
            RuntimeTimer publish{stats.publication_ns};
            RuntimeTimer registration{options.compile_details && options.decode_timing
                                              ? &stats.publication_detail.registration_ns
                                              : nullptr};
            Cache().InsertReady(compiler.EntryPC(), code, compiler.LastOutput()->entries);
        }
        // Registration has consumed extra entries; only now may the backend
        // trim its oversized output capacities. All work remains charged.
        if (backend.trim) backend.trim(backend.context);
        entry = Cache().Find(PC(), &code);
        if (!entry)
            swift::ThrowError("native unit does not export the requested guest entry",
                              ErrorKind::InvalidState);
        ObserveCache();
    }
    thread.anchor = code;
    if (thread.l1.Data()) thread.l1.Fill(PC(), entry);
    thread.bridge_failure.Clear();
    RefreshNative(context);
    return reinterpret_cast<std::uintptr_t>(entry);
}
}  // namespace swift::runtime::execution
