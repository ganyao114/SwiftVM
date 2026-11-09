#include "emit.h"
#include "runtime/arch/x86/vector_crypto.h"
#include "runtime/exit.h"
#include "vector_helper.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64::detail {
namespace {
namespace crypto = ::swift::runtime::x86::crypto;

crypto::Block128 ExecuteCrypto(u64 metadata, const crypto::Block128* arguments) noexcept {
    return crypto::Execute(static_cast<crypto::Operation>(metadata & 255),
                           arguments[0],
                           arguments[1],
                           arguments[2],
                           static_cast<std::uint8_t>(metadata >> 8));
}
}  // namespace

std::uintptr_t CryptoAddress() noexcept { return reinterpret_cast<std::uintptr_t>(&ExecuteCrypto); }

void Writer::HandleCryptoFamily(OpContext& ctx) {
    if (!ctx.vector) Reject(CompileError::UnsupportedType);
    auto& cursor = ctx.cursor;
    std::array<CellView, 3> inputs{};
    inputs[0] = cursor.Take();
    crypto::Operation operation;
    unsigned count = 2;
    u64 immediate{};
    bool native{};
    switch (ctx.operation.opcode) {
        case OpCode::VecAesEnc:
        case OpCode::VecAesEncFast:
            operation = crypto::Operation::AesEncrypt;
            native = options.aes;
            break;
        case OpCode::VecAesEncLast:
        case OpCode::VecAesEncLastFast:
            operation = crypto::Operation::AesEncryptLast;
            native = options.aes;
            break;
        case OpCode::VecAesDec:
        case OpCode::VecAesDecFast:
            operation = crypto::Operation::AesDecrypt;
            native = options.aes;
            break;
        case OpCode::VecAesDecLast:
        case OpCode::VecAesDecLastFast:
            operation = crypto::Operation::AesDecryptLast;
            native = options.aes;
            break;
        case OpCode::VecAesImc:
            operation = crypto::Operation::AesInverseMixColumns;
            count = 1;
            native = options.aes;
            break;
        case OpCode::VecAesKeygenAssist:
            operation = crypto::Operation::AesKeygenAssist;
            count = 1;
            immediate = IntegerImmediate(cursor.Take());
            native = options.aes;
            break;
        case OpCode::VecPclMul:
            operation = crypto::Operation::PolynomialMultiply;
            native = options.pmull;
            break;
        case OpCode::VecSha256Msg1:
            operation = crypto::Operation::Sha256Message1;
            native = options.sha256;
            break;
        case OpCode::VecSha256Msg2:
            operation = crypto::Operation::Sha256Message2;
            native = options.sha256;
            break;
        case OpCode::VecSha256Rnds2:
            operation = crypto::Operation::Sha256Rounds2;
            count = 3;
            native = options.sha256;
            break;
        default:
            Reject(CompileError::UnsupportedOpcode);
    }
    if (count > 1) inputs[1] = cursor.Take();
    if (count > 2) inputs[2] = cursor.Take();
    if (ctx.operation.opcode == OpCode::VecPclMul) immediate = IntegerImmediate(cursor.Take());
    const bool explicit_zero = ctx.operation.opcode == OpCode::VecAesEncFast ||
                               ctx.operation.opcode == OpCode::VecAesEncLastFast ||
                               ctx.operation.opcode == OpCode::VecAesDecFast ||
                               ctx.operation.opcode == OpCode::VecAesDecLastFast;
    if (explicit_zero) {
        // These forms have the same round semantics and an explicit shared
        // zero-key dependency. Consume its ordinary V128 schema as well.
        const auto zero = cursor.Take();
        VectorValue(zero, VectorTemporary());
    }
    if (immediate > 255) Reject(CompileError::UnsupportedOperand);
    const auto first_temp = VectorTemporary(), second_temp = VectorTemporary(true);
    // Snapshot all actual inputs before touching a result home. This also
    // handles two/three spills and a result tied to any of the source pins.
    VectorMove(0, VectorValue(inputs[0], 0));
    if (count > 1)
        VectorMove(1, VectorValue(inputs[1], 1));
    else
        __ eor(Lanes(1, 8), Lanes(1, 8), Lanes(1, 8));
    if (count > 2)
        VectorMove(first_temp, VectorValue(inputs[2], first_temp));
    else
        __ eor(Lanes(first_temp, 8), Lanes(first_temp, 8), Lanes(first_temp, 8));

    if (!native) {
        HelperFPEnvironment(false);
        // RawValue/VectorValue above use the original spill SP. Only after
        // those reads does this call own its 48-byte argument snapshot.
        __ sub(v::sp, v::sp, Operand(48));
        VectorMemory(false, 0, 31, 0);
        VectorMemory(false, 1, 31, 1);
        VectorMemory(false, first_temp, 31, 2);
        __ add(v::x17, v::sp, Operand(0));
        Immediate(16, static_cast<u64>(operation) | (immediate << 8));
        HostAddress(0, reinterpret_cast<std::uintptr_t>(&ExecuteCrypto), HostSymbol::ArmCrypto);
        HostAddress(30, VectorHelperBridge(), HostSymbol::ArmVectorBridge);
        __ blr(v::x30);
        HelperFPEnvironment(true);
        __ add(v::sp, v::sp, Operand(48));
        VectorMove(ctx.destination, 0);
        NativePhase(SVM_NATIVE_PHASE_GUEST);
        return;
    }

    switch (operation) {
        case crypto::Operation::AesEncrypt:
        case crypto::Operation::AesEncryptLast:
        case crypto::Operation::AesDecrypt:
        case crypto::Operation::AesDecryptLast: {
            const bool inverse = operation == crypto::Operation::AesDecrypt ||
                                 operation == crypto::Operation::AesDecryptLast;
            const bool last = operation == crypto::Operation::AesEncryptLast ||
                              operation == crypto::Operation::AesDecryptLast;
            VectorMove(first_temp, 0);
            __ eor(Lanes(second_temp, 8), Lanes(second_temp, 8), Lanes(second_temp, 8));
            Crypto(masm.exact,
                   inverse ? CryptoUnary::AesDecrypt : CryptoUnary::AesEncrypt,
                   Q(first_temp),
                   Q(second_temp));
            if (!last)
                Crypto(masm.exact,
                       inverse ? CryptoUnary::AesInverseMixColumns : CryptoUnary::AesMixColumns,
                       Q(first_temp),
                       Q(first_temp));
            __ eor(Lanes(ctx.destination, 8), Lanes(first_temp, 8), Lanes(1, 8));
            break;
        }
        case crypto::Operation::AesInverseMixColumns:
            Crypto(masm.exact, CryptoUnary::AesInverseMixColumns, Q(ctx.destination), Q(0));
            break;
        case crypto::Operation::AesKeygenAssist: {
            VectorMove(first_temp, 0);
            Crypto(masm.exact,
                   CryptoUnary::AesEncrypt,
                   Q(first_temp),
                   Q(1));  // v1 is the zero round key.
            Immediate(16, UINT64_C(0x040b0e010b0e0104));
            __ fmov(D(1), v::x16);
            Immediate(16, UINT64_C(0x0c0306090306090c));
            __ ins(Lanes(1, 64), 1, v::x16);
            __ tbl(Lanes(ctx.destination, 8), Lanes(first_temp, 8), Lanes(1, 8));
            Immediate(16, immediate << 32);
            __ dup(Lanes(first_temp, 64), v::x16);
            __ eor(Lanes(ctx.destination, 8), Lanes(ctx.destination, 8), Lanes(first_temp, 8));
            break;
        }
        case crypto::Operation::PolynomialMultiply:
            if (immediate & 1) __ dup(Lanes(0, 64), Lanes(0, 64), 1);
            if (immediate & 16) __ dup(Lanes(1, 64), Lanes(1, 64), 1);
            Crypto(masm.exact, CryptoTernary::PolynomialMultiply, Q(ctx.destination), Q(0), Q(1));
            break;
        case crypto::Operation::Sha256Message1:
            VectorMove(ctx.destination, 0);
            Crypto(masm.exact, CryptoUnary::Sha256Schedule0, Q(ctx.destination), Q(1));
            break;
        case crypto::Operation::Sha256Message2:
            __ ext(Lanes(first_temp, 8), Lanes(0, 8), Lanes(0, 8), 12);
            __ dup(Lanes(second_temp, 32), Lanes(0, 32), 3);
            __ zip2(Lanes(second_temp, 64), Lanes(second_temp, 64), Lanes(1, 64));
            __ eor(Lanes(0, 8), Lanes(0, 8), Lanes(0, 8));
            Crypto(masm.exact, CryptoTernary::Sha256Schedule1, Q(0), Q(first_temp), Q(second_temp));
            VectorMove(ctx.destination, 0);
            break;
        case crypto::Operation::Sha256Rounds2:
            __ zip2(Lanes(second_temp, 64), Lanes(1, 64), Lanes(0, 64));
            __ rev64(Lanes(second_temp, 32), Lanes(second_temp, 32));
            __ zip1(Lanes(1, 64), Lanes(1, 64), Lanes(0, 64));
            __ rev64(Lanes(1, 32), Lanes(1, 32));
            __ dup(Lanes(first_temp, 64), Lanes(first_temp, 64), 0);
            VectorMove(0, 1);
            Crypto(masm.exact, CryptoTernary::Sha256Hash2, Q(0), Q(second_temp), Q(first_temp));
            Crypto(masm.exact, CryptoTernary::Sha256Hash, Q(second_temp), Q(1), Q(first_temp));
            __ zip2(Lanes(ctx.destination, 64), Lanes(0, 64), Lanes(second_temp, 64));
            __ rev64(Lanes(ctx.destination, 32), Lanes(ctx.destination, 32));
            break;
    }
}
}  // namespace swift::runtime::backend::arm64::detail

#undef __
