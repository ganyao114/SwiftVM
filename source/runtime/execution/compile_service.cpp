#include "compile_service.h"
#include "vm.h"
#include "timing.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/compile/optimize.h"
#include "runtime/frontend/x86/build_ir.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <sys/mman.h>

namespace swift::runtime::execution {
namespace front = ::swift::x86;
namespace {
using u64 = std::uint64_t;
using x86::GuestState;
const char* Name(ir::OpCode opcode) {
    switch (opcode) {
#define INST(name, ...)                                                                            \
    case ir::OpCode::name:                                                                         \
        return #name;
#include "runtime/ir/opcodes.inc"
#undef INST
        default:
            return "invalid";
    }
}
}  // namespace
NativeCode CompileService::Compile(ir::Scope scope) {
    auto& stats = vm.stats;
    auto& options = vm.options;
    auto& memory = vm.memory;
    auto& state = vm.thread.state;
    auto& faults = vm.thread.faults;
    auto& native = vm.thread.native;
    auto& diagnostic = vm.thread.diagnostic;
    last = nullptr;
    failure = CompileFailure::None;
    ++stats.compile_attempts;
    scope == ir::Scope::Function ? ++stats.function_attempts : ++stats.block_attempts;
    front::StateLayout layout;
    layout.gprs_offset = offsetof(GuestState, gprs);
    layout.pc_offset = offsetof(GuestState, pc);
    layout.stride = 8;
    layout.fprs_offset = offsetof(GuestState, xmm);
    layout.mxcsr_offset = offsetof(GuestState, mxcsr);
    layout.x87_offset = offsetof(GuestState, x87);
    layout.x87_ip_offset = offsetof(GuestState, x87_ip);
    layout.x87_dp_offset = offsetof(GuestState, x87_dp);
    layout.x87_opcode_offset = offsetof(GuestState, x87_opcode);
    layout.ymm_high_offset = offsetof(GuestState, ymm_high);
    layout.fs_base_offset = offsetof(GuestState, fs_base);
    layout.gs_base_offset = offsetof(GuestState, gs_base);
    layout.cpu_query = reinterpret_cast<std::uintptr_t>(&x86::QueryGuestCpu);
    layout.system_query = reinterpret_cast<std::uintptr_t>(&x86::QueryGuestSystem);
    layout.runtime_state_services = true;
    const bool private_guest_state =
            faults.registered && native.pinned_uniform_offset != UINT32_MAX;
    auto target = vm.backend.target;
    if (faults.registered) target.features |= static_cast<u64>(ir::TargetFeature::FaultLocations);
    if (private_guest_state)
        target.features |= static_cast<u64>(ir::TargetFeature::PrivateGPRState) |
                           static_cast<u64>(ir::TargetFeature::PrivateVectorState);
    const auto source_mode = memory.ClassifySource(state.pc);
    if (!source_mode) {
        diagnostic = "guest instruction source is not readable/executable";
        failure = CompileFailure::InvalidInput;
        return {};
    }
    if (*source_mode == SourceMode::Volatile) {
        diagnostic = "volatile guest executable backing is unsupported for persistent JIT";
        failure = CompileFailure::Unsupported;
        return {};
    }
    const bool mutable_entry = *source_mode == SourceMode::WatchedBlock;
    const bool mutable_function = scope == ir::Scope::Function && mutable_entry;
    front::BuildResult built;
    {
        RuntimeTimer front_time{stats.frontend_ns};
        if (scope == ir::Scope::Function && !mutable_function) {
            if (private_guest_state)
                target.features |= static_cast<u64>(ir::TargetFeature::ExternalEntries);
            const auto ready =
                    private_guest_state
                            ? front::ReadyEntryLookup{&vm.Cache(),
                                                      [](void* cache, u64 pc) noexcept {
                                                          return bool(static_cast<CodeCache*>(cache)
                                                                              ->Find(pc));
                                                      }}
                            : front::ReadyEntryLookup{};
            std::vector<front::BlockWindow> windows;
            for (const auto& region : memory.Regions())
                if ((region.protection & (PROT_READ | PROT_EXEC)) == (PROT_READ | PROT_EXEC) &&
                    memory.ClassifySource(region.address) == SourceMode::Immutable) {
                    const auto bytes = memory.CodeAt(region.address);
                    if (!bytes.empty()) windows.push_back({region.address, bytes});
                }
            built = front::BuildFunctionIR(windows,
                                           state.pc,
                                           layout,
                                           options.function_limits,
                                           target,
                                           nullptr,
                                           options.decode_timing ? &stats.compile_decode : nullptr,
                                           &reusable,
                                           ready);
        } else {
            if (mutable_function) ++stats.smc_function_single_block_attempts;
            const front::BlockWindow window{state.pc, memory.CodeAt(state.pc)};
            built = front::BuildIR({&window, 1},
                                   scope,
                                   layout,
                                   target,
                                   nullptr,
                                   options.decode_timing ? &stats.compile_decode : nullptr,
                                   mutable_entry,
                                   &reusable);
        }
    }
    stats.emitted_instructions += built.instruction_count;
    stats.emitted_guest_bytes += built.decoded_bytes;
    if (!built) {
        char message[256];
        std::snprintf(message,
                      sizeof(message),
                      "x86 frontend rejected pc=0x%llx scope=%s error=%u ir_error=%u emitted=%zu "
                      "bytes=%zu",
                      static_cast<unsigned long long>(built.error_pc),
                      scope == ir::Scope::Function ? "function" : "block",
                      static_cast<unsigned>(built.error),
                      static_cast<unsigned>(built.ir_error),
                      built.instruction_count,
                      built.decoded_bytes);
        diagnostic = message;
        failure = built.error == front::BuildError::Unsupported ? CompileFailure::Unsupported
                                                                : CompileFailure::InvalidInput;
        return {};
    }
    if (options.decode_timing) {
        stats.ir_nodes += built.program.Operations().size();
        stats.operand_words += built.program.OperandArena().size();
    }
    if (options.optimize) {
        RuntimeTimer optimize{stats.optimize_ns};
        auto optimized = compile::Optimize(
                std::move(built.program),
                optimizer_rows,
                std::numeric_limits<std::size_t>::max(),
                compile::UniformPolicy{
                        .flags = compile::UniformRange{offsetof(GuestState, packed_flags),
                                                       sizeof(state.packed_flags)},
                        .guest_memory_disjoint = private_guest_state});
        if (options.decode_timing) {
            stats.optimized_erased += optimized.stats.erased;
            stats.optimized_uniform_forwarded += optimized.stats.uniform_forwarded;
            stats.optimized_folded += optimized.stats.folded;
        }
        built.program = std::move(optimized.program);
        if (optimizer_rows.capacity() > 8u * 1024 * 1024 / sizeof(std::uint64_t))
            std::vector<std::uint64_t>{}.swap(optimizer_rows);
        if (!optimized) {
            diagnostic = "IR optimization failed at the current compilation request";
            failure = CompileFailure::InvalidInput;
            return {};
        }
    }
    backend::CompileResult* output;
    {
        RuntimeTimer backend_time{stats.backend_ns};
        output = &vm.backend.compile(
                vm.backend.context,
                built.program,
                HostCompileRequest{
                        state.pc, vm.thread.fp_mode, faults.registered, private_guest_state});
    }
    auto& compiled = *output;
    last = output;
    if (options.decode_timing) {
        stats.spill_slots += compiled.spill_slots;
        stats.max_frame_bytes =
                std::max(stats.max_frame_bytes, static_cast<u64>(compiled.frame_bytes));
    }
    if (!compiled) {
        const auto nodes = built.program.Operations();
        const char* opcode = compiled.node.value < nodes.size()
                                     ? Name(nodes[compiled.node.value].opcode)
                                     : "none";
        char message[256];
        std::snprintf(
                message,
                sizeof(message),
                "native compiler rejected pc=0x%llx scope=%s error=%u node=%u block=%u opcode=%s",
                static_cast<unsigned long long>(state.pc),
                scope == ir::Scope::Function ? "function" : "block",
                static_cast<unsigned>(compiled.error),
                compiled.node.value,
                compiled.block.value,
                opcode);
        diagnostic = message;
        failure = compiled.error == backend::CompileError::InvalidProgram
                          ? CompileFailure::InvalidInput
                          : CompileFailure::Unsupported;
        return {};
    }
    NativeCode code;
    {
        RuntimeTimer publish{stats.publication_ns};
        code = vm.heap.Publish(
                compiled.words,
                std::move(compiled.faults),
                compiled.links,
                [](void* cache, u64 pc) noexcept -> std::uintptr_t {
                    return reinterpret_cast<std::uintptr_t>(
                            static_cast<CodeCache*>(cache)->Find(pc));
                },
                &vm.Cache());
        ++stats.code_units_published;
        stats.static_links +=
                std::count_if(compiled.links.begin(), compiled.links.end(), [](const auto& link) {
                    return link.kind == backend::LinkKind::Static;
                });
        if (options.decode_timing) stats.fault_sites += code.faults.size();
        stats.native_bytes += code.bytes;
        stats.peak_code_bytes =
                std::max(stats.peak_code_bytes, static_cast<u64>(vm.heap.ReservedBytes()));
        stats.peak_slabs = std::max(stats.peak_slabs, static_cast<u64>(vm.heap.Slabs()));
        stats.peak_dual_slabs =
                std::max(stats.peak_dual_slabs, static_cast<u64>(vm.heap.DualSlabs()));
        stats.code_wx_fallback |= vm.heap.Slabs() != vm.heap.DualSlabs();
        if (code.writable)
            for (const auto& link : compiled.links) {
                if (link.kind == backend::LinkKind::Dynamic)
                    vm.Cache().AddIndirectSite(code, link);
                else
                    vm.Cache().AddStaticLink(code, link);
            }
        // Function formation excludes mutable source regions, including RX
        // mappings with shared writers. Entry requests keep Function RA but
        // externalize every single-source-block edge.
        if (mutable_entry) vm.smc.Arm(state.pc, built.decoded_bytes);
    }
    // Keep only the actual working buffers; no native metadata borrows
    // these. Oversized units finish normally without retaining their peak.
    if (built.program.ReservedBytes() <= 8u * 1024 * 1024) reusable = std::move(built.program);

    return code;
}
void CompileService::Release() noexcept {
    reusable = {};
    last = nullptr;
    std::vector<std::uint64_t>{}.swap(optimizer_rows);
    if (vm.backend.release) vm.backend.release(vm.backend.context);
}
}  // namespace swift::runtime::execution
