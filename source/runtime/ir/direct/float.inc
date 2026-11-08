// Execute neutral numerical operations with explicit per-call FP state. Results
// and exception flags share one leaf with the checked architectural consumer.
template <OpCode op>
Interpreter::Value Interpreter::HandleFloating(Type type, std::span<const Operand> a) {
    constexpr bool floating = (op >= OpCode::VecFAddScalar32 && op <= OpCode::VecFCvtPacked) ||
                              op == OpCode::VecFMulAdd || op == OpCode::VecFRoundInt;
    if constexpr (!floating) {
        return HandleCrypto<op>(type, a);
    } else {
        using F = runtime::x86::FloatingOperation;
        using G = runtime::x86::FloatingFlag;
        runtime::x86::FloatingCall call{};
        call.control = environment.mxcsr;
        const auto raw = [&](unsigned i, u64* out) {
            const auto bits = Raw(a[i]);
            out[0] = static_cast<u64>(bits);
            out[1] = static_cast<u64>(bits >> 64);
        };
        const auto imm = [&](unsigned i) { return static_cast<unsigned>(a[i].payload); };
        const auto select = [&](F operation) { call.operation = static_cast<unsigned>(operation); };
        raw(0, call.a);
        constexpr bool single = op == OpCode::VecFAddScalar32 || op == OpCode::VecFSubScalar32 ||
                                op == OpCode::VecFMulScalar32 || op == OpCode::VecFDivScalar32;
        constexpr bool scalar = single || op == OpCode::VecFAddScalar64 ||
                                op == OpCode::VecFSubScalar64 || op == OpCode::VecFMulScalar64 ||
                                op == OpCode::VecFDivScalar64;
        if constexpr (op >= OpCode::VecFAddScalar32 && op <= OpCode::VecFDiv) {
            raw(1, call.b);
            call.lane_bits = scalar ? (single ? 32 : 64) : imm(2);
            call.flags = scalar ? static_cast<unsigned>(G::Scalar) : 0;
            if constexpr (op == OpCode::VecFAdd || op == OpCode::VecFAddScalar32 ||
                          op == OpCode::VecFAddScalar64)
                select(F::Add);
            else if constexpr (op == OpCode::VecFSub || op == OpCode::VecFSubScalar32 ||
                               op == OpCode::VecFSubScalar64)
                select(F::Sub);
            else if constexpr (op == OpCode::VecFMul || op == OpCode::VecFMulScalar32 ||
                               op == OpCode::VecFMulScalar64)
                select(F::Mul);
            else
                select(F::Div);
        } else if constexpr (op == OpCode::VecFMinMax || op == OpCode::VecFUnary ||
                             op == OpCode::VecFRoundInt || op == OpCode::VecFCmpMask ||
                             op == OpCode::VecFCmp) {
            raw(1, call.b);
            call.lane_bits = imm(2) & 127;
            call.flags = (op == OpCode::VecFCmp || imm(4)) ? static_cast<unsigned>(G::Scalar) : 0;
            if constexpr (op == OpCode::VecFMinMax)
                select(imm(3) ? F::Max : F::Min);
            else if constexpr (op == OpCode::VecFUnary)
                select(imm(3) == 0 ? F::Sqrt : imm(3) == 1 ? F::Reciprocal : F::ReciprocalSqrt);
            else if constexpr (op == OpCode::VecFRoundInt) {
                select(F::RoundIntegral);
                call.immediate = imm(3) & 3;
                if (imm(3) & 8) call.flags |= static_cast<unsigned>(G::SuppressPrecision);
            } else {
                select(op == OpCode::VecFCmp ? F::CompareFlags : F::CompareMask);
                if constexpr (op == OpCode::VecFCmp) {
                    if (imm(2) & 128) call.flags |= static_cast<unsigned>(G::Signaling);
                } else {
                    call.immediate = imm(3) & 15;
                    if (imm(3) & 16) call.flags |= static_cast<unsigned>(G::Signaling);
                }
            }
        } else if constexpr (op == OpCode::VecFMulAdd) {
            select(F::FusedMultiplyAdd);
            raw(1, call.b);
            raw(2, call.c);
            call.lane_bits = imm(3);
            call.immediate = imm(4);
            call.flags = imm(4) & 4 ? static_cast<unsigned>(G::Scalar) : 0;
        } else if constexpr (op == OpCode::VecFCvtPacked) {
            select(F::ConvertPacked);
            call.immediate = imm(1);
        } else {
            call.source_bits = imm(1);
            if constexpr (op == OpCode::VecFCvtIntToFloat) {
                select(F::FromSignedInteger);
                call.lane_bits = imm(2);
            } else if constexpr (op == OpCode::VecFCvtFloatToInt) {
                select(F::ToSignedInteger);
                call.lane_bits = imm(2);
                call.flags = imm(3) ? static_cast<unsigned>(G::Truncate) : 0;
            } else {
                select(F::ConvertPrecision);
                call.lane_bits = call.source_bits == 32 ? 64 : 32;
            }
        }
        runtime::x86::ExecuteFloating(&call);
        observed_effects |= call.exceptions != 0;
        floating_exceptions |= call.exceptions;
        environment.mxcsr |= call.exceptions;
        return Number((u128{call.result[1]} << 64) | call.result[0], type);
    }
}
