#include "uniform_store_sink_pass.h"

#include <algorithm>
#include <vector>

#include "runtime/ir/opts/uniform_elimination_pass.h"

namespace swift::runtime::ir {

namespace {

struct PendingStore {
    Inst* inst{};
    Value value{};
    u32 offset{};
    u32 size{};
};

[[nodiscard]] bool Overlaps(u32 left_offset, u32 left_size, u32 right_offset, u32 right_size) {
    return left_offset < right_offset + right_size && right_offset < left_offset + left_size;
}

// A delayed store is still the only use that gives a faulting value producer a
// live interval. Deleting it must not strand a guest load, atomic, or helper:
// those operations remain executable for their side effects and the block
// allocator requires their result to have a use.
[[nodiscard]] bool SurvivesDCE(const Inst* def) {
    switch (def->GetOp()) {
        case OpCode::LoadMemory:
        case OpCode::LoadMemoryTSO:
        case OpCode::CompareAndSwap:
        case OpCode::CompareAndSwap128:
        case OpCode::AtomicExchange:
        case OpCode::AtomicFetchAdd:
        case OpCode::AtomicRMW:
        case OpCode::X87Op:
        case OpCode::CallLambda:
        case OpCode::CallLocation:
        case OpCode::CallDynamic:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool ChainWouldStrandASurvivor(const Value& root, const Inst* store) {
    StackVector<Inst*, 16> work{};
    u32 visited{};
    constexpr u32 kMaxVisited = 64;
    if (auto* def = root.Def()) {
        work.push_back(def);
    }
    while (!work.empty()) {
        if (++visited > kMaxVisited) {
            return true;
        }
        auto* def = work.back();
        work.pop_back();
        if (SurvivesDCE(def)) {
            return true;
        }
        if (const_cast<Inst*>(def)->GetUses(false) > 1) {
            continue;
        }
        for (auto& value : const_cast<Inst*>(def)->GetValues()) {
            if (auto* next = value.Def(); next && next != store) {
                work.push_back(next);
            }
        }
    }
    return false;
}

[[nodiscard]] bool IsFaultOrObservationPoint(OpCode op) {
    switch (op) {
        // Every guest memory operation can enter HandleFault or the SMC
        // sigreturn path. The context must be current before it executes.
        case OpCode::LoadMemory:
        case OpCode::StoreMemory:
        case OpCode::LoadMemoryTSO:
        case OpCode::StoreMemoryTSO:
        case OpCode::MemoryCopy:
        case OpCode::MemoryCopyTSO:
        case OpCode::CompareAndSwap:
        case OpCode::CompareAndSwap128:
        case OpCode::CheckMemoryAlignment:
        case OpCode::AtomicExchange:
        case OpCode::AtomicFetchAdd:
        case OpCode::AtomicRMW:
            return true;

        // Helpers are opaque. XSAVE/XRSTOR are CallLambda instances that
        // directly dereference ThreadContext64, so the conservative call-wide
        // rule is also the required architectural boundary.
        case OpCode::CallLambda:
        case OpCode::CallLocation:
        case OpCode::CallDynamic:
        case OpCode::X87Op:
        case OpCode::GetUniformAddress:
            return true;

        // These operations expose or mutate state outside the ordinary
        // Load/StoreUniform stream; any converted Get/SetHostFPR is a flush
        // boundary.
        case OpCode::UniformBarrier:
        case OpCode::GetHostGPR:
        case OpCode::GetHostFPR:
        case OpCode::SetHostGPR:
        case OpCode::SetHostFPR:
        case OpCode::SetLocation:
        case OpCode::GetLocation:
            return true;

        // Unit-local branches are observation points because a store may not
        // be moved from one arm to the other. Push/PopRSB affect control flow
        // outside the ordinary straight-line instruction stream as well.
        case OpCode::Goto:
        case OpCode::NotGoto:
        case OpCode::BindLabel:
        case OpCode::PushRSB:
        case OpCode::PopRSB:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool TryForwardLoad(Inst& load, const std::vector<PendingStore>& pending) {
    const auto uniform = load.GetArg<Uniform>(0);
    const u32 load_offset = uniform.GetOffset();
    const u32 load_size = GetValueSizeByte(uniform.GetType());
    const PendingStore* source{};
    u32 source_byte{};

    for (u32 byte = 0; byte < load_size; ++byte) {
        const u32 absolute = load_offset + byte;
        const PendingStore* latest{};
        for (const auto& store : pending) {
            if (absolute >= store.offset && absolute < store.offset + store.size) {
                latest = &store;
            }
        }
        if (!latest) {
            return false;
        }
        const u32 within = absolute - latest->offset;
        if (!source) {
            source = latest;
            source_byte = within;
        } else if (source != latest || within != source_byte + byte) {
            return false;
        }
    }

    const auto load_type = uniform.GetType();
    const auto source_type = source->value.Type();
    const bool load_is_float = IsFloatValueType(load_type);
    const bool source_is_float = IsFloatValueType(source_type);

    // Exact views are zero-cost aliases. Partial low vector views are aliases
    // too: V32/V64 consumers name only the low lane and stay in the FPR class.
    if (source_byte == 0 && load_is_float && source_is_float && load_size <= source->size) {
        load.Reset();
        load.BitCast(source->value).SetReturn(load_type);
        return true;
    }
    if (source_byte == 0 && load_size == source->size && load_is_float == source_is_float) {
        load.Reset();
        load.BitCast(source->value).SetReturn(load_type);
        return true;
    }

    // XmmLo/XmmHi are scalar U64 views of the same architectural V128 slot.
    // UMOV materializes the requested lane without touching ThreadContext64.
    if (source_type == ValueType::V128 && load_type == ValueType::U64 && load_size == sizeof(u64) &&
        (source_byte == 0 || source_byte == sizeof(u64))) {
        load.Reset();
        load.VecExtract64(source->value, Imm{static_cast<u32>(source_byte / sizeof(u64))})
                .SetReturn(load_type);
        return true;
    }
    return false;
}

}  // namespace

void UniformStoreSinkPass::Run(Block* block, const UniformInfo& info, HIRFunction* hir_function) {
    if (!block || info.xmm_uniform_ranges.empty()) {
        return;
    }

    std::vector<Inst*> original;
    original.reserve(block->GetInstList().size());
    for (auto& inst : block->GetInstList()) {
        original.push_back(&inst);
    }

    std::vector<PendingStore> pending;
    pending.reserve(16);

    auto flush = [&](Inst* before, u32 observed_offset, u32 observed_size, bool all) {
        if (pending.empty()) {
            return;
        }

        std::vector<bool> selected(pending.size(), false);
        bool any{};
        for (size_t index = 0; index < pending.size(); ++index) {
            selected[index] = all || Overlaps(pending[index].offset,
                                              pending[index].size,
                                              observed_offset,
                                              observed_size);
            any |= selected[index];
        }
        if (!any) {
            return;
        }

        // Determine dead writes before relinking. A store is removable only
        // when later selected writes cover every byte and its producer chain
        // remains allocatable after DCE.
        std::vector<u8> covered(info.uniform_size, 0);
        std::vector<bool> victim(pending.size(), false);
        for (size_t reverse = pending.size(); reverse-- > 0;) {
            if (!selected[reverse]) {
                continue;
            }
            const auto& store = pending[reverse];
            bool fully_covered = true;
            for (u32 byte = 0; byte < store.size; ++byte) {
                fully_covered &= covered[store.offset + byte] != 0;
            }
            victim[reverse] = fully_covered && !ChainWouldStrandASurvivor(store.value, store.inst);
            for (u32 byte = 0; byte < store.size; ++byte) {
                covered[store.offset + byte] = 1;
            }
        }

        // Relink first so both block-mode deletion and HIRFunction::EraseInst
        // can use their normal list-aware destruction paths.
        for (size_t index = 0; index < pending.size(); ++index) {
            if (!selected[index]) {
                continue;
            }
            if (before) {
                block->InsertBefore(pending[index].inst, before);
            } else {
                block->AppendInst(pending[index].inst);
            }
        }

        std::vector<PendingStore> remaining;
        remaining.reserve(pending.size());
        for (size_t index = 0; index < pending.size(); ++index) {
            if (!selected[index]) {
                remaining.push_back(pending[index]);
                continue;
            }
            if (!victim[index]) {
                continue;
            }
            if (hir_function) {
                hir_function->EraseInst(block, pending[index].inst);
            } else {
                block->DestroyInst(pending[index].inst);
            }
        }
        pending = std::move(remaining);
    };

    auto flush_all = [&](Inst* before) { flush(before, 0, 0, true); };

    for (auto* inst : original) {
        const auto op = inst->GetOp();
        if (op == OpCode::StoreUniform) {
            const auto uniform = inst->GetArg<Uniform>(0);
            const auto value = inst->GetArg<Value>(1);
            const u32 offset = uniform.GetOffset();
            const u32 size = GetValueSizeByte(value.Type());
            if (offset + size <= info.uniform_size && info.IsXmmUniformRange(offset, size)) {
                block->RemoveInst(inst);
                pending.push_back({inst, value, offset, size});
                continue;
            }
        } else if (op == OpCode::LoadUniform) {
            const auto uniform = inst->GetArg<Uniform>(0);
            const u32 offset = uniform.GetOffset();
            const u32 size = GetValueSizeByte(uniform.GetType());
            if (offset + size <= info.uniform_size && info.IsXmmUniformRange(offset, size)) {
                if (!TryForwardLoad(*inst, pending)) {
                    flush(inst, offset, size, false);
                }
                continue;
            }
        }

        if (IsFaultOrObservationPoint(op)) {
            flush_all(inst);
        }
    }

    // Terminals are the unit boundary. Runtime signal injection is sampled
    // only after a translated unit returns, so this final materialization makes
    // the context read by rt_sigframe construction current as well.
    flush_all(nullptr);
}

void UniformStoreSinkPass::Run(HIRFunction* hir_function, const UniformInfo& info) {
    if (!hir_function) {
        return;
    }
    for (auto* hir_block : hir_function->GetHIRBlocks()) {
        if (hir_block) {
            Run(hir_block->GetBlock(), info, hir_function);
        }
    }
}

}  // namespace swift::runtime::ir
