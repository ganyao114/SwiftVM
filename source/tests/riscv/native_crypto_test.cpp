#include "test_support.h"

#include <bit>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::riscv {

using T = ir::ValueType;
using O = ir::OpCode;

void NativeCrypto(bool vector, bool scalar_crypto, bool vector_crypto) {
    rv::HostFeatures features{};
    features.vector = vector; features.zbb = scalar_crypto; features.zbc = scalar_crypto;
    features.zkne = features.zknd = features.zknh = scalar_crypto;
    features.vector_crypto_aes = features.vector_crypto_sha256 = features.vector_crypto_clmul = vector_crypto;
    u64 seed = 0x2187639856173fabULL;
    const auto random = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
    for (auto op : {O::VecAesEnc, O::VecAesEncLast, O::VecAesDec, O::VecAesDecLast,
                   O::VecAesEncFast, O::VecAesEncLastFast, O::VecAesDecFast, O::VecAesDecLastFast,
                   O::VecAesKeygenAssist, O::VecPclMul, O::VecSha256Msg1, O::VecSha256Msg2, O::VecSha256Rnds2}) {
        const u32 modes = op == O::VecAesKeygenAssist ? 12 : op == O::VecPclMul ? 4 : 1;
        for (u32 mode = 0; mode < modes; ++mode) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc900})};
            ir::Assembler as{block.get()};
            auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
            auto b = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
            auto c = as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128);
            ir::Value output;
            if (op == O::VecAesKeygenAssist) output = ir::Value{block->AppendInst(op, a, ir::Imm{u64(mode < 10 ? (1u << mode) : mode == 10 ? 0x1b : 0xff)})};
            else if (op == O::VecPclMul) output = ir::Value{block->AppendInst(op, a, b, ir::Imm{u64((mode & 1) | ((mode & 2) << 3))})};
            else if (op == O::VecSha256Rnds2 || (op >= O::VecAesEncFast && op <= O::VecAesDecLastFast))
                output = ir::Value{block->AppendInst(op, a, b, c)};
            else output = ir::Value{block->AppendInst(op, a, b)};
            as.StoreUniform(ir::Uniform{64, T::V128}, output.SetType(T::V128));
            block->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled cached{block.get(), true, features}, uncached{block.get(), false, features};
            Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0, "every crypto IR emits native instructions");
            if (op >= O::VecAesEnc && op <= O::VecAesDecLastFast && (scalar_crypto || vector_crypto))
                Check(cached.translator.Stats().bytes[size_t(op)] <= 64, "AES hardware round instruction budget");
            if (op == O::VecSha256Rnds2 && vector_crypto)
                Check(cached.translator.Stats().bytes[size_t(op)] <= 32, "SHA256 two-round vector compression instruction budget");
            for (u32 sample = 0; sample < 32; ++sample) {
                StateStorage a_state, b_state, reference;
                for (u32 offset = 0; offset < 48; offset += 8) {
                    const auto value = sample == 0 ? 0 : sample == 1 ? UINT64_MAX : sample == 2 ? u64{1} << (offset % 63) : random();
                    a_state.Put(offset, value); b_state.Put(offset, value); reference.Put(offset, value);
                }
                backend::interp::Interpreter interpreter{*reference.state, block.get()};
                const auto expected = interpreter.Run();
                Check(cached.fn(a_state.state) == expected && uncached.fn(b_state.state) == expected &&
                      a_state.Get(64) == reference.Get(64) && a_state.Get(72) == reference.Get(72) &&
                      b_state.Get(64) == reference.Get(64) && b_state.Get(72) == reference.Get(72),
                      "crypto differential op=" + std::to_string(u32(op)) + " control=" + std::to_string(mode) + " sample=" + std::to_string(sample));
            }
        }
    }
    // FIPS 197 Appendix C: ten connected AES rounds with independently
    // published keys/output, including intermediate values kept in registers.
    constexpr std::array<const char*, 11> keys{
        "000102030405060708090a0b0c0d0e0f", "d6aa74fdd2af72fadaa678f1d6ab76fe",
        "b692cf0b643dbdf1be9bc5006830b3fe", "b6ff744ed2c2c9bf6c590cbf0469bf41",
        "47f7f7bc95353e03f96c32bcfd058dfd", "3caaa3e8a99f9deb50f3af57adf622aa",
        "5e390f7df7a69296a7553dc10aa31f6b", "14f9701ae35fe28c440adf4d4ea9c026",
        "47438735a41c65b9e016baf4aebf7ad2", "549932d1f08557681093ed9cbe2c974e",
        "13111d7fe3944a17f307a78b4d2b30c5"};
    const auto decode = [](const char* hex) {
        std::array<u8, 16> bytes{};
        for (u32 i = 0; i < 16; ++i) {
            const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            bytes[i] = u8(digit(hex[i * 2]) * 16 + digit(hex[i * 2 + 1]));
        }
        return bytes;
    };
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc910})};
    ir::Assembler as{block.get()};
    auto data = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
    auto key = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
    data = as.VecXor(data, key).SetType(T::V128);
    for (u32 round = 1; round <= 10; ++round) {
        key = as.LoadUniform(ir::Uniform{16 + round * 16, T::V128}).SetType(T::V128);
        data = (round == 10 ? as.VecAesEncLast(data, key) : as.VecAesEnc(data, key)).SetType(T::V128);
    }
    as.StoreUniform(ir::Uniform{256, T::V128}, data); block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{block.get(), true, features}; StateStorage state;
    const auto input = decode("00112233445566778899aabbccddeeff");
    std::memcpy(state.state->uniform_buffer_begin, input.data(), 16);
    for (u32 round = 0; round <= 10; ++round) { const auto bytes = decode(keys[round]); std::memcpy(state.state->uniform_buffer_begin + 16 + round * 16, bytes.data(), 16); }
    const auto expected = decode("69c4e0d86a7b0430d8cdb78070b4c55a");
    Check(compiled.fn(state.state) == HaltReason::CallHost && std::memcmp(state.state->uniform_buffer_begin + 256, expected.data(), 16) == 0, "FIPS 197 AES128 known-answer chain");
    Check(compiled.context.ValueStats().loads == 0 && compiled.context.ValueStats().stores == 0, "AES chain retains intermediate state without SSA traffic");
    std::cout << "PASS native AES/SHA256/PCLMUL and FIPS 197 known-answer chain\n";
}

}  // namespace swift::tests::riscv
