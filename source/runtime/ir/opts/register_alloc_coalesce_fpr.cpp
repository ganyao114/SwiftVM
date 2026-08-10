#include "register_alloc_internal.h"

namespace swift::runtime::ir {

bool IsResidentFPRTarget(u32 reg) {
    // The actual enabled subset is determined by the allocator's reserved
    // FPR mask below.  Keeping one architectural range avoids a parallel
    // high-XMM coalescer and naturally supports the P1 partial map.
    return reg >= 16 && reg <= 31;
}

bool IsScalarFPRBinaryProducer(OpCode op) {
    switch (op) {
        case OpCode::VecFAddScalar32:
        case OpCode::VecFSubScalar32:
        case OpCode::VecFMulScalar32:
        case OpCode::VecFDivScalar32:
        case OpCode::VecFAddScalar64:
        case OpCode::VecFSubScalar64:
        case OpCode::VecFMulScalar64:
        case OpCode::VecFDivScalar64:
            return true;
        default:
            return false;
    }
}

bool IsResidentFPRProducer(OpCode op, bool scalar_tie) {
    switch (op) {
        // These emitters either write a fresh destination or use a single
        // A64 three-register SIMD instruction. Their source/destination
        // aliasing is architectural; the input-liveness guard below still
        // rejects an input that must survive the publication point.
        case OpCode::LoadUniform:
        case OpCode::LoadMemory:
        case OpCode::VecAnd:
        case OpCode::VecOr:
        case OpCode::VecXor:
        case OpCode::VecAdd:
        case OpCode::VecSub:
        case OpCode::VecMul:
        case OpCode::VecFAdd:
        case OpCode::VecFSub:
        case OpCode::VecFMul:
        case OpCode::VecFDiv:
            return true;
        default:
            return scalar_tie && IsScalarFPRBinaryProducer(op);
    }
}

bool IsAesEncChainProducer(OpCode op) {
    return op == OpCode::VecAesEncFast || op == OpCode::VecAesEncLastFast;
}

Vector<u32> CollectGuestFPRUseEnds(Block* lir_block, u32 instr_count) {
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
    return use_end;
}

bool GuestFPRMappedTo(Value value, u32 target, backend::RegAlloc* reg_alloc) {
    value = ResolveBitCastSource(value);
    return value.Defined() &&
           reg_alloc->ValueType(value) == backend::RegAlloc::FPR &&
           reg_alloc->ValueFPR(value).id == target;
}

bool TryCoalesceAesChain(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        const Vector<u32>& use_end,
        Inst& store,
        Value produced,
        u32 target) {
    auto& list = lir_block->GetInstList();
    auto mapped_to = [&](Value value, u32 mapped_target) {
        return GuestFPRMappedTo(value, mapped_target, reg_alloc);
    };

    if (!features.ra_aes_chain_tie || !produced.Def() ||
        !IsAesEncChainProducer(produced.Def()->GetOp())) {
        return false;
    }

    Vector<Inst*> reverse_chain;
    Value head{};
    for (auto* node = produced.Def(); node && IsAesEncChainProducer(node->GetOp());) {
        reverse_chain.push_back(node);
        auto data = ResolveBitCastSource(node->GetArg<Value>(0));
        if (!data.Defined() || !data.Def()) {
            return false;
        }
        if (!IsAesEncChainProducer(data.Def()->GetOp())) {
            head = data;
            break;
        }
        node = data.Def();
    }
    if (reverse_chain.size() < 2 || !head.Defined() || !head.Def() ||
        head.Def()->GetOp() != OpCode::GetHostFPR ||
        head.Def()->GetArg<Imm>(0).Get() != target ||
        head.Def()->GetArg<Imm>(1).Get() != 0 ||
        !reg_alloc->IsHostReadCoalesced(head.Id()) ||
        !mapped_to(head, target)) {
        return false;
    }
    std::reverse(reverse_chain.begin(), reverse_chain.end());
    const auto& chain = reverse_chain;
    auto is_chain_node = [&](const Inst* candidate) {
        return std::find(chain.begin(), chain.end(), candidate) != chain.end();
    };

    for (std::size_t i = 0; i < chain.size(); ++i) {
        auto* node = chain[i];
        Value data = ResolveBitCastSource(node->GetArg<Value>(0));
        Value expected_data = i == 0 ? head : Value{chain[i - 1]};
        if (data.Def() != expected_data.Def() || data.Id() >= use_end.size() ||
            use_end[data.Id()] != node->Id() ||
            node->ReturnType() != ValueType::V128 ||
            reg_alloc->ValueType(Value{node}) != backend::RegAlloc::FPR) {
            return false;
        }
        for (std::size_t arg = 1; arg < 3; ++arg) {
            auto input = ResolveBitCastSource(node->GetArg<Value>(arg));
            if (mapped_to(input, target)) {
                return false;
            }
        }

        Inst* expected_consumer = i + 1 < chain.size() ? chain[i + 1] : &store;
        u32 semantic_uses = 0;
        for (auto& scan : list) {
            if (scan.IsBitCastOperation()) {
                continue;
            }
            for (auto value : scan.GetValues()) {
                if (ResolveBitCastSource(value).Def() == node) {
                    if (&scan != expected_consumer) {
                        return false;
                    }
                    ++semantic_uses;
                }
            }
        }
        if (semantic_uses != 1) {
            return false;
        }
    }

    const u32 begin = chain.front()->Id();
    if (produced.Id() >= use_end.size() || use_end[produced.Id()] != store.Id()) {
        return false;
    }
    for (auto& other : list) {
        if (&other == &store || &other == head.Def() || is_chain_node(&other) ||
            !other.HasValue() || other.IsBitCastOperation() ||
            other.Id() >= store.Id()) {
            continue;
        }
        Value value{&other};
        if (mapped_to(value, target) && value.Id() < use_end.size() &&
            use_end[value.Id()] > begin) {
            return false;
        }
    }

    for (auto& scan : list) {
        if (scan.Id() <= begin || scan.Id() >= store.Id() ||
            is_chain_node(&scan) || scan.IsBitCastOperation()) {
            continue;
        }
        if ((scan.GetOp() == OpCode::GetHostFPR &&
             scan.GetArg<Imm>(0).Get() == target) ||
            (scan.GetOp() == OpCode::SetHostFPR &&
             scan.GetArg<Imm>(1).Get() == target)) {
            return false;
        }
        // A completed AES node has already committed the resident
        // XMM value in its fixed home. Fault recovery saves that
        // home through BuildSaveStaticUniform, so an ordinary load
        // may fault without observing a stale value. Other generic
        // observers keep the existing fail-closed rule.
        const bool committed_home_safe =
                scan.GetOp() == OpCode::LoadMemory ||
                scan.GetOp() == OpCode::StoreMemory ||
                scan.GetOp() == OpCode::SaveFlags;
        if (IsPinnedCoalesceObserver(scan.GetOp()) &&
            !committed_home_safe) {
            return false;
        }
    }

    for (auto* node : chain) {
        reg_alloc->MapRegister(node->Id(), HostFPR{static_cast<u16>(target)});
        reg_alloc->MarkAesChainTied(node->Id(), static_cast<u16>(target));
    }
    reg_alloc->MarkHostWriteCoalesced(store.Id());
    return true;
}

void CoalesceGuestFPRWrites(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        u32 instr_count,
        bool scalar_insert) {
    auto& list = lir_block->GetInstList();
    auto use_end = CollectGuestFPRUseEnds(lir_block, instr_count);

    auto mapped_to = [&](Value value, u32 target) {
        return GuestFPRMappedTo(value, target, reg_alloc);
    };

    auto try_aes_chain = [&](Inst& store, Value produced, u32 target) {
        return TryCoalesceAesChain(
                lir_block, reg_alloc, features, use_end, store, produced, target);
    };

    for (auto& store : list) {
        if (store.GetOp() != OpCode::SetHostFPR ||
            store.GetArg<Imm>(2).Get() != 0) {
            continue;
        }
        const u32 target = store.GetArg<Imm>(1).Get();
        if (!IsResidentFPRTarget(target) || !reg_alloc->GetFprs().Get(target)) {
            continue;
        }
        Value produced = ResolveBitCastSource(store.GetArg<Value>(0));
        auto* producer = produced.Def();
        if (try_aes_chain(store, produced, target)) {
            continue;
        }
        if (!producer || produced.Id() >= use_end.size() ||
            produced.Type() != ValueType::V128 ||
            use_end[produced.Id()] != store.Id() ||
            !IsResidentFPRProducer(producer->GetOp(),
                                   features.sse_scalar_tie && scalar_insert) ||
            reg_alloc->ValueType(produced) != backend::RegAlloc::FPR) {
            continue;
        }
        if (IsScalarFPRBinaryProducer(producer->GetOp())) {
            auto left = ResolveBitCastSource(producer->GetArg<Value>(0));
            if (!left.Defined() || left.Id() >= use_end.size() ||
                !reg_alloc->IsHostReadCoalesced(left.Id()) ||
                !mapped_to(left, target) ||
                use_end[left.Id()] != producer->Id()) {
                continue;
            }
        }
        if (mapped_to(produced, target)) {
            reg_alloc->MarkHostWriteCoalesced(store.Id());
            continue;
        }

        bool blocked = false;
        for (auto& other : list) {
            if (&other == producer || &other == &store || !other.HasValue() ||
                other.IsBitCastOperation() || other.Id() >= store.Id()) {
                continue;
            }
            Value value{&other};
            if (mapped_to(value, target) && value.Id() < use_end.size() &&
                use_end[value.Id()] > producer->Id()) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;

        bool after_producer = false;
        for (auto& scan : list) {
            if (&scan == producer) {
                after_producer = true;
                continue;
            }
            if (!after_producer) continue;
            if (&scan == &store) break;
            if (IsPinnedCoalesceObserver(scan.GetOp()) ||
                (scan.GetOp() == OpCode::GetHostFPR &&
                 scan.GetArg<Imm>(0).Get() == target) ||
                (scan.GetOp() == OpCode::SetHostFPR &&
                 scan.GetArg<Imm>(1).Get() == target)) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;

        for (auto input : producer->GetValues()) {
            auto root = ResolveBitCastSource(input);
            if (mapped_to(root, target) && root.Id() < use_end.size() &&
                use_end[root.Id()] > producer->Id()) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;

        reg_alloc->MapRegister(producer->Id(), HostFPR{static_cast<u16>(target)});
        reg_alloc->MarkHostWriteCoalesced(store.Id());
    }
}


}  // namespace swift::runtime::ir
