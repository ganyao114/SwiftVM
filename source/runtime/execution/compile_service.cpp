#include "compile_service.h"
#include "vm.h"
#include "timing.h"
#include "disk_cache.h"
#include "runtime/ir/optimize.h"

#include <algorithm>
#include "base/format.h"
#include <limits>
#include <sys/mman.h>

namespace swift::runtime::execution {
namespace front = frontend;

namespace {
using u64 = std::uint64_t;

}  // namespace

NativeCode CompileService::Compile(ir::Scope scope) {
    auto& stats = vm.stats;
    auto& options = vm.options;
    auto& memory = vm.memory;
    auto& pc = vm.PC();
    auto& faults = vm.thread.faults;
    diagnostic = {};
    diagnostic.scope = scope;
    diagnostic.pc = pc;
    last = nullptr;
    entry_pc = pc;
    failure = CompileFailure::None;
    continuations.clear();
    if (vm.disk_cache && vm.disk_cache->Enabled()) {
        RuntimeTimer load{stats.cache_load_ns};
        if (const auto* saved = vm.disk_cache->Find(pc, vm.thread.fp_mode)) {
            try {
                if (auto restored = Restore(*saved); restored.entry) {
                    ++stats.disk_cache_hits;
                    return restored;
                }
            } catch (const swift::RuntimeError& error) {
                // A rejected unpublished file record is a miss. Mapping,
                // protection and synchronization failures remain host errors.
                if (error.Kind() != ErrorKind::InvalidArgument) throw;
            }
            ++stats.disk_cache_rejects;
            vm.DisablePersistence();
        }
        ++stats.disk_cache_misses;
    }
    entry_pc = pc;
    const bool collect = vm.CollectCode();
    ++stats.compile_attempts;
    scope == ir::Scope::Function ? ++stats.function_attempts : ++stats.block_attempts;
    // The VM's guest mappings and owned state are disjoint regardless of
    // whether this backend keeps architectural fields in physical registers.
    const bool private_guest_state = faults.registered;
    auto target = vm.backend.target;
    if (faults.registered) target.features |= static_cast<u64>(ir::TargetFeature::FaultLocations);
    if (private_guest_state)
        target.features |= static_cast<u64>(ir::TargetFeature::PrivateGPRState) |
                           static_cast<u64>(ir::TargetFeature::PrivateVectorState);
    const auto source_mode = memory.ClassifySource(pc);
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
    front::BuildResult built;
    {
        RuntimeTimer front_time{stats.frontend_ns};
        built = BuildFrontend(scope, target, mutable_entry, private_guest_state, collect);
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
                ir::UniformPolicy{
                        .flags = ir::UniformRange{vm.layout.flags_offset, sizeof(std::uint64_t)},
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
                        pc, vm.thread.fp_mode, faults.registered, private_guest_state, collect});
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
    auto code = Publish(compiled, std::move(compiled.faults));
    // Mutable entries remain one source block, even with Function allocation.
    if (mutable_entry) {
        if (collect) {
            for (const auto& source : source_spans) vm.smc.Arm(source.pc, source.size);
        } else
            vm.smc.Arm(pc, built.decoded_bytes);
    }
    if (collect) vm.RecordUnit(scope, compiled, code, source_spans);
    // Keep only the actual working buffers; no native metadata borrows
    // these. Oversized units finish normally without retaining their peak.
    if (built.program.ReservedBytes() <= 8u * 1024 * 1024) reusable = std::move(built.program);

    return code;
}

NativeCode CompileService::Publish(const backend::CompileResult& compiled,
                                   std::vector<backend::FaultSite> faults) {
    auto& stats = vm.stats;
    RuntimeTimer publish{stats.publication_ns};
    auto code = vm.heap.Publish(
            compiled.words,
            std::move(faults),
            compiled.links,
            [](void* opaque, u64 pc, std::uintptr_t unit) noexcept -> std::uintptr_t {
                auto& owner = *static_cast<VM*>(opaque);
                if (const auto ready = owner.Cache().Find(pc))
                    return reinterpret_cast<std::uintptr_t>(ready);
                return pc == owner.compiler.EntryPC() ? unit : 0;
            },
            &vm,
            compiled.relocations);
    ++stats.code_units_published;
    if (vm.options.decode_timing) stats.fault_sites += code.faults.size();
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
    return code;
}

NativeCode CompileService::Restore(const CodeUnit& unit, bool sources_verified) {
    if (!sources_verified && !vm.SourcesMatch(unit.sources)) return {};
    entry_pc = unit.pc;
    auto code = Publish(unit.code, unit.code.faults);
    for (const auto& source : unit.sources)
        if (source.mode == static_cast<std::uint8_t>(SourceMode::WatchedBlock))
            vm.smc.Arm(source.span.pc, source.span.size);
    last = &unit.code;
    return code;
}

void CompileService::Release() noexcept {
    reusable = {};
    last = nullptr;
    std::vector<std::uint64_t>{}.swap(optimizer_rows);
    std::vector<SourceSpan>{}.swap(source_spans);
    std::vector<std::uint64_t>{}.swap(continuations);
    if (vm.backend.release) vm.backend.release(vm.backend.context);
}

std::string CompileService::DescribeFailure() const {
    if (diagnostic.message) return diagnostic.message;
    const auto scope = diagnostic.scope == ir::Scope::Function ? "function" : "block";
    if (diagnostic.stage == CompileStage::Frontend)
        return swift::Format(
                "guest frontend rejected pc={:#x} scope={} error={} ir_error={} emitted={} "
                "bytes={}",
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
