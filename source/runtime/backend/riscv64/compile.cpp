#include "emit.h"

#include <algorithm>

namespace swift::runtime::backend::riscv64 {
namespace {
void ClearCode(backend::CompileResult& result) noexcept {
    result.words.clear();
    result.faults.clear();
    result.links.clear();
    result.entries.clear();
    result.relocations.clear();
    result.relocatable = false;
}
}  // namespace

void Compile(const ir::Program& program,
             CompileOptions options,
             backend::CompileResult& result,
             Allocation& allocation) {
    result.error = CompileError::None;
    result.node = {};
    result.block = {};
    result.frame_bytes = result.spill_slots = 0;
    ClearCode(result);
    result.relocatable = options.relocatable;
    if (program.ScopeKind() == ir::Scope::Block)
        AllocateBlock(program, allocation);
    else
        AllocateFunction(program, allocation);
    if (!allocation) {
        result.relocatable = false;
        result.error = allocation.error;
        result.node = allocation.node;
        result.block = allocation.block;
        return;
    }
    result.frame_bytes =
            allocation.spill_bytes + (options.dispatcher && options.exit_handler ? 0 : 112);
    result.spill_slots = allocation.spill_bytes / 8;
    result.words.reserve(std::max<std::size_t>(256, program.Operations().size() * 4 + 32));
    try {
        detail::Writer(program, allocation, result, options).Run();
    } catch (const detail::Rejected& error) {
        result.error = error.error;
        result.node = error.node;
        result.block = error.block;
        ClearCode(result);
    }
}

void Compile(const ir::Program& program, CompileOptions options, backend::CompileResult& result) {
    Allocation allocation;
    Compile(program, options, result, allocation);
}

backend::CompileResult Compile(const ir::Program& program, CompileOptions options) {
    backend::CompileResult result;
    Compile(program, options, result);
    return result;
}
}  // namespace swift::runtime::backend::riscv64
