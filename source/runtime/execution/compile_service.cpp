#include "compile_service.h"
#include "vm.h"
#include "timing.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/ir/optimize.h"
#include "runtime/frontend/x86/build_ir.h"

#include <algorithm>
#include "base/format.h"
#include <limits>
#include <sys/mman.h>

namespace swift::runtime::execution {
namespace front = ::swift::x86;

namespace {
using u64 = std::uint64_t;
using x86::GuestState;

}  // namespace

NativeCode CompileService::Compile(ir::Scope scope) {
    auto& stats = vm.stats;
    auto& options = vm.options;
    auto& memory = vm.memory;
    auto& state = vm.thread.native.state;
    auto& faults = vm.thread.faults;
    auto& native = vm.thread.native;
    diagnostic = {};
    diagnostic.scope = scope;
    diagnostic.pc = state.pc;
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
    // The VM's guest mappings and owned state are disjoint regardless of
    // whether this backend keeps architectural fields in physical registers.
    const bool private_guest_state = faults.registered;
    auto target = vm.backend.target;
    if (faults.registered) target.features |= static_cast<u64>(ir::TargetFeature::FaultLocations);
    if (private_guest_state)
        target.features |= static_cast<u64>(ir::TargetFeature::PrivateGPRState) |
                           static_cast<u64>(ir::TargetFeature::PrivateVectorState);
    const auto source_mode = memory.ClassifySource(state.pc);
    if (!source_mode) {
        diagnostic.message = "guest instruction source is not readable/executable";
        failure = CompileFailure::InvalidInput;
        return {};
    }
    if (*source_mode == SourceMode::Volatile) {
        diagnostic.message = "volatile guest executable backing is unsupported for persistent JIT";
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
        diagnostic.stage = CompileStage::Frontend;
        diagnostic.pc = built.error_pc;
        diagnostic.frontend_error = built.error;
        diagnostic.ir_error = built.ir_error;
        diagnostic.instructions = built.instruction_count;
        diagnostic.bytes = built.decoded_bytes;
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
        auto optimized = ir::Optimize(
                std::move(built.program),
                optimizer_rows,
                std::numeric_limits<std::size_t>::max(),
                ir::UniformPolicy{.flags = ir::UniformRange{offsetof(GuestState, packed_flags),
                                                            sizeof(state.packed_flags)},
                                  .guest_memory_disjoint = private_guest_state});
        if (options.decode_timing) {
            stats.optimized_erased += optimized.stats.erased;
            stats.optimized_uniform_forwarded += optimized.stats.uniform_forwarded;
            stats.optimized_folded += optimized.stats.folded;
            stats.optimized_copy_forwarded += optimized.stats.copy_forwarded;
            stats.optimized_final_walks_skipped += optimized.stats.final_walk_skipped;
        }
        built.program = std::move(optimized.program);
        if (optimizer_rows.capacity() > 8u * 1024 * 1024 / sizeof(std::uint64_t))
            std::vector<std::uint64_t>{}.swap(optimizer_rows);
        if (!optimized) {
            diagnostic.stage = CompileStage::Optimize;
            diagnostic.message = "IR optimization failed at the current compilation request";
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
        diagnostic.stage = CompileStage::Backend;
        diagnostic.backend_error = compiled.error;
        diagnostic.node = compiled.node;
        diagnostic.block = compiled.block;
        diagnostic.has_opcode = compiled.node.value < nodes.size();
        if (diagnostic.has_opcode) diagnostic.opcode = nodes[compiled.node.value].opcode;
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
                [](void* opaque, u64 pc, std::uintptr_t unit) noexcept -> std::uintptr_t {
                    auto& owner = *static_cast<VM*>(opaque);
                    if (const auto ready = owner.Cache().Find(pc))
                        return reinterpret_cast<std::uintptr_t>(ready);
                    return pc == owner.thread.native.state.pc ? unit : 0;
                },
                &vm);
        ++stats.code_units_published;
        if (options.decode_timing) stats.fault_sites += code.faults.size();
        stats.native_bytes += code.bytes;
        stats.peak_code_bytes =
                std::max(stats.peak_code_bytes, static_cast<u64>(vm.heap.ReservedBytes()));
        const auto slabs = vm.heap.Slabs(), dual_slabs = vm.heap.DualSlabs();
        stats.peak_slabs = std::max(stats.peak_slabs, static_cast<u64>(slabs));
        stats.peak_dual_slabs = std::max(stats.peak_dual_slabs, static_cast<u64>(dual_slabs));
        stats.code_wx_fallback |= slabs != dual_slabs;
        for (const auto& link : compiled.links) {
            if (link.kind == backend::LinkKind::Static) ++stats.static_links;
            if (!code.writable) continue;
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

std::string CompileService::DescribeFailure() const {
    if (diagnostic.message) return diagnostic.message;
    const auto scope = diagnostic.scope == ir::Scope::Function ? "function" : "block";
    if (diagnostic.stage == CompileStage::Frontend)
        return swift::Format(
                "x86 frontend rejected pc={:#x} scope={} error={} ir_error={} emitted={} bytes={}",
                diagnostic.pc,
                scope,
                static_cast<unsigned>(diagnostic.frontend_error),
                static_cast<unsigned>(diagnostic.ir_error),
                diagnostic.instructions,
                diagnostic.bytes);
    if (diagnostic.stage == CompileStage::Backend)
        return swift::Format(
                "native compiler rejected pc={:#x} scope={} error={} node={} block={} opcode={}",
                diagnostic.pc,
                scope,
                static_cast<unsigned>(diagnostic.backend_error),
                diagnostic.node.value,
                diagnostic.block.value,
                diagnostic.has_opcode ? ir::Name(diagnostic.opcode) : "none");
    return "native resolver did not produce a runnable entry";
}

}  // namespace swift::runtime::execution
