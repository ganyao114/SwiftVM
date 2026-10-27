#include "state_internal.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>

#include "base/logging.h"
#include "runtime/common/perf_stats.h"
#include "runtime/compile/analysis/guest_state.h"

namespace swift::runtime::compile {
using namespace ir;

// --- dead uniform store elimination ----------------------------------------
//
// The forward pass above forwards uniform *loads*; nothing removed a uniform
// *store* whose bytes are overwritten again before anybody reads them.  The x86
// front end emits such stores constantly: every flag-setting arithmetic
// instruction writes the carry-polarity byte (decoder.cc StorePolarity ->
// LoadImm + StoreUniform), and a guest register written twice in one block
// stores twice.  Measured over the 25 e2e guests plus the five bench kernels,
// StoreUniform was 11.4% of all emitted IR and 7.7% of all emitted host bytes.
//
// Backward observation differs from forward invalidation: a fault may expose
// guest state without changing it. Unknown effects, possible faults and explicit
// state exposure therefore stop this dead-store proof even when a forward load
// could still reuse its value. Beyond that:
//   * the walk is backward, starting with nothing known dead -- uniforms are
//     the guest context and every byte is live out of the block;
//   * a LoadUniform resurrects exactly the bytes it reads;
//   * a store is removed only when *every* byte it writes is already known to
//     be overwritten later.
// Nothing here is x86-specific: "a register write that another register write
// overwrites before any read" is dead in any guest ISA.

// Shared DCE retains side effects, and both allocators assign a destination
// to surviving zero-use definitions. The reverse scan preserves real stores
// at fault/state boundaries; no deferred carrier or SSA-use proof is needed.
void EliminateStateDeadStores(RewriteContext& rewrite, Block* block,
                                const GuestStateLayout& info,
                                const FeatureSet& features,
                                bool xmm_only) {
    PerfScope2 perf_dse{GetPerfStats2().uniform_dse};
    auto& inst_list = block->GetInstList();
    // killed[i] = byte i of the uniform buffer is overwritten later in this
    // block before any read.
    StackVector<u8, 0x100> killed{};
    killed.resize(info.uniform_size);
    std::fill(killed.begin(), killed.end(), u8{0});
    StackVector<const Inst*, 16> victims{};
    // Backward counterpart of the forward value intersection below. At a
    // BindLabel, `killed` describes writes after the merge and therefore
    // applies to both incoming paths. Keep that state for the taken edge, walk
    // the fallthrough edge, then intersect at its Goto/NotGoto: a byte is dead
    // before the branch only when it is overwritten on both successors.
    std::unordered_map<const Inst*, std::vector<u8>> label_killed;

    auto clear_all = [&] { std::fill(killed.begin(), killed.end(), u8{0}); };

    // Reverse home lookup for SetHost*/GetHost*: host register ->
    // (uniform offset, register size) from the same pin map the
    // forward pass converts with. Built lazily; -1 unknown,
    // -2 ambiguous (both stay barriers). Bound-backend decode
    // emits mapped stores directly, so dead-store elimination
    // must see through them or every early-mapped dead store
    // survives as a dead publication.
    struct HomeRange { u32 offset{}; u32 size{}; };
    std::optional<std::array<s32, 64>> home_offsets;
    std::array<u32, 64> home_sizes{};
    const auto home_range = [&](bool fpr, u32 reg) -> std::optional<HomeRange> {
        if (!home_offsets) {
            home_offsets.emplace();
            home_offsets->fill(-1);
            info.uniform_regs_map.ForEachValue([&](const swift::runtime::compile::GuestRegisterBinding& entry) {
                if (entry.Null()) {
                    return;
                }
                const u32 idx = entry.host_reg.is_fpr
                        ? 32 + entry.host_reg.fpr.id
                        : entry.host_reg.gpr.id;
                if (idx >= 64) {
                    return;
                }
                const auto off = static_cast<s32>(entry.uniform.GetOffset());
                if ((*home_offsets)[idx] == -1) {
                    (*home_offsets)[idx] = off;
                    home_sizes[idx] = GetValueSizeByte(entry.uniform.GetType());
                } else if ((*home_offsets)[idx] != off) {
                    (*home_offsets)[idx] = -2;
                }
            });
        }
        const u32 idx = fpr ? 32 + reg : reg;
        if (idx >= 64 || (*home_offsets)[idx] < 0) {
            return std::nullopt;
        }
        return HomeRange{static_cast<u32>((*home_offsets)[idx]), home_sizes[idx]};
    };

    for (auto it = inst_list.rbegin(); it != inst_list.rend(); ++it) {
        const Inst& inst = *it;
        // A fault can expose guest state even without a normal IR load of it.
        // Forward-value invalidation and backward-store observation differ:
        // an unchanged value may be forwarded across a memory access, while
        // an earlier publication cannot be discarded across its fault.
        if ((inst.GetEffects() & (IREffect::MayFault | IREffect::Unknown)) != IREffect::None ||
            inst.GetOp() == OpCode::GetUniformAddress ||
            inst.GetOp() == OpCode::UniformBarrier) {
            clear_all();
            continue;
        }
        switch (inst.GetOp()) {
            case OpCode::StoreUniform: {
                const auto uniform = inst.GetArg<Uniform>(0);
                const auto value = inst.GetArg<Value>(1);
                const u32 off = uniform.GetOffset();
                // The backend stores according to the *value* width, not the
                // descriptor width -- the same rule the forward pass tracks.
                const u32 size = GetValueSizeByte(value.Type());
                if (xmm_only && !info.IsXmmRange(off, size)) {
                    break;
                }
                if (!features.xmm_uniform_fwd && info.IsXmmRange(off, size)) {
                    break;
                }
                if (off + size > killed.size()) {
                    break;
                }
                bool all_dead = true;
                for (u32 i = 0; i < size; ++i) {
                    if (!killed[off + i]) {
                        all_dead = false;
                        break;
                    }
                }
                if (all_dead) {
                    victims.push_back(&inst);
                    break;  // a removed store kills nothing
                }
                for (u32 i = 0; i < size; ++i) {
                    killed[off + i] = 1;
                }
                break;
            }
            case OpCode::LoadUniform: {
                const auto uniform = inst.GetArg<Uniform>(0);
                const u32 off = uniform.GetOffset();
                const u32 size = GetValueSizeByte(uniform.GetType());
                if (xmm_only && !info.IsXmmRange(off, size)) {
                    break;
                }
                if (!features.xmm_uniform_fwd && info.IsXmmRange(off, size)) {
                    break;
                }
                for (u32 i = 0; i < size && off + i < killed.size(); ++i) {
                    killed[off + i] = 0;
                }
                break;
            }
            case OpCode::CallLambda:
            case OpCode::CallDynamic:
            case OpCode::CallLocation:
            case OpCode::X87Op:
            case OpCode::MemoryCopy:
            case OpCode::MemoryCopyTSO:
            case OpCode::SetLocation:
                clear_all();
                break;
            case OpCode::SetHostGPR:
            case OpCode::SetHostFPR: {
                const bool fpr = inst.GetOp() == OpCode::SetHostFPR;
                if (xmm_only && !fpr) {
                    break;  // disjoint from XMM bytes in the cleanup sweep
                }
                const auto value = inst.GetArg<Value>(0);
                const u32 size = GetValueSizeByte(value.Type());
                const auto home = home_range(fpr, inst.GetArg<Imm>(1).Get());
                const u32 off_in = inst.GetArg<Imm>(2).Get();
                if (!home || off_in + size > home->size) {
                    clear_all();
                    break;
                }
                const u32 off = home->offset + off_in;
                if (xmm_only && !info.IsXmmRange(off, size)) {
                    break;
                }
                if (!features.xmm_uniform_fwd && info.IsXmmRange(off, size)) {
                    break;
                }
                if (off + size > killed.size()) {
                    clear_all();
                    break;
                }
                bool all_dead = true;
                for (u32 i = 0; i < size; ++i) {
                    if (!killed[off + i]) {
                        all_dead = false;
                        break;
                    }
                }
                // Only full-width publications are victim-eligible:
                // a partial write's upper-byte semantics belong to
                // the planner's width model, not to this sweep.
                if (off_in == 0 && size == home->size && all_dead) {
                    victims.push_back(&inst);
                    break;
                }
                for (u32 i = 0; i < size; ++i) {
                    killed[off + i] = 1;
                }
                break;
            }
            case OpCode::GetHostGPR:
            case OpCode::GetHostFPR: {
                const bool fpr = inst.GetOp() == OpCode::GetHostFPR;
                if (xmm_only && !fpr) {
                    break;
                }
                const auto home = home_range(fpr, inst.GetArg<Imm>(0).Get());
                const u32 off_in = inst.GetArg<Imm>(1).Get();
                const u32 size = GetValueSizeByte(inst.ReturnType());
                if (!home || off_in + size > home->size) {
                    clear_all();
                    break;
                }
                const u32 off = home->offset + off_in;
                if (xmm_only && !info.IsXmmRange(off, size)) {
                    break;
                }
                if (!features.xmm_uniform_fwd && info.IsXmmRange(off, size)) {
                    break;
                }
                for (u32 i = 0; i < size && off + i < killed.size(); ++i) {
                    killed[off + i] = 0;
                }
                break;
            }
            case OpCode::BindLabel:
                if (!features.uniform_path_fwd) {
                    clear_all();
                } else {
                    label_killed[inst.GetArg<Value>(0).Def()] =
                            std::vector<u8>(killed.begin(), killed.end());
                }
                break;
            case OpCode::Goto:
            case OpCode::NotGoto: {
                if (!features.uniform_path_fwd) {
                    clear_all();
                    break;
                }
                auto taken = label_killed.find(&inst);
                if (taken == label_killed.end() ||
                    taken->second.size() != killed.size()) {
                    clear_all();
                    break;
                }
                for (size_t i = 0; i < killed.size(); ++i) {
                    killed[i] &= taken->second[i];
                }
                label_killed.erase(taken);
                break;
            }
            default:
                break;
        }
    }

    for (auto* victim : victims) {
        rewrite.Erase(block, victim);
    }

    if (!victims.empty() && GetSvmConfig().dump_ir) {
        SVM_DIAG_FORMAT(Codegen, "[uniform-dse] block {:#x}: removed {} dead uniform store(s)\n",
                   block->GetStartLocation().Value(), victims.size());
    }
    if (Perf2Enabled()) {
        auto& stats = GetPerfStats2();
        // The PIN_EXT repair sweep revisits the same block for XMM stores
        // only. Keep the historical one-block/one-count check contract while
        // still reporting every additional victim it finds.
        if (!xmm_only) {
            stats.uniform_dse_blocks.fetch_add(1, std::memory_order_relaxed);
        }
        stats.uniform_dse_victims.fetch_add(victims.size(), std::memory_order_relaxed);
    }
}

}  // namespace swift::runtime::compile
