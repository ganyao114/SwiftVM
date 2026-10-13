#include "test_support.h"

#include <bit>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
using T = ir::ValueType;
using O = ir::OpCode;

namespace {
u32 ReadFCSR() {
    u32 result{};
    asm volatile("movfcsr2gr %0, $fcsr0" : "=r"(result) :: "memory");
    return result;
}
void WriteFCSR(u32 value) { asm volatile("movgr2fcsr $fcsr0, %0" :: "r"(value) : "memory"); }

u64 HostRoundHelper(u64 value, u64, u64) {
    WriteFCSR((ReadFCSR() & ~u32{0x300}) | 0x300);
    return value + 37;
}
}

void FloatHelpers() {
    // Host rounding is an external caller property. Legacy guest FP is
    // nearest/even, and callbacks must still observe the caller's mode.
    for (u32 mode = 0; mode < 4; ++mode) {
        IntrusivePtr<ir::Block> ties{new ir::Block(ir::Location{0x6380})};
        ir::Assembler as{ties.get()};
        auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
        auto b = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
        auto sum = as.VecFAddScalar32(a, b).SetType(T::V128);
        as.StoreUniform(ir::Uniform{64, T::V128}, sum);
        auto callback = +[](u64, u64, u64) -> u64 { return ReadFCSR() & 0x300; };
        auto snapshot = as.CallHost(callback, as.Zero().SetType(T::U64)).SetType(T::U64);
        as.StoreUniform(ir::Uniform{80, T::U64}, snapshot);
        ties->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{ties.get()};
        StateStorage state; state.Put(0, 0x3f800000); state.Put(16, 0x33800000);
        const auto before = ReadFCSR(), hostile = (before & ~u32{0x300}) | (mode << 8);
        WriteFCSR(hostile);
        const auto halt = compiled.fn(state.state);
        const auto after = ReadFCSR();
        WriteFCSR(before);
        Check(halt == HaltReason::CallHost && state.Get(64) == 0x3f800000,
              "guest 1.0f + 2^-24 rounds nearest/even under every caller rounding mode=" + std::to_string(mode) +
              " halt=" + std::to_string(u32(halt)) + " bits=" + std::to_string(state.Get(64)) +
              " callbackRM=" + std::to_string(state.Get(80)));
        Check(state.Get(80) == (mode << 8) && after == hostile, "FP guard restores caller rounding before callbacks and host return");
    }
    constexpr std::array<u64, 16> edge32{0, 0x80000000, 0x3f800000, 0xbf800000, 0x3fc00000, 0xc0800000,
        0x7f800000, 0xff800000, 0x7fc12345, 0xffc23456, 0x7f812345, 0xff812345,
        1, 0x00800000, 0x7f7fffff, 0x4f000000};
    constexpr std::array<u64, 16> edge64{0, 0x8000000000000000ULL, 0x3ff0000000000000ULL, 0xbff0000000000000ULL,
        0x3ff8000000000000ULL, 0xc010000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
        0x7ff8123456789abcULL, 0xfff823456789abcdULL, 0x7ff0123456789abcULL, 0xfff023456789abcdULL,
        1, 0x0010000000000000ULL, 0x7fefffffffffffffULL, 0x43e0000000000000ULL};
    u64 seed = 0xf98a32096843958eULL;
    const auto random = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
    for (auto op : {O::VecFAddScalar32, O::VecFSubScalar32, O::VecFMulScalar32, O::VecFDivScalar32,
                   O::VecFAddScalar64, O::VecFSubScalar64, O::VecFMulScalar64, O::VecFDivScalar64,
                   O::VecFAdd, O::VecFSub, O::VecFMul, O::VecFDiv, O::VecFMinMax, O::VecFUnary,
                   O::VecFCmp, O::VecFCmpMask, O::VecFMulAdd, O::VecFRoundInt, O::VecFCvtPacked})
    for (u32 bits : {32u, 64u}) {
        if (op >= O::VecFAddScalar32 && op <= O::VecFDivScalar32 && bits != 32) continue;
        if (op >= O::VecFAddScalar64 && op <= O::VecFDivScalar64 && bits != 64) continue;
        if (op == O::VecFCvtPacked && bits != 32) continue;
        const bool scalar_mode = op == O::VecFMinMax || op == O::VecFUnary || op == O::VecFCmpMask || op == O::VecFRoundInt;
        const u32 modes = op == O::VecFMinMax ? 2 : op == O::VecFUnary ? 3 : op == O::VecFCmpMask ? 16 :
                          op == O::VecFMulAdd || op == O::VecFRoundInt ? 4 : op == O::VecFCvtPacked ? 8 : 1;
        for (u32 scalar = 0; scalar < (scalar_mode ? 2u : 1u); ++scalar)
        for (u32 mode = 0; mode < modes; ++mode) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x6400})};
            ir::Assembler as{block.get()};
            auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
            auto b = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
            auto c = as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128);
            ir::Value result;
            if (op >= O::VecFAddScalar32 && op <= O::VecFDivScalar64) result = ir::Value{block->AppendInst(op, a, b)};
            else if (op == O::VecFCvtPacked) result = as.VecFCvtPacked(a, ir::Imm{u64(mode)});
            else if (op == O::VecFMulAdd) result = as.VecFMulAdd(a, b, c, ir::Imm{u64(bits)}, ir::Imm{u64(mode)});
            else if (scalar_mode) result = ir::Value{block->AppendInst(op, a, b, ir::Imm{u64(bits)}, ir::Imm{u64(mode)}, ir::Imm{u64(scalar)})};
            else if (op == O::VecFCmp) result = as.VecFCmp(a, b, ir::Imm{u64(bits)}, ir::Imm{u64{0}});
            else result = ir::Value{block->AppendInst(op, a, b, ir::Imm{u64(bits)})};
            result = result.SetType(op == O::VecFCmp ? T::U64 : T::V128);
            as.StoreUniform(ir::Uniform{64, result.Type()}, result);
            as.StoreUniform(ir::Uniform{80, T::V128}, a);
            as.StoreUniform(ir::Uniform{96, T::V128}, b);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled cached{block.get()}, uncached{block.get(), false};
            Check(cached.translator.Stats().helpers >= 1, "FP semantic fallback remains explicit in emission statistics");
            const bool guarded = (op >= O::VecFAddScalar32 && op <= O::VecFDivScalar64) ||
                                 op == O::VecFAdd || op == O::VecFSub || op == O::VecFMul || op == O::VecFDiv;
            if (guarded) {
                Check(cached.translator.Stats().guarded_native == 1 && uncached.translator.Stats().guarded_native == 1,
                      std::string{ir::GetIRMetaInfo(op).name} + " emits native normal-number FP with an explicit cold fallback");
            }
            const u32 input_bits = op == O::VecFCvtPacked && (mode == 4 || mode == 5 || mode == 7) ? 64 : bits;
            for (u32 sample = 0; sample < 48; ++sample) {
                StateStorage actual, control, reference;
                for (u32 operand = 0; operand < 3; ++operand) {
                    std::array<u64, 2> raw{};
                    for (u32 lane = 0; lane < 128 / input_bits; ++lane) {
                        const u32 selected = sample + operand * 5 + lane * (operand + 1);
                        u64 value{};
                        if (sample < 32) value = input_bits == 32 ? edge32[selected % 16] : edge64[selected % 16];
                        else if (sample < 40) value = random();
                        else {
                            const auto magnitude = s64(random() % 1024) - 512;
                            value = input_bits == 32 ? u64(std::bit_cast<u32>(float(magnitude) / 4.0f)) :
                                                     std::bit_cast<u64>(double(magnitude) / 4.0);
                        }
                        raw[lane * input_bits / 64] |= (input_bits == 32 ? u64(u32(value)) : value) << (lane * input_bits % 64);
                    }
                    for (u32 part = 0; part < 2; ++part) {
                        actual.Put(operand * 16 + part * 8, raw[part]); control.Put(operand * 16 + part * 8, raw[part]);
                        reference.Put(operand * 16 + part * 8, raw[part]);
                    }
                }
                const u32 before = ReadFCSR(), hostile = (before & ~u32{0x300}) | ((sample % 4) << 8);
                WriteFCSR(hostile & ~u32{0x300});
                backend::interp::Interpreter interpreter{*reference.state, block.get()};
                const auto halt = interpreter.Run();
                WriteFCSR(hostile);
                const auto actual_halt = cached.fn(actual.state);
                const auto after_cached = ReadFCSR();
                WriteFCSR(hostile);
                const auto control_halt = uncached.fn(control.state);
                const auto after_control = ReadFCSR();
                WriteFCSR(before);
                const auto detail = std::string{ir::GetIRMetaInfo(op).name} + "/" + std::to_string(bits) + "/" +
                                    std::to_string(mode) + "/" + std::to_string(scalar) + "/" + std::to_string(sample);
                Check(actual_halt == halt && control_halt == halt, detail + " FP helper halt");
                Check(std::memcmp(actual.state->uniform_buffer_begin + 64, reference.state->uniform_buffer_begin + 64, 48) == 0 &&
                      std::memcmp(control.state->uniform_buffer_begin + 64, reference.state->uniform_buffer_begin + 64, 48) == 0,
                      detail + " FP bits/NaN payloads and live vector operands");
                Check(after_cached == hostile && after_control == hostile, detail + " caller FCSR and rounding mode preserved");
                if (sample == 1) Check(CheckABI(cached.fn, actual.state) == 1, detail + " FP helper LP64D");
            }
        }
    }
    IntrusivePtr<ir::Block> callback{new ir::Block(ir::Location{0x6500})};
    ir::Assembler as{callback.get()};
    auto value = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto result = as.CallHost(&HostRoundHelper, value).SetType(T::U64);
    as.StoreUniform(ir::Uniform{8, T::U64}, result);
    callback->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{callback.get()};
    StateStorage state; state.Put(0, 19);
    const u32 before = ReadFCSR(), hostile = (before & ~u32{0x300}) | 0x200;
    WriteFCSR(hostile);
    const auto halt = compiled.fn(state.state);
    const auto after = ReadFCSR();
    WriteFCSR(before);
    Check(halt == HaltReason::CallHost && state.Get(8) == 56 && after == hostile,
          "callback-changing host rounding mode is contained by the generated C ABI frame");
    Check(CheckABI(compiled.fn, state.state) == 1, "FP-sensitive callback preserves callee-saved banks");
    std::cout << "PASS guarded native FP and semantic helpers: NaNs, FMA, conversions, rounding and LP64D/FCSR restoration\n";
}

}  // namespace swift::tests::loongarch64
