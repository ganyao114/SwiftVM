#include "test_support.h"

#include <bit>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {

using O = ir::OpCode;
using T = ir::ValueType;

namespace {

IntrusivePtr<ir::Block> ScalarProgram(O op, T type) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x1000})};
    ir::Assembler as{block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto b = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
    ir::Value result;
    switch (op) {
        case O::Add: case O::Sub: case O::Adc: case O::Sbb: case O::Mul: case O::Div:
        case O::And: case O::Or: case O::Xor: case O::AndNot:
            result = ir::Value{block->AppendInst(op, a, ir::Operand{b})}.SetType(type); break;
        case O::LslValue: case O::LsrValue: case O::AsrValue: case O::RorValue:
            result = ir::Value{block->AppendInst(op, a, b)}.SetType(type); break;
        case O::LslImm: case O::LsrImm: case O::AsrImm: case O::RorImm:
            result = ir::Value{block->AppendInst(op, a, ir::Imm{u64{65}})}.SetType(type); break;
        case O::MulHigh: result = as.MulHigh(a, b, ir::Imm{ir::IsSignValueType(type)}).SetType(T::U64); break;
        case O::MulSub: result = as.MulSub(a, b, a).SetType(type); break;
        case O::SignedDiv64: result = as.SignedDiv64(a, b).SetType(T::U64); break;
        case O::TestBit: result = as.TestBit(a, ir::Imm{u64{7}}).SetType(T::U8); break;
        case O::ZeroExtend32To64: case O::ZeroExtend64:
            result = ir::Value{block->AppendInst(op, a)}.SetType(T::U64); break;
        case O::ZeroExtend32: result = as.ZeroExtend32(a).SetType(T::U32); break;
        case O::SignExtend: result = as.SignExtend(a).SetType(T::U64); break;
        case O::Neg: case O::Not: case O::TestZero: case O::TestNotZero:
            result = ir::Value{block->AppendInst(op, a)}.SetType(type); break;
        default: throw std::runtime_error("unsupported scalar test fixture");
    }
    as.SaveFlags(result, ir::Flags::All);
    as.StoreUniform(ir::Uniform{16, result.Type()}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

u64 Helper(u64 a, u64 b, u64 c) { return (a ^ std::rotl(b, 13)) + c; }

u64 ReverseBytes(u64 value, u32 bits) {
    u64 result{};
    for (u32 byte = 0; byte < bits / 8; ++byte) {
        result = (result << 8) | (value & 255);
        value >>= 8;
    }
    return result;
}

u32 ReferenceCrc(u32 crc, u64 input, u32 bits) {
    for (u32 bit = 0; bit < bits; ++bit) {
        const bool feedback = ((crc ^ input) & 1) != 0;
        crc >>= 1; input >>= 1;
        if (feedback) crc ^= 0x82f63b78;
    }
    return crc;
}

}  // namespace

void ScalarDifferential() {
    const std::array ops{O::Add, O::Sub, O::Adc, O::Sbb, O::Mul, O::Div,
        O::And, O::Or, O::Xor, O::AndNot, O::Neg, O::Not, O::TestBit,
        O::TestZero, O::TestNotZero, O::LslValue, O::LsrValue, O::AsrValue,
        O::RorValue, O::LslImm, O::LsrImm, O::AsrImm, O::RorImm,
        O::ZeroExtend32, O::ZeroExtend32To64, O::ZeroExtend64, O::SignExtend,
        O::MulHigh, O::MulSub, O::SignedDiv64};
    const std::array types{T::U8, T::U16, T::U32, T::U64, T::S8, T::S16, T::S32, T::S64};
    const std::array<u64, 12> edges{0, 1, 2, 7, 63, 64, 65, UINT64_MAX,
        0x8000000000000000ULL, 0x7fffffffffffffffULL, 0x8000000080000000ULL, 0xabcdef1234567890ULL};
    u64 seed = 0x742305df27b19381ULL;
    for (auto type : types) for (auto op : ops) {
        auto native_ir = ScalarProgram(op, type);
        auto reference_ir = ScalarProgram(op, type);
        Compiled cached{native_ir.get()}, uncached{native_ir.get(), false};
        Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0,
              "scalar arithmetic and flag production must use direct native lowering");
        for (u32 i = 0; i < 96; ++i) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            const u64 a = i < edges.size() ? edges[i] : seed;
            const u64 b = i < edges.size() ? edges[(i + 3) % edges.size()] : std::rotl(seed, 17);
            StateStorage actual, control, reference;
            for (auto* storage : {&actual, &control, &reference}) {
                storage->Put(0, a); storage->Put(8, b);
                storage->state->host_cpu_flags = i & 1 ? UINT64_MAX : std::rotl(seed, 31);
            }
            backend::interp::Interpreter interpreter{*reference.state, reference_ir.get()};
            const auto wanted = interpreter.Run();
            const auto detail = std::string{ir::GetIRMetaInfo(op).name} + "/" +
                                ir::ValueTypeString(type) + "/" + std::to_string(i);
            Check(cached.fn(actual.state) == wanted && uncached.fn(control.state) == wanted, detail + " halt");
            Check(actual.Get(16) == reference.Get(16) && control.Get(16) == reference.Get(16),
                  detail + " value: expected=" + std::to_string(reference.Get(16)) +
                  " actual=" + std::to_string(actual.Get(16)));
            Check(actual.state->host_cpu_flags == reference.state->host_cpu_flags &&
                  control.state->host_cpu_flags == reference.state->host_cpu_flags,
                  detail + " flags: expected=" + std::to_string(reference.state->host_cpu_flags) +
                  " actual=" + std::to_string(actual.state->host_cpu_flags));
            if (op == O::Neg) {
                const auto bits = ir::GetValueSizeByte(type) * 8;
                const auto mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
                Check(((actual.state->host_cpu_flags >> 29) & 1) == ((a & mask) == 0), detail + " negate carry");
                Check(((actual.state->host_cpu_flags >> 28) & 1) == ((a & mask) == (u64{1} << (bits - 1))),
                      detail + " negate overflow");
            }
        }
        StateStorage abi;
        Check(CheckABI(cached.fn, abi.state) == 1, "scalar block preserves LP64D registers and SP");
    }
    std::cout << "PASS 240 scalar programs, 46080 cached/uncached differential executions\n";
}

