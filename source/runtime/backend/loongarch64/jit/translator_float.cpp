#include "translator.h"

#include <stdexcept>

namespace swift::runtime::backend::loongarch64 {
namespace {

const FPR f0{0}, f1{1}, f2{2};

// LoongArch Vol. 1, floating-point programming model: FCSR0.RM[9:8]
// value 0 is IEEE roundTiesToEven. Enables[4:0] value 0 disables traps;
// Flags[20:16]/Cause[28:24] are the V/Z/O/U/I status fields. There is no
// flush-to-zero control: the base FP arithmetic implements gradual underflow.
constexpr u64 kExceptionalStatus = 0x1e1e0000;

void RequireNormal(Assembler& as, GPR raw, u32 bits, Label& slow) {
    // Extract the exponent without the sign. Zero/denormal and Inf/NaN
    // operands and results use the typed semantic path, preserving NaN payload
    // order and signed-zero behavior across different host FPUs.
    as.bstrpick_d(t2, raw, bits == 32 ? 30 : 62, bits == 32 ? 23 : 52);
    as.BEQ(t2, zero, &slow);
    as.LI(t3, bits == 32 ? 255 : 2047);
    as.BEQ(t2, t3, &slow);
}

void FloatOperation(Assembler& as, ir::OpCode operation, u32 bits) {
    using O = ir::OpCode;
    switch (operation) {
        case O::VecFAdd: case O::VecFAddScalar32: case O::VecFAddScalar64:
            if (bits == 32) as.fadd_s(f2, f0, f1); else as.fadd_d(f2, f0, f1);
            break;
        case O::VecFSub: case O::VecFSubScalar32: case O::VecFSubScalar64:
            if (bits == 32) as.fsub_s(f2, f0, f1); else as.fsub_d(f2, f0, f1);
            break;
        case O::VecFMul: case O::VecFMulScalar32: case O::VecFMulScalar64:
            if (bits == 32) as.fmul_s(f2, f0, f1); else as.fmul_d(f2, f0, f1);
            break;
        case O::VecFDiv: case O::VecFDivScalar32: case O::VecFDivScalar64:
            if (bits == 32) as.fdiv_s(f2, f0, f1); else as.fdiv_d(f2, f0, f1);
            break;
        default: throw std::logic_error("unsupported LoongArch basic FP operation");
    }
}

}  // namespace

void JitTranslator::EmitFloat(ir::Inst* inst, bool scalar, u32 bits) {
    if (bits != 32 && bits != 64) throw std::invalid_argument("LoongArch basic FP requires f32/f64 lanes");
    const auto lhs = inst->GetArg<ir::Value>(0), rhs = inst->GetArg<ir::Value>(1);
    if (ir::GetValueSizeByte(lhs.Type()) > 16 || ir::GetValueSizeByte(rhs.Type()) > 16 ||
        ir::GetValueSizeByte(inst->ReturnType()) > 16)
        throw std::invalid_argument("LoongArch basic FP exceeds the legalized 128-bit container");
    auto& as = context.GetMasm();
    context.FlushValues(); context.DiscardValues();
    context.PublishInput(lhs.Def()); context.PublishInput(rhs.Def());
    const auto lhs_offset = context.HomeOffset(lhs.Def());
    const auto rhs_offset = context.HomeOffset(rhs.Def());
    const auto result_offset = context.HomeOffset(inst);
    Label slow, finished;

    // The caller's mode/exception state is restored before every ABI call,
    // including the cold helper path. Only caller-saved f0-f2 are temporary.
    as.movfcsr2gr(t6, fcsr0);
    as.movgr2fcsr(fcsr0, zero);
    if (scalar) {
        // Scalar SSE-like operations replace only the low lane. Read/write
        // canonical homes directly so narrow IR views retain the raw payload.
        context.Load(t4, values, lhs_offset);
        context.Load(t5, values, lhs_offset + 8);
        context.Store(t4, values, result_offset);
        context.Store(t5, values, result_offset + 8);
    }
    const u32 lane_bytes = bits / 8;
    for (u32 lane = 0; lane < (scalar ? 1u : 128 / bits); ++lane) {
        context.Load(t0, values, lhs_offset + lane * lane_bytes, lane_bytes);
        context.Load(t1, values, rhs_offset + lane * lane_bytes, lane_bytes);
        RequireNormal(as, t0, bits, slow);
        RequireNormal(as, t1, bits, slow);
        if (bits == 32) { as.movgr2fr_w(f0, t0); as.movgr2fr_w(f1, t1); }
        else { as.movgr2fr_d(f0, t0); as.movgr2fr_d(f1, t1); }
        FloatOperation(as, inst->GetOp(), bits);
        if (bits == 32) as.movfr2gr_s(t0, f2); else as.movfr2gr_d(t0, f2);
        RequireNormal(as, t0, bits, slow);
        // This provisional SSA home is private. A later slow lane overwrites
        // the complete result through the typed helper; no guest store occurs.
        context.Store(t0, values, result_offset + lane * lane_bytes, lane_bytes);
    }
    // Inexact is a normal IEEE rounded result. Severe host exceptions use the
    // semantic path even if rounding produced a normal boundary value.
    as.movfcsr2gr(t2, fcsr0); as.LI(t3, kExceptionalStatus); as.and_(t2, t2, t3);
    as.BNE(t2, zero, &slow);
    as.movgr2fcsr(fcsr0, t6); as.J(&finished);

    as.Bind(&slow);
    as.movgr2fcsr(fcsr0, t6);
    EmitHelper(inst);
    // helpers counts the emitted cold call site. Preserve the native emitter
    // count as well; guarded_native explicitly records this overlap instead
    // of claiming that dynamically special inputs never need a helper.
    ++stats.direct;
    ++stats.guarded_native;
    as.Bind(&finished);
    context.ReloadDefinition(inst);
    context.DiscardValues();
}

#define SCALAR_FLOAT(name, bits) \
    void JitTranslator::Emit##name(ir::Inst* inst) { EmitFloat(inst, true, bits); }
SCALAR_FLOAT(VecFAddScalar32, 32)
SCALAR_FLOAT(VecFSubScalar32, 32)
SCALAR_FLOAT(VecFMulScalar32, 32)
SCALAR_FLOAT(VecFDivScalar32, 32)
SCALAR_FLOAT(VecFAddScalar64, 64)
SCALAR_FLOAT(VecFSubScalar64, 64)
SCALAR_FLOAT(VecFMulScalar64, 64)
SCALAR_FLOAT(VecFDivScalar64, 64)
#undef SCALAR_FLOAT
#define PACKED_FLOAT(name) \
    void JitTranslator::Emit##name(ir::Inst* inst) { \
        const auto bits = inst->GetArg<ir::Imm>(2).Get(); \
        if (bits != 32 && bits != 64) throw std::invalid_argument("invalid LoongArch packed FP lane width"); \
        EmitFloat(inst, false, u32(bits)); \
    }
PACKED_FLOAT(VecFAdd)
PACKED_FLOAT(VecFSub)
PACKED_FLOAT(VecFMul)
PACKED_FLOAT(VecFDiv)
#undef PACKED_FLOAT

}  // namespace swift::runtime::backend::loongarch64
