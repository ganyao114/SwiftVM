#include "emit.h"

#include <algorithm>

namespace swift::runtime::backend::x64 {
namespace {
void ClearCode(backend::CompileResult& result) noexcept {
    result.words.clear();
    result.faults.clear();
    result.links.clear();
    result.entries.clear();
    result.relocations.clear();
    result.relocatable = false;
    result.has_indirect_dispatch = false;
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
    if (options.linked_flags && (!options.local_flags || !options.dispatcher ||
                                 !options.exit_handler || options.flags_offset > INT32_MAX - 8)) {
        result.error = CompileError::UnsupportedOperand;
        return;
    }
    options.linked_flags &= options.local_flags && options.dispatcher && options.exit_handler;
    options.local_flags &= options.flags_offset <= INT32_MAX - 8;
    options.linked_flags &= options.local_flags;
    result.relocatable = options.relocatable;
    if (program.ScopeKind() == ir::Scope::Block)
        AllocateBlock(program, allocation, options.pinned_uniform_slots, &options);
    else
        AllocateFunction(program, allocation, options.pinned_uniform_slots, &options);
    if (!allocation) {
        result.relocatable = false;
        result.error = allocation.error;
        result.node = allocation.node;
        result.block = allocation.block;
        return;
    }
    if (options.local_flags && allocation.flags_slot > 0xffffff) {
        if (options.linked_flags) {
            result.error = CompileError::FrameTooLarge;
            result.node = {program.Blocks()[program.EntryBlock().value].first_node};
            result.block = program.EntryBlock();
            ClearCode(result);
            return;
        }
        options.local_flags = options.linked_flags = false;
        if (program.ScopeKind() == ir::Scope::Block)
            AllocateBlock(program, allocation, options.pinned_uniform_slots, &options);
        else
            AllocateFunction(program, allocation, options.pinned_uniform_slots, &options);
        if (!allocation) {
            result.error = allocation.error;
            result.node = allocation.node;
            result.block = allocation.block;
            ClearCode(result);
            return;
        }
    }
    result.spill_slots = allocation.spill_bytes / 8;
    result.words.reserve(std::max<std::size_t>(256, program.Operations().size() * 4 + 32));
    try {
        detail::Writer(program, allocation, result, options).Run();
    } catch (const Xbyak::Error&) {
        result.error = CompileError::CodeTooLarge;
        ClearCode(result);
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
}  // namespace swift::runtime::backend::x64