void ScalarBits() {
    u64 seed = 0x609323548734e5bdULL;
    const std::array<u64, 12> edges{0, 1, 2, 3, 127, 128, 255, 256,
        0x8000000000000000ULL, 0xffffffff, 0x0123456789abcdefULL, UINT64_MAX};
    for (O op : {O::PopCount, O::CountLeadingZeros64, O::CountTrailingZeros64,
                 O::CountLeadingZeros32, O::CountTrailingZeros32, O::ByteSwap, O::Crc32c,
                 O::BitExtract, O::BitInsert, O::BitClear, O::LocalParitySet}) {
        const bool count32 = op == O::CountLeadingZeros32 || op == O::CountTrailingZeros32;
        const bool field = op == O::BitExtract || op == O::BitInsert || op == O::BitClear;
        const u32 variants = op == O::ByteSwap ? 3 : op == O::Crc32c ? 4 :
                             op == O::LocalParitySet ? 2 : field ? 8 : 1;
        for (u32 variant = 0; variant < variants; ++variant) {
            const u32 bits = op == O::ByteSwap ? 16u << variant : op == O::Crc32c ? 8u << variant :
                             std::array{0u, 1u, 8u, 16u, 31u, 32u, 63u, 64u}[variant];
            const u32 position = field ? (bits ? 64 - bits : 37) : 0;
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x1200})};
            ir::Assembler as{block.get()};
            auto a = as.LoadUniform(ir::Uniform{0, count32 ? T::U32 : T::U64}).SetType(count32 ? T::U32 : T::U64);
            auto b = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
            ir::Value output;
            if (op == O::ByteSwap) output = as.ByteSwap(a, ir::Imm{u64(bits)}).SetType(T::U64);
            else if (op == O::Crc32c) output = as.Crc32c(a, b, ir::Imm{u64(bits)}).SetType(T::U32);
            else if (op == O::BitExtract || op == O::BitClear)
                output = ir::Value{block->AppendInst(op, a, ir::Imm{u64(position)}, ir::Imm{u64(bits)})}.SetType(T::U64);
            else if (op == O::BitInsert)
                output = as.BitInsert(a, b, ir::Imm{u64(position)}, ir::Imm{u64(bits)}).SetType(T::U64);
            else if (op == O::LocalParitySet) output = as.LocalParitySet(a, ir::Imm{u64(variant)});
            else output = ir::Value{block->AppendInst(op, a)}.SetType(count32 ? T::U32 : T::U64);
            as.StoreUniform(ir::Uniform{16, output.Type()}, output);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled compiled{block.get()};
            Check(compiled.translator.Stats().helpers == 0, "bit operations must lower to native instructions");
            for (u32 i = 0; i < 96; ++i) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                const u64 a = i < edges.size() ? edges[i] : seed;
                const u64 b = std::rotl(a, 17) ^ 0xcbaed1943206587fULL;
                u64 wanted{};
                if (op == O::PopCount) wanted = std::popcount(a);
                else if (op == O::CountLeadingZeros64) wanted = std::countl_zero(a);
                else if (op == O::CountTrailingZeros64) wanted = std::countr_zero(a);
                else if (op == O::CountLeadingZeros32) wanted = std::countl_zero(u32(a));
                else if (op == O::CountTrailingZeros32) wanted = std::countr_zero(u32(a));
                else if (op == O::ByteSwap) wanted = ReverseBytes(a, bits);
                else if (op == O::Crc32c) wanted = ReferenceCrc(a, b, bits);
                else if (op == O::LocalParitySet) wanted = (std::popcount(u8(a)) % 2 == 0) ^ (variant != 0);
                else {
                    wanted = op == O::BitExtract ? 0 : a;
                    for (u32 bit = 0; bit < bits; ++bit) {
                        const u64 mask = u64{1} << (op == O::BitExtract ? bit : position + bit);
                        const bool set = op == O::BitExtract ? ((a >> (position + bit)) & 1) :
                                         op == O::BitInsert ? ((b >> bit) & 1) : false;
                        wanted = set ? wanted | mask : wanted & ~mask;
                    }
                }
                StateStorage state;
                state.Put(0, a); state.Put(8, b);
                Check(compiled.fn(state.state) == HaltReason::CallHost, "native bit operation halt");
                Check(state.Get(16) == wanted, std::string{ir::GetIRMetaInfo(op).name} + "/" +
                      std::to_string(variant) + "/" + std::to_string(i) + " independent result");
            }
        }
    }
    std::cout << "PASS native counts, reversal, CRC32C, field operations and parity\n";
}

