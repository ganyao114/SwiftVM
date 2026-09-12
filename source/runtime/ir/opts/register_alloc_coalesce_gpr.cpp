#include "base/logging.h"
#include "register_alloc_internal.h"
#include "runtime/backend/gpr_coalescing_contract.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "runtime/common/svm_config.h"

namespace swift::runtime::ir {

static void MapGuestFixedGPR(backend::RegAlloc* reg_alloc,
                             u32 id,
                             u32 target) {
    if (backend::FixedGPRClassEnabled(reg_alloc->GetGprs(),
                                      reg_alloc->GetFeatures())) {
        reg_alloc->MapFixedRegister(id, HostGPR{static_cast<u16>(target)});
    } else {
        reg_alloc->MapRegister(id, HostGPR{static_cast<u16>(target)});
    }
}

bool IsPinnedCoalesceTarget(u32 reg) {
    return backend::IsFixedGPRHome(reg);
}

bool IsPinnedCoalesceProducer(OpCode op) {
    return backend::IsGPRPublicationProducer(op);
}

bool IsWidthChainRootProducer(const Inst* producer) {
    if (!producer || GetValueSizeByte(producer->ReturnType()) != sizeof(u32)) {
        return false;
    }
    switch (producer->GetOp()) {
        case OpCode::GetHostGPR:
            return producer->GetArg<Imm>(1).Get() == 0;
        case OpCode::SignExtend:
        case OpCode::Mul:
        case OpCode::Add:
            // All three emit a real W destination for an U32 result.  This is
            // stronger than a W-view read: the physical X high half is known
            // zero after the producer.  Their inputs may have multiple uses;
            // the publication transaction proves every alias window instead
            // of substituting a last-use heuristic here.
            return true;
        default:
            return false;
    }
}

bool IsPinnedCoalesceObserver(OpCode op) {
    switch (op) {
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
        case OpCode::CallLambda:
        case OpCode::CallLocation:
        case OpCode::CallDynamic:
        case OpCode::X87Op:
        case OpCode::Sse42Str:
        case OpCode::GetUniformAddress:
        case OpCode::UniformBarrier:
            return true;
        default:
            return false;
    }
}

bool HasKnownWWrite(Value value) {
    if (!value.Defined()) {
        return false;
    }
    auto* def = value.Def();
    // These are aliases/reads, not physical W writes. In particular a
    // U32 GetHostGPR only selects the W view of a pinned X register; it
    // does not prove that X[63:32] is zero.
    if (def->GetOp() == OpCode::GetHostGPR || def->IsBitCastOperation()) {
        return false;
    }
    if (def->GetOp() == OpCode::BitExtract &&
        GetValueSizeByte(def->ReturnType()) == sizeof(u32) &&
        def->GetArg<Imm>(1).Get() == 0 &&
        def->GetArg<Imm>(2).Get() == 32) {
        return HasKnownWWrite(def->GetArg<Value>(0));
    }
    if (def->GetOp() == OpCode::ZeroExtend32To64 &&
        GetValueSizeByte(def->GetArg<Value>(0).Type()) == sizeof(u32)) {
        return HasKnownWWrite(def->GetArg<Value>(0));
    }
    return GetValueSizeByte(def->ReturnType()) == sizeof(u32);
}

void RecipeWidthComponentOwners(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        const Vector<u32>& use_end) {
    if (!features.ra_width_chain) {
        return;
    }
    // Owner selection is intentionally made from the untouched linear-scan
    // result.  The first eligible publication in IR order freezes the fixed
    // home and the high-half fact; later write/read passes can consume that
    // decision but can neither replace it nor retry another home.
    for (auto& store : lir_block->GetInstList()) {
        if (store.GetOp() != OpCode::SetHostGPR ||
            store.GetArg<Imm>(2).Get() != 0) {
            continue;
        }
        const u32 target = store.GetArg<Imm>(1).Get();
        if (!IsPinnedCoalesceTarget(target) ||
            !reg_alloc->GetGprs().Get(target)) {
            continue;
        }
        auto stored = ResolveBitCastSource(store.GetArg<Value>(0));
        auto produced = stored;
        bool zero_extend = false;
        if (stored.Def() && stored.Def()->GetOp() == OpCode::ZeroExtend32To64 &&
            GetValueSizeByte(stored.Def()->GetArg<Value>(0).Type()) == sizeof(u32)) {
            produced = ResolveBitCastSource(stored.Def()->GetArg<Value>(0));
            zero_extend = true;
        }
        auto* root = produced.Def();
        if (!root || !IsWidthChainRootProducer(root) ||
            produced.Id() >= use_end.size() || root->Id() >= store.Id() ||
            reg_alloc->ValueType(produced) != backend::RegAlloc::GPR ||
            reg_alloc->ValueType(stored) != backend::RegAlloc::GPR) {
            continue;
        }
        // Mapping an U32 producer to the selected home makes its ordinary
        // emitter write Wtarget (GetHostGPR included), so the published X
        // high half is zero even when the pre-transaction source was only a
        // W view.  The wrapper provides the same fact explicitly.
        const bool high_zero = zero_extend ||
                GetValueSizeByte(produced.Type()) == sizeof(u32);
        (void)reg_alloc->FreezeWidthComponentOwner(
                root->Id(), static_cast<u16>(target), high_zero);
    }
}

bool ValidateWidthComponentTransaction(
        Block* lir_block,
        backend::RegAlloc* reg_alloc) {
    Vector<u8> committed_store(reg_alloc->MapCount());
    for (auto& inst : lir_block->GetInstList()) {
        if (inst.HasValue() && reg_alloc->IsWidthChainCoalesced(inst.Id())) {
            const u32 anchor = reg_alloc->WidthChainAnchor(inst.Id());
            if (reg_alloc->HasWidthComponentOwner(anchor)) {
                if (!reg_alloc->WidthComponentOwnerCommitted(anchor) ||
                    reg_alloc->ValueType(Value{&inst}) != backend::RegAlloc::GPR ||
                    reg_alloc->ValueGPR(Value{&inst}).id !=
                            reg_alloc->WidthComponentOwnerTarget(anchor)) {
                    return false;
                }
            }
        }
        if (inst.GetOp() != OpCode::SetHostGPR ||
            !reg_alloc->IsHostWriteCoalesced(inst.Id())) {
            continue;
        }
        auto stored = ResolveBitCastSource(inst.GetArg<Value>(0));
        if (!stored.Defined() || !reg_alloc->IsWidthChainCoalesced(stored.Id())) {
            continue;
        }
        const u32 anchor = reg_alloc->WidthChainAnchor(stored.Id());
        if (!reg_alloc->HasWidthComponentOwner(anchor) ||
            !reg_alloc->WidthComponentOwnerCommitted(anchor) ||
            reg_alloc->WidthComponentOwnerTarget(anchor) !=
                    inst.GetArg<Imm>(1).Get()) {
            return false;
        }
        committed_store[anchor] = 1;
    }
    for (u32 anchor = 0; anchor < reg_alloc->MapCount(); ++anchor) {
        if (reg_alloc->WidthComponentOwnerCommitted(anchor) &&
            !committed_store[anchor]) {
            return false;
        }
    }
    return true;
}

Vector<u32> CollectWidthChainUseEnds(Block* lir_block, u32 instr_count) {
    auto& list = lir_block->GetInstList();
    Vector<u32> use_end(instr_count);
    for (auto& inst : list) {
        for (auto value : inst.GetValues()) {
            value = ResolveBitCastSource(value);
            if (value.Defined() && value.Id() < use_end.size()) {
                use_end[value.Id()] = std::max<u32>(use_end[value.Id()], inst.Id());
            }
        }
    }
    if (list.begin() != list.end()) {
        const u32 block_end = std::prev(list.end())->Id();
        auto extend_terminal = [&](const Value& value) {
            auto root = ResolveBitCastSource(value);
            if (root.Defined() && root.Id() < use_end.size()) {
                use_end[root.Id()] = std::max(use_end[root.Id()], block_end);
            }
        };
        std::function<void(const Terminal&)> walk_terminal =
                [&](const Terminal& terminal_value) {
                    VisitVariant<void>(terminal_value, [&](const auto& edge) {
                        using T = std::decay_t<decltype(edge)>;
                        if constexpr (std::is_same_v<T, terminal::If>) {
                            extend_terminal(edge.cond);
                            walk_terminal(edge.then_);
                            walk_terminal(edge.else_);
                        } else if constexpr (std::is_same_v<T, terminal::Switch>) {
                            extend_terminal(edge.value);
                            for (const auto& item : edge.cases) {
                                walk_terminal(item.then);
                            }
                        } else if constexpr (std::is_same_v<T, terminal::Condition>) {
                            walk_terminal(edge.then_);
                            walk_terminal(edge.else_);
                        } else if constexpr (std::is_same_v<T, terminal::CheckHalt>) {
                            walk_terminal(edge.else_);
                        }
                    });
                };
        walk_terminal(lir_block->GetTerminal());
    }
    return use_end;
}

Vector<u8> CollectLongWidthChainBridges(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        u32 instr_count,
        const Vector<u32>& use_end) {
    auto& list = lir_block->GetInstList();
    // 旧的宽度链实验会保护所有已被 W-alpha 合并的发布点。这里只对
    // 同一 fixed home 的长 Add/Xor 发布链建立完整拓扑证明，短链仍走
    // 原路径。阈值用于排除 STREAM 一类短依赖链，不按 guest PC 特判。
    Vector<u8> long_bridge(instr_count);
    if (features.ra_width_chain_long) {
        struct LongStep {
            Inst* producer{};
            Inst* wrapper{};
            Inst* store{};
            u16 target{};
        };
        Vector<LongStep> steps{};
        for (auto& store : list) {
            if (store.GetOp() != OpCode::SetHostGPR ||
                store.GetArg<Imm>(2).Get() != 0) {
                continue;
            }
            auto stored = ResolveBitCastSource(store.GetArg<Value>(0));
            auto* wrapper = stored.Def();
            if (!wrapper || wrapper->GetOp() != OpCode::ZeroExtend32To64 ||
                GetValueSizeByte(wrapper->GetArg<Value>(0).Type()) != sizeof(u32)) {
                continue;
            }
            auto produced = ResolveBitCastSource(wrapper->GetArg<Value>(0));
            auto* producer = produced.Def();
            if (!producer ||
                (producer->GetOp() != OpCode::Add &&
                 producer->GetOp() != OpCode::Xor) ||
                GetValueSizeByte(producer->ReturnType()) != sizeof(u32)) {
                continue;
            }
            const u16 target = static_cast<u16>(store.GetArg<Imm>(1).Get());
            if (!IsPinnedCoalesceTarget(target) ||
                reg_alloc->ValueType(produced) != backend::RegAlloc::GPR ||
                reg_alloc->ValueType(stored) != backend::RegAlloc::GPR ||
                reg_alloc->ValueGPR(produced).id != target ||
                reg_alloc->ValueGPR(stored).id != target) {
                continue;
            }
            steps.push_back({producer, wrapper, &store, target});
        }
        auto low_extract_source = [](Value value) -> Value {
            value = ResolveBitCastSource(value);
            auto* def = value.Def();
            if (!def || def->GetOp() != OpCode::BitExtract ||
                GetValueSizeByte(def->ReturnType()) != sizeof(u32) ||
                def->GetArg<Imm>(1).Get() != 0 ||
                def->GetArg<Imm>(2).Get() != 32) {
                return {};
            }
            return ResolveBitCastSource(def->GetArg<Value>(0));
        };
        auto safe_between = [&](u32 begin, u32 end) {
            for (auto& scan : list) {
                if (scan.Id() <= begin || scan.Id() >= end) {
                    continue;
                }
                switch (scan.GetOp()) {
                    case OpCode::AdvancePC:
                    case OpCode::BitExtract:
                    case OpCode::LoadImm:
                    case OpCode::ClearFlags:
                    case OpCode::SaveFlags:
                        break;
                    default:
                        return false;
                }
            }
            return true;
        };
        auto step_is_closed = [&](const LongStep& step) {
            for (auto& scan : list) {
                if (scan.Id() <= step.producer->Id() ||
                    scan.Id() >= step.store->Id() ||
                    &scan == step.wrapper) {
                    continue;
                }
                switch (scan.GetOp()) {
                    case OpCode::LoadImm:
                    case OpCode::ClearFlags:
                    case OpCode::SaveFlags:
                        break;
                    default:
                        return false;
                }
            }
            return true;
        };
        auto links = [&](const LongStep& previous, const LongStep& current) {
            if (previous.target != current.target ||
                !step_is_closed(previous) || !step_is_closed(current) ||
                !safe_between(previous.store->Id(), current.producer->Id())) {
                return false;
            }
            auto inputs = current.producer->GetValues();
            return std::any_of(
                    inputs.begin(), inputs.end(),
                    [&](Value input) {
                        return low_extract_source(input).Def() == previous.wrapper;
                    });
        };
        auto mark_run = [&](size_t begin, size_t end) {
            constexpr size_t kMinLongWidthChainSteps = 32;
            if (end - begin < kMinLongWidthChainSteps) {
                return;
            }
            for (size_t index = begin; index < end; ++index) {
                auto* producer = steps[index].producer;
                for (auto input : producer->GetValues()) {
                    auto input_root = ResolveBitCastSource(input);
                    auto* bridge = input_root.Def();
                    auto source = low_extract_source(input_root);
                    if (!bridge || !source.Defined() || bridge->GetUses() != 1 ||
                        bridge->Id() >= use_end.size() ||
                        use_end[bridge->Id()] != producer->Id() ||
                        reg_alloc->ValueType(source) != backend::RegAlloc::GPR) {
                        continue;
                    }
                    long_bridge[bridge->Id()] = 1;
                }
            }
        };
        size_t run_begin = 0;
        for (size_t index = 1; index <= steps.size(); ++index) {
            if (index < steps.size() && links(steps[index - 1], steps[index])) {
                continue;
            }
            mark_run(run_begin, index);
            run_begin = index;
        }
    }
    return long_bridge;
}


void CoalesceWidthChainBridges(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        const Vector<u32>& use_end,
        const Vector<u8>& long_bridge,
        const Vector<u32>& fixed_gpr_clobbers,
        const RegisterAllocFamilyCallbacks& callbacks) {
    auto& list = lir_block->GetInstList();
    auto CheckInstr = [&](Inst* inst, u32 extra_gpr, u32 extra_fpr) {
        return callbacks.check_instr(callbacks.context, inst, extra_gpr, extra_fpr);
    };
    auto uses_stay_in_block = [&](Value value) {
        return !callbacks.value_uses_stay_in_block ||
               callbacks.value_uses_stay_in_block(
                       callbacks.context, lir_block, value);
    };
    auto component_anchor = [&](Value value) {
        value = ResolveBitCastSource(value);
        if (!value.Defined()) {
            return UINT32_MAX;
        }
        return reg_alloc->IsWidthChainCoalesced(value.Id())
                ? reg_alloc->WidthChainAnchor(value.Id())
                : value.Id();
    };
    auto in_component = [&](Value value, u32 anchor) {
        value = ResolveBitCastSource(value);
        return value.Defined() && component_anchor(value) == anchor;
    };

    for (auto& bridge : list) {
        Value source{};
        if (bridge.GetOp() == OpCode::BitExtract &&
            GetValueSizeByte(bridge.ReturnType()) == sizeof(u32) &&
            bridge.GetArg<Imm>(1).Get() == 0 &&
            bridge.GetArg<Imm>(2).Get() == 32) {
            source = ResolveBitCastSource(bridge.GetArg<Value>(0));
        } else if (bridge.GetOp() == OpCode::ZeroExtend32To64 &&
                   GetValueSizeByte(bridge.GetArg<Value>(0).Type()) == sizeof(u32)) {
            source = ResolveBitCastSource(bridge.GetArg<Value>(0));
        } else {
            continue;
        }
        const bool component_w_write =
                source.Defined() && source.Def()->GetOp() == OpCode::GetHostGPR &&
                reg_alloc->IsWidthChainCoalesced(source.Id()) &&
                reg_alloc->WidthChainAnchor(source.Id()) == source.Id() &&
                GetValueSizeByte(source.Type()) == sizeof(u32);
        const bool long_candidate = bridge.Id() < long_bridge.size() &&
                                    long_bridge[bridge.Id()] != 0;
        if (!features.ra_width_chain && !long_candidate) {
            continue;
        }
        const bool long_u32_capture =
                long_candidate && source.Defined() && source.Def() &&
                source.Def()->GetOp() == OpCode::GetHostGPR &&
                GetValueSizeByte(source.Type()) == sizeof(u32);
        if (!source.Defined() ||
            (!HasKnownWWrite(source) && !component_w_write &&
             !long_u32_capture) ||
            reg_alloc->ValueType(source) != backend::RegAlloc::GPR ||
            reg_alloc->ValueType(Value{&bridge}) != backend::RegAlloc::GPR ||
            bridge.Id() >= use_end.size() || bridge.GetUses() == 0 ||
            !uses_stay_in_block(source) ||
            !uses_stay_in_block(Value{&bridge})) {
            continue;
        }
        const u16 target = reg_alloc->ValueGPR(source).id;
        const u16 old_target = reg_alloc->ValueGPR(Value{&bridge}).id;
        if (!long_candidate &&
            (target == old_target || reg_alloc->GetGprs().Get(target))) {
            continue;
        }
        const u32 end = std::max<u32>(bridge.Id(), use_end[bridge.Id()]);
        const u32 anchor = component_anchor(source);

        bool blocked = false;
        for (auto& other : list) {
            if (&other == &bridge || !other.HasValue() ||
                other.IsBitCastOperation() || in_component(Value{&other}, anchor) ||
                reg_alloc->ValueType(Value{&other}) != backend::RegAlloc::GPR ||
                reg_alloc->ValueGPR(Value{&other}).id != target) {
                continue;
            }
            const u32 other_end = other.Id() < use_end.size()
                    ? std::max<u32>(other.Id(), use_end[other.Id()])
                    : other.Id();
            if (other.Id() <= end && other_end >= bridge.Id()) {
                auto inputs = other.GetValues();
                const bool exact_last_use_handoff =
                        long_candidate && other.Id() == end &&
                        std::any_of(inputs.begin(), inputs.end(),
                                    [&](Value input) {
                                        return ResolveBitCastSource(input).Def() ==
                                               &bridge;
                                    });
                if (exact_last_use_handoff) {
                    continue;
                }
                blocked = true;
                break;
            }
        }
        if (blocked) {
            continue;
        }
        for (auto& scan : list) {
            if (scan.Id() < bridge.Id() || scan.Id() > end) {
                continue;
            }
            if (scan.Id() < fixed_gpr_clobbers.size() &&
                (fixed_gpr_clobbers[scan.Id()] & (1u << target))) {
                blocked = true;
                break;
            }
            if (scan.GetOp() == OpCode::SetHostGPR &&
                scan.GetArg<Imm>(1).Get() == target &&
                !in_component(scan.GetArg<Value>(0), anchor)) {
                blocked = true;
                break;
            }
        }
        // W-alpha ran first and its emitter will independently replay
        // the fixed-home proof. Do not let this later pass change a
        // producer/wrapper named by that proof, or introduce a new
        // interval in the same fixed home before publication.
        for (auto& store : list) {
            if (long_candidate) {
                break;
            }
            if (blocked || store.GetOp() != OpCode::SetHostGPR ||
                !reg_alloc->IsHostWriteCoalesced(store.Id())) {
                continue;
            }
            auto stored = ResolveBitCastSource(store.GetArg<Value>(0));
            auto produced = stored;
            if (stored.Def() && stored.Def()->GetOp() == OpCode::ZeroExtend32To64) {
                produced = ResolveBitCastSource(stored.Def()->GetArg<Value>(0));
            }
            if (stored.Def() == &bridge || produced.Def() == &bridge) {
                blocked = true;
                break;
            }
            if (produced.Def()) {
                for (auto input : produced.Def()->GetValues()) {
                    if (ResolveBitCastSource(input).Def() == &bridge) {
                        blocked = true;
                        break;
                    }
                }
            }
            if (blocked) {
                break;
            }
            if (store.GetArg<Imm>(1).Get() == target &&
                bridge.Id() < store.Id() && end >= produced.Id()) {
                blocked = true;
                break;
            }
        }
        if (blocked) {
            continue;
        }

        struct SavedMask {
            u32 id;
            backend::GPRSMask gprs;
            backend::FPRSMask fprs;
        };
        Vector<SavedMask> saved{};
        for (auto& scan : list) {
            if (scan.Id() < bridge.Id() || scan.Id() > end) {
                continue;
            }
            auto gprs = reg_alloc->DirtyGPR(scan.Id());
            auto fprs = reg_alloc->DirtyFPR(scan.Id());
            saved.push_back({scan.Id(), gprs, fprs});
            gprs.Mark(target);
            reg_alloc->SetActiveRegs(scan.Id(), gprs, fprs);
        }
        bool verified = true;
        for (auto& scan : list) {
            if (scan.Id() >= bridge.Id() && scan.Id() <= end &&
                !CheckInstr(&scan, 0, 0)) {
                verified = false;
                break;
            }
        }
        if (!verified) {
            for (auto& old : saved) {
                reg_alloc->SetActiveRegs(old.id, old.gprs, old.fprs);
            }
            continue;
        }

        reg_alloc->MapRegister(bridge.Id(), HostGPR{target});
        reg_alloc->MarkWidthChainCoalesced(bridge.Id(), anchor);
    }
}


Vector<u32> CollectGuestGPRUseEnds(Block* lir_block, u32 instr_count) {
    auto& list = lir_block->GetInstList();
    Vector<u32> use_end(instr_count);
    for (auto& inst : list) {
        for (auto value : inst.GetValues()) {
            value = ResolveBitCastSource(value);
            if (value.Defined() && value.Id() < use_end.size()) {
                use_end[value.Id()] = std::max<u32>(use_end[value.Id()], inst.Id());
            }
        }
    }
    if (list.begin() != list.end()) {
        const u32 block_end = std::prev(list.end())->Id();
        auto extend_terminal = [&](const Value& value) {
            auto root = ResolveBitCastSource(value);
            if (root.Defined() && root.Id() < use_end.size()) {
                use_end[root.Id()] = std::max(use_end[root.Id()], block_end);
            }
        };
        std::function<void(const Terminal&)> walk_terminal =
                [&](const Terminal& terminal_value) {
                    VisitVariant<void>(terminal_value, [&](const auto& edge) {
                        using T = std::decay_t<decltype(edge)>;
                        if constexpr (std::is_same_v<T, terminal::If>) {
                            extend_terminal(edge.cond);
                            walk_terminal(edge.then_);
                            walk_terminal(edge.else_);
                        } else if constexpr (std::is_same_v<T, terminal::Switch>) {
                            extend_terminal(edge.value);
                            for (const auto& item : edge.cases) {
                                walk_terminal(item.then);
                            }
                        } else if constexpr (std::is_same_v<T, terminal::Condition>) {
                            walk_terminal(edge.then_);
                            walk_terminal(edge.else_);
                        } else if constexpr (std::is_same_v<T, terminal::CheckHalt>) {
                            walk_terminal(edge.else_);
                        }
                    });
                };
        walk_terminal(lir_block->GetTerminal());
    }
    return use_end;
}

void CoalesceGuestGPRReads(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const Vector<u32>& use_end,
        const RegisterAllocFamilyCallbacks& callbacks) {
    auto& list = lir_block->GetInstList();
    auto uses_stay_in_block = [&](Value value) {
        return !callbacks.value_uses_stay_in_block ||
               callbacks.value_uses_stay_in_block(
                       callbacks.context, lir_block, value);
    };
    for (auto& read : list) {
        if (read.GetOp() != OpCode::GetHostGPR ||
            read.GetArg<Imm>(1).Get() != 0 ||
            GetValueSizeByte(read.ReturnType()) != sizeof(u32) ||
            reg_alloc->IsWidthChainCoalesced(read.Id()) ||
            !uses_stay_in_block(Value{&read})) {
            continue;
        }
        const u32 target = read.GetArg<Imm>(0).Get();
        if (!IsPinnedCoalesceTarget(target) ||
            !reg_alloc->GetGprs().Get(target) || read.Id() >= use_end.size()) {
            continue;
        }
        bool width_conflict = false;
        const u32 read_end = std::max<u32>(read.Id(), use_end[read.Id()]);
        for (auto& node : list) {
            if (!node.HasValue() ||
                !reg_alloc->IsWidthChainCoalesced(node.Id()) ||
                reg_alloc->ValueType(Value{&node}) != backend::RegAlloc::GPR ||
                reg_alloc->ValueGPR(Value{&node}).id != target) {
                continue;
            }
            const u32 node_end = node.Id() < use_end.size()
                    ? std::max<u32>(node.Id(), use_end[node.Id()])
                    : node.Id();
            if (node.Id() <= read_end && node_end >= read.Id()) {
                width_conflict = true;
                break;
            }
        }
        if (width_conflict) {
            continue;
        }
        Inst* latest_store = nullptr;
        bool blocked = false;
        for (auto& scan : list) {
            if (&scan == &read) {
                break;
            }
            if (scan.GetOp() == OpCode::SetHostGPR &&
                scan.GetArg<Imm>(1).Get() == target) {
                latest_store = &scan;
                blocked = false;
                continue;
            }
            if (latest_store && IsPinnedCoalesceObserver(scan.GetOp())) {
                blocked = true;
            }
        }
        if (!latest_store || blocked) {
            continue;
        }
        auto published = ResolveBitCastSource(latest_store->GetArg<Value>(0));
        bool high_zero = published.Def() &&
                ((published.Def()->GetOp() == OpCode::ZeroExtend32To64 &&
                  GetValueSizeByte(published.Def()->GetArg<Value>(0).Type()) == sizeof(u32)) ||
                 (GetValueSizeByte(published.Type()) == sizeof(u32) &&
                  HasKnownWWrite(published)));
        if (published.Defined() &&
            reg_alloc->IsWidthChainCoalesced(published.Id())) {
            const u32 anchor = reg_alloc->WidthChainAnchor(published.Id());
            if (reg_alloc->HasWidthComponentOwner(anchor)) {
                // A read reuse of a component publication is the read half of
                // the same transaction.  It cannot survive if the write half
                // failed or selected another home.
                if (!reg_alloc->WidthComponentOwnerCommitted(anchor) ||
                    !reg_alloc->IsHostWriteCoalesced(latest_store->Id()) ||
                    reg_alloc->WidthComponentOwnerTarget(anchor) != target) {
                    continue;
                }
                high_zero = reg_alloc->WidthComponentOwnerHighZero(anchor);
            }
        }
        if (!high_zero) {
            continue;
        }
        for (auto& scan : list) {
            if (scan.Id() <= read.Id() || scan.Id() > use_end[read.Id()]) {
                continue;
            }
            if (scan.GetOp() == OpCode::SetHostGPR &&
                scan.GetArg<Imm>(1).Get() == target) {
                blocked = true;
                break;
            }
        }
        if (blocked) {
            continue;
        }
        MapGuestFixedGPR(reg_alloc, read.Id(), target);
        reg_alloc->MarkHostReadCoalesced(read.Id());
    }
}

bool GuestGPRMappedTo(Value value, u32 target, backend::RegAlloc* reg_alloc) {
    value = ResolveBitCastSource(value);
    return value.Defined() &&
           reg_alloc->ValueType(value) == backend::RegAlloc::GPR &&
           reg_alloc->ValueGPR(value).id == target;
}

bool HasGuestGPRTargetConflict(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const Vector<u32>& use_end,
        Inst* producer,
        Inst* wrapper,
        Inst* store,
        u32 target,
        u32 live_end) {
    auto& list = lir_block->GetInstList();
    auto mapped_to = [&](Value value, u32 mapped_target) {
        return GuestGPRMappedTo(value, mapped_target, reg_alloc);
    };
    for (auto& other : list) {
        if (&other == producer || &other == wrapper || &other == store ||
            !other.HasValue() || other.IsBitCastOperation()) {
            continue;
        }
        // A later GetHost of this home is a read of the published value,
        // not a third-party writer occupying the pin.
        if (other.Id() > store->Id() && other.GetOp() == OpCode::GetHostGPR &&
            other.GetArg<Imm>(0).Get() == target) {
            continue;
        }
        Value value{&other};
        if (!mapped_to(value, target)) {
            continue;
        }
        const u32 start = other.Id();
        const u32 end = value.Id() < use_end.size() ? use_end[value.Id()] : value.Id();
        // A pre-existing tie can define a new value in the
        // producer->publication window. Its ordinary emitter then
        // writes the fixed home even though no Get/SetHostGPR is
        // present for the observer scan to see. Reject every
        // third-party target interval intersecting [producer, live_end].
        if (start <= live_end && end > producer->Id()) {
            return true;
        }
    }
    return false;
}


void CoalesceGuestGPRWrites(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        const Vector<u32>& use_end,
        const Vector<u32>& fixed_gpr_clobbers,
        const RegisterAllocFamilyCallbacks& callbacks) {
    auto& list = lir_block->GetInstList();
    auto mapped_to = [&](Value value, u32 target) {
        return GuestGPRMappedTo(value, target, reg_alloc);
    };
    auto has_target_conflict = [&](Inst* producer, Inst* wrapper,
                                   Inst* store, u32 target, u32 live_end) {
        return HasGuestGPRTargetConflict(
                lir_block, reg_alloc, use_end, producer, wrapper, store, target,
                live_end);
    };
    auto CheckInstr = [&](Inst* inst, u32 extra_gpr, u32 extra_fpr) {
        return callbacks.check_instr(callbacks.context, inst, extra_gpr, extra_fpr);
    };
    auto uses_stay_in_block = [&](Value value) {
        return !callbacks.value_uses_stay_in_block ||
               callbacks.value_uses_stay_in_block(
                       callbacks.context, lir_block, value);
    };

    for (auto& store : list) {
        if (store.GetOp() != OpCode::SetHostGPR ||
            store.GetArg<Imm>(2).Get() != 0) {
            continue;
        }
        const u32 target = store.GetArg<Imm>(1).Get();
        if (!IsPinnedCoalesceTarget(target) ||
            !reg_alloc->GetGprs().Get(target)) {
            continue;
        }

        Value stored = ResolveBitCastSource(store.GetArg<Value>(0));
        auto* wrapper = stored.Def();
        const bool tentative_width_root = features.ra_width_chain && wrapper &&
                (IsWidthChainRootProducer(wrapper) ||
                 (wrapper->GetOp() == OpCode::ZeroExtend32To64 &&
                  IsWidthChainRootProducer(
                          ResolveBitCastSource(wrapper->GetArg<Value>(0)).Def())));
        const bool last_use_is_store =
                wrapper && stored.Id() < use_end.size() &&
                wrapper->GetUses() == 1 && use_end[stored.Id()] == store.Id();
        const bool live_publish =
                features.ra_coalesce_live && wrapper &&
                stored.Id() < use_end.size() && !tentative_width_root &&
                use_end[stored.Id()] >= store.Id() &&
                (wrapper->GetUses() != 1 || use_end[stored.Id()] != store.Id());
        if (!wrapper || stored.Id() >= use_end.size() ||
            (!tentative_width_root && !last_use_is_store && !live_publish)) {
            continue;
        }

        Value produced = stored;
        bool zero_extend_chain = false;
        bool width_root = false;
        if (wrapper->GetOp() == OpCode::ZeroExtend32To64) {
            produced = ResolveBitCastSource(wrapper->GetArg<Value>(0));
            zero_extend_chain = true;
            width_root = features.ra_width_chain &&
                         IsWidthChainRootProducer(produced.Def());
            const bool inner_last =
                    produced.Def() && produced.Id() < use_end.size() &&
                    produced.Def()->GetUses() == 1 &&
                    use_end[produced.Id()] == wrapper->Id();
            const bool inner_live =
                    features.ra_coalesce_live && !width_root && produced.Def() &&
                    produced.Id() < use_end.size() &&
                    use_end[produced.Id()] >= wrapper->Id();
            if ((!HasKnownWWrite(produced) && !width_root) || !produced.Def() ||
                produced.Id() >= use_end.size() ||
                (!width_root && !inner_last && !inner_live)) {
                continue;
            }
        }
        auto* producer = produced.Def();
        width_root |= features.ra_width_chain &&
                      IsWidthChainRootProducer(producer);
        if (!producer ||
            (!IsPinnedCoalesceProducer(producer->GetOp()) && !width_root) ||
            reg_alloc->ValueType(produced) != backend::RegAlloc::GPR ||
            !uses_stay_in_block(produced) ||
            (zero_extend_chain && !uses_stay_in_block(stored))) {
            continue;
        }
        if (width_root &&
            (!reg_alloc->HasWidthComponentOwner(producer->Id()) ||
             reg_alloc->WidthComponentOwnerTarget(producer->Id()) != target)) {
            // The owner was frozen before this pass touched any mapping.  A
            // second fixed-home publication is an ordinary move; failure of
            // the selected publication never causes a retry at this home.
            continue;
        }
        const u32 width = GetValueSizeByte(produced.Type());
        if ((width != sizeof(u32) && width != sizeof(u64)) ||
            (width == sizeof(u32) &&
             !HasKnownWWrite(produced) && !width_root)) {
            continue;
        }
        if (width_root && producer->GetOp() == OpCode::GetHostGPR &&
            producer->GetArg<Imm>(0).Get() == target) {
            // Reading and then overwriting the same fixed home needs a
            // real capture. Mapping the read to that home would make
            // later consumers observe the newly published value.
            continue;
        }
        if (width_root) {
            bool displaces_baseline_read = false;
            for (auto& scan : list) {
                if (scan.GetOp() == OpCode::GetHostGPR &&
                    scan.GetArg<Imm>(0).Get() == target &&
                    reg_alloc->IsHostReadCoalesced(scan.Id())) {
                    displaces_baseline_read = true;
                    break;
                }
            }
            if (displaces_baseline_read) {
                // A gate-OFF read tie has already proved one publication
                // history for this fixed home.  Replacing any producer in
                // that history would require invalidating and rebuilding the
                // read half of the transaction, which is exactly the partial
                // W-alpha state v2 must not create.  Keep the baseline whole.
                continue;
            }
        }
        Vector<Inst*> width_component{};
        if (width_root) {
            width_component.push_back(producer);
            if (producer->GetOp() == OpCode::SignExtend) {
                auto source = ResolveBitCastSource(producer->GetArg<Value>(0));
                auto* source_def = source.Def();
                if (source_def && source_def->GetOp() == OpCode::LoadMemory &&
                    GetValueSizeByte(source.Type()) <= sizeof(u16) &&
                    source_def->GetUses() == 1 &&
                    source.Id() < use_end.size() &&
                    use_end[source.Id()] == producer->Id() &&
                    reg_alloc->ValueType(source) == backend::RegAlloc::GPR) {
                    // Keep the faulting narrow load in the same immutable
                    // component.  A fault does not commit its destination;
                    // success writes Wtarget and lets EmitLoadMemory fold the
                    // following SignExtend into LDRSB/LDRSH.
                    width_component.push_back(source_def);
                }
            }
            if (zero_extend_chain) {
                width_component.push_back(wrapper);
            }
            bool changed = true;
            while (changed) {
                changed = false;
                for (auto& node : list) {
                    if (std::find(width_component.begin(), width_component.end(),
                                  &node) != width_component.end()) {
                        continue;
                    }
                    Value input{};
                    if (node.GetOp() == OpCode::BitExtract &&
                        GetValueSizeByte(node.ReturnType()) == sizeof(u32) &&
                        node.GetArg<Imm>(1).Get() == 0 &&
                        node.GetArg<Imm>(2).Get() == 32) {
                        input = ResolveBitCastSource(node.GetArg<Value>(0));
                    } else if (node.GetOp() == OpCode::ZeroExtend32To64 &&
                               GetValueSizeByte(node.GetArg<Value>(0).Type()) ==
                                       sizeof(u32)) {
                        input = ResolveBitCastSource(node.GetArg<Value>(0));
                    } else {
                        continue;
                    }
                    if (input.Defined() &&
                        std::find(width_component.begin(), width_component.end(),
                                  input.Def()) != width_component.end()) {
                        width_component.push_back(&node);
                        changed = true;
                    }
                }
            }
            if (std::any_of(width_component.begin(), width_component.end(),
                            [&](Inst* node) {
                                return reg_alloc->IsHostReadCoalesced(node->Id());
                            })) {
                // A GetHostGPR that the baseline already mapped to its own
                // home cannot simultaneously become the root of a component
                // owned by a different publication home.  That would retain
                // the read marker while overwriting its mapping, leaving a
                // half-committed transaction for the emitter to discover.
                continue;
            }
        }
        if (mapped_to(produced, target) &&
            (!zero_extend_chain || mapped_to(stored, target))) {
            continue;
        }
        u32 live_end = store.Id();
        if (features.ra_coalesce_live && stored.Id() < use_end.size()) {
            live_end = std::max<u32>(live_end, use_end[stored.Id()]);
            if (zero_extend_chain && produced.Id() < use_end.size()) {
                live_end = std::max<u32>(live_end, use_end[produced.Id()]);
            }
        }
        if (has_target_conflict(producer, wrapper, &store, target, live_end)) {
            continue;
        }
        if (live_end > store.Id()) {
            bool after_store = false;
            bool live_blocked = false;
            for (auto& scan : list) {
                if (&scan == &store) {
                    after_store = true;
                    continue;
                }
                if (!after_store || scan.Id() > live_end) {
                    continue;
                }
                if (scan.GetOp() == OpCode::SetHostGPR &&
                    scan.GetArg<Imm>(1).Get() == target) {
                    live_blocked = true;
                    break;
                }
                if (target <= 9 &&
                    (scan.GetOp() == OpCode::CallLambda ||
                     scan.GetOp() == OpCode::CallLocation ||
                     scan.GetOp() == OpCode::CallDynamic ||
                     scan.GetOp() == OpCode::X87Op ||
                     scan.GetOp() == OpCode::Sse42Str)) {
                    live_blocked = true;
                    break;
                }
            }
            if (live_blocked) {
                continue;
            }
        }

        bool after_producer = false;
        bool blocked = false;
        for (auto& scan : list) {
            if (&scan == producer) {
                after_producer = true;
                continue;
            }
            if (!after_producer || &scan == wrapper) {
                continue;
            }
            if (&scan == &store) {
                break;
            }
            const bool flags_only = scan.GetOp() == OpCode::SaveFlags;
            if ((!flags_only && IsPinnedCoalesceObserver(scan.GetOp())) ||
                (scan.GetOp() == OpCode::GetHostGPR &&
                 scan.GetArg<Imm>(0).Get() == target) ||
                (scan.GetOp() == OpCode::SetHostGPR &&
                 scan.GetArg<Imm>(1).Get() == target)) {
                blocked = true;
                break;
            }
        }
        if (blocked) {
            continue;
        }
        for (auto input : producer->GetValues()) {
            auto root = ResolveBitCastSource(input);
            if (mapped_to(root, target) && root.Id() < use_end.size() &&
                use_end[root.Id()] > producer->Id()) {
                blocked = true;
                break;
            }
        }
        if (blocked) {
            continue;
        }

        if (width_root) {
            u32 component_start = producer->Id();
            u32 component_end = producer->Id();
            for (auto* node : width_component) {
                component_start = std::min<u32>(component_start, node->Id());
                component_end = std::max<u32>(
                        component_end,
                        node->Id() < use_end.size()
                                ? std::max<u32>(node->Id(), use_end[node->Id()])
                                : node->Id());
            }
            auto in_width_component = [&](Inst* node) {
                return std::find(width_component.begin(), width_component.end(),
                                 node) != width_component.end();
            };
            for (auto& other : list) {
                if (in_width_component(&other) || &other == &store ||
                    !other.HasValue() || other.IsBitCastOperation() ||
                    reg_alloc->ValueType(Value{&other}) != backend::RegAlloc::GPR ||
                    !mapped_to(Value{&other}, target)) {
                    continue;
                }
                const u32 other_end = other.Id() < use_end.size()
                        ? std::max<u32>(other.Id(), use_end[other.Id()])
                        : other.Id();
                if (other.Id() <= component_end && other_end >= component_start) {
                    blocked = true;
                    break;
                }
            }
            for (auto& scan : list) {
                if (blocked || scan.Id() < component_start ||
                    scan.Id() > component_end) {
                    continue;
                }
                if ((scan.Id() < fixed_gpr_clobbers.size() &&
                     (fixed_gpr_clobbers[scan.Id()] & (1u << target))) ||
                    (scan.Id() < producer->Id() &&
                     IsPinnedCoalesceObserver(scan.GetOp()) &&
                     !in_width_component(&scan)) ||
                    (scan.GetOp() == OpCode::SetHostGPR &&
                     &scan != &store && scan.GetArg<Imm>(1).Get() == target)) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) {
                continue;
            }

            struct SavedMask {
                u32 id;
                backend::GPRSMask gprs;
                backend::FPRSMask fprs;
            };
            Vector<SavedMask> saved{};
            for (auto& scan : list) {
                if (scan.Id() < component_start || scan.Id() > component_end) {
                    continue;
                }
                auto gprs = reg_alloc->DirtyGPR(scan.Id());
                auto fprs = reg_alloc->DirtyFPR(scan.Id());
                saved.push_back({scan.Id(), gprs, fprs});
                gprs.Mark(target);
                reg_alloc->SetActiveRegs(scan.Id(), gprs, fprs);
            }
            bool verified = true;
            for (auto& scan : list) {
                if (scan.Id() >= component_start && scan.Id() <= component_end &&
                    !CheckInstr(&scan, 0, 0)) {
                    verified = false;
                    break;
                }
            }
            if (!verified) {
                for (auto& old : saved) {
                    reg_alloc->SetActiveRegs(old.id, old.gprs, old.fprs);
                }
                continue;
            }
        }

        if (width_root &&
            !reg_alloc->CommitWidthComponentOwner(producer->Id(),
                                                   static_cast<u16>(target))) {
            continue;
        }
        MapGuestFixedGPR(reg_alloc, producer->Id(), target);
        if (zero_extend_chain) {
            MapGuestFixedGPR(reg_alloc, wrapper->Id(), target);
        }
        reg_alloc->MarkHostWriteCoalesced(store.Id());
        if (width_root) {
            for (auto* node : width_component) {
                MapGuestFixedGPR(reg_alloc, node->Id(), target);
                reg_alloc->MarkWidthChainCoalesced(node->Id(), producer->Id());
            }
        }
    }
}

namespace {

bool IsPinnedWViewProducer(OpCode op) {
    switch (op) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::And:
        case OpCode::AndNot:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::Mul:
            return true;
        default:
            return false;
    }
}

Inst* PublishedPinnedWViewConsumer(Block* lir_block,
                                   backend::RegAlloc* reg_alloc,
                                   Inst& bridge,
                                   u32 target) {
    Inst* consumer = nullptr;
    u32 uses = 0;
    for (auto& candidate : lir_block->GetInstList()) {
        for (auto value : candidate.GetValues()) {
            if (ResolveBitCastSource(value).Def() != &bridge) {
                continue;
            }
            ++uses;
            consumer = &candidate;
        }
    }
    if (uses != 1 || !consumer || !IsPinnedWViewProducer(consumer->GetOp()) ||
        GetValueSizeByte(consumer->ReturnType()) != sizeof(u32) ||
        reg_alloc->ValueType(Value{consumer}) != backend::RegAlloc::GPR ||
        reg_alloc->ValueGPR(Value{consumer}).id != target) {
        return nullptr;
    }

    for (auto& store : lir_block->GetInstList()) {
        if (store.Id() <= consumer->Id() || store.GetOp() != OpCode::SetHostGPR ||
            store.GetArg<Imm>(1).Get() != target || store.GetArg<Imm>(2).Get() != 0 ||
            !reg_alloc->IsHostWriteCoalesced(store.Id())) {
            continue;
        }
        auto published = ResolveBitCastSource(store.GetArg<Value>(0));
        if (published.Def() && published.Def()->GetOp() == OpCode::ZeroExtend32To64) {
            published = ResolveBitCastSource(published.Def()->GetArg<Value>(0));
        }
        if (published.Def() == consumer) {
            return consumer;
        }
    }
    return nullptr;
}

}  // namespace

