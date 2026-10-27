#include "base/logging.h"
#include "runtime/compile/workspace.h"
//
// Created by 甘尧 on 2024/6/20.
//

#include "runtime/compile/analysis/guest_state.h"
#include "state_internal.h"

#include <algorithm>

namespace swift::runtime::compile {
using namespace ir;


// 诊断开关来自进程级配置快照，不在逐块路径重复扫描 environ。
static bool EnvDumpIr() { return GetSvmConfig().dump_ir; }
static bool EnvDumpIrPost() { return GetSvmConfig().dump_ir_post; }

static void CleanupStateBlock(RewriteContext& rewrite, Block* block,
                              const GuestStateLayout& info, bool fast_path,
                              const FeatureSet& features) {
    PerfScope2 perf_forward{GetPerfStats2().uniform_forward};

    // The corpus is dominated by blocks with no uniform operations. Check only
    // until the first relevant instruction: a miss replaces the legacy forward
    // scan and lets us avoid both fixed-size byte tables plus the reverse scan;
    // a hit pays only the usually short prefix before entering the unchanged
    // transfer logic below.
    if (fast_path) {
        u32 checked_instructions{};
        bool has_uniform_op = false;
        for (const auto& inst : block->GetInstList()) {
            checked_instructions++;
            const auto op = inst.GetOp();
            has_uniform_op = op == OpCode::LoadUniform || op == OpCode::StoreUniform ||
                    op == OpCode::SetHostGPR || op == OpCode::SetHostFPR;
            if (has_uniform_op) {
                break;
            }
        }
        if (Perf2Enabled()) {
            auto& stats = GetPerfStats2();
            stats.uniform_check_insts.fetch_add(checked_instructions, std::memory_order_relaxed);
            stats.uniform_check_hits.fetch_add(has_uniform_op, std::memory_order_relaxed);
        }
        if (!has_uniform_op) {
            perf_forward.Stop();
            if (Perf2Enabled()) {
                auto& stats = GetPerfStats2();
                stats.uniform_blocks.fetch_add(1, std::memory_order_relaxed);
                stats.uniform_no_ops_blocks.fetch_add(1, std::memory_order_relaxed);
                stats.uniform_insts.fetch_add(checked_instructions, std::memory_order_relaxed);
            }
            return;
        }
    }

    // A target may require elimination on the original logical byte stores:
    // partial mapped writes have separate physical-width obligations. Preserve
    // that phase choice without interpreting a target's register numbering.
    const bool dse_off = !features.uniform_dse;
    const bool dse_before_mapping = info.optimization.dse_before_mapping;
    if (dse_before_mapping && !dse_off) {
        EliminateStateDeadStores(rewrite, block, info, features);
    }

    StackVector<GuestStateByteFact, 0x100> uniform_values{info.uniform_size};
    // Both mutable IR stages use the same byte transfers and taken-edge meet.
    // This pass owns its table and snapshots only until the block is rewritten.
    GuestStateForwarding forwarding;
    const std::span bytes{uniform_values.data(), uniform_values.size()};
    u32 load_count{};
    u32 folded_load_count{};
    u32 mapped_load_count{};
    u32 mapped_store_count{};
    u32 invalidation_count{};
    u32 full_invalidation_count{};
    u32 range_invalidation_count{};
    u64 preserved_fact_count{};
    u32 instruction_count{};
    u32 store_count{};
    u32 barrier_count{};
    u32 path_merge_count{};
    u32 path_merge_bytes{};

    auto invalidate_uniform_values = [&] {
        GuestStateForwarding::Invalidate(bytes);
        invalidation_count++;
        full_invalidation_count++;
    };
    auto update_mapped_gpr_facts = [&](u32 offset, u32 size, const Value& value) {
        preserved_fact_count += std::count_if(
                uniform_values.begin(), uniform_values.begin() + offset,
                [](const GuestStateByteFact& fact) { return fact.Defined(); });
        preserved_fact_count += std::count_if(
                uniform_values.begin() + offset + size, uniform_values.end(),
                [](const GuestStateByteFact& fact) { return fact.Defined(); });
        GuestStateForwarding::Write(bytes, offset, value);
        invalidation_count++;
        range_invalidation_count++;
    };

    for (auto& inst : block->GetInstList()) {
        instruction_count++;
        switch (inst.GetOp()) {
            case OpCode::LoadUniform: {
                load_count++;
                auto uniform = inst.GetArg<Uniform>(0);
                auto uni_offset{uniform.GetOffset()};
                auto uni_type{uniform.GetType()};
                auto uni_size{GetValueSizeByte(uni_type)};
                ASSERT_MSG(uni_offset + uni_size <= uniform_values.size(),
                           "uniform load [{}, {}) exceeds uniform buffer size {}",
                           uni_offset, uni_offset + uni_size, uniform_values.size());
                const auto forward = DescribeGuestStateForwardAccess(
                        info, features, uni_offset, uni_type, GetSvmConfig().xmm_resident);
                const auto& access = forward.access;
                auto try_forward_load = [&] {
                    if (!forward.read) {
                        return false;
                    }
                    const auto policy = features.xmm_narrow_fwd
                            ? GuestStateReadPolicy::ForwardNarrowFPR
                            : GuestStateReadPolicy::Forward;
                    const auto read = ReadGuestStateBytes(
                            bytes, uniform, policy);
                    if (!read.Found()) {
                        return false;
                    }
                    rewrite.Change(block, &inst, [&](Inst& inst) {
                        inst.Reset();
                        if (read.kind == GuestStateReadKind::Alias) {
                            inst.BitCast(read.value).SetReturn(uni_type);
                        } else {
                            inst.BitExtract(read.value, Imm(read.offset * 8u),
                                            Imm(uni_size * 8u)).SetReturn(uni_type);
                        }
                    });
                    folded_load_count++;
                    return true;
                };
                if (access.kind == GuestStateAccessKind::Invalid) {
                    PANIC("Cross uniform load: {}", fmt::format("{}", inst));
                    break;
                }
                const auto& uniform_register = access.binding;
                if (access.kind == GuestStateAccessKind::Register) {
                    const auto uni_reg_offset = uniform_register.uniform.GetOffset();
                    if (try_forward_load()) break;
                    const bool mapped_fpr = uniform_register.host_reg.is_fpr;
                    auto reg_index = mapped_fpr ? uniform_register.host_reg.fpr.id
                                                : uniform_register.host_reg.gpr.id;
                    Imm offset_in{static_cast<u8>(uni_offset - uni_reg_offset)};
                    rewrite.Change(block, &inst, [&](Inst& inst) {
                        inst.Reset();
                        if (mapped_fpr) {
                            inst.GetHostFPR(HostRegIndex(reg_index), offset_in).SetReturn(uni_type);
                        } else {
                            inst.GetHostGPR(HostRegIndex(reg_index), offset_in).SetReturn(uni_type);
                        }
                    });
                    mapped_load_count++;
                    if (forward.seed_load) {
                        GuestStateForwarding::Write(bytes, uni_offset, Value{&inst});
                    }
                    break;
                }

                if (try_forward_load()) {
                    break;
                }
                if (forward.seed_load) {
                    GuestStateForwarding::Write(bytes, uni_offset, Value{&inst});
                }
                break;
            }
            case OpCode::StoreUniform: {
                store_count++;
                auto uniform = inst.GetArg<Uniform>(0);
                auto value = inst.GetArg<Value>(1);
                auto uni_offset{uniform.GetOffset()};
                auto value_type{value.Type()};
                auto value_size{GetValueSizeByte(value_type)};
                ASSERT_MSG(uni_offset + value_size <= uniform_values.size(),
                           "uniform store [{}, {}) exceeds uniform buffer size {}",
                           uni_offset, uni_offset + value_size, uniform_values.size());
                const auto forward = DescribeGuestStateForwardAccess(
                        info, features, uni_offset, value_type, GetSvmConfig().xmm_resident);
                const auto& access = forward.access;
                if (access.kind == GuestStateAccessKind::Invalid) {
                    PANIC("Cross uniform store: {}", fmt::format("{}", inst));
                    break;
                }
                const auto& uniform_register = access.binding;
                if (access.kind == GuestStateAccessKind::Register) {
                    const auto uni_reg_offset = uniform_register.uniform.GetOffset();
                    const bool mapped_fpr = uniform_register.host_reg.is_fpr;
                    auto reg_index = mapped_fpr ? uniform_register.host_reg.fpr.id
                                                : uniform_register.host_reg.gpr.id;
                    Imm offset_in{static_cast<u8>(uni_offset - uni_reg_offset)};
                    rewrite.Change(block, &inst, [&](Inst& inst) {
                        inst.Reset();
                        if (mapped_fpr) {
                            inst.SetHostFPR(value, HostRegIndex(reg_index), offset_in);
                        } else {
                            inst.SetHostGPR(value, HostRegIndex(reg_index), offset_in);
                        }
                    });
                    mapped_store_count++;
                    if (forward.write == GuestStateForwardWrite::Write) {
                        update_mapped_gpr_facts(uni_offset, value_size, value);
                    } else {
                        // OFF retains the legacy opaque mapped-store barrier.
                        // Mapped FPR stores stay conservative in this phase.
                        invalidate_uniform_values();
                    }
                    break;
                }
                if (forward.write == GuestStateForwardWrite::Write) {
                    GuestStateForwarding::Write(bytes, uni_offset, value);
                }
                break;
            }
            case OpCode::CallLambda:
            case OpCode::CallLocation:
            case OpCode::CallDynamic:
            case OpCode::X87Op:
            case OpCode::MemoryCopy:
            case OpCode::MemoryCopyTSO:
            case OpCode::SetHostGPR:
            case OpCode::SetHostFPR:
            case OpCode::SetLocation:
            case OpCode::Goto:
            case OpCode::NotGoto:
            case OpCode::BindLabel: {
                barrier_count++;
                const auto op = inst.GetOp();
                store_count += op == OpCode::X87Op || op == OpCode::MemoryCopy ||
                        op == OpCode::MemoryCopyTSO || op == OpCode::SetHostGPR ||
                        op == OpCode::SetHostFPR;
                const auto update = forwarding.Observe(bytes, inst,
                        features.ir_uniform_range, features.uniform_path_fwd);
                switch (update.event) {
                    case GuestStateForwardEvent::FullInvalidation:
                        invalidation_count++;
                        full_invalidation_count++;
                        break;
                    case GuestStateForwardEvent::RangeInvalidation:
                        invalidation_count++;
                        range_invalidation_count++;
                        preserved_fact_count += std::count_if(bytes.begin(), bytes.end(),
                                [](const auto& fact) { return fact.Defined(); });
                        break;
                    case GuestStateForwardEvent::Merge:
                        path_merge_count++;
                        path_merge_bytes += update.merged_bytes;
                        break;
                    default:
                        break;
                }
                break;
            }
            default:
                break;
        }
    }

    perf_forward.Stop();

    if (Perf2Enabled()) {
        auto& stats = GetPerfStats2();
        stats.uniform_blocks.fetch_add(1, std::memory_order_relaxed);
        stats.uniform_no_ops_blocks.fetch_add(load_count + store_count == 0,
                                               std::memory_order_relaxed);
        stats.uniform_insts.fetch_add(instruction_count, std::memory_order_relaxed);
        stats.uniform_loads.fetch_add(load_count, std::memory_order_relaxed);
        stats.uniform_stores.fetch_add(store_count, std::memory_order_relaxed);
        stats.uniform_barriers.fetch_add(barrier_count, std::memory_order_relaxed);
        stats.uniform_invalidations.fetch_add(invalidation_count, std::memory_order_relaxed);
        stats.uniform_full_invalidations.fetch_add(full_invalidation_count,
                                                   std::memory_order_relaxed);
        stats.uniform_range_invalidations.fetch_add(range_invalidation_count,
                                                    std::memory_order_relaxed);
        stats.uniform_preserved_facts.fetch_add(preserved_fact_count,
                                                std::memory_order_relaxed);
    }

    if (EnvDumpIr()) {
        SVM_DIAG_FORMAT(Codegen,
                   "[uniform-elim] block {:#x}: LoadUniform {} -> {} "
                   "(folded {}, mapped {}), mapped stores {}, invalidations {}, "
                   "full {}, range {}, preserved facts {}, "
                   "path merges {} ({} byte-facts preserved)\n",
                   block->GetStartLocation().Value(), load_count,
                   load_count - folded_load_count - mapped_load_count,
                   folded_load_count, mapped_load_count, mapped_store_count,
                   invalidation_count, full_invalidation_count,
                   range_invalidation_count, preserved_fact_count,
                   path_merge_count, path_merge_bytes);
        if (EnvDumpIrPost()) {
            SVM_DIAG_FORMAT(Codegen, "--- post-uniform block {:#x} ---\n{}\n",
                       block->GetStartLocation().Value(), block->ToString());
        }
    }

    // Escape hatch for bisecting a suspected DSE bug against the load-folding
    // half of this pass, which SVM_UNIFORM_ELIM=0 cannot separate.
    // With every uniform byte live-out, a store can be dead only if at least
    // one later store overwrites it. Fewer than two stores therefore makes the
    // reverse dataflow scan provably a no-op.
    if (!dse_off && (!fast_path || store_count >= 2)) {
        if (!dse_before_mapping) {
            EliminateStateDeadStores(rewrite, block, info, features);
        } else if (features.xmm_narrow_fwd) {
            // The early sweep still sees V32/V64 loads and cannot know they
            // will subsequently fold to a V128 SSA value.
            // Revisit only XMM stores after forwarding; mapped GPR operations
            // are disjoint and no longer block the newly exposed dead stores.
            EliminateStateDeadStores(rewrite, block, info, features, true);
        }
    }
}

void CleanupState(RewriteContext& rewrite, const GuestStateLayout& info,
                  const FeatureSet& features) {
    for (auto* block : rewrite.Blocks()) {
        CleanupStateBlock(rewrite, block, info, features.uniform_fast, features);
    }
}

}  // namespace swift::runtime::compile
