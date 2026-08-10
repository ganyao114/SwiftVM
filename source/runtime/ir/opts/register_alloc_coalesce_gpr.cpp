#include "register_alloc_internal.h"

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
    return reg <= 9 || reg == 22 || reg == 23 || reg == 29;
}

bool IsPinnedCoalesceProducer(OpCode op) {
    switch (op) {
        case OpCode::LoadImm:
        case OpCode::LoadUniform:
        case OpCode::Zero:
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::And:
        case OpCode::AndNot:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::Adc:
        case OpCode::Sbb:
        case OpCode::Mul:
        case OpCode::Div:
        case OpCode::Not:
        case OpCode::LslImm:
        case OpCode::LslValue:
        case OpCode::LsrImm:
        case OpCode::LsrValue:
        case OpCode::AsrImm:
        case OpCode::AsrValue:
        case OpCode::RorImm:
        case OpCode::RorValue:
        case OpCode::ByteSwap:
        case OpCode::BitExtract:
        case OpCode::BitClear:
        case OpCode::Select:
        case OpCode::CondSelect:
        case OpCode::MulHigh:
            return true;
        default:
            return false;
    }
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
        case OpCode::SaveFlags:
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

void PlanWidthComponentOwners(
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
            return std::any_of(
                    current.producer->GetValues().begin(),
                    current.producer->GetValues().end(),
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
        const bool long_u32_snapshot =
                long_candidate && source.Defined() && source.Def() &&
                source.Def()->GetOp() == OpCode::GetHostGPR &&
                GetValueSizeByte(source.Type()) == sizeof(u32);
        if (!source.Defined() ||
            (!HasKnownWWrite(source) && !component_w_write &&
             !long_u32_snapshot) ||
            reg_alloc->ValueType(source) != backend::RegAlloc::GPR ||
            reg_alloc->ValueType(Value{&bridge}) != backend::RegAlloc::GPR ||
            bridge.Id() >= use_end.size() || bridge.GetUses() == 0) {
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
                const bool exact_last_use_handoff =
                        long_candidate && other.Id() == end &&
                        std::any_of(other.GetValues().begin(),
                                    other.GetValues().end(),
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
        const Vector<u32>& use_end) {
    auto& list = lir_block->GetInstList();
    for (auto& read : list) {
        if (read.GetOp() != OpCode::GetHostGPR ||
            read.GetArg<Imm>(1).Get() != 0 ||
            GetValueSizeByte(read.ReturnType()) != sizeof(u32) ||
            reg_alloc->IsWidthChainCoalesced(read.Id())) {
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
        u32 target) {
    auto& list = lir_block->GetInstList();
    auto mapped_to = [&](Value value, u32 mapped_target) {
        return GuestGPRMappedTo(value, mapped_target, reg_alloc);
    };
    for (auto& other : list) {
        if (&other == producer || &other == wrapper || &other == store ||
            !other.HasValue() || other.IsBitCastOperation() ||
            other.Id() >= store->Id()) {
            continue;
        }
        Value value{&other};
        if (!mapped_to(value, target)) {
            continue;
        }
        const u32 end = value.Id() < use_end.size() ? use_end[value.Id()] : value.Id();
        // A pre-existing tie can define a new value in the
        // producer->publication window. Its ordinary emitter then
        // writes the fixed home even though no Get/SetHostGPR is
        // present for the observer scan to see. Reject every
        // third-party target interval intersecting that window,
        // from either side of the producer definition.
        if (end > producer->Id()) {
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
                                   Inst* store, u32 target) {
        return HasGuestGPRTargetConflict(
                lir_block, reg_alloc, use_end, producer, wrapper, store, target);
    };
    auto CheckInstr = [&](Inst* inst, u32 extra_gpr, u32 extra_fpr) {
        return callbacks.check_instr(callbacks.context, inst, extra_gpr, extra_fpr);
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
        if (!wrapper || stored.Id() >= use_end.size() ||
            (!tentative_width_root &&
             (wrapper->GetUses() != 1 || use_end[stored.Id()] != store.Id()))) {
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
            if ((!HasKnownWWrite(produced) && !width_root) || !produced.Def() ||
                produced.Id() >= use_end.size() ||
                (!width_root &&
                 (produced.Def()->GetUses() != 1 ||
                  use_end[produced.Id()] != wrapper->Id()))) {
                continue;
            }
        }
        auto* producer = produced.Def();
        width_root |= features.ra_width_chain &&
                      IsWidthChainRootProducer(producer);
        if (!producer ||
            (!IsPinnedCoalesceProducer(producer->GetOp()) && !width_root) ||
            reg_alloc->ValueType(produced) != backend::RegAlloc::GPR) {
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
            // real snapshot. Mapping the read to that home would make
            // later consumers observe the newly published value.
            continue;
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
        }
        if (mapped_to(produced, target) &&
            (!zero_extend_chain || mapped_to(stored, target))) {
            continue;
        }
        if (has_target_conflict(producer, wrapper, &store, target)) {
            continue;
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
            if (IsPinnedCoalesceObserver(scan.GetOp()) ||
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


}  // namespace swift::runtime::ir