void CoalescePinnedWViewInputs(Block* lir_block,
                               backend::RegAlloc* reg_alloc,
                               const Vector<u32>& use_end,
                               const RegisterAllocFamilyCallbacks& callbacks) {
    auto CheckInstr = [&](Inst* inst) {
        return callbacks.check_instr(callbacks.context, inst, 0, 0);
    };
    auto uses_stay_in_block = [&](Value value) {
        return !callbacks.value_uses_stay_in_block ||
               callbacks.value_uses_stay_in_block(
                       callbacks.context, lir_block, value);
    };
    for (auto& bridge : lir_block->GetInstList()) {
        if (bridge.GetOp() != OpCode::BitExtract || bridge.GetUses() != 1 ||
            reg_alloc->IsWidthChainCoalesced(bridge.Id()) ||
            GetValueSizeByte(bridge.ReturnType()) != sizeof(u32) ||
            bridge.GetArg<Imm>(1).Get() != 0 || bridge.GetArg<Imm>(2).Get() != 32 ||
            bridge.Id() >= use_end.size() ||
            !uses_stay_in_block(Value{&bridge})) {
            continue;
        }
        auto source = ResolveBitCastSource(bridge.GetArg<Value>(0));
        if (!source.Defined() || source.Id() >= use_end.size() ||
            use_end[source.Id()] != bridge.Id() ||
            reg_alloc->ValueType(source) != backend::RegAlloc::GPR ||
            !uses_stay_in_block(source)) {
            continue;
        }
        const u32 target = reg_alloc->ValueGPR(source).id;
        if (!IsPinnedCoalesceTarget(target) || !reg_alloc->GetGprs().Get(target)) {
            continue;
        }
        auto* consumer = PublishedPinnedWViewConsumer(lir_block, reg_alloc, bridge, target);
        if (!consumer || use_end[bridge.Id()] != consumer->Id()) {
            continue;
        }

        const auto old = reg_alloc->Mapping(bridge.Id());
        if (old.type != backend::RegAlloc::GPR) {
            continue;
        }
        MapGuestFixedGPR(reg_alloc, bridge.Id(), target);
        if (!CheckInstr(&bridge) || !CheckInstr(consumer)) {
            if (old.fixed_gpr) {
                reg_alloc->MapFixedRegister(bridge.Id(), HostGPR{static_cast<u16>(old.slot)});
            } else {
                reg_alloc->MapRegister(bridge.Id(), HostGPR{static_cast<u16>(old.slot)});
            }
            continue;
        }
        const u32 anchor = reg_alloc->IsWidthChainCoalesced(source.Id())
                                   ? reg_alloc->WidthChainAnchor(source.Id())
                                   : source.Id();
        reg_alloc->MarkWidthChainCoalesced(bridge.Id(), anchor);
    }
}

