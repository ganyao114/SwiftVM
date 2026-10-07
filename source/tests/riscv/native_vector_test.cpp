#include "test_support.h"

#include <cstdlib>
#include <fstream>

#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

#if defined(__riscv) && __riscv_xlen == 64
extern "C" bool SwiftRiscvVectorRange(void*, swift::u64, swift::u64);
#endif

namespace swift::tests::riscv {

using T = ir::ValueType;
using O = ir::OpCode;
using U128 = unsigned __int128;

void NativeVectors(bool vector) {
    rv::HostFeatures features{};
    features.vector = vector;
    u64 seed = 0x39d402a5c195378bULL;
    for (auto op : {O::Vec4And, O::Vec4Or, O::VecAnd, O::VecOr, O::VecXor, O::VecAndNot,
                   O::Vec4Add, O::Vec4Sub, O::VecAdd, O::VecSub, O::VecAvg,
                   O::VecShiftLeft, O::VecShiftRight, O::VecShiftRightArithmetic,
                   O::VecShiftLeftImm, O::VecShiftRightImm, O::VecShiftRightArithmeticImm}) {
        const auto* filter = std::getenv("SVM_RV64_TEST_OP");
        if (filter && std::strtoul(filter, nullptr, 10) != u32(op)) continue;
        for (u32 bits : {8u, 16u, 32u, 64u}) {
            if (op == O::VecAvg && bits > 16) continue;
            for (u64 count : {u64{0}, u64{1}, u64{7}, u64{15}, u64{31}, u64{63}, u64{64}, UINT64_MAX}) {
                IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc000})};
                ir::Assembler as{block.get()};
                auto left = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
                auto right = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
                auto shift = as.LoadUniform(ir::Uniform{32, T::U64}).SetType(T::U64);
                ir::Value result;
                switch (op) {
                    case O::Vec4And: case O::Vec4Or: case O::VecAnd: case O::VecOr: case O::VecXor: case O::VecAndNot:
                    case O::Vec4Add: case O::Vec4Sub: result = ir::Value{block->AppendInst(op, left, right)}; break;
                    case O::VecAdd: case O::VecSub: case O::VecAvg:
                        result = ir::Value{block->AppendInst(op, left, right, ir::Imm{u64(bits)})}; break;
                    case O::VecShiftLeft: case O::VecShiftRight: case O::VecShiftRightArithmetic:
                        result = ir::Value{block->AppendInst(op, left, shift, ir::Imm{u64(bits)})}; break;
                    default: result = ir::Value{block->AppendInst(op, left, ir::Imm{count}, ir::Imm{u64(bits)})}; break;
                }
                as.StoreUniform(ir::Uniform{48, T::V128}, result.SetType(T::V128));
                block->SetTerminal(ir::terminal::ReturnToHost{});
                Compiled cached{block.get(), true, features}, uncached{block.get(), false, features};
                Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0,
                      "native vector data/arithmetic/shift operations have no interpreter calls");
                if (vector && op == O::VecAvg)
                    Check(cached.translator.Stats().bytes[size_t(op)] <= 12, "RVV average uses one instruction plus vtype/rounding setup");
                for (u32 sample = 0; sample < 16; ++sample) {
                    StateStorage a, b, reference;
                    for (u32 offset = 0; offset < 32; offset += 8) {
                        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                        const u64 value = sample == 0 ? 0 : sample == 1 ? UINT64_MAX : sample == 2 ? u64{1} << 63 : seed;
                        a.Put(offset, value); b.Put(offset, value); reference.Put(offset, value);
                    }
                    a.Put(32, count); b.Put(32, count); reference.Put(32, count);
                    backend::interp::Interpreter interpreter{*reference.state, block.get()};
                    Check(cached.fn(a.state) == interpreter.Run() && uncached.fn(b.state) == HaltReason::CallHost,
                          "native vector arithmetic halt");
                    if (std::memcmp(a.state->uniform_buffer_begin + 48, reference.state->uniform_buffer_begin + 48, 16) != 0 ||
                        std::memcmp(b.state->uniform_buffer_begin + 48, reference.state->uniform_buffer_begin + 48, 16) != 0) {
                        for (auto* compiled : {&cached, &uncached}) {
                            std::ofstream dump{compiled == &cached ? "/tmp/swiftvm-rvv-cached-failure.bin" :
                                                                      "/tmp/swiftvm-rvv-uncached-failure.bin", std::ios::binary};
                            dump.write(reinterpret_cast<const char*>(compiled->context.GetMasm().GetCodeBuffer().GetOffsetPointer(0)),
                                       compiled->context.CurrentBufferSize());
                        }
                    }
                    Check(std::memcmp(a.state->uniform_buffer_begin + 48, reference.state->uniform_buffer_begin + 48, 16) == 0 &&
                          std::memcmp(b.state->uniform_buffer_begin + 48, reference.state->uniform_buffer_begin + 48, 16) == 0,
                          "native vector differential: op=" + std::to_string(u32(op)) + " width=" +
                          std::to_string(bits) + " count=" + std::to_string(count) + " sample=" + std::to_string(sample) +
                          " cached=" + std::to_string(a.Get(48)) + ":" + std::to_string(a.Get(56)) +
                          " uncached=" + std::to_string(b.Get(48)) + ":" + std::to_string(b.Get(56)) +
                          " expected=" + std::to_string(reference.Get(48)) + ":" + std::to_string(reference.Get(56)));
                }
            }
        }
    }
    for (u32 lane = 0; lane < 8; ++lane) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc080})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
        auto scalar = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        auto inserted = as.VecInsert16(input, scalar, ir::Imm{u64(lane)}).SetType(T::V128);
        as.StoreUniform(ir::Uniform{32, T::V128}, inserted);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features};
        if (vector) Check(compiled.translator.Stats().bytes[size_t(O::VecInsert16)] <= 24,
                          "RVV halfword insertion remains in vector registers without a GPR-pair round trip");
        StateStorage state;
        std::array<u16, 8> expected{0x8235, 0x7149, 0xa136, 0x2395, 0xff00, 0x1874, 0x3145, 0x8756};
        std::memcpy(state.state->uniform_buffer_begin, expected.data(), 16);
        state.Put(16, 0xdeadbeefcafe9671ULL);
        expected[lane] = 0x9671;
        Check(compiled.fn(state.state) == HaltReason::CallHost &&
              std::memcmp(state.state->uniform_buffer_begin + 32, expected.data(), 16) == 0,
              "halfword insertion changes only the selected lane and truncates its scalar input");
    }
    // Narrow vector homes must retain zero high halves, independently of VLEN.
    for (auto type : {T::V8, T::V16, T::V32, T::V64, T::V128}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc100})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto raw = as.BitCast(input).SetType(T::V128);
        as.StoreUniform(ir::Uniform{32, T::V128}, raw);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features};
        StateStorage state;
        state.Put(0, 0x39ed7245a593275aULL); state.Put(8, 0x83694123a19edcb7ULL);
        const u32 bytes = ir::GetValueSizeByte(type);
        std::array<u8, 16> expected{};
        std::memcpy(expected.data(), state.state->uniform_buffer_begin, bytes);
        Check(compiled.fn(state.state) == HaltReason::CallHost &&
              std::memcmp(expected.data(), state.state->uniform_buffer_begin + 32, 16) == 0,
              "narrow vector load zeroes every unused bit of its 128-bit home");
    }
    for (bool from_vector : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc180})};
        ir::Assembler as{block.get()};
        auto input = from_vector ? as.VecLoadConst(ir::Imm{u64{0x100}}, ir::Imm{u64{0x837251694283fbea}}).SetType(T::V128)
                                 : as.LoadImm(ir::Imm{u64{0x100}}).SetType(T::U64);
        auto narrow = as.BitCast(input).SetType(T::U8);
        auto test = as.TestNotZero(narrow);
        auto wide = as.BitCast(narrow).SetType(T::V128);
        as.StoreUniform(ir::Uniform{0, T::V128}, wide);
        as.StoreUniform(ir::Uniform{16, T::U8}, test);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features};
        StateStorage state;
        Check(compiled.fn(state.state) == HaltReason::CallHost && state.Get(0) == 0x100 &&
              state.Get(8) == (from_vector ? u64{0x837251694283fbea} : 0) && state.Get(16) == 1,
              "raw BitCast keeps the entire slot through narrow scalar types");
    }
    // Guest vector accesses support every byte alignment, and an oracle call
    // must preserve both live RVV values and vtype across its C ABI clobbers.
    for (bool callback : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc190})};
        ir::Assembler as{block.get()};
        auto live = as.VecLoadConst(ir::Imm{u64{0x31468925147abcee}}, ir::Imm{u64{0x986751fa47b234cd}}).SetType(T::V128);
        auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto loaded = as.LoadMemoryTSO(ir::Operand{address}).SetType(T::V128);
        auto value = as.VecXor(live, loaded).SetType(T::V128);
        as.StoreMemoryTSO(ir::Operand{address}, value);
        as.StoreUniform(ir::Uniform{32, T::V128}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features};
        Check(compiled.translator.Stats().helpers == 0, "vector guest memory is native on both host profiles");
        for (u32 offset = 0; offset < 16; ++offset) {
            alignas(16) std::array<u8, 48> memory;
            for (u32 i = 0; i < memory.size(); ++i) memory[i] = u8(i * 17 + 3);
            auto expected = memory;
            const std::array<u64, 2> key{0x31468925147abcee, 0x986751fa47b234cd};
            for (u32 i = 0; i < 16; ++i) expected[offset + i] ^= reinterpret_cast<const u8*>(key.data())[i];
            StateStorage state;
            state.Put(0, reinterpret_cast<u64>(memory.data() + offset));
#if defined(__riscv) && __riscv_xlen == 64
            if (vector && callback) state.state->interp_range_check = &SwiftRiscvVectorRange;
#endif
            Check(compiled.fn(state.state) == HaltReason::CallHost && memory == expected &&
                  std::memcmp(expected.data() + offset, state.state->uniform_buffer_begin + 32, 16) == 0,
                  "vector guest memory alignment and oracle-call register preservation");
        }
    }
    IntrusivePtr<ir::Block> pressure{new ir::Block(ir::Location{0xc195})};
    ir::Assembler pressure_as{pressure.get()};
    std::array<ir::Value, 24> inputs;
    u64 expected_low{}, expected_high{};
    for (u32 i = 0; i < inputs.size(); ++i) {
        const u64 low = 0x8325e195e786392aULL * (i + 1), high = low ^ (0x9856ac71435ULL << i);
        inputs[i] = pressure_as.VecLoadConst(ir::Imm{low}, ir::Imm{high}).SetType(T::V128);
        expected_low ^= low; expected_high ^= high;
    }
    auto sum = inputs[0];
    for (u32 i = 1; i < inputs.size(); ++i) sum = pressure_as.VecXor(sum, inputs[i]).SetType(T::V128);
    pressure_as.StoreUniform(ir::Uniform{0, T::V128}, sum);
    pressure->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled pressure_compiled{pressure.get(), true, features};
    StateStorage pressure_state;
    Check(pressure_compiled.context.ValueStats().spills > 0 && pressure_compiled.fn(pressure_state.state) == HaltReason::CallHost &&
          pressure_state.Get(0) == expected_low && pressure_state.Get(8) == expected_high,
          "vector pressure spills exactly 16-byte homes at every host VLEN");
    IntrusivePtr<ir::Block> chain{new ir::Block(ir::Location{0xc200})};
    ir::Assembler as{chain.get()};
    auto value = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
    auto addend = as.VecLoadConst(ir::Imm{u64{0x0000000100000001}}, ir::Imm{u64{0x0000000100000001}}).SetType(T::V128);
    for (u32 i = 0; i < 48; ++i) value = as.VecAdd(value, addend, ir::Imm{u64{32}}).SetType(T::V128);
    as.StoreUniform(ir::Uniform{32, T::V128}, value);
    chain->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{chain.get(), true, features};
    Check(compiled.context.ValueStats().loads == 0 && compiled.context.ValueStats().stores == 0 &&
          compiled.context.ValueStats().spills == 0, "straight vector chains keep all live values in registers");
    if (vector) Check(compiled.translator.Stats().bytes[size_t(O::VecAdd)] <= 49 * 4,
                      "RVV VecAdd chain emits one arithmetic instruction per IR after one vtype setup");
    StateStorage state;
    state.Put(0, UINT64_MAX); state.Put(8, 0x00000000ffffffffULL);
    const std::array<u32, 4> wanted{47, 47, 47, 48};
    Check(compiled.fn(state.state) == HaltReason::CallHost &&
          std::memcmp(wanted.data(), state.state->uniform_buffer_begin + 32, 16) == 0,
          "independent modular vector chain expected lanes");
    std::cout << "METRIC " << (vector ? "RVV" : "GPR-pair") << " vector chain bytes " << compiled.context.CurrentBufferSize()
              << ", VecAdd bytes " << compiled.translator.Stats().bytes[size_t(O::VecAdd)] << '\n';
    std::cout << "PASS native vector data, bitwise, SWAR arithmetic, lane shifts and register chains\n";
}