void HelpersABI() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x1300})};
    ir::Assembler as{block.get()};
    std::array<ir::Value, 24> inputs;
    for (u32 i = 0; i < inputs.size(); ++i)
        inputs[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
    auto result = as.CallHost(&Helper, inputs[0], inputs[11], inputs[23]).SetType(T::U64);
    for (u32 i = 0; i < inputs.size(); ++i) result = as.Add(result, ir::Operand{inputs[i]}).SetType(T::U64);
    as.SaveFlags(result, ir::Flags::All);
    as.StoreUniform(ir::Uniform{192, T::U64}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled cached{block.get()}, uncached{block.get(), false};
    Check(cached.translator.Stats().direct >= 48, "scalar pressure around callback uses direct native lowering");
    for (u32 run = 0; run < 16; ++run) {
        StateStorage actual, expected, reference;
        u64 wanted = Helper(run + 0xabc0, run + 0xabcb, run + 0xabd7);
        for (u32 i = 0; i < inputs.size(); ++i) {
            const u64 value = run + 0xabc0 + i;
            actual.Put(i * 8, value); expected.Put(i * 8, value); reference.Put(i * 8, value);
            wanted += value;
        }
        backend::interp::Interpreter interpreter{*reference.state, block.get()};
        const auto halt = interpreter.Run();
        const auto cached_halt = cached.fn(actual.state), uncached_halt = uncached.fn(expected.state);
        Check(cached_halt == halt && uncached_halt == halt,
              "helper pressure halt run=" + std::to_string(run) + " expected=" + std::to_string(u32(halt)) +
              " cached=" + std::to_string(u32(cached_halt)) + " uncached=" + std::to_string(u32(uncached_halt)) +
              " value=" + std::to_string(actual.Get(192)) + "/" + std::to_string(expected.Get(192)));
        Check(actual.Get(192) == wanted && expected.Get(192) == wanted, "24 live scalar values survive host callback");
        Check(actual.state->host_cpu_flags == reference.state->host_cpu_flags &&
              expected.state->host_cpu_flags == reference.state->host_cpu_flags, "helper pressure flags");
        Check(CheckABI(cached.fn, actual.state) == 1, "helper and spill paths preserve LP64D and 16-byte SP");
    }
    std::cout << "PASS host C ABI and 24-value pressure across callback\n";
}

}  // namespace swift::tests::loongarch64