namespace {

enum class SetHostResidual : u32 {
    Coalesced = 0,
    AlreadyHome,
    Partial,
    Narrow,
    CalleeHome,
    OtherHome,
    NoProducer,
    NotProducer,
    LiveOk,
    LiveRewrite,
    LiveHelper,
    Conflict,
    Observer,
    Other,
    Count,
};

enum class GetHostResidual : u32 {
    Coalesced = 0,
    CalleeHome,
    OtherHome,
    Partial,
    Narrow,
    NoStore,
    Observer,
    Other,
    Count,
};

std::atomic<u64> g_sethost[static_cast<u32>(SetHostResidual::Count)]{};
std::atomic<u64> g_gethost[static_cast<u32>(GetHostResidual::Count)]{};
std::atomic<u64> g_notprod[256]{};
std::once_flag g_residual_dump{};

bool IsCallerSavedPin(u32 reg) {
    return reg <= 9;
}

bool IsHelperClobber(OpCode op) {
    switch (op) {
        case OpCode::CallLambda:
        case OpCode::CallLocation:
        case OpCode::CallDynamic:
        case OpCode::X87Op:
        case OpCode::Sse42Str:
            return true;
        default:
            return false;
    }
}

void DumpPinnedHostResidual() {
    auto get = [](auto& arr, auto key) {
        return arr[static_cast<u32>(key)].load(std::memory_order_relaxed);
    };
    const u64 stores = get(g_sethost, SetHostResidual::Coalesced) +
                       get(g_sethost, SetHostResidual::AlreadyHome) +
                       get(g_sethost, SetHostResidual::Partial) +
                       get(g_sethost, SetHostResidual::Narrow) +
                       get(g_sethost, SetHostResidual::CalleeHome) +
                       get(g_sethost, SetHostResidual::OtherHome) +
                       get(g_sethost, SetHostResidual::NoProducer) +
                       get(g_sethost, SetHostResidual::NotProducer) +
                       get(g_sethost, SetHostResidual::LiveOk) +
                       get(g_sethost, SetHostResidual::LiveRewrite) +
                       get(g_sethost, SetHostResidual::LiveHelper) +
                       get(g_sethost, SetHostResidual::Conflict) +
                       get(g_sethost, SetHostResidual::Observer) +
                       get(g_sethost, SetHostResidual::Other);
    const u64 reads = get(g_gethost, GetHostResidual::Coalesced) +
                      get(g_gethost, GetHostResidual::CalleeHome) +
                      get(g_gethost, GetHostResidual::OtherHome) +
                      get(g_gethost, GetHostResidual::Partial) +
                      get(g_gethost, GetHostResidual::Narrow) +
                      get(g_gethost, GetHostResidual::NoStore) +
                      get(g_gethost, GetHostResidual::Observer) +
                      get(g_gethost, GetHostResidual::Other);
    SVM_DIAG_PRINT(RegisterAllocation,
                 "[svm-sethost-residual] stores=%llu coalesced=%llu already_home=%llu "
                 "partial=%llu narrow=%llu callee_home=%llu other_home=%llu "
                 "no_producer=%llu not_producer=%llu live_ok=%llu live_rewrite=%llu "
                 "live_helper=%llu conflict=%llu observer=%llu other=%llu\n",
                 static_cast<unsigned long long>(stores),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::Coalesced)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::AlreadyHome)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::Partial)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::Narrow)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::CalleeHome)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::OtherHome)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::NoProducer)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::NotProducer)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::LiveOk)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::LiveRewrite)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::LiveHelper)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::Conflict)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::Observer)),
                 static_cast<unsigned long long>(get(g_sethost, SetHostResidual::Other)));
    SVM_DIAG_PRINT(RegisterAllocation,
                 "[svm-gethost-residual] reads=%llu coalesced=%llu callee_home=%llu "
                 "other_home=%llu partial=%llu narrow=%llu no_store=%llu observer=%llu "
                 "other=%llu\n",
                 static_cast<unsigned long long>(reads),
                 static_cast<unsigned long long>(get(g_gethost, GetHostResidual::Coalesced)),
                 static_cast<unsigned long long>(get(g_gethost, GetHostResidual::CalleeHome)),
                 static_cast<unsigned long long>(get(g_gethost, GetHostResidual::OtherHome)),
                 static_cast<unsigned long long>(get(g_gethost, GetHostResidual::Partial)),
                 static_cast<unsigned long long>(get(g_gethost, GetHostResidual::Narrow)),
                 static_cast<unsigned long long>(get(g_gethost, GetHostResidual::NoStore)),
                 static_cast<unsigned long long>(get(g_gethost, GetHostResidual::Observer)),
                 static_cast<unsigned long long>(get(g_gethost, GetHostResidual::Other)));
    SVM_DIAG_TEXT(RegisterAllocation, "[svm-sethost-notprod]");
    for (u32 op = 0; op < 256; ++op) {
        const u64 n = g_notprod[op].load(std::memory_order_relaxed);
        if (n == 0) {
            continue;
        }
        SVM_DIAG_PRINT(RegisterAllocation, " op%u=%llu", op, static_cast<unsigned long long>(n));
    }
    SVM_DIAG_PRINT(RegisterAllocation, "%c", '\n');
}

