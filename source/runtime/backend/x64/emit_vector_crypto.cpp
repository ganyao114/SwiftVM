#include "translator.h"
#include <stdexcept>
#include "helpers.h"

namespace swift::runtime::backend::x64 {
using O = ir::OpCode;
using namespace Xbyak;

void Translator::EmitSha256Message(ir::Inst* inst, ir::OpCode op) {
    if (features.sha) {
        ReadVectorOperands(inst);
        if (op == O::VecSha256Msg1) sha256msg1(xmm0, xmm1); else sha256msg2(xmm0, xmm1);
        WriteVector(inst, xmm0); return;
    }
    lea(rdi, ptr[r13 + Home(inst)]); lea(rsi, ptr[r13 + Home(Source(inst, 0).Def())]);
    lea(rdx, ptr[r13 + Home(Source(inst, 1).Def())]); mov(ecx, u32(op == O::VecSha256Msg2));
    Call(reinterpret_cast<u64>(&Sha256Message)); return;
}

void Translator::EmitVecSha256Msg1(ir::Inst* inst) { EmitSha256Message(inst, O::VecSha256Msg1); }

void Translator::EmitVecSha256Msg2(ir::Inst* inst) { EmitSha256Message(inst, O::VecSha256Msg2); }

void Translator::EmitVecSha256Rnds2(ir::Inst* inst) {
    if (features.sha) {
        // SHA256RNDS2 implicitly consumes XMM0[63:0]. Load state
        // operands first so the explicit key cannot be overwritten.
        ReadVector(xmm1, Source(inst, 0)); ReadVector(xmm3, Source(inst, 1)); ReadVector(xmm0, Source(inst, 2));
        sha256rnds2(xmm1, xmm3); movdqa(xmm0, xmm1); WriteVector(inst, xmm0); return;
    }
    lea(rdi, ptr[r13 + Home(inst)]); lea(rsi, ptr[r13 + Home(Source(inst, 0).Def())]);
    lea(rdx, ptr[r13 + Home(Source(inst, 1).Def())]); lea(rcx, ptr[r13 + Home(Source(inst, 2).Def())]);
    Call(reinterpret_cast<u64>(&Sha256Rounds)); return;
}

void Translator::EmitAesRound(ir::Inst* inst, ir::OpCode op) {
    const bool fast = op == O::VecAesEncFast || op == O::VecAesEncLastFast ||
          op == O::VecAesDecFast || op == O::VecAesDecLastFast;
    if (fast) {
        const auto* zero = Source(inst, 2).Def();
        const bool proven_zero = zero && (zero->GetOp() == O::VecSharedZero || zero->GetOp() == O::Zero ||
            (zero->GetOp() == O::VecLoadConst && !zero->GetArg<ir::Imm>(0).Get() && !zero->GetArg<ir::Imm>(1).Get()));
        if (!proven_zero) throw std::runtime_error("x64 AES Fast requires a proven zero third operand");
    }
    const bool inverse = op == O::VecAesDec || op == O::VecAesDecLast || op == O::VecAesDecFast || op == O::VecAesDecLastFast;
    const bool last = op == O::VecAesEncLast || op == O::VecAesDecLast || op == O::VecAesEncLastFast || op == O::VecAesDecLastFast;
    if (features.aes) {
        ReadVectorOperands(inst);
        if (inverse) { if (last) aesdeclast(xmm0, xmm1); else aesdec(xmm0, xmm1); }
        else { if (last) aesenclast(xmm0, xmm1); else aesenc(xmm0, xmm1); }
        WriteVector(inst, xmm0); return;
    }
    lea(rdi, ptr[r13 + Home(inst)]); lea(rsi, ptr[r13 + Home(Source(inst, 0).Def())]);
    lea(rdx, ptr[r13 + Home(Source(inst, 1).Def())]); mov(ecx, u32(inverse) * 2 + u32(last));
    Call(reinterpret_cast<u64>(&AesRound)); return;
}

void Translator::EmitVecAesEnc(ir::Inst* inst) { EmitAesRound(inst, O::VecAesEnc); }

void Translator::EmitVecAesEncLast(ir::Inst* inst) { EmitAesRound(inst, O::VecAesEncLast); }

void Translator::EmitVecAesDec(ir::Inst* inst) { EmitAesRound(inst, O::VecAesDec); }

void Translator::EmitVecAesDecLast(ir::Inst* inst) { EmitAesRound(inst, O::VecAesDecLast); }

void Translator::EmitVecAesEncFast(ir::Inst* inst) { EmitAesRound(inst, O::VecAesEncFast); }

void Translator::EmitVecAesEncLastFast(ir::Inst* inst) { EmitAesRound(inst, O::VecAesEncLastFast); }

void Translator::EmitVecAesDecFast(ir::Inst* inst) { EmitAesRound(inst, O::VecAesDecFast); }

void Translator::EmitVecAesDecLastFast(ir::Inst* inst) { EmitAesRound(inst, O::VecAesDecLastFast); }

void Translator::EmitVecAesKeygenAssist(ir::Inst* inst) {
    if (features.aes) { ReadVector(xmm1, Source(inst, 0)); aeskeygenassist(xmm0, xmm1, VectorImmediate(inst, 1) & 255); WriteVector(inst, xmm0); return; }
    lea(rdi, ptr[r13 + Home(inst)]); lea(rsi, ptr[r13 + Home(Source(inst, 0).Def())]); mov(edx, VectorImmediate(inst, 1) & 255);
    Call(reinterpret_cast<u64>(&AesKeygen)); return;
}

void Translator::EmitVecPclMul(ir::Inst* inst) {
    if (features.pclmul) { ReadVectorOperands(inst); pclmulqdq(xmm0, xmm1, VectorImmediate(inst, 2) & 0x11); WriteVector(inst, xmm0); return; }
    lea(rdi, ptr[r13 + Home(inst)]);
    mov(rsi, qword[r13 + Home(Source(inst, 0).Def()) + ((VectorImmediate(inst, 2) & 1) ? 8 : 0)]);
    mov(rdx, qword[r13 + Home(Source(inst, 1).Def()) + ((VectorImmediate(inst, 2) & 16) ? 8 : 0)]);
    Call(reinterpret_cast<u64>(&PolynomialMultiply)); return;
}

}  // namespace swift::runtime::backend::x64
