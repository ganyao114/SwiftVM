template<OpCode op>
DirectSink::Value DirectSink::String(Type type, std::span<const Operand> operands) {
    namespace sse42 = ::swift::runtime::x86::sse42;
    if constexpr (op == OpCode::PublishSse42StrFlags) {
        const auto result = static_cast<u64>(Read(operands[0]));
        const auto flags = ((result & (u64{1} << sse42::SignBit)) << 7) |
                ((result & (u64{1} << sse42::ZeroBit)) << 5) |
                ((result & (u64{1} << sse42::CarryBit)) << 3) |
                ((result & (u64{1} << sse42::OverflowBit)) << 1) | 1; // PF=0, AF=0.
        MergeFlags(flags, operands[1].payload); return {};
    } else if constexpr (op == OpCode::Sse42Str || op == OpCode::Sse42StrExplicit) {
        const auto vector = [&](unsigned index) {
            const auto bits = Raw(operands[index]);
            return sse42::Block128{static_cast<u64>(bits), static_cast<u64>(bits >> 64)};
        };
        if constexpr (op == OpCode::Sse42StrExplicit)
            return Number(sse42::CompareExplicit(vector(0), vector(1), vector(2),
                    static_cast<std::uint16_t>(operands[3].payload)), type);
        else return Number(sse42::CompareImplicit(vector(0), vector(1),
                    static_cast<std::uint8_t>(operands[2].payload)), type);
    } else throw detail::Rejected{BuildError::Unsupported};
}
