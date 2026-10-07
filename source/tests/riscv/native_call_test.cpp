#include "test_support.h"

#include "runtime/frontend/ir_assembler.h"
#include "runtime/common/host_pair_result.h"

namespace swift::tests::riscv {

using T = ir::ValueType;
using O = ir::OpCode;

namespace {

u64 ObserveArguments(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h) {
    return a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13 + g * 17 + h * 19;
}

HostPairResult PairProbe(u64 a, u64 b, u64 c) { return {a ^ c, b + c}; }

u64 ObserveRounding(u64, u64, u64, u64, u64, u64, u64, u64) {
#if defined(__riscv) && __riscv_xlen == 64
    u64 mode{}; asm volatile("frrm %0" : "=r"(mode)); return mode;
#else
    return 0;
#endif
}

}  // namespace

#if defined(__riscv) && __riscv_xlen == 64
extern "C" u64 SwiftRiscvVectorRange(u64, u64, u64, u64, u64, u64, u64, u64);
#endif

void NativeCalls(bool vector) {
    rv::HostFeatures features{}; features.vector = vector;
    constexpr std::array<u64, 8> weights{1, 3, 5, 7, 11, 13, 17, 19};
    for (auto op : {O::CallLambda, O::CallLocation, O::CallDynamic}) for (u32 count = 0; count <= 8; ++count) {
        if (op == O::CallLambda && count > 3) continue;
        for (bool dynamic : {false, true}) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc700})};
            ir::Assembler as{block.get()};
            auto live = as.LoadImm(ir::Imm{u64{0x79548ed21678fb99}}).SetType(T::U64);
            std::array<ir::Value, 8> inputs;
            ir::Params params;
            for (u32 i = 0; i < count; ++i) {
                inputs[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
                params.Push(inputs[i]);
            }
            const auto target = dynamic ? ir::Lambda{as.LoadUniform(ir::Uniform{80, T::U64}).SetType(T::U64)}
                                        : ir::Lambda{ir::Imm{reinterpret_cast<u64>(&ObserveArguments)}};
            ir::Value result;
            if (op == O::CallLambda) {
                switch (count) {
                    case 0: result = ir::Value{block->AppendInst(op, target)}; break;
                    case 1: result = ir::Value{block->AppendInst(op, target, inputs[0])}; break;
                    case 2: result = ir::Value{block->AppendInst(op, target, inputs[0], inputs[1])}; break;
                    default: result = ir::Value{block->AppendInst(op, target, inputs[0], inputs[1], inputs[2])}; break;
                }
                params.Destroy();
            } else result = ir::Value{block->AppendInst(op, target, params)};
            result = result.SetType(T::U64);
            auto retained = as.Xor(result, ir::Operand{live}).SetType(T::U64);
            as.StoreUniform(ir::Uniform{96, T::U64}, result);
            as.StoreUniform(ir::Uniform{104, T::U64}, retained);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled cached{block.get(), true, features}, uncached{block.get(), false, features};
            Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0,
                  "host calls use a specialized ABI boundary without IR dispatch");
            Check(cached.translator.Stats().saved_gprs == 9 && cached.translator.Stats().saved_fprs == 12 &&
                  cached.translator.Stats().frame_size == rv::kBlockSavedFrameSize,
                  "a host-call frame preserves the full set needed for abandoned-callee fault recovery");
            for (u64 seed : {u64{0}, u64{1}, u64{0x9573ace287bd9f31}, UINT64_MAX}) {
                StateStorage a, b; u64 expected{};
                for (u32 i = 0; i < count; ++i) { const auto value = seed + 17 * i; a.Put(i * 8, value); b.Put(i * 8, value); expected += value * weights[i]; }
                a.Put(80, reinterpret_cast<u64>(&ObserveArguments)); b.Put(80, a.Get(80));
                Check(cached.fn(a.state) == HaltReason::CallHost && uncached.fn(b.state) == HaltReason::CallHost &&
                      a.Get(96) == expected && b.Get(96) == expected && a.Get(104) == (expected ^ 0x79548ed21678fb99ULL) && b.Get(104) == a.Get(104),
                      "zero through eight host arguments, stack arguments, dynamic targets and live values");
#if defined(__riscv) && __riscv_xlen == 64
                a.state->halt_reason = HaltReason::None;
                Check(SwiftRiscvCheckABI(cached.fn, a.state) == 1, "native host call preserves the complete LP64D register set");
#endif
            }
        }
    }
    for (bool sign : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc710})};
        ir::Assembler as{block.get()};
        auto high = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto low = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        auto divisor = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        auto quotient = as.Div128(high, low, divisor, ir::Imm{u64(sign)});
        auto remainder = as.Div128Remainder(quotient);
        as.StoreUniform(ir::Uniform{32, T::U64}, quotient); as.StoreUniform(ir::Uniform{40, T::U64}, remainder);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled cached{block.get(), true, features}, uncached{block.get(), false, features};
        Check(cached.translator.Stats().helpers == 0, "wide division has a native narrow fast path and direct wide kernel");
        for (u64 hi : {u64{0}, u64{1}, u64{1} << 63, UINT64_MAX}) for (u64 lo : {u64{0}, u64{1}, u64{1} << 63, UINT64_MAX})
            for (u64 d : {u64{0}, u64{1}, u64{3}, u64{1} << 63, UINT64_MAX}) {
                StateStorage a, b; a.Put(0, hi); a.Put(8, lo); a.Put(16, d); b.Put(0, hi); b.Put(8, lo); b.Put(16, d);
                u64 q{}, r{};
                if (d && !(sign && d == UINT64_MAX && hi == (u64{1} << 63) && lo == 0)) {
                    const auto raw = (static_cast<unsigned __int128>(hi) << 64) | lo;
                    if (sign) { q = u64(static_cast<__int128>(raw) / static_cast<s64>(d)); r = u64(static_cast<__int128>(raw) % static_cast<s64>(d)); }
                    else { q = u64(raw / d); r = u64(raw % d); }
                }
                Check(cached.fn(a.state) == HaltReason::CallHost && uncached.fn(b.state) == HaltReason::CallHost &&
                      a.Get(32) == q && a.Get(40) == r && b.Get(32) == q && b.Get(40) == r,
                      "signed/unsigned wide division, narrow paths, divide by zero and overflow");
            }
    }
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc720})};
    ir::Assembler as{block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto b = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto first = as.Cpuid(a, b, ir::Imm{u64{0x9876543210}}, ir::Imm{reinterpret_cast<u64>(&PairProbe)});
    auto second = as.CpuidUpper(first);
    as.StoreUniform(ir::Uniform{16, T::U64}, first); as.StoreUniform(ir::Uniform{24, T::U64}, second);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{block.get(), true, features}; StateStorage state; state.Put(0, 0x872641f); state.Put(8, UINT64_MAX);
    Check(compiled.translator.Stats().helpers == 0 && compiled.fn(state.state) == HaltReason::CallHost &&
          state.Get(16) == (state.Get(0) ^ 0x9876543210ULL) && state.Get(24) == state.Get(8) + 0x9876543210ULL,
          "CPUID pair-return ABI and deferred upper pseudo");
    if (vector) {
        IntrusivePtr<ir::Block> boundary{new ir::Block(ir::Location{0xc730})};
        ir::Assembler emit{boundary.get()};
        auto left = emit.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
        auto right = emit.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
        auto live = emit.VecFAdd(left, right, ir::Imm{u64{32}}).SetType(T::V128);
#if defined(__riscv) && __riscv_xlen == 64
        (void)emit.CallHost(&SwiftRiscvVectorRange).SetType(T::U64);
#endif
        auto mode = emit.CallHost(&ObserveRounding).SetType(T::U64);
        auto after = emit.VecFMul(live, right, ir::Imm{u64{32}}).SetType(T::V128);
        emit.StoreUniform(ir::Uniform{32, T::V128}, after); emit.StoreUniform(ir::Uniform{48, T::U64}, mode);
        boundary->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled call{boundary.get(), true, features}; StateStorage input;
        input.Put(0, 0x3f8000003f800000ULL); input.Put(8, input.Get(0));
        input.Put(16, 0x4000000040000000ULL); input.Put(24, input.Get(16));
#if defined(__riscv) && __riscv_xlen == 64
        u64 previous{}, current{}; asm volatile("fsrmi %0, 2" : "=r"(previous) :: "memory");
        const auto reason = call.fn(input.state);
        asm volatile("frrm %0\nfsrm %1" : "=&r"(current) : "r"(previous) : "memory");
        Check(reason == HaltReason::CallHost && current == 2 && input.Get(48) == 2 &&
              input.Get(32) == 0x40c0000040c00000ULL && input.Get(40) == input.Get(32),
              "live FP vectors survive complete caller-saved vector clobber; host callbacks observe caller FRM");
#endif
    }
    std::cout << "PASS native host-call ABI, wide division and paired results\n";
}

}  // namespace swift::tests::riscv
