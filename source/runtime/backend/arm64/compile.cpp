#include "emit.h"
#include "allocation_detail.h"

#include <algorithm>

namespace swift::runtime::backend::arm64 {
namespace {
void ClearCode(backend::CompileResult& result) noexcept {
    result.words.clear();
    result.faults.clear();
    result.links.clear();
    result.entries.clear();
}
}  // namespace

void Compile(const ir::Program& program,
             CompileOptions options,
             CompileResult& result,
             Allocation& allocation) {
    result.error = CompileError::None;
    result.node = {};
    result.block = {};
    result.frame_bytes = result.spill_slots = 0;
    ClearCode(result);
    try {
        if (program.ScopeKind() == ir::Scope::Block)
            AllocateBlock(program, allocation, &options);
        else
            AllocateFunction(program, allocation, &options);
        if (!allocation) {
            result.error = allocation.error;
            result.node = allocation.node;
            result.block = allocation.block;
            return;
        }
        result.frame_bytes = (options.dispatcher || options.exit_handler ? 0 : 16) +
                             ((allocation.spill_bytes + 15) & ~15u);
        result.spill_slots = allocation.spill_bytes / 8;
        // A small unit still has real entry/exit, fault and link words. One KiB
        // avoids repeated growth for those fixed costs; large units keep the hint.
        result.words.reserve(std::max(std::size_t{256}, program.Operations().size() * 2 + 32));
        if (!detail::Writer(program, allocation, result, options).Run()) {
            // No slice has been published. Keep the actual output capacity
            // and Allocation; fresh Writer state rebuilds every word-based
            // branch, fault and link location exactly once in the old form.
            ClearCode(result);
            (void)detail::Writer(program, allocation, result, options, true).Run();
        }
    } catch (const detail::Rejected& failure) {
        result.error = failure.error;
        result.node = failure.node;
        result.block = failure.block;
        ClearCode(result);
    } catch (...) {
        result.error = CompileError::InvalidProgram;
        result.node = {};
        result.block = {};
        result.frame_bytes = result.spill_slots = 0;
        ClearCode(result);
        detail::ResetAllocation(allocation, CompileError::InvalidProgram);
        throw;
    }
}

void Compile(const ir::Program& program, CompileOptions options, CompileResult& result) {
    Allocation allocation;
    Compile(program, options, result, allocation);
}

CompileResult Compile(const ir::Program& program, CompileOptions options) {
    CompileResult result;
    Compile(program, options, result);
    return result;
}

}  // namespace swift::runtime::backend::arm64
