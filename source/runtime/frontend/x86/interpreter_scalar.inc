// Definitions for the single DirectSink; no separate interpreter or state owner.
static void StoreX87Metadata(u64 address, u64 pc, u64 dp, u64 metadata) {
    auto* base = reinterpret_cast<std::uint8_t*>(address);
    const auto opcode = arch::X87MetadataOpcode(metadata);
    std::memcpy(base + arch::X87MetadataOffset(metadata, 0), &pc, 8);
    if (arch::X87MetadataHasDP(metadata))
        std::memcpy(base + arch::X87MetadataOffset(metadata, 1), &dp, 8);
    std::memcpy(base + arch::X87MetadataOffset(metadata, 2), &opcode, 2);
}

template <OpCode op>
DirectSink::Value DirectSink::Compute(Type requested, std::span<const Operand> a) {
    const auto type = requested != Type::Void ? requested : a.empty() ? Type::Void : View(a[0]);
    const auto read = [&](std::size_t i) { return static_cast<u64>(Read(a[i])); };
    const auto raw = [&](std::size_t i) { return static_cast<u64>(Raw(a[i])); };
    if constexpr (op == OpCode::ReadSystem || op == OpCode::ReadSystemPair) {
        using Query = runtime::x86::CpuQueryResult (*)(u64, u64) noexcept;
        const auto pair = reinterpret_cast<Query>(a[1].payload)(a[0].payload, 0);
        return Number(op == OpCode::ReadSystemPair ? u128{pair.lower} | (u128{pair.upper} << 64)
                                                   : u128{pair.lower},
                      type);
    } else if constexpr (op == OpCode::LoadImm)
        return Number(a[0].payload, View(a[0]));
    else if constexpr (op == OpCode::LoadUniform) {
        u128 value{};
        const auto size = TypeBits(type) / 8;
        std::memcpy(&value, reinterpret_cast<const std::uint8_t*>(&state) + a[0].payload, size);
        return Number(value, type);
    } else if constexpr (op == OpCode::StoreUniform) {
        const auto value = Read(a[1]);
        std::memcpy(reinterpret_cast<std::uint8_t*>(&state) + a[0].payload,
                    &value,
                    TypeBits(a[0].type) / 8);
        return {};
    } else if constexpr (op == OpCode::GetUniformAddress)
        return Number(reinterpret_cast<std::uintptr_t>(&state) + a[0].payload, Type::U64);
    else if constexpr (op == OpCode::GetOperand) {
        std::size_t cursor{};
        return Number(Expression(a, cursor), type);
    } else if constexpr (op == OpCode::LoadMemory || op == OpCode::LoadMemoryTSO) {
        std::size_t cursor{};
        return MemoryRead(
                static_cast<u64>(Expression(a, cursor)), type, op == OpCode::LoadMemoryTSO);
    } else if constexpr (op == OpCode::StoreMemory || op == OpCode::StoreMemoryTSO) {
        std::size_t cursor{};
        const auto address = static_cast<u64>(Expression(a, cursor));
        MemoryWrite(address, a[cursor], op == OpCode::StoreMemoryTSO);
        return {};
    } else if constexpr (op == OpCode::CheckMemoryAlignment) {
        if (read(0) & read(1)) throw DirectExit{StepKind::Fault, read(0)};
        return {};
    } else if constexpr (op == OpCode::Nop || op == OpCode::UniformBarrier ||
                         op == OpCode::XchgBarrier || op == OpCode::FaultLocation)
        return {};
    else if constexpr (op == OpCode::MemoryStoreFence || op == OpCode::MemoryBarrierTSO ||
                       op == OpCode::MemoryLoadFence) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return {};
    } else if constexpr (op == OpCode::CpuYield) {
        return {};
    } else if constexpr (op == OpCode::Add || op == OpCode::Sub || op == OpCode::Adc ||
                         op == OpCode::Sbb || op == OpCode::Neg) {
        std::size_t cursor = op == OpCode::Neg ? 0 : 1;
        const auto right = static_cast<u64>(Expression(a, cursor));
        return Arithmetic(op,
                          type,
                          op == OpCode::Neg ? 0 : read(0),
                          right,
                          arch::flags::GetCF(state.packed_flags));
    } else if constexpr (op == OpCode::And || op == OpCode::Or || op == OpCode::Xor ||
                         op == OpCode::AndNot || op == OpCode::Not) {
        std::size_t cursor = 1;
        const auto left = read(0),
                   right = a.size() > 1 ? static_cast<u64>(Expression(a, cursor)) : 0;
        const auto result = op == OpCode::And      ? left & right
                            : op == OpCode::Or     ? left | right
                            : op == OpCode::Xor    ? left ^ right
                            : op == OpCode::AndNot ? left & ~right
                                                   : ~left;
        auto out = Number(result, type);
        const auto value = static_cast<u64>(out.number.bits);
        out.number.flags = (value & 255) | (u64{value == 0} << 30) |
                           (u64{bool(value & (u64{1} << (TypeBits(type) - 1)))} << 31);
        return out;
    } else if constexpr (op == OpCode::Mul)
        return Number(u128{read(0)} * read(1), type);
    else if constexpr (op == OpCode::MulHigh) {
        const u128 product = read(2) ? static_cast<u128>(s128{static_cast<std::int64_t>(raw(0))} *
                                                         static_cast<std::int64_t>(raw(1)))
                                     : u128{raw(0)} * raw(1);
        return Number(product >> 64, type);
    } else if constexpr (op == OpCode::ZeroExtend32 || op == OpCode::ZeroExtend32To64 ||
                         op == OpCode::ZeroExtend64)
        return Number(read(0), op == OpCode::ZeroExtend32 ? Type::U32 : Type::U64);
    else if constexpr (op == OpCode::SignExtend)
        return Number(static_cast<u64>(Signed(read(0), TypeBits(View(a[0])))), type);
    else if constexpr (op == OpCode::LslImm || op == OpCode::LslValue || op == OpCode::LsrImm ||
                       op == OpCode::LsrValue || op == OpCode::AsrImm || op == OpCode::AsrValue) {
        const auto width = TypeBits(type) <= 32 ? 32u : 64u;
        const auto count = static_cast<unsigned>(read(1)) & (width - 1);
        const auto value = read(0);
        if constexpr (op == OpCode::LslImm || op == OpCode::LslValue)
            return Number(value << count, type);
        else if constexpr (op == OpCode::LsrImm || op == OpCode::LsrValue)
            return Number(value >> count, type);
        else
            return Number(static_cast<u64>(Signed(value, width) >> count), type);
    } else if constexpr (op == OpCode::RorImm || op == OpCode::RorValue) {
        const auto value = read(0);
        return Number(TypeBits(type) <= 32
                              ? std::rotr(static_cast<u32>(value), static_cast<int>(read(1) & 31))
                              : std::rotr(value, static_cast<int>(read(1) & 63)),
                      type);
    } else if constexpr (op == OpCode::ByteSwap) {
        const auto width = static_cast<unsigned>(read(1));
        return Number(width == 16   ? __builtin_bswap16(static_cast<std::uint16_t>(raw(0)))
                      : width == 32 ? __builtin_bswap32(static_cast<u32>(raw(0)))
                                    : __builtin_bswap64(raw(0)),
                      type);
    } else if constexpr (op == OpCode::BitExtract) {
        return Number((raw(0) >> read(1)) & Mask(static_cast<unsigned>(read(2))), type);
    } else if constexpr (op == OpCode::BitClear) {
        return Number(raw(0) & ~(Mask(static_cast<unsigned>(read(2))) << read(1)), type);
    } else if constexpr (op == OpCode::BitInsert) {
        const auto first = static_cast<unsigned>(read(2)), count = static_cast<unsigned>(read(3));
        const auto mask = Mask(count) << first;
        return Number((raw(0) & ~mask) | ((raw(1) << first) & mask), type);
    } else if constexpr (op == OpCode::BitCast)
        return Number(Raw(a[0]), type, true);
    else if constexpr (op == OpCode::TestBit)
        return Number((read(0) >> read(1)) & 1, Type::Bool);
    else if constexpr (op == OpCode::TestZero || op == OpCode::TestNotZero)
        return Number(op == OpCode::TestZero ? Read(a[0]) == 0 : Read(a[0]) != 0, Type::Bool);
    else if constexpr (op == OpCode::CountLeadingZeros32 || op == OpCode::CountTrailingZeros32)
        return Number(op == OpCode::CountLeadingZeros32
                              ? std::countl_zero(static_cast<u32>(raw(0)))
                              : std::countr_zero(static_cast<u32>(raw(0))),
                      type);
    else if constexpr (op == OpCode::CountLeadingZeros64 || op == OpCode::CountTrailingZeros64)
        return Number(op == OpCode::CountLeadingZeros64 ? std::countl_zero(raw(0))
                                                        : std::countr_zero(raw(0)),
                      type);
    else if constexpr (op == OpCode::PopCount)
        return Number(std::popcount(raw(0)), type);
    else if constexpr (op == OpCode::CondSet)
        return Number(ConditionValue(static_cast<Condition>(a[0].payload), state.packed_flags),
                      Type::Bool);
    else if constexpr (op == OpCode::TestFlags || op == OpCode::TestNotFlags)
        return Number(op == OpCode::TestFlags ? AnyFlags(a[0].payload) : !AnyFlags(a[0].payload),
                      Type::Bool);
    else if constexpr (op == OpCode::GetFlags)
        return Number(state.packed_flags, type, true);
    else if constexpr (op == OpCode::RestoreFlags) {
        MergeFlags(raw(0), a[1].payload);
        return {};
    } else if constexpr (op == OpCode::ClearFlags) {
        MergeFlags((a[0].payload & FlagBits(Flag::Parity)) ? 1 : 0, a[0].payload);
        return {};
    } else if constexpr (op == OpCode::InvertCarry) {
        state.packed_flags ^= arch::flags::Carry;
        return {};
    } else if constexpr (op == OpCode::SetCarry || op == OpCode::SetOverflow) {
        const auto bit = op == OpCode::SetCarry ? arch::flags::Carry : arch::flags::Overflow;
        state.packed_flags = (state.packed_flags & ~bit) | (Read(a[0]) ? bit : 0);
        return {};
    } else if constexpr (op == OpCode::Select || op == OpCode::SelectZero ||
                         op == OpCode::CondSelect) {
        const bool test =
                op == OpCode::CondSelect
                        ? ConditionValue(static_cast<Condition>(a[0].payload), state.packed_flags)
                        : ((Read(a[0]) != 0) != (op == OpCode::SelectZero));
        return Number(Raw(a[test ? 1 : 2]), type, true);
    } else if constexpr (op == OpCode::ParallelBitDeposit) {
        return Number(arch::ParallelBitDeposit(read(0), read(1)), type);
    } else if constexpr (op == OpCode::ParallelBitExtract) {
        return Number(arch::ParallelBitExtract(read(0), read(1)), type);
    } else if constexpr (op == OpCode::Crc32c) {
        auto crc = static_cast<u32>(raw(0));
        auto value = raw(1);
        for (unsigned byte = 0; byte < read(2) / 8; ++byte) {
            crc ^= static_cast<std::uint8_t>(value);
            value >>= 8;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
        }
        return Number(crc, Type::U32);
    } else if constexpr (op == OpCode::Cpuid) {
        if (!services.cpuid) throw detail::Rejected{BuildError::Unsupported};
        const auto cpu = services.cpuid(
                services.opaque, static_cast<u32>(read(0)), static_cast<u32>(read(1)));
        auto out = Number(cpu.lower, Type::U64);
        out.number.extra = cpu.upper;
        return out;
    } else if constexpr (op == OpCode::CpuidUpper || op == OpCode::Div128Remainder)
        return Number(a[0].value.extra, Type::U64);
    else if constexpr (op == OpCode::X87CommitMetadata) {
        StoreX87Metadata(raw(0), raw(1), raw(2), raw(3));
        return {};
    } else if constexpr (op == OpCode::X87Op || op == OpCode::X87Execute) {
        const auto result = static_cast<arch::X87Result>(
                SvmX87Execute(reinterpret_cast<void*>(raw(0)), raw(1), raw(2)));
        if constexpr (op == OpCode::X87Execute) {
            if (result == arch::X87Result::RaisedException ||
                (result == arch::X87Result::Ok && arch::X87MetadataCommitsSuccess(raw(4)))) {
                StoreX87Metadata(raw(0), state.pc, raw(3), raw(4));
            }
        }
        if (result == arch::X87Result::UnmaskedPending ||
            result == arch::X87Result::RaisedException)
            throw DirectExit{StepKind::Service, 0, 4};
        if (result != arch::X87Result::Ok) throw detail::Rejected{BuildError::Unsupported};
        return Number(0, Type::U64);
    } else if constexpr (op == OpCode::PublishFCmpFlags) {
        const auto value = raw(0);
        MergeFlags(((value & 1) << 29) | ((value & 4) << 28) | ((value & 2) ? 0 : 1),
                   FlagBits(Flag::All));
        return {};
    } else if constexpr (op == OpCode::Div128) {
        const unsigned width = TypeBits(View(a[0]));
        const auto high = read(0), low = read(1), divisor = read(2);
        if (!divisor) throw DirectExit{StepKind::Fault};
        const u128 dividend = (u128{high} << width) | low;
        u128 quotient{}, remainder{};
        if (read(3)) {
            const bool minus = bool(high & (u64{1} << (width - 1))),
                       divisor_minus = bool(divisor & (u64{1} << (width - 1)));
            const auto absolute = minus ? ((~dividend + 1) & WideMask(width * 2)) : dividend;
            const u64 d = divisor_minus ? ((~divisor + 1) & Mask(width)) : divisor;
            quotient = absolute / d;
            remainder = absolute % d;
            const auto limit = u128{1} << (width - 1);
            if (quotient > limit || (!(minus ^ divisor_minus) && quotient == limit))
                throw DirectExit{StepKind::Fault};
            if (minus ^ divisor_minus) quotient = -quotient;
            if (minus) remainder = -remainder;
        } else {
            quotient = dividend / divisor;
            remainder = dividend % divisor;
            if (quotient > Mask(width)) throw DirectExit{StepKind::Fault};
        }
        auto out = Number(quotient & Mask(width), Type::U64);
        out.number.extra = static_cast<u64>(remainder) & Mask(width);
        return out;
    } else if constexpr (op == OpCode::CompareAndSwap128) {
        if (!services.atomic) throw detail::Rejected{BuildError::Unsupported};
        AtomicRequest request;
        request.address = read(0);
        request.bits = 128;
        request.operation = AtomicOperation::CompareExchange128;
        request.expected = {read(1), read(2)};
        request.desired = {read(3), read(4)};
        AtomicValue observed;
        Access(services.atomic(services.opaque, request, observed));
        return Number(u128{observed.low} | (u128{observed.high} << 64), Type::V128);
    } else if constexpr (op == OpCode::AtomicExchange || op == OpCode::AtomicFetchAdd ||
                         op == OpCode::AtomicRMW || op == OpCode::CompareAndSwap) {
        if (!services.atomic) throw detail::Rejected{BuildError::Unsupported};
        AtomicRequest request;
        request.bits = TypeBits(type);
        if constexpr (op == OpCode::AtomicRMW) {
            request.address = read(1);
            request.desired.low = read(2);
            request.carry = read(3) != 0;
            request.operation = static_cast<AtomicOperation>(
                    static_cast<unsigned>(AtomicOperation::Add) + read(0));
        } else {
            request.address = read(0);
            request.desired.low = read(op == OpCode::CompareAndSwap ? 2 : 1);
            request.expected.low = op == OpCode::CompareAndSwap ? read(1) : 0;
            request.operation = op == OpCode::CompareAndSwap   ? AtomicOperation::CompareExchange
                                : op == OpCode::AtomicExchange ? AtomicOperation::Exchange
                                                               : AtomicOperation::FetchAdd;
        }
        AtomicValue observed;
        Access(services.atomic(services.opaque, request, observed));
        return Number(observed.low, type);
    } else
        return Vector<op>(type, a);
}