void Count(SetHostResidual reason) {
    g_sethost[static_cast<u32>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void Count(GetHostResidual reason) {
    g_gethost[static_cast<u32>(reason)].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void CensusPinnedHostResidual(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const Vector<u32>& use_end) {
    if (!GetSvmConfig().density_prof) {
        return;
    }
    std::call_once(g_residual_dump, [] { std::atexit(DumpPinnedHostResidual); });

    auto& list = lir_block->GetInstList();
    u64 set_local[static_cast<u32>(SetHostResidual::Count)]{};
    u64 get_local[static_cast<u32>(GetHostResidual::Count)]{};
    auto count_set = [&](SetHostResidual reason) {
        Count(reason);
        ++set_local[static_cast<u32>(reason)];
    };
    auto count_get = [&](GetHostResidual reason) {
        Count(reason);
        ++get_local[static_cast<u32>(reason)];
    };
    for (auto& store : list) {
        if (store.GetOp() != OpCode::SetHostGPR) {
            continue;
        }
        if (reg_alloc->IsHostWriteCoalesced(store.Id())) {
            count_set(SetHostResidual::Coalesced);
            continue;
        }
        const u32 offset = store.GetArg<Imm>(2).Get();
        const u32 target = store.GetArg<Imm>(1).Get();
        Value stored = ResolveBitCastSource(store.GetArg<Value>(0));
        auto* wrapper = stored.Def();
        const u32 store_width = stored.Defined()
                ? GetValueSizeByte(stored.Type())
                : 0;
        if (offset != 0) {
            count_set(SetHostResidual::Partial);
            continue;
        }
        if (store_width != 0 && store_width < sizeof(u32)) {
            count_set(SetHostResidual::Narrow);
            continue;
        }
        if (!IsPinnedCoalesceTarget(target)) {
            count_set(target == 19 || target == 20 || target == 21
                              ? SetHostResidual::CalleeHome
                              : SetHostResidual::OtherHome);
            continue;
        }
        if (!wrapper) {
            count_set(SetHostResidual::NoProducer);
            continue;
        }

        Value produced = stored;
        Inst* producer = wrapper;
        Inst* zext = nullptr;
        if (wrapper->GetOp() == OpCode::ZeroExtend32To64) {
            produced = ResolveBitCastSource(wrapper->GetArg<Value>(0));
            producer = produced.Def();
            zext = wrapper;
        }
        if (!producer) {
            count_set(SetHostResidual::NoProducer);
            continue;
        }
        if (GuestGPRMappedTo(produced, target, reg_alloc) &&
            (!zext || GuestGPRMappedTo(stored, target, reg_alloc))) {
            count_set(SetHostResidual::AlreadyHome);
            continue;
        }
        if (!IsPinnedCoalesceProducer(producer->GetOp()) &&
            !IsWidthChainRootProducer(producer)) {
            count_set(SetHostResidual::NotProducer);
            g_notprod[static_cast<u8>(producer->GetOp())].fetch_add(
                    1, std::memory_order_relaxed);
            continue;
        }

        const bool last_use =
                stored.Id() < use_end.size() &&
                wrapper->GetUses() == 1 &&
                use_end[stored.Id()] == store.Id() &&
                (!zext || (producer->GetUses() == 1 && produced.Id() < use_end.size() &&
                           use_end[produced.Id()] == zext->Id()));
        if (!last_use && stored.Id() < use_end.size()) {
            const u32 last = std::max<u32>(store.Id(), use_end[stored.Id()]);
            bool after = false;
            bool rewrite = false;
            bool helper = false;
            for (auto& scan : list) {
                if (&scan == &store) {
                    after = true;
                    continue;
                }
                if (!after || scan.Id() > last) {
                    continue;
                }
                if (scan.GetOp() == OpCode::SetHostGPR &&
                    scan.GetArg<Imm>(1).Get() == target) {
                    rewrite = true;
                    break;
                }
                if (IsCallerSavedPin(target) && IsHelperClobber(scan.GetOp())) {
                    helper = true;
                }
            }
            if (rewrite) {
                count_set(SetHostResidual::LiveRewrite);
                continue;
            }
            if (helper) {
                count_set(SetHostResidual::LiveHelper);
                continue;
            }
            count_set(SetHostResidual::LiveOk);
            continue;
        }

        if (HasGuestGPRTargetConflict(
                    lir_block, reg_alloc, use_end, producer, wrapper, &store,
                    target, store.Id())) {
            count_set(SetHostResidual::Conflict);
            continue;
        }
        bool after_producer = false;
        bool observer = false;
        for (auto& scan : list) {
            if (&scan == producer) {
                after_producer = true;
                continue;
            }
            if (!after_producer || &scan == wrapper) {
                continue;
            }
            if (&scan == &store) {
                break;
            }
            if (IsPinnedCoalesceObserver(scan.GetOp()) ||
                (scan.GetOp() == OpCode::GetHostGPR &&
                 scan.GetArg<Imm>(0).Get() == target) ||
                (scan.GetOp() == OpCode::SetHostGPR &&
                 scan.GetArg<Imm>(1).Get() == target)) {
                observer = true;
                break;
            }
        }
        if (observer) {
            count_set(SetHostResidual::Observer);
            continue;
        }
        count_set(SetHostResidual::Other);
    }

    for (auto& read : list) {
        if (read.GetOp() != OpCode::GetHostGPR) {
            continue;
        }
        if (reg_alloc->IsHostReadCoalesced(read.Id())) {
            count_get(GetHostResidual::Coalesced);
            continue;
        }
        const u32 offset = read.GetArg<Imm>(1).Get();
        const u32 target = read.GetArg<Imm>(0).Get();
        const u32 width = GetValueSizeByte(read.ReturnType());
        if (offset != 0) {
            count_get(GetHostResidual::Partial);
            continue;
        }
        if (width < sizeof(u32)) {
            count_get(GetHostResidual::Narrow);
            continue;
        }
        if (!IsPinnedCoalesceTarget(target)) {
            count_get(target == 19 || target == 20 || target == 21
                              ? GetHostResidual::CalleeHome
                              : GetHostResidual::OtherHome);
            continue;
        }
        Inst* latest_store = nullptr;
        bool blocked = false;
        for (auto& scan : list) {
            if (&scan == &read) {
                break;
            }
            if (scan.GetOp() == OpCode::SetHostGPR &&
                scan.GetArg<Imm>(1).Get() == target) {
                latest_store = &scan;
                blocked = false;
                continue;
            }
            if (latest_store && IsPinnedCoalesceObserver(scan.GetOp())) {
                blocked = true;
            }
        }
        if (!latest_store) {
            count_get(GetHostResidual::NoStore);
            continue;
        }
        if (blocked) {
            count_get(GetHostResidual::Observer);
            continue;
        }
        count_get(GetHostResidual::Other);
    }
    auto s = [&](SetHostResidual r) {
        return set_local[static_cast<u32>(r)];
    };
    auto g = [&](GetHostResidual r) {
        return get_local[static_cast<u32>(r)];
    };
    SVM_DIAG_PRINT(RegisterAllocation,
            "[svm-sethost-residual-block] pc=0x%llx stores=%llu coalesced=%llu already_home=%llu "
            "partial=%llu narrow=%llu callee_home=%llu other_home=%llu no_producer=%llu "
            "not_producer=%llu live_ok=%llu live_rewrite=%llu live_helper=%llu conflict=%llu "
            "observer=%llu other=%llu reads=%llu r_coalesced=%llu r_callee_home=%llu "
            "r_other_home=%llu r_partial=%llu r_narrow=%llu r_no_store=%llu r_observer=%llu "
            "r_other=%llu\n",
            static_cast<unsigned long long>(lir_block->GetStartLocation().Value()),
            static_cast<unsigned long long>(s(SetHostResidual::Coalesced) +
                                            s(SetHostResidual::AlreadyHome) +
                                            s(SetHostResidual::Partial) +
                                            s(SetHostResidual::Narrow) +
                                            s(SetHostResidual::CalleeHome) +
                                            s(SetHostResidual::OtherHome) +
                                            s(SetHostResidual::NoProducer) +
                                            s(SetHostResidual::NotProducer) +
                                            s(SetHostResidual::LiveOk) +
                                            s(SetHostResidual::LiveRewrite) +
                                            s(SetHostResidual::LiveHelper) +
                                            s(SetHostResidual::Conflict) +
                                            s(SetHostResidual::Observer) +
                                            s(SetHostResidual::Other)),
            static_cast<unsigned long long>(s(SetHostResidual::Coalesced)),
            static_cast<unsigned long long>(s(SetHostResidual::AlreadyHome)),
            static_cast<unsigned long long>(s(SetHostResidual::Partial)),
            static_cast<unsigned long long>(s(SetHostResidual::Narrow)),
            static_cast<unsigned long long>(s(SetHostResidual::CalleeHome)),
            static_cast<unsigned long long>(s(SetHostResidual::OtherHome)),
            static_cast<unsigned long long>(s(SetHostResidual::NoProducer)),
            static_cast<unsigned long long>(s(SetHostResidual::NotProducer)),
            static_cast<unsigned long long>(s(SetHostResidual::LiveOk)),
            static_cast<unsigned long long>(s(SetHostResidual::LiveRewrite)),
            static_cast<unsigned long long>(s(SetHostResidual::LiveHelper)),
            static_cast<unsigned long long>(s(SetHostResidual::Conflict)),
            static_cast<unsigned long long>(s(SetHostResidual::Observer)),
            static_cast<unsigned long long>(s(SetHostResidual::Other)),
            static_cast<unsigned long long>(g(GetHostResidual::Coalesced) +
                                            g(GetHostResidual::CalleeHome) +
                                            g(GetHostResidual::OtherHome) +
                                            g(GetHostResidual::Partial) +
                                            g(GetHostResidual::Narrow) +
                                            g(GetHostResidual::NoStore) +
                                            g(GetHostResidual::Observer) +
                                            g(GetHostResidual::Other)),
            static_cast<unsigned long long>(g(GetHostResidual::Coalesced)),
            static_cast<unsigned long long>(g(GetHostResidual::CalleeHome)),
            static_cast<unsigned long long>(g(GetHostResidual::OtherHome)),
            static_cast<unsigned long long>(g(GetHostResidual::Partial)),
            static_cast<unsigned long long>(g(GetHostResidual::Narrow)),
            static_cast<unsigned long long>(g(GetHostResidual::NoStore)),
            static_cast<unsigned long long>(g(GetHostResidual::Observer)),
            static_cast<unsigned long long>(g(GetHostResidual::Other)));
}

}  // namespace swift::runtime::ir
