#include "translator.h"

#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

Label* JitTranslator::GetUnalignedAtomicFallback(const ir::Inst* inst) {
    ASSERT(edge_actions);
    const auto* cohort = context.GetActions().AtomicFor(inst);
    if (!cohort || cohort->count < 2) return nullptr;
    auto [it, inserted] = memory_state.unaligned_atomic_fallbacks.try_emplace(cohort->key);
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
}

#undef __

}  // namespace swift::runtime::backend::arm64
