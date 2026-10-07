#include "jit_context.h"

#include "runtime/ir/instr.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;

void JitContext::EnterFloatMode() {
    if (vector_float) masm.FSRMI(0);
}

void JitContext::LeaveFloatMode() {
    if (vector_float) { Load(t5, frame, float_mode_offset); masm.FSRM(t5); }
}

std::array<GPR, 2> JitContext::ResultPair(ir::Inst* inst) {
    if (!cache_scalars) return {t0, t1};
    // Split V256 chains already have eight live operand GPRs. Reuse a
    // dying input pair in place instead of spilling it merely to allocate
    // another pair. Keep the old mapping until WritePair: operand reads must
    // still see the original registers. Pinned phis/uniforms are excluded.
    if (pair_coalescing) for (u32 slot = 0; slot < 2; ++slot) {
        if (!inst->ArgAt(slot).IsValue()) continue;
        auto* input = inst->ArgAt(slot).Get<ir::Value>().Def();
        bool later_alias{};
        for (u32 other = 2; other < ir::Inst::max_args; ++other)
            later_alias |= inst->ArgAt(other).IsValue() && inst->ArgAt(other).Get<ir::Value>().Def() == input;
        if (later_alias) continue;
        auto last = last_uses.find(input);
        if (last == last_uses.end() || last->second != current_position) continue;
        std::array<GPR, 2> pair{x0, x0};
        for (u32 i = 0; i < cached_values.size(); ++i)
            if (cached_values[i] == input && cached_parts[i] < 2) pair[cached_parts[i]] = scalar_registers[i];
        if (pair[0] != x0 && pair[1] != x0) return pair;
    }
    // Reserve both before reads: either destination may evict an input half.
    const auto low = ReserveRegister();
    const auto high = ReserveRegister(low);
    ASSERT(low != high);
    return {low, high};
}

void JitContext::WritePair(ir::Inst* inst, const std::array<GPR, 2>& registers) {
    for (u32 part = 0; part < 2; ++part) {
        if (!cache_scalars) {
            Store(registers[part], values, static_cast<s64>(inst->Id()) * kValueStride + part * 8);
            ++value_stats.stores;
        } else {
            bool found{};
            for (size_t i = 0; i < scalar_registers.size(); ++i) {
                if (scalar_registers[i] == registers[part]) {
                    ASSERT(!cached_values[i] || (pair_coalescing && last_uses.at(cached_values[i]) == current_position));
                    cached_values[i] = inst;
                    cached_parts[i] = part;
                    found = true;
                }
            }
            ASSERT(found);
        }
    }
}

void JitContext::SetVectorType(u32 bits, u32 lanes) {
    ASSERT(features.vector && bits >= 8 && bits <= 64 && lanes * bits <= 128);
    if (vector_bits == bits && vector_lanes == lanes) return;
    const auto sew = bits == 8 ? SEW::E8 : bits == 16 ? SEW::E16 : bits == 32 ? SEW::E32 : SEW::E64;
    masm.VSETIVLI(x0, lanes, sew, LMUL::M1, VTA::No, VMA::No);
    vector_bits = bits; vector_lanes = lanes;
}

void JitContext::SetVectorRounding(u32 mode) {
    ASSERT(features.vector && mode < 4);
    if (vector_round_mode != mode) masm.CSRWI(CSR::VXRM, mode);
    vector_round_mode = mode;
}

void JitContext::SpillVector(size_t index) {
    auto* inst = cached_vectors[index];
    if (!inst) return;
    SetVectorType(64, 2);
    Address(t6, values, static_cast<s64>(inst->Id()) * kValueStride);
    // A whole-register store would overflow a 16-byte home when VLEN > 128.
    masm.VSE64(Vec{static_cast<u32>(index + 8)}, t6);
    ++value_stats.stores;
}

