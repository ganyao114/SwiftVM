#include "test_support.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/backend/interp/interpreter.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace swift::tests::riscv {
using T = ir::ValueType;
using O = ir::OpCode;
using s128 = __int128_t;
namespace {
using Bytes = std::array<u8, 32>;
u64 Lane(const Bytes& bytes, u32 index, u32 width) {
    u64 result{}; std::memcpy(&result, bytes.data() + index * width, width); return result;
}
void Lane(Bytes& bytes, u32 index, u32 width, u64 value) { std::memcpy(bytes.data() + index * width, &value, width); }
struct WideRange { u32 calls{}; u64 size{}; };
bool WideRangeCheck(void* opaque, u64, u64 size) {
    auto& range = *static_cast<WideRange*>(opaque); ++range.calls; range.size = size; return true;
}
struct Fixture {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf000})};
    ir::Assembler as{block.get()};
    ir::Value left = as.LoadUniform(ir::Uniform{0, T::V256}).SetType(T::V256);
    ir::Value right = as.LoadUniform(ir::Uniform{32, T::V256}).SetType(T::V256);
    void Run(ir::Value result, const Bytes& a, const Bytes& b, const Bytes& expected, bool vector) {
        as.StoreUniform(ir::Uniform{96, T::V256}, result.SetType(T::V256));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        rv::HostFeatures features{}; features.vector = vector;
        for (bool cache : {true, false}) {
            Compiled compiled{block.get(), cache, features};
            Check(compiled.translator.Stats().helpers == 0, "V256 lowering has no interpreter dispatch");
            StateStorage state;
            std::memcpy(state.state->uniform_buffer_begin, a.data(), 32);
            std::memcpy(state.state->uniform_buffer_begin + 32, b.data(), 32);
            Check(compiled.fn(state.state) == HaltReason::CallHost, "native V256 halt");
            Check(std::memcmp(state.state->uniform_buffer_begin + 96, expected.data(), 32) == 0,
                  "native V256 full-width result: opcode=" + std::to_string(u32(result.Def()->GetOp())) + " cache=" + std::to_string(cache));
#if defined(__riscv) && __riscv_xlen == 64
            Check(SwiftRiscvCheckABI(compiled.fn, state.state) == 1, "native V256 LP64D");
#endif
        }
    }
};
} // namespace

