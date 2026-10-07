#include "jit_context.h"

#include "runtime/ir/instr.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;

void JitContext::EnterFloatMode() {
    if (vector_float) masm.FSRMI(0);
}

void JitContext::LeaveFloatMode() {
    if (vector_float) { Load(t5, frame, 200); masm.FSRM(t5); }
}

std::array<GPR, 2> JitContext::ResultPair(ir::Inst*) {
    if (!cache_scalars) return {t0, t1};
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
                    ASSERT(!cached_values[i]);
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
    for (size_t scanned = 0; scanned < cached_vectors.size(); ++scanned) {
        const auto candidate = (next_vector + scanned) % cached_vectors.size();
        if (!cached_vectors[candidate]) { index = candidate; break; }
    }
    next_vector = (index + 1) % cached_vectors.size();
    if (cached_vectors[index]) { SpillVector(index); ++value_stats.spills; }
    cached_vectors[index] = nullptr;
    return Vec{static_cast<u32>(index + 8)};
}

Vec JitContext::SourceVector(ir::Value value, Vec scratch) {
    for (size_t i = 0; i < cached_vectors.size(); ++i) {
        if (cached_vectors[i] == value.Def()) { ++value_stats.cache_hits; return Vec{static_cast<u32>(i + 8)}; }
    }
    SetVectorType(64, 2);
    // A GPR pair can contain scalar bitcasts or baseline results. Build [lo,
    // hi] without writing a temporary home; slide's source differs from vd.
    bool gpr_cached{};
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

void JitContext::SaveVectorsForCall() {
    for (size_t i = 0; i < cached_vectors.size(); ++i) SpillVector(i);
}

void JitContext::RestoreVectorsAfterCall() {
    ResetVectorType();
    for (size_t i = 0; i < cached_vectors.size(); ++i) {
        if (!cached_vectors[i]) continue;
        SetVectorType(64, 2);
        Address(t6, values, static_cast<s64>(cached_vectors[i]->Id()) * kValueStride);
        masm.VLE64(Vec{static_cast<u32>(i + 8)}, t6);
        ++value_stats.loads;
    }
}

}  // namespace swift::runtime::backend::riscv64
