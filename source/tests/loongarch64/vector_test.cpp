#include "test_support.h"

#include <algorithm>
#include <bit>
#include <optional>
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
using T = ir::ValueType;
using O = ir::OpCode;
namespace {

void CheckProgram(ir::Block* block, la::HostFeatures features, bool native, u32 samples,
                  u32 output_size = 512, std::optional<u64> count_override = {}) {
    std::string operation;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != O::LoadUniform && inst.GetOp() != O::StoreUniform)
            operation += std::string{ir::GetIRMetaInfo(inst.GetOp()).name} + ",";
    }
    Compiled cached{block, true, features}, uncached{block, false, features};
    if (native)
        Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0,
              "qualified packed integer path uses native emission");
    u64 seed = 0x39d402a5c195378bULL;
    for (u32 sample = 0; sample < samples; ++sample) {
        StateStorage a, b, reference;
        for (u32 offset = 0; offset < 48; offset += 8) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            const u64 value = sample == 0 ? 0 : sample == 1 ? UINT64_MAX :
                sample == 2 ? 0x8000800080008000ULL : sample == 3 ? 0x7fff7fff7fff7fffULL : seed;
            a.Put(offset, value); b.Put(offset, value); reference.Put(offset, value);
        }
        if (count_override) {
            a.Put(32, *count_override); b.Put(32, *count_override); reference.Put(32, *count_override);
        }
        a.state->host_cpu_flags = b.state->host_cpu_flags = reference.state->host_cpu_flags = std::rotl(seed, 31);
        backend::interp::Interpreter interpreter{*reference.state, block};
        const auto halt = interpreter.Run();
        const auto cached_halt = cached.fn(a.state), uncached_halt = uncached.fn(b.state);
        Check(cached_halt == halt && uncached_halt == halt,
              "vector differential halt ops=" + operation + " expected=" + std::to_string(u32(halt)) +
              " cached=" + std::to_string(u32(cached_halt)) + " uncached=" + std::to_string(u32(uncached_halt)));
        Check(std::memcmp(a.state->uniform_buffer_begin + 48, reference.state->uniform_buffer_begin + 48, output_size) == 0 &&
              std::memcmp(b.state->uniform_buffer_begin + 48, reference.state->uniform_buffer_begin + 48, output_size) == 0,
              "vector differential ops=" + operation + " sample=" + std::to_string(sample) + " cached=" + std::to_string(a.Get(64)) +
              ":" + std::to_string(a.Get(72)) + " uncached=" + std::to_string(b.Get(64)) +
              ":" + std::to_string(b.Get(72)) + " lsx=" + std::to_string(features.lsx) +
              " expected=" + std::to_string(reference.Get(64)) +
              ":" + std::to_string(reference.Get(72)));
        Check(a.state->host_cpu_flags == reference.state->host_cpu_flags && b.state->host_cpu_flags == reference.state->host_cpu_flags,
              "packed integer operations preserve guest flags without SaveFlags");
    }
}

u64 Saturated(u64 a, u64 b, u32 bits, bool sign, bool subtract) {
    const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
    a &= mask; b &= mask;
    if (!sign) {
        if (subtract) return a < b ? 0 : a - b;
        const auto sum = static_cast<unsigned __int128>(a) + b;
        return sum > mask ? mask : u64(sum);
    }
    const __int128 sign_bit = __int128{1} << (bits - 1), modulus = __int128{1} << bits;
    const __int128 left = (a & u64(sign_bit)) ? __int128(a) - modulus : __int128(a);
    const __int128 right = (b & u64(sign_bit)) ? __int128(b) - modulus : __int128(b);
    const __int128 result = subtract ? left - right : left + right;
    return u64(std::clamp(result, -sign_bit, sign_bit - 1)) & mask;
}

