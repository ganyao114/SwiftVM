#include "passes.h"
#include "runtime/compile/analysis/guest_state.h"
#include "runtime/compile/workspace.h"
#include "runtime/ir/hir_builder.h"

#include <algorithm>
#include <vector>


namespace swift::runtime::compile {
using namespace ir;

namespace {

struct PendingStore {
    enum class Action : u8 { Pending, Publish, Erase };
    const Inst* inst{};
    u32 offset{};
    u32 size{};
    Action action{Action::Pending};
    bool detached{};
};

[[nodiscard]] bool Overlaps(u32 left_offset, u32 left_size, u32 right_offset, u32 right_size) {
    return left_offset < right_offset + right_size && right_offset < left_offset + left_size;
}

[[nodiscard]] bool IsPublicationBoundary(const Inst& inst) {
    constexpr auto effects = IREffect::MayFault | IREffect::Unknown |
            IREffect::Call | IREffect::ControlFlow;
    if ((inst.GetEffects() & effects) != IREffect::None) return true;
    switch (inst.GetOp()) {
        // Mapped accesses and address exposure are conservative boundaries
        // for this mutable logical stream. Memory-only barriers do not read
        // guest state; UniformBarrier explicitly does.
        case OpCode::GetUniformAddress:
        case OpCode::UniformBarrier:
        case OpCode::GetHostGPR:
        case OpCode::GetHostFPR:
        case OpCode::SetHostGPR:
        case OpCode::SetHostFPR:
        case OpCode::SetLocation:
        case OpCode::GetLocation:
        case OpCode::PushRSB:
        case OpCode::PopRSB:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool TryForwardLoad(compile::RewriteContext& rewrite,
                                  Block* block,
                                  const Inst& load,
                                  const std::vector<PendingStore>& pending) {
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

    const auto read = ClassifyGuestStateRead(source->inst->GetArg<Value>(1), source_byte, uniform.GetType(),
                                             GuestStateReadPolicy::PendingStore);
    if (!read.Found()) {
        return false;
    }
    rewrite.Change(block, &load, [&](Inst& load) {
        load.Reset();
        if (read.kind == GuestStateReadKind::Extract) {
            load.BitExtract(read.value, Imm{read.offset * 8u}, Imm{load_size * 8u})
                    .SetReturn(uniform.GetType());
        } else if (read.kind == GuestStateReadKind::VectorLane64) {
            load.VecExtract64(read.value, Imm{static_cast<u32>(read.offset / sizeof(u64))})
                    .SetReturn(uniform.GetType());
        } else {
            load.BitCast(read.value).SetReturn(uniform.GetType());
        }
    });
    return true;
}

}  // namespace

static void SinkBlockStores(RewriteContext& rewrite, Block* block,
                            const GuestStateLayout& info) {
    if (!block || info.xmm_uniform_ranges.empty()) {
        return;
    }

    using Action = PendingStore::Action;
    std::vector<PendingStore> pending;
    std::vector<u8> covered;
    auto flush = [&](const Inst* before, u32 observed_offset, u32 observed_size, bool all) {
        if (pending.empty()) return;

        bool any{};
        u32 coverage_begin = info.uniform_size, coverage_end{};
        for (auto& store : pending) {
            coverage_begin = std::min(coverage_begin, store.offset);
            coverage_end = std::max(coverage_end, store.offset + store.size);
            const bool selected = all || Overlaps(store.offset, store.size,
                                                   observed_offset, observed_size);
            store.action = selected ? Action::Publish : Action::Pending;
            any |= selected;
        }
        if (!any) {
            return;
        }

        // Only later selected writes can kill pending bytes. An actual
        // observation flushes them before the observing instruction; no
        // transient recipe may replace an earlier store with a future value.
        // Include all pending ranges: partial flush dependency closure may
        // reach beyond the initially selected stores. Unrelated uniform bytes
        // need no coverage storage or initialization.
        covered.assign(coverage_end - coverage_begin, 0);
        for (size_t reverse = pending.size(); reverse-- > 0;) {
            auto& store = pending[reverse];
            const u32 byte_offset = store.offset - coverage_begin;
            if (store.action == Action::Pending) {
                // An earlier overlapping write cannot be left to execute
                // after a selected later write, even outside the read range.
                // The same reverse coverage walk closes transitive overlaps.
                for (u32 byte = 0; byte < store.size; ++byte) {
                    if (covered[byte_offset + byte]) {
                        store.action = Action::Publish;
                        break;
                    }
                }
                if (store.action == Action::Pending) continue;
            }
            bool fully_covered = true;
            for (u32 byte = 0; byte < store.size; ++byte) {
                fully_covered &= covered[byte_offset + byte] != 0;
            }
            // Side effects keep the producer alive through shared DCE. Both
            // allocators assign surviving definitions their own destination,
            // including when all stores consuming the result are dead.
            if (fully_covered) store.action = Action::Erase;
            for (u32 byte = 0; byte < store.size; ++byte) {
                covered[byte_offset + byte] = 1;
            }
        }

        // Relink stores with their original operands. Deletion uses the
        // same ownership boundary below.
        for (size_t index = 0; index < pending.size(); ++index) {
            if (pending[index].action == Action::Pending) {
                continue;
            }
            if (before) {
                block->InsertBefore(pending[index].inst, before);
            } else {
                block->AppendInst(pending[index].inst);
            }
            // A later erase notification can throw after deleting the
            // node. Recovery owns only still-detached rows.
            pending[index].detached = false;
        }

        // Retain the unselected stores in source order. Writes only target
        // already visited rows; no later decision or erase is overwritten.
        size_t remaining{};
        for (size_t index = 0; index < pending.size(); ++index) {
            if (pending[index].action == Action::Pending) {
                if (remaining != index) {
                    pending[remaining] = pending[index];
                    pending[index].detached = false;
                }
                ++remaining;
            } else if (pending[index].action == Action::Erase) {
                rewrite.Erase(block, pending[index].inst);
            }
        }
        pending.erase(pending.begin() + remaining, pending.end());
    };

    auto flush_all = [&](const Inst* before) { flush(before, 0, 0, true); };

    // Eligibility to sink a whole access does not imply disjointness. A
    // direct access spanning an XMM edge or gap still observes/overwrites
    // pending bytes there. Disjoint accesses need no pending/coverage scan.
    const auto observe_access = [&](const Inst* before, u32 offset, u32 size) {
        if (pending.empty()) return;
        for (const auto& range : info.xmm_uniform_ranges) {
            if (offset < range.end && range.begin < offset + size) {
                flush(before, offset, size, false);
                return;
            }
        }
    };

    // Advance before detaching the current store. Flush only relinks/deletes
    // earlier pending stores, behind this cursor; it never erases its target
    // or a future instruction. Newly inserted stores must not be visited again.
    const auto& instructions = block->GetInstList();
    try {
        for (auto cursor = instructions.begin(); cursor != instructions.end();) {
            const auto* inst = &*cursor++;
            const auto op = inst->GetOp();
            if (op == OpCode::StoreUniform) {
                const auto uniform = inst->GetArg<Uniform>(0);
                const auto value = inst->GetArg<Value>(1);
                const u32 offset = uniform.GetOffset();
                const u32 size = GetValueSizeByte(value.Type());
                if (offset > info.uniform_size || size > info.uniform_size - offset) {
                    flush_all(inst);
                    continue;
                }
                if (info.IsXmmRange(offset, size)) {
                    if (pending.capacity() == 0) pending.reserve(16);
                    pending.push_back({inst, offset, size});
                    block->RemoveInst(inst);
                    pending.back().detached = true;
                    continue;
                }
                observe_access(inst, offset, size);
            } else if (op == OpCode::LoadUniform) {
                const auto uniform = inst->GetArg<Uniform>(0);
                const u32 offset = uniform.GetOffset();
                const u32 size = GetValueSizeByte(uniform.GetType());
                if (offset > info.uniform_size || size > info.uniform_size - offset) {
                    flush_all(inst);
                    continue;
                }
                if (info.IsXmmRange(offset, size)) {
                    if (!TryForwardLoad(rewrite, block, *inst, pending)) {
                        flush(inst, offset, size, false);
                    }
                    continue;
                }
                observe_access(inst, offset, size);
            }

            if (IsPublicationBoundary(*inst)) {
                flush_all(inst);
            }
        }

        // Terminals are the unit boundary. Runtime signal injection is sampled
        // only after a translated unit returns, so this final materialization makes
        // the context read by rt_sigframe construction current as well.
        flush_all(nullptr);
    } catch (...) {
        // The failed input must be discarded. Return every still-detached
        // instruction to its owning Block so ordinary teardown can reclaim
        // it; preserve registered uses and do not replay uncommitted edits.
        // Intrusive insertion does not allocate or validate operand types.
        for (auto& store : pending) {
            if (!store.detached) continue;
            if (instructions.empty()) block->InsertBefore(store.inst, nullptr);
            else block->InsertAfter(store.inst, &instructions.back());
            store.detached = false;
        }
        throw;
    }
}

void SinkStateStores(RewriteContext& rewrite, const GuestStateLayout& info) {
    for (auto* block : rewrite.Blocks()) SinkBlockStores(rewrite, block, info);
}

}  // namespace swift::runtime::compile
