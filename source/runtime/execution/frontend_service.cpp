#include "compile_service.h"
#include "vm.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/frontend/x86/build_ir.h"
#include "runtime/frontend/arm64/build_ir.h"

#include <sys/mman.h>

namespace swift::runtime::execution {

frontend::BuildResult CompileService::BuildFrontend(ir::Scope scope,
                                                    ir::TargetInfo target,
                                                    bool mutable_entry,
                                                    bool private_state,
                                                    bool collect) {
    region_windows.clear();
    const bool function = scope == ir::Scope::Function && !mutable_entry;
    if (scope == ir::Scope::Function && mutable_entry)
        ++vm.stats.smc_function_single_block_attempts;
    if (function && private_state)
        target.features |= static_cast<std::uint64_t>(ir::TargetFeature::ExternalEntries);
    const frontend::ReadyEntryLookup ready =
            private_state
                    ? frontend::ReadyEntryLookup{&vm.Cache(),
                                                 [](void* raw, std::uint64_t pc) noexcept {
                                                     return bool(static_cast<CodeCache*>(raw)->Find(
                                                             pc));
                                                 }}
                    : frontend::ReadyEntryLookup{};
    auto* timing = vm.options.decode_timing ? &vm.stats.compile_decode : nullptr;
    auto* sources = collect ? &source_spans : nullptr;
    auto* after = vm.offline_output ? &continuations : nullptr;
    const auto pc = vm.PC();
    const frontend::BlockWindow block{pc, vm.memory.CodeAt(pc)};
    std::span<const frontend::BlockWindow> windows{&block, 1};
    if (function) {
        for (const auto& region : vm.memory.Regions())
            if ((region.protection & (PROT_READ | PROT_EXEC)) == (PROT_READ | PROT_EXEC) &&
                vm.memory.ClassifySource(region.address) == SourceMode::Immutable) {
                const auto bytes = vm.memory.CodeAt(region.address);
                if (!bytes.empty()) region_windows.push_back({region.address, bytes});
            }
        windows = region_windows;
    }
    if (vm.ISA() == GuestISA::Arm64) {
        const ::swift::arm64::StateLayout layout;
        if (function)
            return ::swift::arm64::BuildFunctionIR(windows,
                                                   pc,
                                                   layout,
                                                   vm.options.function_limits,
                                                   target,
                                                   nullptr,
                                                   timing,
                                                   &reusable,
                                                   ready,
                                                   sources,
                                                   after);
        return ::swift::arm64::BuildIR(windows,
                                       scope,
                                       layout,
                                       target,
                                       nullptr,
                                       timing,
                                       mutable_entry,
                                       &reusable,
                                       sources,
                                       after);
    }
    using State = x86::GuestState;
    ::swift::x86::StateLayout layout;
    layout.gprs_offset = offsetof(State, gprs);
    layout.pc_offset = offsetof(State, pc);
    layout.fprs_offset = offsetof(State, xmm);
    layout.mxcsr_offset = offsetof(State, mxcsr);
    layout.x87_offset = offsetof(State, x87);
    layout.x87_ip_offset = offsetof(State, x87_ip);
    layout.x87_dp_offset = offsetof(State, x87_dp);
    layout.x87_opcode_offset = offsetof(State, x87_opcode);
    layout.ymm_high_offset = offsetof(State, ymm_high);
    layout.fs_base_offset = offsetof(State, fs_base);
    layout.gs_base_offset = offsetof(State, gs_base);
    layout.cpu_query = reinterpret_cast<std::uintptr_t>(&x86::QueryGuestCpu);
    layout.system_query = reinterpret_cast<std::uintptr_t>(&x86::QueryGuestSystem);
    layout.runtime_state_services = true;
    if (function)
        return ::swift::x86::BuildFunctionIR(windows,
                                             pc,
                                             layout,
                                             vm.options.function_limits,
                                             target,
                                             nullptr,
                                             timing,
                                             &reusable,
                                             ready,
                                             x86::BaselineGuestCpu,
                                             sources,
                                             after);
    return ::swift::x86::BuildIR(windows,
                                 scope,
                                 layout,
                                 target,
                                 nullptr,
                                 timing,
                                 mutable_entry,
                                 &reusable,
                                 x86::BaselineGuestCpu,
                                 sources,
                                 after);
}

}  // namespace swift::runtime::execution