void ModuleFeaturePolicy() {
    backend::AddressSpace space{TestConfig()};
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x6400})};
    ir::Assembler as{block.get()};
    auto left = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
    auto right = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
    auto seed = as.LoadUniform(ir::Uniform{32, T::U64}).SetType(T::U64);
    auto data = as.LoadUniform(ir::Uniform{40, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{64, T::V128}, as.Vec4Add(left, right).SetType(T::V128));
    as.StoreUniform(ir::Uniform{80, T::U32}, as.Crc32c(seed, data, ir::Imm{u64{64}}).SetType(T::U32));
    block->SetTerminal(ir::terminal::ReturnToHost{});
    for (bool allow : {false, true}) {
        backend::ModuleConfig policy;
        for (auto id : {FeatureId::loongarch_lsx, FeatureId::loongarch_lasx, FeatureId::loongarch_crc32})
            policy.feature_overrides.Set(id, allow);
        auto module = std::make_shared<backend::Module>(space, 0, TestConfig().loc_end, policy);
        // Explicitly allowing a capability still requires host admission.
        const auto features = allow ? la::HostFeatures{} : la::HostFeatures::Detect();
        for (bool cache_values : {false, true}) {
            Compiled compiled{block.get(), cache_values, features, module.get()};
            const auto effective = compiled.context.Features();
            Check(!effective.lsx && !effective.lasx && !effective.crc32,
                  allow ? "module policy cannot enable absent host SIMD/CRC" :
                          "module overrides disable admitted SIMD/CRC");
            Check(compiled.translator.Stats().helpers == 1,
                  "disabled LSX uses the explicit vector helper; CRC stays in scalar emission");
            u64 random = 0x43aaefa177040894ULL;
            for (u32 sample = 0; sample < 64; ++sample) {
                StateStorage actual, reference;
                for (u32 offset = 0; offset < 48; offset += 8) {
                    random ^= random << 13; random ^= random >> 7; random ^= random << 17;
                    actual.Put(offset, random); reference.Put(offset, random);
                }
                actual.state->host_cpu_flags = reference.state->host_cpu_flags = 0x91a5;
                backend::interp::Interpreter interpreter{*reference.state, block.get()};
                const auto expected_halt = interpreter.Run();
                Check(compiled.fn(actual.state) == expected_halt &&
                      std::memcmp(actual.state->uniform_buffer_begin + 64,
                                  reference.state->uniform_buffer_begin + 64, 24) == 0 &&
                      actual.state->host_cpu_flags == reference.state->host_cpu_flags,
                      "module SIMD/CRC software path matches interpreter");
                Check(CheckABI(compiled.fn, actual.state) == 1,
                      "module feature fallback preserves the complete LP64D ABI");
            }
        }
    }
    std::cout << "PASS module SIMD/CRC overrides and absent-capability admission\n";
}

}  // namespace