void NativeVectorInteger(bool vector) {
    rv::HostFeatures features{};
    features.vector = vector;
    u64 seed = 0x87519fab13521486ULL;
    for (auto op : {O::Vec4Mul, O::VecMul, O::VecMulHigh16, O::VecMulWiden,
                   O::VecCmpEq, O::VecCmpGt, O::VecMin, O::VecMax, O::VecSatAdd, O::VecSatSub, O::VecPack}) {
        const auto* filter = std::getenv("SVM_RV64_TEST_OP");
        if (filter && std::strtoul(filter, nullptr, 10) != u32(op)) continue;
        for (u32 bits : {8u, 16u, 32u, 64u}) {
            if (op == O::Vec4Mul && bits != 32) continue;
            if (op == O::VecMulHigh16 && bits != 16) continue;
            if (op == O::VecMulWiden && bits == 64) continue;
            if (op == O::VecPack && (bits == 8 || bits == 64)) continue;
            // The interpreter's 64-bit saturation currently shifts by 64 /
            // overflows signed addition. Test those lanes independently below.
            if ((op == O::VecSatAdd || op == O::VecSatSub) && bits == 64) continue;
            for (u32 mode = 0; mode < 2; ++mode) {
                IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc300})};
                ir::Assembler as{block.get()};
                auto left = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
                auto right = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
                ir::Value result;
                if (op == O::Vec4Mul) result = as.Vec4Mul(left, right);
                else if (op == O::VecMulHigh16) result = as.VecMulHigh16(left, right, ir::Imm{u64(mode)});
                else if (op == O::VecMul || op == O::VecCmpEq || op == O::VecCmpGt)
                    result = ir::Value{block->AppendInst(op, left, right, ir::Imm{u64(bits)})};
                else result = ir::Value{block->AppendInst(op, left, right, ir::Imm{u64(bits)}, ir::Imm{u64(mode)})};
                as.StoreUniform(ir::Uniform{32, T::V128}, result.SetType(T::V128));
                block->SetTerminal(ir::terminal::ReturnToHost{});
                Compiled cached{block.get(), true, features}, uncached{block.get(), false, features};
                Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0,
                      "packed integer arithmetic and saturation are native");
                if (vector && (op == O::Vec4Mul || op == O::VecMul || op == O::VecMulHigh16 || op == O::VecMin ||
                               op == O::VecMax || op == O::VecSatAdd || op == O::VecSatSub))
                    Check(cached.translator.Stats().bytes[size_t(op)] <= 8, "RVV arithmetic uses one instruction plus vtype if needed");
                for (u32 sample = 0; sample < 64; ++sample) {
                    StateStorage a, b, reference;
                    for (u32 offset = 0; offset < 32; offset += 8) {
                        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                        const u64 value = sample == 0 ? 0 : sample == 1 ? UINT64_MAX :
                                sample == 2 ? 0x8000800080008000ULL : sample == 3 ? 0x7fff7fff7fff7fffULL : seed;
                        a.Put(offset, value); b.Put(offset, value); reference.Put(offset, value);
                    }
                    backend::interp::Interpreter interpreter{*reference.state, block.get()};
                    Check(cached.fn(a.state) == interpreter.Run() && uncached.fn(b.state) == HaltReason::CallHost,
                          "packed integer arithmetic halt");
                    Check(std::memcmp(a.state->uniform_buffer_begin + 32, reference.state->uniform_buffer_begin + 32, 16) == 0 &&
                          std::memcmp(b.state->uniform_buffer_begin + 32, reference.state->uniform_buffer_begin + 32, 16) == 0,
                          "packed integer differential: op=" + std::to_string(u32(op)) + " width=" +
                          std::to_string(bits) + " mode=" + std::to_string(mode) + " sample=" + std::to_string(sample));
                }
            }
        }
    }
    for (auto op : {O::VecSatAdd, O::VecSatSub}) {
        for (bool signed_lanes : {false, true}) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc400})};
            ir::Assembler as{block.get()};
            auto left = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
            auto right = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
            auto result = ir::Value{block->AppendInst(op, left, right, ir::Imm{u64{64}}, ir::Imm{u64(signed_lanes)})}.SetType(T::V128);
            as.StoreUniform(ir::Uniform{32, T::V128}, result);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled compiled{block.get(), true, features};
            for (u64 a : {u64{0}, u64{1}, UINT64_MAX, u64{1} << 63, (u64{1} << 63) - 1}) {
                for (u64 b : {u64{0}, u64{1}, UINT64_MAX, u64{1} << 63, (u64{1} << 63) - 1}) {
                    u64 wanted;
                    if (signed_lanes) {
                        const __int128 wide_a = s64(a), wide_b = s64(b);
                        auto wide = op == O::VecSatAdd ? wide_a + wide_b : wide_a - wide_b;
                        const auto minimum = -(__int128{1} << 63), maximum = (__int128{1} << 63) - 1;
                        if (wide < minimum) wide = minimum; if (wide > maximum) wide = maximum;
                        wanted = u64(wide);
                    } else if (op == O::VecSatAdd) {
                        const U128 wide = U128(a) + b;
                        wanted = wide > UINT64_MAX ? UINT64_MAX : u64(wide);
                    } else wanted = a < b ? 0 : a - b;
                    StateStorage state;
                    state.Put(0, a); state.Put(8, a); state.Put(16, b); state.Put(24, b);
                    Check(compiled.fn(state.state) == HaltReason::CallHost && state.Get(32) == wanted && state.Get(40) == wanted,
                          "independent 128-bit oracle for 64-bit signed/unsigned vector saturation");
                }
            }
        }
    }
    std::cout << "PASS native packed integer multiply, comparisons, min/max, saturation and narrowing\n";
}

}  // namespace swift::tests::riscv
