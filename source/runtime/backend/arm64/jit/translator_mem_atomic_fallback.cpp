#include "translator.h"

#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

std::optional<JitTranslator::UnalignedAtomicFallbackKey>
JitTranslator::GetUnalignedAtomicFallbackKey(ir::Inst* inst) {
    if (!CanUseLSE()) {
        return std::nullopt;
    }

    UnalignedAtomicFallbackKey key{};
    ir::Value address{};
    ir::Value first{};
    ir::Value second{};
    switch (inst->GetOp()) {
        case ir::OpCode::CompareAndSwap:
            key.kind = UnalignedAtomicFallbackKind::CompareAndSwap;
            address = inst->GetArg<ir::Value>(0);
            first = inst->GetArg<ir::Value>(1);
            second = inst->GetArg<ir::Value>(2);
            key.type = first.Type();
            break;
        case ir::OpCode::AtomicExchange:
            key.kind = UnalignedAtomicFallbackKind::Exchange;
            address = inst->GetArg<ir::Value>(0);
            first = inst->GetArg<ir::Value>(1);
            key.type = first.Type();
            break;
        case ir::OpCode::AtomicFetchAdd:
            key.kind = UnalignedAtomicFallbackKind::FetchAdd;
            address = inst->GetArg<ir::Value>(0);
            first = inst->GetArg<ir::Value>(1);
            key.type = first.Type();
            break;
        default:
            return std::nullopt;
    }

    const u32 width = ir::GetValueSizeByte(key.type);
    if (width != sizeof(u32) && width != sizeof(u64)) {
        return std::nullopt;
    }

    const auto result = context.MappedGPRCode(ir::Value{inst});
    const auto first_code = context.MappedGPRCode(first);
    const auto address_code = memory_state.use_memory_base
            ? std::optional<u8>{static_cast<u8>(mem_scratch.GetCode())}
            : context.MappedGPRCode(address);
    if (!result || !first_code || !address_code) {
        return std::nullopt;
    }

    key.address = *address_code;
    key.result = *result;
    key.first = *first_code;
    if (key.result == key.first) {
        return std::nullopt;
    }
    if (key.kind == UnalignedAtomicFallbackKind::CompareAndSwap) {
        const auto second_code = context.MappedGPRCode(second);
        if (!second_code || key.result == *second_code) {
            return std::nullopt;
        }
        key.second = *second_code;
    }
    return key;
}

void JitTranslator::PrepareUnalignedAtomicFallbacks(
        std::span<ir::Block* const> blocks) {
    ASSERT(memory_state.unaligned_atomic_fallbacks.empty());
    memory_state.unaligned_atomic_fallback_counts.clear();
    for (auto* block : blocks) {
        for (auto& inst : block->GetInstList()) {
            if (const auto key = GetUnalignedAtomicFallbackKey(&inst)) {
                ++memory_state.unaligned_atomic_fallback_counts[*key];
            }
        }
    }
}

Label* JitTranslator::GetUnalignedAtomicFallback(ir::Inst* inst) {
    const auto key = GetUnalignedAtomicFallbackKey(inst);
    if (!key) {
        return nullptr;
    }
    const auto count = memory_state.unaligned_atomic_fallback_counts.find(*key);
    if (count == memory_state.unaligned_atomic_fallback_counts.end() || count->second < 2) {
        return nullptr;
    }
    auto [it, inserted] = memory_state.unaligned_atomic_fallbacks.try_emplace(*key);
    if (inserted) {
        it->second = std::make_unique<Label>();
    }
    return it->second.get();
}

void JitTranslator::EmitUnalignedAtomicFallback(
        const UnalignedAtomicFallbackKey& key) {
    const bool wide = ir::GetValueSizeByte(key.type) == sizeof(u64);
    const Register address = XRegister{key.address};
    const Register result = wide ? Register{XRegister{key.result}}
                                 : Register{WRegister{key.result}};
    const Register first = wide ? Register{XRegister{key.first}}
                                : Register{WRegister{key.first}};
    const Register second = wide ? Register{XRegister{key.second}}
                                 : Register{WRegister{key.second}};

    __ Dmb(InnerShareable, BarrierAll);
    LoadUnalignedAtomicLockAddress(atomic_scratch);
    Label retry;
    __ Bind(&retry);
    __ Ldaxr(result.W(), MemOperand(atomic_scratch));
    __ Cbnz(result.W(), &retry);
    __ Mov(result.W(), 1);
    __ Stxr(ipw, result.W(), MemOperand(atomic_scratch));
    __ Cbnz(ipw, &retry);

    EmitBasicAtomicLoad(key.type, result, address);
    switch (key.kind) {
        case UnalignedAtomicFallbackKind::CompareAndSwap: {
            Label no_store;
            __ Cmp(result, first);
            __ B(&no_store, ne);
            EmitBasicAtomicStore(key.type, second, address);
            __ Bind(&no_store);
            break;
        }
        case UnalignedAtomicFallbackKind::Exchange:
            EmitBasicAtomicStore(key.type, first, address);
            break;
        case UnalignedAtomicFallbackKind::FetchAdd:
            if (wide) {
                __ Add(ip, result, first);
            } else {
                __ Add(ipw, result.W(), first.W());
            }
            EmitBasicAtomicStore(key.type, wide ? Register{ip} : Register{ipw},
                                 address);
            break;
    }
    ReleaseUnalignedAtomicLock(atomic_scratch);
    __ Dmb(InnerShareable, BarrierAll);
    __ Br(atomic_pair_scratch);
}

void JitTranslator::EmitUnalignedAtomicFallbacks() {
    if (memory_state.unaligned_atomic_fallbacks.empty()) {
        memory_state.unaligned_atomic_fallback_counts.clear();
        return;
    }
    const bool own_cold_scratch = !context.ColdScratchActive();
    if (own_cold_scratch) {
        context.BeginColdScratch();
    }
    for (const auto& [key, label] : memory_state.unaligned_atomic_fallbacks) {
        __ Bind(label.get());
        EmitUnalignedAtomicFallback(key);
    }
    if (own_cold_scratch) {
        context.EndColdScratch();
    }
    memory_state.unaligned_atomic_fallbacks.clear();
    memory_state.unaligned_atomic_fallback_counts.clear();
}

#undef __

}  // namespace swift::runtime::backend::arm64