void NativeWide(bool vector) {
#if defined(__riscv) && __riscv_xlen == 64
    {
        Fixture f; f.as.StoreUniform(ir::Uniform{96, T::V256}, f.left);
        f.block->SetTerminal(ir::terminal::ReturnToHost{}); StateStorage state;
        std::array<u8, 32> before{}; before.fill(0xcc);
        std::memcpy(state.state->uniform_buffer_begin + 96, before.data(), 32);
        Check(backend::interp::Interpreter{*state.state, f.block.get()}.Run() == HaltReason::IllegalCode &&
              std::memcmp(state.state->uniform_buffer_begin + 96, before.data(), 32) == 0,
              "portable interpreter rejects V256 before touching its 128-bit homes");
    }
    Bytes a{}, b{};
    for (u32 i = 0; i < 32; ++i) { a[i] = u8(i * 19 + 7); b[i] = u8(i * 11 + 129); }
    for (auto op : {O::VecAdd, O::VecSub, O::VecAnd, O::VecOr, O::VecXor, O::VecAndNot, O::VecMul, O::VecCmpEq}) {
        for (u32 bits : {8u, 16u, 32u, 64u}) {
            Fixture f; ir::Value result;
            if (op == O::VecAnd || op == O::VecOr || op == O::VecXor || op == O::VecAndNot)
                result = ir::Value{f.block->AppendInst(op, f.left, f.right)};
            else result = ir::Value{f.block->AppendInst(op, f.left, f.right, ir::Imm{u64(bits)})};
            Bytes expected{}; const u32 width = bits / 8;
            for (u32 lane = 0; lane < 32 / width; ++lane) {
                const auto x = Lane(a, lane, width), y = Lane(b, lane, width);
                u64 value{};
                if (op == O::VecAdd) value = x + y;
                if (op == O::VecSub) value = x - y;
                if (op == O::VecAnd) value = x & y;
                if (op == O::VecOr) value = x | y;
                if (op == O::VecXor) value = x ^ y;
                if (op == O::VecAndNot) value = x & ~y;
                if (op == O::VecMul) value = x * y;
                if (op == O::VecCmpEq) value = x == y ? UINT64_MAX : 0;
                Lane(expected, lane, width, value);
            }
            f.Run(result, a, b, expected, vector);
        }
    }
    for (u32 count : {0u, 1u, 7u, 15u, 16u, 17u, 24u, 31u, 32u}) {
        for (bool left : {false, true}) {
            Fixture f; Bytes expected{};
            auto result = f.as.VecByteShift(f.left, f.right, ir::Imm{u64(count)}, ir::Imm{u64(left)});
            for (u32 i = 0; i < 32; ++i) {
                if (left && i >= count) expected[i] = a[i - count];
                if (!left && i + count < 32) expected[i] = a[i + count];
            }
            f.Run(result, a, b, expected, vector);
        }
        if (count == 32) continue;
        Fixture f; Bytes expected{};
        auto result = f.as.VecExtractBytes(f.left, f.right, ir::Imm{u64(count)});
        for (u32 i = 0; i < 32; ++i) expected[i] = i + count < 32 ? a[i + count] : b[i + count - 32];
        f.Run(result, a, b, expected, vector);
    }
    for (auto op : {O::VecSatAdd, O::VecSatSub}) for (u32 bits : {8u, 16u, 32u, 64u}) for (bool sign : {false, true}) {
        Fixture f; Bytes expected{}; const u32 width = bits / 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        const auto signed_lane = [&](u64 raw) -> s128 {
            return (raw & (u64{1} << (bits - 1))) ? s128(raw) - (s128{1} << bits) : s128(raw);
        };
        for (u32 lane = 0; lane < 32 / width; ++lane) {
            const u64 x = Lane(a, lane, width), y = Lane(b, lane, width);
            const s128 sx = sign ? signed_lane(x) : s128(x), sy = sign ? signed_lane(y) : s128(y);
            const s128 minimum = sign ? -(s128{1} << (bits - 1)) : 0;
            const s128 maximum = sign ? (s128{1} << (bits - 1)) - 1 : s128(mask);
            Lane(expected, lane, width, u64(std::clamp(op == O::VecSatAdd ? sx + sy : sx - sy, minimum, maximum)));
        }
        auto result = ir::Value{f.block->AppendInst(op, f.left, f.right, ir::Imm{u64(bits)}, ir::Imm{u64(sign)})};
        f.Run(result, a, b, expected, vector);
    }
    for (u32 bits : {16u, 32u, 64u}) for (bool unsigned_destination : {false, true}) {
        Fixture f; Bytes expected{}; const u32 source_width = bits / 8, destination_width = bits / 16;
        const s128 minimum = unsigned_destination ? 0 : -(s128{1} << (bits / 2 - 1));
        const s128 maximum = (s128{1} << (bits / 2 - (unsigned_destination ? 0 : 1))) - 1;
        for (u32 source = 0; source < 2; ++source) for (u32 lane = 0; lane < 32 / source_width; ++lane) {
            const u64 raw = Lane(source ? b : a, lane, source_width);
            const s128 value = (raw & (u64{1} << (bits - 1))) ? s128(raw) - (s128{1} << bits) : s128(raw);
            Lane(expected, source * (32 / source_width) + lane, destination_width, u64(std::clamp(value, minimum, maximum)));
        }
        auto result = f.as.VecPack(f.left, f.right, ir::Imm{u64(bits)}, ir::Imm{u64(unsigned_destination)});
        f.Run(result, a, b, expected, vector);
    }
    for (auto op : {O::VecZip, O::VecUnzip}) for (u32 bits : {8u, 16u, 32u, 64u}) for (u32 upper : {0u, 1u}) {
        Fixture f; Bytes expected{}; const u32 width = bits / 8, lanes = 32 / width;
        auto result = ir::Value{f.block->AppendInst(op, f.left, f.right, ir::Imm{u64(bits)}, ir::Imm{u64(upper)})};
        for (u32 lane = 0; lane < lanes; ++lane) {
            const bool right = op == O::VecZip ? (lane & 1) : lane >= lanes / 2;
            const u32 index = op == O::VecZip ? lane / 2 + upper * lanes / 2 : (lane % (lanes / 2)) * 2 + upper;
            Lane(expected, lane, width, Lane(right ? b : a, index, width));
        }
        f.Run(result, a, b, expected, vector);
    }
    for (u32 lane = 0; lane < 16; ++lane) {
        Fixture f; Bytes expected = a;
        auto scalar = f.as.LoadImm(ir::Imm{u64{0xabcd}}).SetType(T::U64);
        auto result = f.as.VecInsert16(f.left, scalar, ir::Imm{u64(lane)});
        Lane(expected, lane, 2, 0xabcd); f.Run(result, a, b, expected, vector);
    }
    for (u32 bits : {8u, 16u, 32u, 64u}) {
        Fixture f; const u32 width = bits / 8; u64 expected{};
        auto mask = f.as.VecMovMask(f.left, ir::Imm{u64(bits)}).SetType(T::U64);
        f.as.StoreUniform(ir::Uniform{96, T::U64}, mask);
        for (u32 lane = 0; lane < 32 / width; ++lane) expected |= ((Lane(a, lane, width) >> (bits - 1)) & 1) << lane;
        f.block->SetTerminal(ir::terminal::ReturnToHost{});
        rv::HostFeatures features{}; features.vector = vector;
        Compiled compiled{f.block.get(), true, features}; StateStorage state;
        std::memcpy(state.state->uniform_buffer_begin, a.data(), 32);
        Check(compiled.fn(state.state) == HaltReason::CallHost && state.Get(96) == expected, "V256 movmask includes upper lanes");
    }
    {
        Fixture f; Bytes indices{}, expected{};
        for (u32 lane = 0; lane < 32; ++lane) {
            indices[lane] = u8((lane * 7) % 48); expected[lane] = indices[lane] < 32 ? a[indices[lane]] : 0;
        }
        auto result = f.as.VecShuffle32Indexed(f.left, f.right);
        f.Run(result, a, indices, expected, vector);
    }
    {
        Fixture f; Bytes expected{};
        for (u32 lane = 0; lane < 8; ++lane) {
            float x = float(lane) - 3.25f, y = float(lane) * 0.5f + 1.0f, z = x + y;
            std::memcpy(a.data() + lane * 4, &x, 4); std::memcpy(b.data() + lane * 4, &y, 4);
            std::memcpy(expected.data() + lane * 4, &z, 4);
        }
        auto result = f.as.VecFAdd(f.left, f.right, ir::Imm{u64{32}});
        f.Run(result, a, b, expected, vector);
    }
    for (u32 kind = 0; kind < 8; ++kind) {
        Fixture f; Bytes source{}, expected{};
        const bool doubles = kind == 4 || kind == 5 || kind == 7;
        for (u32 lane = 0; lane < (doubles ? 4u : 8u); ++lane) {
            const double value = double(lane) * 1.5 - 3.25;
            if (kind <= 1) Lane(source, lane, 4, u32(s32(lane) * 19 - 27));
            else if (doubles) Lane(source, lane, 8, std::bit_cast<u64>(value));
            else Lane(source, lane, 4, std::bit_cast<u32>(float(value)));
        }
        const u32 lanes = kind == 1 || kind == 6 || doubles ? 4 : 8;
        for (u32 lane = 0; lane < lanes; ++lane) {
            if (kind == 0) Lane(expected, lane, 4, std::bit_cast<u32>(float(s32(Lane(source, lane, 4)))));
            else if (kind == 1) Lane(expected, lane, 8, std::bit_cast<u64>(double(s32(Lane(source, lane, 4)))));
            else if (kind == 6) Lane(expected, lane, 8, std::bit_cast<u64>(double(std::bit_cast<float>(u32(Lane(source, lane, 4))))));
            else if (kind == 7) Lane(expected, lane, 4, std::bit_cast<u32>(float(std::bit_cast<double>(Lane(source, lane, 8)))));
            else {
                const double value = doubles ? std::bit_cast<double>(Lane(source, lane, 8)) : double(std::bit_cast<float>(u32(Lane(source, lane, 4))));
                Lane(expected, lane, 4, u32(s32(kind == 3 || kind == 5 ? std::trunc(value) : std::nearbyint(value))));
            }
        }
        f.Run(f.as.VecFCvtPacked(f.left, ir::Imm{u64(kind)}), source, b, expected, vector);
    }
    {
        Fixture f; Bytes expected = a;
        const float x = std::bit_cast<float>(u32(Lane(a, 0, 4))), y = std::bit_cast<float>(u32(Lane(b, 0, 4)));
        Lane(expected, 0, 4, std::bit_cast<u32>(x + y));
        f.Run(f.as.VecFAddScalar32(f.left, f.right), a, b, expected, vector);
    }
    {
        Fixture f; Bytes expected{};
        for (u32 lane = 0; lane < 8; ++lane) {
            const float x = std::bit_cast<float>(u32(Lane(a, lane, 4))), y = std::bit_cast<float>(u32(Lane(b, lane, 4)));
            Lane(expected, lane, 4, std::bit_cast<u32>(std::fma(x, y, x)));
        }
        f.Run(f.as.VecFMulAdd(f.left, f.right, f.left, ir::Imm{u64{32}}, ir::Imm{u64{0}}), a, b, expected, vector);
    }
    {
        Fixture f;
        auto bad = f.as.Add(f.left, ir::Operand{f.right}).SetType(T::V256);
        f.as.StoreUniform(ir::Uniform{96, T::V256}, bad); f.block->SetTerminal(ir::terminal::ReturnToHost{});
        bool rejected{};
        try { Compiled compiled{f.block.get()}; }
        catch (const std::runtime_error& error) { rejected = std::string{error.what()}.find("V256 result is invalid") != std::string::npos; }
        Check(rejected, "V256 rejects invalid scalar arithmetic shapes before native emission");
    }
    {
        Fixture f; auto chain = f.left;
        for (u32 i = 0; i < 40; ++i) chain = f.as.VecAdd(chain, f.right, ir::Imm{u64{32}}).SetType(T::V256);
        f.as.StoreUniform(ir::Uniform{96, T::V256}, chain);
        f.block->SetTerminal(ir::terminal::ReturnToHost{});
        rv::HostFeatures features{}; features.vector = vector;
        Compiled compiled{f.block.get(), true, features};
        Check(compiled.translator.Stats().helpers == 0 && compiled.context.ValueStats().loads == 0 && compiled.context.ValueStats().stores == 0,
              "V256 arithmetic chain stays resident without SSA traffic");
        Check(compiled.translator.Stats().bytes[size_t(O::VecAdd)] <= (vector ? 392 : 8064),
              "V256 chain retains two native V128 lowering budgets");
    }
    for (bool ordered : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf200})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::V256}).SetType(T::V256);
        auto address = as.LoadUniform(ir::Uniform{64, T::U64}).SetType(T::U64);
        if (ordered) as.StoreMemoryTSO(ir::Operand{address}, input);
        else as.StoreMemory(ir::Operand{address}, input);
        auto value = (ordered ? as.LoadMemoryTSO(ir::Operand{address}) : as.LoadMemory(ir::Operand{address})).SetType(T::V256);
        as.StoreUniform(ir::Uniform{96, T::V256}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        rv::HostFeatures features{}; features.vector = vector;
        for (bool cache : {false, true}) {
            Compiled compiled{block.get(), cache, features};
            if (ordered) {
                const auto change = [](ir::Inst& inst, O load, O store) {
                    const auto op = inst.GetOp();
                    if (op != O::LoadMemory && op != O::LoadMemoryTSO && op != O::StoreMemory && op != O::StoreMemoryTSO) return;
                    const auto address = inst.GetArg<ir::Operand>(0);
                    if (op == O::LoadMemory || op == O::LoadMemoryTSO) inst.SetInst(load, address);
                    else { const auto value = inst.GetArg<ir::Value>(1); inst.SetInst(store, address, value); }
                    if (op == O::LoadMemory || op == O::LoadMemoryTSO) inst.SetReturn(T::V256);
                };
                for (auto& inst : block->GetInstList()) {
                    change(inst, O::LoadMemory, O::StoreMemory);
                }
                Compiled plain{block.get(), cache, features};
                Check(compiled.context.CurrentBufferSize() == plain.context.CurrentBufferSize() + 16,
                      "V256 TSO emits exactly one fence pair per logical access");
                for (auto& inst : block->GetInstList()) {
                    change(inst, O::LoadMemoryTSO, O::StoreMemoryTSO);
                }
            }
            for (u32 offset : {0u, 1u, 7u, 8u, 15u, 16u, 31u}) {
                std::array<u8, 64> memory{}; StateStorage state; WideRange range;
                state.state->pt = memory.data(); state.state->guest_addr_limit = memory.size();
                state.state->interp_range_check = &WideRangeCheck; state.state->interp_range_check_ctx = &range;
                state.Put(64, offset); std::memcpy(state.state->uniform_buffer_begin, a.data(), 32);
                Check(compiled.fn(state.state) == HaltReason::CallHost && range.calls == 2 && range.size == 32,
                      "V256 validates full ranges once per guest access");
                Check(std::memcmp(memory.data() + offset, a.data(), 32) == 0 &&
                      std::memcmp(state.state->uniform_buffer_begin + 96, a.data(), 32) == 0,
                      "V256 memory roundtrip preserves every byte at all alignments");
                Check(state.memory_participant.active == 0 && state.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0,
                      "V256 callback path releases its memory lease");
            }
            for (bool masked : {false, true}) {
                std::array<u8, 64> memory{}; memory.fill(0xcc); const auto before = memory;
                StateStorage state; state.state->pt = memory.data(); state.state->guest_addr_limit = 64;
                state.state->guest_addr_mask = masked ? 31 : UINT64_MAX; state.Put(64, masked ? 16 : 40);
                std::memcpy(state.state->uniform_buffer_begin, a.data(), 32);
                Check(compiled.fn(state.state) == HaltReason::PageFatal && memory == before,
                      "V256 range/mask failure occurs before its first store");
                Check(state.memory_participant.active == 0, "V256 fatal path releases its memory lease");
            }
        }
    }
    {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf400})}; ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::V256}).SetType(T::V256);
        as.SetHostFPR(input, ir::Imm{u16{19}}, ir::Imm{u8{0}});
        auto captured = as.GetHostFPR(ir::Imm{u16{19}}, ir::Imm{u8{0}}).SetType(T::V256);
        auto replacement = as.LoadImm(ir::Imm{u32{0x76543210}}).SetType(T::U32);
        as.SetHostFPR(replacement, ir::Imm{u16{19}}, ir::Imm{u8{20}});
        auto updated = as.GetHostFPR(ir::Imm{u16{19}}, ir::Imm{u8{0}}).SetType(T::V256);
        as.StoreUniform(ir::Uniform{96, T::V256}, captured); as.StoreUniform(ir::Uniform{128, T::V256}, updated);
        const ir::Local local{4, T::V256}; as.StoreLocal(local, captured);
        auto local_value = as.LoadLocal(local).SetType(T::V256); as.StoreUniform(ir::Uniform{160, T::V256}, local_value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        rv::HostFeatures features{}; features.vector = vector;
        const std::array bindings{UniformMapDesc{256, 32, 19, true}};
        Compiled compiled{block.get(), true, features, bindings};
        StateStorage state; std::array<u8, 128> locals{}; state.state->local_buffer = locals.data();
        std::memcpy(state.state->uniform_buffer_begin, a.data(), 32);
        auto expected = a; Lane(expected, 5, 4, 0x76543210);
        Check(compiled.fn(state.state) == HaltReason::CallHost &&
              std::memcmp(state.state->uniform_buffer_begin + 96, a.data(), 32) == 0 &&
              std::memcmp(state.state->uniform_buffer_begin + 128, expected.data(), 32) == 0 &&
              std::memcmp(state.state->uniform_buffer_begin + 160, a.data(), 32) == 0,
              "V256 logical FPR captures, upper-half partial writes and local buffers");
    }
    {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf600})}; ir::Assembler as{block.get()};
        auto first = as.LoadUniform(ir::Uniform{0, T::V256}).SetType(T::V256);
        auto second = as.LoadUniform(ir::Uniform{32, T::V256}).SetType(T::V256);
        auto* branch = new ir::Inst(O::Goto);
        as.BindLabel(ir::Value{branch});
        ir::Params a_inputs; a_inputs.Push(first); a_inputs.Push(first);
        auto phi_a = as.AddPhi(a_inputs).SetType(T::V256);
        ir::Params b_inputs; b_inputs.Push(second); b_inputs.Push(phi_a);
        auto phi_b = as.AddPhi(b_inputs).SetType(T::V256);
        ir::Params final_inputs; final_inputs.Push(first); final_inputs.Push(phi_b); phi_a.Def()->SetArg(0, final_inputs);
        auto count = as.LoadUniform(ir::Uniform{80, T::U64}).SetType(T::U64);
        auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{80, T::U64}, next);
        auto condition = as.TestNotZero(next).SetType(T::U8); branch->SetArgs(condition); block->AppendInst(branch);
        as.StoreUniform(ir::Uniform{96, T::V256}, phi_a); as.StoreUniform(ir::Uniform{128, T::V256}, phi_b);
        block->SetTerminal(ir::terminal::ReturnToHost{}); block->ReIdInstr();
        rv::HostFeatures features{}; features.vector = vector;
        Compiled compiled{block.get(), true, features}; StateStorage state;
        state.Put(80, 4); std::memcpy(state.state->uniform_buffer_begin, a.data(), 32);
        std::memcpy(state.state->uniform_buffer_begin + 32, b.data(), 32);
        Check(compiled.fn(state.state) == HaltReason::CallHost &&
              std::memcmp(state.state->uniform_buffer_begin + 96, b.data(), 32) == 0 &&
              std::memcmp(state.state->uniform_buffer_begin + 128, a.data(), 32) == 0,
              "V256 phi parallel-copy cycles include both halves under register pressure");
    }
    for (auto op : {O::VecAesEnc, O::VecAesEncLast, O::VecAesDec, O::VecAesDecLast,
                   O::VecSha256Msg1, O::VecSha256Msg2, O::VecSha256Rnds2, O::VecPclMul}) {
        Fixture f; ir::Value result; Bytes expected{};
        if (op == O::VecSha256Rnds2) result = ir::Value{f.block->AppendInst(op, f.left, f.right, f.left)};
        else if (op == O::VecPclMul) result = ir::Value{f.block->AppendInst(op, f.left, f.right, ir::Imm{u8{17}})};
        else result = ir::Value{f.block->AppendInst(op, f.left, f.right)};
        IntrusivePtr<ir::Block> reference{new ir::Block(ir::Location{0xf700})}; ir::Assembler as{reference.get()};
        auto left = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
        auto right = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128); ir::Value value;
        if (op == O::VecSha256Rnds2) value = ir::Value{reference->AppendInst(op, left, right, left)};
        else if (op == O::VecPclMul) value = ir::Value{reference->AppendInst(op, left, right, ir::Imm{u8{17}})};
        else value = ir::Value{reference->AppendInst(op, left, right)};
        as.StoreUniform(ir::Uniform{32, T::V128}, value.SetType(T::V128)); reference->SetTerminal(ir::terminal::ReturnToHost{}); reference->ReIdInstr();
        for (u32 part = 0; part < 2; ++part) {
            StateStorage state; std::memcpy(state.state->uniform_buffer_begin, a.data() + part * 16, 16);
            std::memcpy(state.state->uniform_buffer_begin + 16, b.data() + part * 16, 16);
            backend::interp::Interpreter interpreter{*state.state, reference.get()}; Check(interpreter.Run() == HaltReason::CallHost, "V128 crypto reference halt");
            std::memcpy(expected.data() + part * 16, state.state->uniform_buffer_begin + 32, 16);
        }
        f.Run(result, a, b, expected, vector);
    }
    std::cout << "PASS native V256 arithmetic, cross-half permutations, FP and resident chains\n";
#else
    (void)vector;
#endif
}
} // namespace swift::tests::riscv
