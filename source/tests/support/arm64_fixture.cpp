#include "arm64_fixture.h"

namespace swift::tests::arm64_fixture {

std::uint32_t WordOffset(swift::runtime::backend::NativeOffset offset) noexcept {
    return offset == swift::runtime::backend::NoNativeOffset ? UINT32_MAX : offset / 4;
}
#if defined(__aarch64__) && defined(__ANDROID__)
const swift::runtime::execution::HostBackendOps& TestBackendOps() {
    static swift::runtime::platform::Arm64Host binding{false};
    static const auto operations = binding.Bind();
    return operations;
}

void BindFaultCallbacks(swift::runtime::execution::FaultContext& context,
                        swift::runtime::execution::NativeRunContext& native) {
    context.native = &native;
    native.faults = &context;
    context.patch_ordered = TestBackendOps().patch_fault;
    context.recover_native = TestBackendOps().recover_native;
}

void PatchLinkWords(std::uintptr_t* write,
                    const std::uintptr_t* execute,
                    std::uintptr_t target,
                    std::uint32_t back) {
    arm64::PatchNativeLink(write, execute, target, back * 4);
}

void UnlinkWords(std::uintptr_t* write, const std::uintptr_t* execute, std::uint32_t back) {
    arm64::UnlinkNativeLink(write, execute, back * 4);
}
#endif

Value Load(Builder& builder, std::uint32_t offset) {
    return Need(
            builder.Emit<OpCode::LoadUniform>(Type::U64, {Operand::Uniform(offset, Type::U64)}));
}

void Store(Builder& builder, std::uint32_t offset, Value value) {
    Need(builder.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(offset, Type::U64), Operand::ValueOf(value)}));
}

void StoreCarrier(Builder& builder, std::uint32_t offset, Value value) {
    Need(builder.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(offset, Type::U64), Operand::ValueOf(value, Type::U64)}));
}

[[maybe_unused]] u64 Read64(const void* data) {
    u64 value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

void Write64(void* data, u64 value) { std::memcpy(data, &value, sizeof(value)); }

arm64::CompileResult Compile(const Program& program, arm64::CompileOptions options) {
    auto result = arm64::Compile(program, options);
    Check(bool(result) && !result.words.empty(), "native writer rejected supported IR");
    Check(result.frame_bytes >= 16 && result.frame_bytes % 16 == 0 &&
                  result.spill_slots * 8 <= result.frame_bytes - 16,
          "unaligned native frame");
    Check(result.words.front() == 0xa9bf7bf3 && result.words[1] == 0xaa0003f3,
          "state/callee-save prologue");
    return result;
}

Value Snapshot(Builder& builder, Value anchor) {
    return Need(builder.Emit<OpCode::GetFlags>(
            Type::U64, {Operand::ValueOf(anchor), Operand::Flags(Flag::All)}));
}

u64 ReferenceArithmetic(OpCode op,
                        u64 left,
                        u64 right,
                        unsigned width,
                        FlagProfile profile,
                        u64 mask,
                        u64 previous) {
    const u64 word_mask = width == 64 ? ~u64{} : (u64{1} << width) - 1;
    left &= word_mask;
    right &= word_mask;
    if (op == OpCode::Neg) {
        right = left;
        left = 0;
    }
    const bool subtract = op != OpCode::Add;
    const u64 result = (subtract ? left - right : left + right) & word_mask;
    const bool raw_carry = subtract
                                   ? left >= right
                                   : ((static_cast<unsigned __int128>(left) + right) >> width) != 0;
    const auto sign = static_cast<__int128>(1) << (width - 1);
    const auto modulus = static_cast<__int128>(1) << width;
    const auto signed_left =
            static_cast<__int128>(left) >= sign ? static_cast<__int128>(left) - modulus : left;
    const auto signed_right =
            static_cast<__int128>(right) >= sign ? static_cast<__int128>(right) - modulus : right;
    const auto signed_result = subtract ? signed_left - signed_right : signed_left + signed_right;
    const auto set = [&](Flag flag, unsigned bit, bool value) {
        if (!(mask & FlagBits(flag))) return;
        previous = (previous & ~(u64{1} << bit)) | (u64{value} << bit);
    };
    set(Flag::Carry,
        29,
        raw_carry ^ (subtract && profile == FlagProfile::ArithmeticCanonicalCarry));
    set(Flag::Overflow, 28, signed_result < -sign || signed_result >= sign);
    set(Flag::Zero, 30, result == 0);
    set(Flag::Negate, 31, (result >> (width - 1)) != 0);
    set(Flag::AuxiliaryCarry, 26, ((left ^ right ^ result) & 16) != 0);
    if (mask & FlagBits(Flag::Parity)) previous = (previous & ~u64{255}) | (result & 255);
    return previous;
}

void Execute(const arm64::CompileResult& code, std::uint8_t* state, u64 expected) {
    Check(NativeMode(), "native execution requires --native");
    swift::tests::NativeCode native{code.words, NativeISA::Arm64Android};
    Check(native.Run(state) == expected, "executed native result");
}

}  // namespace swift::tests::arm64_fixture
