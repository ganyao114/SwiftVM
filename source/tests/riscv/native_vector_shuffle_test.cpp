#include "test_support.h"

#include <algorithm>

#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::riscv {

using T = ir::ValueType;
using O = ir::OpCode;

void NativeVectorShuffle(bool vector) {
    rv::HostFeatures features{}; features.vector = vector;
    u64 seed = 0xf642dbc93ec53271ULL;
    for (auto op : {O::VecByteShift, O::VecExtractBytes, O::VecShuffle32, O::VecShuffle32TwoSrc,
                   O::VecShuffle32Indexed, O::VecShuffle16, O::VecZip, O::VecUnzip, O::VecDupPairs32,
                   O::VecMovMask, O::VecTableLookup8, O::VecAbsDiffSum8, O::VecMadd16}) {
        for (u32 bits : {8u, 16u, 32u, 64u}) {
            if (op != O::VecZip && op != O::VecUnzip && op != O::VecMovMask && bits != 8) continue;
            if (op == O::VecMovMask && bits == 16) continue;
            const u32 controls = op == O::VecShuffle32 || op == O::VecShuffle32TwoSrc || op == O::VecShuffle16 ? 256 :
                    op == O::VecByteShift || op == O::VecExtractBytes ? 15 : 1;
            for (u32 mode = 0; mode < 2; ++mode) for (u32 first = 0; first < controls; first += 32) {
                IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc400})};
                ir::Assembler as{block.get()};
                auto left = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
                auto right = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
                const u32 count = std::min(32u, controls - first);
                for (u32 variant = 0; variant < count; ++variant) {
                const u32 control = first + variant;
                ir::Value result;
                switch (op) {
                    case O::VecByteShift: result = as.VecByteShift(left, right, ir::Imm{u64(control + 1)}, ir::Imm{u64(mode)}); break;
                    case O::VecExtractBytes: result = as.VecExtractBytes(left, right, ir::Imm{u64(control + 1)}); break;
                    case O::VecShuffle32: result = as.VecShuffle32(left, ir::Imm{u64(control)}); break;
                    case O::VecShuffle32TwoSrc: result = as.VecShuffle32TwoSrc(left, right, ir::Imm{u64(control)}); break;
                    case O::VecShuffle16: result = as.VecShuffle16(left, ir::Imm{u64(control)}, ir::Imm{u64(mode)}); break;
                    case O::VecZip: case O::VecUnzip:
                        result = ir::Value{block->AppendInst(op, left, right, ir::Imm{u64(bits)}, ir::Imm{u64(mode)})}; break;
                    case O::VecDupPairs32: result = as.VecDupPairs32(left, ir::Imm{u64(mode)}); break;
                    case O::VecMovMask: result = as.VecMovMask(left, ir::Imm{u64(bits)}); break;
                    default: result = ir::Value{block->AppendInst(op, left, right)}; break;
                }
                result = result.SetType(op == O::VecMovMask ? T::U64 : T::V128);
                as.StoreUniform(ir::Uniform{128 + variant * 16, result.Type()}, result);
                }
                // Keep both inputs live while executing the result operation.
                as.StoreUniform(ir::Uniform{64, T::V128}, left);
                as.StoreUniform(ir::Uniform{80, T::V128}, right);
                block->SetTerminal(ir::terminal::ReturnToHost{});
                Compiled cached{block.get(), true, features}, uncached{block.get(), false, features};
                Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0,
                      "shuffle/reduction operations must not dispatch to the interpreter");
                if (vector) Check(cached.translator.Stats().bytes[size_t(op)] <= 112 * count,
                                  "RVV shuffle/reduction has a bounded vector instruction sequence");
                for (u32 sample = 0; sample < 12; ++sample) {
                    StateStorage a, b, reference;
                    for (u32 offset = 0; offset < 32; offset += 8) {
                        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                        const u64 value = sample == 0 ? 0 : sample == 1 ? UINT64_MAX :
                                sample == 2 ? 0x8000800080008000ULL : sample == 3 ? 0x100f080700010f0eULL : seed;
                        a.Put(offset, value); b.Put(offset, value); reference.Put(offset, value);
                    }
                    backend::interp::Interpreter interpreter{*reference.state, block.get()};
                    const auto expected = interpreter.Run();
                    Check(cached.fn(a.state) == expected && uncached.fn(b.state) == expected,
                          "native shuffle/reduction halt");
                    Check(std::memcmp(a.state->uniform_buffer_begin + 32, reference.state->uniform_buffer_begin + 32, 96 + count * 16) == 0 &&
                          std::memcmp(b.state->uniform_buffer_begin + 32, reference.state->uniform_buffer_begin + 32, 96 + count * 16) == 0,
                          "shuffle differential: op=" + std::to_string(u32(op)) + " bits=" + std::to_string(bits) +
                          " control=" + std::to_string(first) + " mode=" + std::to_string(mode) +
                          " sample=" + std::to_string(sample));
                }
            }
        }
    }
    std::cout << "PASS native vector permutations, lookup masks, SAD and multiply-add\n";
}

}  // namespace swift::tests::riscv
