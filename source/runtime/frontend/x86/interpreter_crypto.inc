template <OpCode op>
DirectSink::Value DirectSink::Crypto(Type type, std::span<const Operand> operands) {
    namespace crypto = ::swift::runtime::x86::crypto;
    const auto vector = [&](unsigned index) {
        const auto value = Raw(operands[index]);
        return crypto::Block128{static_cast<u64>(value), static_cast<u64>(value >> 64)};
    };
    crypto::Operation operation;
    crypto::Block128 a{}, b{}, c{};
    unsigned control{};
    if constexpr (op == OpCode::VecAesEnc || op == OpCode::VecAesEncFast)
        operation = crypto::Operation::AesEncrypt;
    else if constexpr (op == OpCode::VecAesEncLast || op == OpCode::VecAesEncLastFast)
        operation = crypto::Operation::AesEncryptLast;
    else if constexpr (op == OpCode::VecAesDec || op == OpCode::VecAesDecFast)
        operation = crypto::Operation::AesDecrypt;
    else if constexpr (op == OpCode::VecAesDecLast || op == OpCode::VecAesDecLastFast)
        operation = crypto::Operation::AesDecryptLast;
    else if constexpr (op == OpCode::VecAesImc)
        operation = crypto::Operation::AesInverseMixColumns;
    else if constexpr (op == OpCode::VecAesKeygenAssist) {
        operation = crypto::Operation::AesKeygenAssist;
        control = static_cast<unsigned>(operands[1].payload);
    } else if constexpr (op == OpCode::VecPclMul) {
        operation = crypto::Operation::PolynomialMultiply;
        control = static_cast<unsigned>(operands[2].payload);
    } else if constexpr (op == OpCode::VecSha256Msg1)
        operation = crypto::Operation::Sha256Message1;
    else if constexpr (op == OpCode::VecSha256Msg2)
        operation = crypto::Operation::Sha256Message2;
    else if constexpr (op == OpCode::VecSha256Rnds2)
        operation = crypto::Operation::Sha256Rounds2;
    else
        return String<op>(type, operands);
    a = vector(0);
    if constexpr (op != OpCode::VecAesImc && op != OpCode::VecAesKeygenAssist) b = vector(1);
    if constexpr (op == OpCode::VecSha256Rnds2) c = vector(2);
    const auto result = crypto::Execute(operation, a, b, c, static_cast<std::uint8_t>(control));
    return Number(u128{result.low} | (u128{result.high} << 64), type);
}
