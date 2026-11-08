#include "emit.h"
#include "runtime/arch/x86/vector_crypto.h"

namespace swift::runtime::backend::x64::detail {
using O = OpCode;

void Writer::Crypto(OpContext& c) {
    namespace cr = runtime::x86::crypto;
    const auto op = c.operation.opcode;
    const bool unary = op == O::VecAesImc || op == O::VecAesKeygenAssist;
    cr::Operation kind{};
    switch (op) {
        case O::VecAesEnc:
        case O::VecAesEncFast:
            kind = cr::Operation::AesEncrypt;
            break;
        case O::VecAesEncLast:
        case O::VecAesEncLastFast:
            kind = cr::Operation::AesEncryptLast;
            break;
        case O::VecAesDec:
        case O::VecAesDecFast:
            kind = cr::Operation::AesDecrypt;
            break;
        case O::VecAesDecLast:
        case O::VecAesDecLastFast:
            kind = cr::Operation::AesDecryptLast;
            break;
        case O::VecAesImc:
            kind = cr::Operation::AesInverseMixColumns;
            break;
        case O::VecAesKeygenAssist:
            kind = cr::Operation::AesKeygenAssist;
            break;
        case O::VecPclMul:
            kind = cr::Operation::PolynomialMultiply;
            break;
        case O::VecSha256Msg1:
            kind = cr::Operation::Sha256Message1;
            break;
        case O::VecSha256Msg2:
            kind = cr::Operation::Sha256Message2;
            break;
        default:
            kind = cr::Operation::Sha256Rounds2;
            break;
    }
    bool native = (unsigned(kind) <= unsigned(cr::Operation::AesKeygenAssist) && options.aes) ||
                  (kind == cr::Operation::PolynomialMultiply && options.pclmul) ||
                  (unsigned(kind) >= unsigned(cr::Operation::Sha256Message1) && options.sha);
    if (native) {
        ReadVector(c.Arg(0), xmm0);
        if (!unary) ReadVector(c.Arg(1), xmm1);
        switch (kind) {
            case cr::Operation::AesEncrypt:
                aesenc(xmm0, xmm1);
                break;
            case cr::Operation::AesEncryptLast:
                aesenclast(xmm0, xmm1);
                break;
            case cr::Operation::AesDecrypt:
                aesdec(xmm0, xmm1);
                break;
            case cr::Operation::AesDecryptLast:
                aesdeclast(xmm0, xmm1);
                break;
            case cr::Operation::AesInverseMixColumns:
                aesimc(xmm0, xmm0);
                break;
            case cr::Operation::AesKeygenAssist:
                aeskeygenassist(xmm0, xmm0, c.Arg(1).Payload());
                break;
            case cr::Operation::PolynomialMultiply:
                pclmulqdq(xmm0, xmm1, c.Arg(2).Payload());
                break;
            case cr::Operation::Sha256Message1:
                sha256msg1(xmm0, xmm1);
                break;
            case cr::Operation::Sha256Message2:
                sha256msg2(xmm0, xmm1);
                break;
            case cr::Operation::Sha256Rounds2:
                movdqa(xmm14, xmm0);
                ReadVector(c.Arg(2), xmm0);
                sha256rnds2(xmm14, xmm1);
                movdqa(xmm0, xmm14);
                break;
        }
        return;
    }
    PrepareCall();
    ReadVector(c.Arg(0), xmm0);
    if (!unary)
        ReadVector(c.Arg(1), xmm1);
    else
        pxor(xmm1, xmm1);
    if (kind == cr::Operation::Sha256Rounds2)
        ReadVector(c.Arg(2), xmm14);
    else
        pxor(xmm14, xmm14);
    movq(rsi, xmm0);
    movhlps(xmm15, xmm0);
    movq(rdx, xmm15);
    movq(rcx, xmm1);
    movhlps(xmm15, xmm1);
    movq(r8, xmm15);
    movq(rax, xmm14);
    movhlps(xmm15, xmm14);
    movq(r10, xmm15);
    mov(edi, unsigned(kind));
    sub(rsp, 16);
    // The third 16-byte aggregate cannot fit in the one remaining GPR.
    // SysV passes both of its words on the stack, then assigns imm to R9.
    mov(qword[rsp], rax);
    mov(qword[rsp + 8], r10);
    mov(r9d,
        op == O::VecPclMul            ? c.Arg(2).Payload()
        : op == O::VecAesKeygenAssist ? c.Arg(1).Payload()
                                      : 0);
    HostAddress(r11, reinterpret_cast<std::uintptr_t>(&cr::Execute), HostSymbol::Crypto);
    call(r11);
    add(rsp, 16);
    FinishCall();
    movq(xmm0, rax);
    movq(xmm1, rdx);
    punpcklqdq(xmm0, xmm1);
}

#define C(name)                                                                                    \
    void Writer::Handle(Op<O::name>, OpContext& c) { Crypto(c); }
// clang-format off
C(VecAesEnc)
C(VecAesEncLast)
C(VecAesDec)
C(VecAesDecLast)
C(VecAesEncFast)
C(VecAesEncLastFast)
C(VecAesDecFast)
C(VecAesDecLastFast)
C(VecAesImc)
C(VecAesKeygenAssist)
C(VecPclMul)
C(VecSha256Msg1)
C(VecSha256Msg2)
C(VecSha256Rnds2)
// clang-format on
#undef C
}  // namespace swift::runtime::backend::x64::detail