void VectorDifferential() {
    ModuleFeaturePolicy();
    const auto detected = la::HostFeatures::Detect();
    for (bool lsx : {false, true}) {
        if (lsx && !detected.lsx) continue;
        la::HostFeatures features = detected; features.lsx = lsx; features.lasx = false;
        for (auto op : {O::Vec4And, O::Vec4Or, O::VecAnd, O::VecOr, O::VecXor, O::VecAndNot,
                       O::Vec4Add, O::Vec4Sub, O::Vec4Mul, O::VecAdd, O::VecSub, O::VecMul,
                       O::VecCmpEq, O::VecCmpGt, O::VecMin, O::VecMax, O::VecAvg}) {
            const bool fixed32 = op == O::Vec4Add || op == O::Vec4Sub || op == O::Vec4Mul;
            const bool logic = op == O::Vec4And || op == O::Vec4Or || op == O::VecAnd ||
                op == O::VecOr || op == O::VecXor || op == O::VecAndNot;
            for (u32 bits : {8u, 16u, 32u, 64u}) {
                if (fixed32 && bits != 32 || logic && bits != 8 || op == O::VecAvg && bits > 16) continue;
                const u32 modes = op == O::VecMin || op == O::VecMax ? 2 : 1;
                for (u32 mode = 0; mode < modes; ++mode) {
                    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x6000})};
                    ir::Assembler as{block.get()};
                    auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
                    auto b = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
                    ir::Value result;
                    if (fixed32 || logic) result = ir::Value{block->AppendInst(op, a, b)};
                    else if (op == O::VecMin || op == O::VecMax)
                        result = ir::Value{block->AppendInst(op, a, b, ir::Imm{u64(bits)}, ir::Imm{u64(mode)})};
                    else result = ir::Value{block->AppendInst(op, a, b, ir::Imm{u64(bits)})};
                    as.StoreUniform(ir::Uniform{64, T::V128}, result.SetType(T::V128));
                    as.StoreUniform(ir::Uniform{80, T::V128}, a);
                    as.StoreUniform(ir::Uniform{96, T::V128}, b);
                    block->SetTerminal(ir::terminal::ReturnToHost{});
                    CheckProgram(block.get(), features, lsx || logic, 64, 64);
                }
            }
        }
        for (auto op : {O::VecShiftLeftImm, O::VecShiftRightImm, O::VecShiftRightArithmeticImm})
        for (u32 bits : {8u, 16u, 32u, 64u}) {
            for (u64 count : {u64{0}, u64{1}, u64(bits - 1), u64(bits), u64(bits + 1), u64{63}, u64{64}, UINT64_MAX}) {
                IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x6100})};
                ir::Assembler as{block.get()};
                auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
                auto result = ir::Value{block->AppendInst(op, a, ir::Imm{count}, ir::Imm{u64(bits)})}.SetType(T::V128);
                as.StoreUniform(ir::Uniform{64, T::V128}, result);
                block->SetTerminal(ir::terminal::ReturnToHost{});
                CheckProgram(block.get(), features, lsx, 16, 32);
            }
        }
        for (auto op : {O::VecShiftLeft, O::VecShiftRight, O::VecShiftRightArithmetic})
        for (u32 bits : {8u, 16u, 32u, 64u})
        for (u64 count : {u64{0}, u64{1}, u64(bits - 1), u64(bits), u64(bits + 1), UINT64_MAX}) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x6180})};
            ir::Assembler as{block.get()};
            auto value = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
            auto amount = as.LoadUniform(ir::Uniform{32, T::U64}).SetType(T::U64);
            auto result = ir::Value{block->AppendInst(op, value, amount, ir::Imm{u64(bits)})}.SetType(T::V128);
            as.StoreUniform(ir::Uniform{64, T::V128}, result);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            CheckProgram(block.get(), features, lsx, 16, 32, count);
        }
        for (auto op : {O::VecSatAdd, O::VecSatSub})
        for (u32 bits : {8u, 16u, 32u, 64u})
        for (bool sign : {false, true}) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x61a0})};
            ir::Assembler as{block.get()};
            auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
            auto b = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
            auto result = ir::Value{block->AppendInst(op, a, b, ir::Imm{u64(bits)}, ir::Imm{u64(sign)})}.SetType(T::V128);
            as.StoreUniform(ir::Uniform{64, T::V128}, result);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled cached{block.get(), true, features}, uncached{block.get(), false, features};
            if (lsx) Check(cached.translator.Stats().helpers == 0, "LSX saturation lowers natively");
            const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
            const std::array<u64, 6> values{0, 1, mask, mask >> 1, mask ^ (mask >> 1), (mask >> 1) - 1};
            for (u64 left : values) for (u64 right : values) {
                StateStorage a, b;
                std::array<u8, 16> expected{};
                const u64 wanted = Saturated(left, right, bits, sign, op == O::VecSatSub);
                for (u32 offset = 0; offset < 16; offset += bits / 8) {
                    std::memcpy(a.state->uniform_buffer_begin + offset, &left, bits / 8);
                    std::memcpy(a.state->uniform_buffer_begin + 16 + offset, &right, bits / 8);
                    std::memcpy(expected.data() + offset, &wanted, bits / 8);
                }
                std::memcpy(b.state->uniform_buffer_begin, a.state->uniform_buffer_begin, 32);
                Check(cached.fn(a.state) == HaltReason::CallHost && uncached.fn(b.state) == HaltReason::CallHost,
                      "saturating vector halt");
                Check(std::memcmp(expected.data(), a.state->uniform_buffer_begin + 64, 16) == 0 &&
                      std::memcmp(expected.data(), b.state->uniform_buffer_begin + 64, 16) == 0,
                      "independent 128-bit saturation arithmetic, width=" + std::to_string(bits));
            }
        }
        for (auto op : {O::VecMulHigh16, O::VecMulWiden, O::VecTableLookup8, O::VecShuffle32Indexed}) {
            for (u32 bits : {8u, 16u, 32u}) {
                if (op != O::VecMulWiden && bits != 8) continue;
                const u32 modes = op == O::VecMulHigh16 || op == O::VecMulWiden ? 2 : 1;
                for (u32 mode = 0; mode < modes; ++mode) {
                    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x61c0})};
                    ir::Assembler as{block.get()};
                    auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
                    auto b = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
                    ir::Value result;
                    if (op == O::VecMulHigh16) result = as.VecMulHigh16(a, b, ir::Imm{u64(mode)});
                    else if (op == O::VecMulWiden) result = as.VecMulWiden(a, b, ir::Imm{u64(bits)}, ir::Imm{u64(mode)});
                    else result = ir::Value{block->AppendInst(op, a, b)};
                    as.StoreUniform(ir::Uniform{64, T::V128}, result.SetType(T::V128));
                    block->SetTerminal(ir::terminal::ReturnToHost{});
                    CheckProgram(block.get(), features, lsx || op == O::VecTableLookup8 || op == O::VecShuffle32Indexed, 64, 32);
                }
            }
        }
        for (auto op : {O::VecShuffle32, O::VecShuffle32TwoSrc, O::VecShuffle16, O::VecZip, O::VecUnzip,
                       O::VecByteShift, O::VecExtractBytes, O::VecExtract64, O::VecExtract16, O::VecInsert16,
                       O::VecMovMask, O::VecDupPairs32, O::VecDup64}) {
            for (u32 bits : {8u, 16u, 32u, 64u}) {
                if (op != O::VecZip && op != O::VecUnzip && op != O::VecMovMask && bits != 8) continue;
                if (op == O::VecMovMask && bits == 16) continue;
                const u32 controls = op == O::VecShuffle32 || op == O::VecShuffle32TwoSrc || op == O::VecShuffle16 ? 256 :
                    op == O::VecByteShift || op == O::VecExtractBytes ? 16 : op == O::VecExtract16 || op == O::VecInsert16 ? 8 :
                    op == O::VecExtract64 ? 2 : 1;
                for (u32 mode = 0; mode < 2; ++mode)
                for (u32 first = 0; first < controls; first += 16) {
                    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x6200})};
                    ir::Assembler as{block.get()};
                    auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
                    auto b = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
                    auto scalar = as.LoadUniform(ir::Uniform{32, T::U64}).SetType(T::U64);
                    const u32 count = std::min(16u, controls - first);
                    for (u32 variant = 0; variant < count; ++variant) {
                        const u64 control = first + variant;
                        if ((op == O::VecByteShift || op == O::VecExtractBytes) && control == 0) continue;
                        ir::Value result;
                        switch (op) {
                            case O::VecShuffle32: result = as.VecShuffle32(a, ir::Imm{control}); break;
                            case O::VecShuffle32TwoSrc: result = as.VecShuffle32TwoSrc(a, b, ir::Imm{control}); break;
                            case O::VecShuffle16: result = as.VecShuffle16(a, ir::Imm{control}, ir::Imm{u64(mode)}); break;
                            case O::VecZip: case O::VecUnzip:
                                result = ir::Value{block->AppendInst(op, a, b, ir::Imm{u64(bits)}, ir::Imm{u64(mode)})}; break;
                            case O::VecByteShift: result = as.VecByteShift(a, b, ir::Imm{control}, ir::Imm{u64(mode)}); break;
                            case O::VecExtractBytes: result = as.VecExtractBytes(a, b, ir::Imm{control}); break;
                            case O::VecExtract64: result = as.VecExtract64(a, ir::Imm{control}); break;
                            case O::VecExtract16: result = as.VecExtract16(a, ir::Imm{control}); break;
                            case O::VecInsert16: result = as.VecInsert16(a, scalar, ir::Imm{control}); break;
                            case O::VecMovMask: result = as.VecMovMask(a, ir::Imm{u64(bits)}); break;
                            case O::VecDupPairs32: result = as.VecDupPairs32(a, ir::Imm{u64(mode)}); break;
                            case O::VecDup64: result = as.VecDup64(a); break;
                            default: throw std::runtime_error("invalid permutation fixture");
                        }
                        result = result.SetType(op == O::VecMovMask || op == O::VecExtract64 || op == O::VecExtract16 ? T::U64 : T::V128);
                        as.StoreUniform(ir::Uniform{64 + variant * 16, result.Type()}, result);
                    }
                    block->SetTerminal(ir::terminal::ReturnToHost{});
                    CheckProgram(block.get(), features, true, 12, 32 + count * 16);
                }
            }
        }
        // A typed scalar view of a vector retains raw bits for a later cast;
        // scalar arithmetic still consumes only that view's width.
        IntrusivePtr<ir::Block> bitcasts{new ir::Block(ir::Location{0x6300})};
        ir::Assembler as{bitcasts.get()};
        auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
        auto narrow = as.BitCast(a).SetType(T::U8);
        auto test = as.TestNotZero(narrow).SetType(T::U8);
        auto restored = as.BitCast(narrow).SetType(T::V128);
        as.StoreUniform(ir::Uniform{64, T::V128}, restored);
        as.StoreUniform(ir::Uniform{80, T::U8}, test);
        auto b = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
        auto condition = as.LoadUniform(ir::Uniform{32, T::U64}).SetType(T::U64);
        auto left = as.BitCast(a).SetType(T::V64), right = as.BitCast(b).SetType(T::V64);
        auto selected = as.Select(ir::BOOL{condition}, left, right).SetType(T::V64);
        as.StoreUniform(ir::Uniform{96, T::V128}, as.BitCast(selected).SetType(T::V128));
        bitcasts->SetTerminal(ir::terminal::ReturnToHost{});
        CheckProgram(bitcasts.get(), features, true, 64, 64);
        std::cout << "PASS packed integer and permutation differential, LSX=" << lsx << '\n';
    }
    if (!detected.lsx) std::cout << "LSX hardware execution unavailable; baseline semantic path qualified\n";
}

}  // namespace swift::tests::loongarch64
