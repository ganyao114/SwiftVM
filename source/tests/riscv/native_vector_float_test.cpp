#include "test_support.h"

#include <cstdlib>

#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::riscv {

using T = ir::ValueType;
using O = ir::OpCode;

void NativeVectorFloat(bool vector) {
    rv::HostFeatures features{}; features.vector = vector;
    constexpr std::array<u64, 16> edge32{0, 0x80000000, 0x3f800000, 0xbf800000, 0x3fc00000, 0xc0800000,
        0x7f800000, 0xff800000, 0x7fc12345, 0xffc23456, 0x7f812345, 0xff812345,
        1, 0x00800000, 0x7f7fffff, 0x4f000000};
    constexpr std::array<u64, 16> edge64{0, 0x8000000000000000ULL, 0x3ff0000000000000ULL, 0xbff0000000000000ULL,
        0x3ff8000000000000ULL, 0xc010000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
        0x7ff8123456789abcULL, 0xfff823456789abcdULL, 0x7ff0123456789abcULL, 0xfff023456789abcdULL,
        1, 0x0010000000000000ULL, 0x7fefffffffffffffULL, 0x43e0000000000000ULL};
    u64 seed = 0xf98a32096843958eULL;
    const auto random = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
    const auto exercise = [&](ir::Block* block, O op, u32 bits, u32 mode, bool scalar) {
        Compiled cached{block, true, features}, uncached{block, false, features};
        Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0,
              "floating instructions must not dispatch through the interpreter");
        if (vector) Check(cached.translator.Stats().bytes[size_t(op)] <= 160,
                          "RVV floating sequence has a fixed instruction budget including NaN repair");
        for (u32 sample = 0; sample < 320; ++sample) {
            StateStorage a, b, reference;
            for (u32 operand = 0; operand < 3; ++operand) {
                std::array<u64, 2> raw{};
                for (u32 lane = 0; lane < 128 / bits; ++lane) {
                    const u32 selected = (operand == 0 ? sample / 16 : operand == 1 ? sample % 16 : sample / 16 + sample % 16) + lane * (operand + 1);
                    const auto value = sample < 256 ? (bits == 32 ? edge32[selected % 16] : edge64[selected % 16]) : random();
                    raw[lane * bits / 64] |= (bits == 32 ? u64(u32(value)) : value) << (lane * bits % 64);
                }
                for (u32 part = 0; part < 2; ++part) { a.Put(operand * 16 + part * 8, raw[part]); b.Put(operand * 16 + part * 8, raw[part]); reference.Put(operand * 16 + part * 8, raw[part]); }
            }
            backend::interp::Interpreter interpreter{*reference.state, block};
            const auto expected = interpreter.Run();
            Check(cached.fn(a.state) == expected && uncached.fn(b.state) == expected, "floating operation halt");
            Check(std::memcmp(a.state->uniform_buffer_begin + 64, reference.state->uniform_buffer_begin + 64, 16) == 0 &&
                  std::memcmp(b.state->uniform_buffer_begin + 64, reference.state->uniform_buffer_begin + 64, 16) == 0,
                  "floating differential: op=" + std::to_string(u32(op)) + " bits=" + std::to_string(bits) +
                  " mode=" + std::to_string(mode) + " scalar=" + std::to_string(scalar) + " sample=" + std::to_string(sample) +
                  " cached=" + std::to_string(a.Get(64)) + ":" + std::to_string(a.Get(72)) +
                  " uncached=" + std::to_string(b.Get(64)) + ":" + std::to_string(b.Get(72)) +
                  " expected=" + std::to_string(reference.Get(64)) + ":" + std::to_string(reference.Get(72)) +
                  " lhs=" + std::to_string(a.Get(0)) + ":" + std::to_string(a.Get(8)) +
                  " ref-lhs=" + std::to_string(reference.Get(0)) + ":" + std::to_string(reference.Get(8)));
#if defined(__riscv) && __riscv_xlen == 64
            // Static scalar rounding and the block's RVV rounding environment
            // must be independent of, and preserve, the caller's rounding mode.
            if (sample == 257) {
                StateStorage hostile; std::memcpy(hostile.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 48);
                u64 previous{}, after{};
                asm volatile("fsrmi %0, 2" : "=r"(previous) :: "memory");
                const auto reason = cached.fn(hostile.state);
                asm volatile("frrm %0\nfsrm %1" : "=&r"(after) : "r"(previous) : "memory");
                Check(after == 2 && reason == expected && std::memcmp(hostile.state->uniform_buffer_begin + 64, reference.state->uniform_buffer_begin + 64, 16) == 0,
                      "native floating output and caller FRM preservation under RDN");
            }
#endif
        }
    };
    for (auto op : {O::VecFAddScalar32, O::VecFSubScalar32, O::VecFMulScalar32, O::VecFDivScalar32,
                   O::VecFAddScalar64, O::VecFSubScalar64, O::VecFMulScalar64, O::VecFDivScalar64,
                   O::VecFAdd, O::VecFSub, O::VecFMul, O::VecFDiv, O::VecFMinMax, O::VecFUnary,
                   O::VecFCmp, O::VecFCmpMask, O::VecFMulAdd, O::VecFRoundInt, O::VecFCvtPacked}) {
        const auto* filter = std::getenv("SVM_RV64_TEST_OP");
        if (filter && std::strtoul(filter, nullptr, 10) != u32(op)) continue;
        for (u32 bits : {32u, 64u}) {
            if (op >= O::VecFAddScalar32 && op <= O::VecFDivScalar32 && bits != 32) continue;
            if (op >= O::VecFAddScalar64 && op <= O::VecFDivScalar64 && bits != 64) continue;
            if (op == O::VecFCvtPacked && bits != 32) continue;
            const bool configurable_scalar = op == O::VecFMinMax || op == O::VecFUnary || op == O::VecFCmpMask || op == O::VecFRoundInt;
            const u32 modes = op == O::VecFMinMax ? 2 : op == O::VecFUnary ? 3 : op == O::VecFCmpMask ? 16 :
                    op == O::VecFMulAdd || op == O::VecFRoundInt ? 4 : op == O::VecFCvtPacked ? 8 : 1;
            for (u32 scalar = 0; scalar < (configurable_scalar ? 2u : 1u); ++scalar) for (u32 mode = 0; mode < modes; ++mode) {
                IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc600})};
                ir::Assembler as{block.get()};
                auto left = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
                auto right = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
                auto third = as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128);
                ir::Value result;
                if (op >= O::VecFAddScalar32 && op <= O::VecFDivScalar64) result = ir::Value{block->AppendInst(op, left, right)};
                else if (op == O::VecFCvtPacked) result = as.VecFCvtPacked(left, ir::Imm{u64(mode)});
                else if (op == O::VecFMulAdd) result = as.VecFMulAdd(left, right, third, ir::Imm{u64(bits)}, ir::Imm{u64(mode)});
                else if (configurable_scalar) result = ir::Value{block->AppendInst(op, left, right, ir::Imm{u64(bits)}, ir::Imm{u64(mode)}, ir::Imm{u64(scalar)})};
                else if (op == O::VecFCmp) result = as.VecFCmp(left, right, ir::Imm{u64(bits)}, ir::Imm{u64{0}});
                else result = ir::Value{block->AppendInst(op, left, right, ir::Imm{u64(bits)})};
                result = result.SetType(op == O::VecFCmp ? T::U64 : T::V128);
                as.StoreUniform(ir::Uniform{64, result.Type()}, result);
                block->SetTerminal(ir::terminal::ReturnToHost{});
                exercise(block.get(), op, op == O::VecFCvtPacked && (mode == 4 || mode == 5 || mode == 7) ? 64 : bits, mode, scalar);
            }
        }
    }
    for (auto op : {O::VecFCvtIntToFloat, O::VecFCvtFloatToInt, O::VecFCvtScalar}) {
        const auto* filter = std::getenv("SVM_RV64_TEST_OP");
        if (filter && std::strtoul(filter, nullptr, 10) != u32(op)) continue;
        for (u32 input_bits : {32u, 64u}) for (u32 output_bits : {32u, 64u}) for (u32 mode = 0; mode < 2; ++mode) {
            if (op == O::VecFCvtScalar && output_bits == input_bits) continue;
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc610})};
            ir::Assembler as{block.get()};
            auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
            ir::Value result;
            if (op == O::VecFCvtScalar) result = as.VecFCvtScalar(input, ir::Imm{u64(input_bits)});
            else if (op == O::VecFCvtIntToFloat) result = as.VecFCvtIntToFloat(input, ir::Imm{u64(input_bits)}, ir::Imm{u64(output_bits)});
            else result = as.VecFCvtFloatToInt(input, ir::Imm{u64(input_bits)}, ir::Imm{u64(output_bits)}, ir::Imm{u64(mode)});
            result = result.SetType(output_bits == 32 ? T::U32 : T::U64);
            as.StoreUniform(ir::Uniform{64, result.Type()}, result);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            exercise(block.get(), op, input_bits, mode, true);
        }
    }
    std::cout << "PASS native floating arithmetic, NaN payloads, comparisons, FMA, rounding and conversions\n";
}

}  // namespace swift::tests::riscv
