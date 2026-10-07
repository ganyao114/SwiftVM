#include "translator.h"

#include <algorithm>
#include "runtime/backend/context.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

bool JitTranslator::EmitVectorLocal(ir::Inst* inst) {
    auto& as = context.GetMasm();
    switch (inst->GetOp()) {
        case O::DefineLocal: return true;
        case O::CheckMemoryAlignment: {
            const auto mask = inst->GetArg<ir::Imm>(1).Get();
            if (!mask) return true;
            const auto source = context.SourceRegister(inst->GetArg<ir::Value>(0), a0);
            if (mask <= 2047) as.ANDI(a0, source, mask);
            else { as.LI(a1, mask); as.AND(a0, source, a1); }
            Label aligned; as.BEQ(a0, x0, &aligned); Return(HaltReason::PageFatal); as.Bind(&aligned);
            return true;
        }
        case O::LoadLocal: case O::StoreLocal: break;
        default: return false;
    }
    const bool store = inst->GetOp() == O::StoreLocal;
    const auto local = inst->GetArg<ir::Local>(0);
    const auto type = store ? inst->GetArg<ir::Value>(1).Type() : inst->ReturnType();
    const u32 size = ir::GetValueSizeByte(local.type);
    const bool vector = ir::IsFloatValueType(type);
    const bool rvv = vector && context.Features().vector;
    const auto scalar_result = !store && !vector ? context.ResultRegister(inst) : x0;
    const auto pair = !store && vector && !rvv ? context.ResultPair(inst) : std::array<GPR, 2>{x0, x0};
    const auto vec = !store && rvv ? context.ResultVector(inst) : v3;
    context.Load(a0, state, state_offset_local_buffer);
    // Locals keep the established eight-byte id stride. A vector local spans
    // adjacent slots, just as a typed access to the supplied buffer does.
    Label absent, done;
    context.PublishFlags();
    as.BEQ(a0, x0, &absent);
    context.Address(a0, a0, u64(local.id) * 8);
    if (rvv) {
        auto value = vec;
        if (store) value = context.SourceVector(inst->GetArg<ir::Value>(1), v1);
        else { context.SetVectorType(64, 2); as.VMV(value, 0); }
        context.SetVectorType(8, size);
        if (store) as.VSE8(value, a0); else as.VLE8(value, a0);
    } else {
        Label unaligned, accessed;
        if (size > 1) { as.ANDI(a1, a0, std::min(size, 8u) - 1); as.BNE(a1, x0, &unaligned); }
        for (u32 part = 0; part < (size + 7) / 8; ++part) {
            const auto reg = vector ? pair[part] : scalar_result;
            const auto width = std::min(size - part * 8, 8u);
            if (store) context.Store(context.SourcePart(inst->GetArg<ir::Value>(1), part, a2), a0, part * 8, width);
            else context.Load(reg, a0, part * 8, width);
        }
        if (!store && vector && size < 16) as.MV(pair[1], x0);
        as.J(&accessed); as.Bind(&unaligned);
        for (u32 part = 0; part < (size + 7) / 8; ++part) {
            const auto reg = vector ? pair[part] : scalar_result;
            if (store) context.ReadPart(a2, inst->GetArg<ir::Value>(1), part); else as.MV(reg, x0);
            for (u32 byte = part * 8; byte < std::min(size, part * 8 + 8); ++byte) {
                if (store) { as.SB(a2, byte, a0); if (byte % 8 != 7) as.SRLI(a2, a2, 8); }
                else { as.LBU(a2, byte, a0); if (byte % 8) as.SLLI(a2, a2, byte % 8 * 8); as.OR(reg, reg, a2); }
            }
        }
        if (!store && vector && size < 16) as.MV(pair[1], x0);
        as.Bind(&accessed);
    }
    as.J(&done); as.Bind(&absent);
    if (!store) {
        if (rvv) { context.SetVectorType(64, 2); as.VMV(vec, 0); }
        else if (vector) { as.MV(pair[0], x0); as.MV(pair[1], x0); }
        else as.MV(scalar_result, x0);
    }
    as.Bind(&done);
    // Both the null and present branches agree on vtype at this join.
    context.ResetVectorType();
    if (!store) {
        if (rvv) context.WriteVector(inst, vec);
        else if (vector) context.WritePair(inst, pair);
        else context.Write(inst, scalar_result, true);
    }
    return true;
}

}  // namespace swift::runtime::backend::riscv64