Vec JitContext::ResultVector(ir::Inst*) {
    ASSERT(features.vector);
    if (!cache_scalars) return v3;
    auto index = next_vector;
    while (reserved_vectors & (1u << index)) index = (index + 1) % cached_vectors.size();
    for (size_t scanned = 0; scanned < cached_vectors.size(); ++scanned) {
        const auto candidate = (next_vector + scanned) % cached_vectors.size();
        if (!(reserved_vectors & (1u << candidate)) && !cached_vectors[candidate]) { index = candidate; break; }
    }
    next_vector = (index + 1) % cached_vectors.size();
    if (cached_vectors[index]) { SpillVector(index); ++value_stats.spills; }
    cached_vectors[index] = nullptr;
    return Vec{static_cast<u32>(index + 8)};
}

Vec JitContext::SourceVector(ir::Value value, Vec scratch) {
    for (const auto& binding : phi_bindings)
        if (binding.inst == value.Def() && binding.vector_index != UINT32_MAX) return Vec{binding.vector_index + 8};
    for (size_t i = 0; i < cached_vectors.size(); ++i) {
        if (cached_vectors[i] == value.Def()) { ++value_stats.cache_hits; return Vec{static_cast<u32>(i + 8)}; }
    }
    SetVectorType(64, 2);
    // A GPR pair can contain scalar bitcasts or baseline results. Build [lo,
    // hi] without writing a temporary home; slide's source differs from vd.
    bool gpr_cached{};
    for (const auto& binding : phi_bindings)
        gpr_cached |= binding.inst == value.Def() && binding.gpr_indices[0] != UINT32_MAX;
    for (auto* cached : cached_values) gpr_cached |= cached == value.Def();
    if (gpr_cached || ZeroHigh(value.Def())) {
        ReadPart(a4, value, 0); ReadPart(a5, value, 1);
        masm.VMV(v6, a5); masm.VSLIDE1UP(scratch, v6, a4);
    } else {
        Address(t6, values, static_cast<s64>(value.Def()->Id()) * kValueStride);
        masm.VLE64(scratch, t6);
        ++value_stats.loads;
    }
    return scratch;
}

void JitContext::WriteVector(ir::Inst* inst, Vec value) {
    if (cache_scalars) {
        const auto index = value.Index() - 8;
        ASSERT(index < cached_vectors.size() && !cached_vectors[index]);
        cached_vectors[index] = inst;
    } else {
        SetVectorType(64, 2);
        Address(t6, values, static_cast<s64>(inst->Id()) * kValueStride);
        masm.VSE64(value, t6);
        ++value_stats.stores;
    }
}

void JitContext::SaveVectorsForCall(bool abi_call) {
    if (abi_call) MarkABICall();
    if (abi_call) for (const auto& binding : phi_bindings) if (binding.vector_index != UINT32_MAX) {
        SetVectorType(64, 2); Address(t6, values, s64(binding.inst->Id()) * kValueStride);
        masm.VSE64(Vec{binding.vector_index + 8}, t6); ++value_stats.stores;
    }
    for (size_t i = 0; i < cached_vectors.size(); ++i) SpillVector(i);
}

void JitContext::RestoreVectorsAfterCall() {
    AcquireMemoryLease();
    ResetVectorType();
    for (const auto& binding : phi_bindings) if (binding.vector_index != UINT32_MAX) {
        SetVectorType(64, 2); Address(t6, values, s64(binding.inst->Id()) * kValueStride);
        masm.VLE64(Vec{binding.vector_index + 8}, t6); ++value_stats.loads;
    }
    for (size_t i = 0; i < cached_vectors.size(); ++i) {
        if (!cached_vectors[i]) continue;
        SetVectorType(64, 2);
        Address(t6, values, static_cast<s64>(cached_vectors[i]->Id()) * kValueStride);
        masm.VLE64(Vec{static_cast<u32>(i + 8)}, t6);
        ++value_stats.loads;
    }
}

}  // namespace swift::runtime::backend::riscv64
