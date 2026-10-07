#include "test_support.h"

#include <algorithm>
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::riscv {

using T = ir::ValueType;
using O = ir::OpCode;

#if defined(__riscv) && __riscv_xlen == 64
extern "C" u64 SwiftRiscvVectorRange(u64, u64, u64, u64, u64, u64, u64, u64);
#endif

namespace {
bool clobber_vectors{};
u64 MutateUniforms(u64 pointer, u64, u64, u64, u64, u64, u64, u64) {
    auto* bytes = reinterpret_cast<u8*>(pointer);
    u64 observed{}; std::memcpy(&observed, bytes, 8);
    const u64 replacement = 0x31569874abcdef81ULL;
    std::memcpy(bytes, &replacement, 8);
    const std::array<u64, 2> vector{0x123456789abcdef0ULL, 0x9876543210abcdefULL};
    std::memcpy(bytes + 16, vector.data(), 16);
#if defined(__riscv) && __riscv_xlen == 64
    if (clobber_vectors) (void)SwiftRiscvVectorRange(0, 0, 0, 0, 0, 0, 0, 0);
#endif
    return observed;
}

void MoveBefore(ir::Block* block, ir::Inst* inst, ir::Inst* before) {
    block->RemoveInst(inst); block->InsertBefore(inst, before); block->ReIdInstr();
}
}

void NativeHostRegisters(bool vector) {
    rv::HostFeatures features{}; features.vector = vector;
    const std::array<UniformMapDesc, 2> bindings{{{0, 8, 3, false}, {16, 16, 19, true}}};
    {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xdff0})};
        ir::Assembler as{block.get()};
        for (u32 i = 0; i < 50; ++i) {
            auto value = as.GetHostGPR(ir::Imm{u32{3}}, ir::Imm{u32{0}}).SetType(T::U64);
            auto next = as.Add(value, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
            as.SetHostGPR(next, ir::Imm{u32{3}}, ir::Imm{u32{0}});
        }
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features, bindings};
        Check(compiled.context.ValueStats().loads == 0 && compiled.context.ValueStats().stores == 0 &&
              compiled.translator.Stats().bytes[size_t(O::GetHostGPR)] == 50 * 4 &&
              compiled.translator.Stats().bytes[size_t(O::SetHostGPR)] == 50 * 4,
              "host GPR chain uses one capture and one publication instruction, with no SSA or uniform round trips");
        StateStorage state; state.Put(0, 73);
        Check(compiled.fn(state.state) == HaltReason::CallHost && state.Get(0) == 123,
              "register-resident host GPR chain result");
    }
    for (u32 bytes : {1u, 2u, 4u, 8u}) for (u32 offset = 0; offset + bytes <= 8; ++offset) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe000})};
        ir::Assembler as{block.get()};
        const auto type = ir::GetIRValueType(bytes);
        auto captured = as.GetHostGPR(ir::Imm{u32{3}}, ir::Imm{u32{0}}).SetType(T::U64);
        auto read = as.GetHostGPR(ir::Imm{u32{3}}, ir::Imm{offset}).SetType(type);
        auto replacement = as.LoadImm(ir::Imm{u64{0x97614583abcd1269}}).SetType(type);
        as.SetHostGPR(replacement, ir::Imm{u32{3}}, ir::Imm{offset});
        auto current = as.GetHostGPR(ir::Imm{u32{3}}, ir::Imm{u32{0}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{64, T::U64}, captured);
        as.StoreUniform(ir::Uniform{72, type}, read);
        as.StoreUniform(ir::Uniform{80, T::U64}, current);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (bool cache : {false, true}) {
            Compiled compiled{block.get(), cache, features, bindings};
            Check(compiled.translator.Stats().helpers == 0, "host GPR fields use native register operations");
            if (cache && offset == 0 && (bytes == 4 || bytes == 8))
                Check(compiled.translator.Stats().bytes[size_t(O::SetHostGPR)] == 4,
                      "normalized full and zero-extending word publications use one register move");
            if (cache && offset == 0 && bytes == 1)
                Check(compiled.translator.Stats().bytes[size_t(O::SetHostGPR)] == 8,
                      "low-byte publication uses immediate AND plus OR without an extra mask or copy");
            StateStorage state; const u64 initial = 0x8231b9754ced7168ULL;
            state.Put(0, initial);
            u64 expected = initial;
            const u64 replacement_bits = 0x97614583abcd1269ULL;
            std::memcpy(reinterpret_cast<u8*>(&expected) + offset, &replacement_bits, bytes);
            if (offset == 0 && bytes == 4) expected &= UINT32_MAX;
            u64 expected_read{}; std::memcpy(&expected_read, reinterpret_cast<const u8*>(&initial) + offset, bytes);
            Check(compiled.fn(state.state) == HaltReason::CallHost && state.Get(0) == expected &&
                  state.Get(64) == initial && state.Get(72) == expected_read && state.Get(80) == expected,
                  "partial GPR writes, upper-half clearing and captured reads match their byte oracle");
#if defined(__riscv) && __riscv_xlen == 64
            Check(SwiftRiscvCheckABI(compiled.fn, state.state) == 1, "pinned GPRs preserve the host ABI");
#endif
        }
    }
    for (u32 bytes : {1u, 2u, 4u, 8u, 16u}) for (u32 offset = 0; offset + bytes <= 16; offset += bytes) {
        for (bool scalar : {false, true}) {
            if (scalar && bytes > 8) continue;
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe080})};
            ir::Assembler as{block.get()};
            const auto type = scalar ? ir::GetIRValueType(bytes) : ir::GetVecIRValueType(bytes);
            auto captured = as.GetHostFPR(ir::Imm{u32{19}}, ir::Imm{u32{0}}).SetType(T::V128);
            auto read = as.GetHostFPR(ir::Imm{u32{19}}, ir::Imm{offset}).SetType(type);
            auto replacement = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
            as.SetHostFPR(replacement, ir::Imm{u32{19}}, ir::Imm{offset});
            auto current = as.GetHostFPR(ir::Imm{u32{19}}, ir::Imm{u32{0}}).SetType(T::V128);
            as.StoreUniform(ir::Uniform{64, T::V128}, captured);
            as.StoreUniform(ir::Uniform{80, type}, read);
            as.StoreUniform(ir::Uniform{96, T::V128}, current);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            for (bool cache : {false, true}) {
                Compiled compiled{block.get(), cache, features, bindings};
                StateStorage state;
                std::array<u8, 16> initial, replacement_bytes, expected;
                for (u32 i = 0; i < 16; ++i) { initial[i] = u8(i * 17 + 13); replacement_bytes[i] = u8(245 - i * 11); }
                expected = initial; std::memcpy(expected.data() + offset, replacement_bytes.data(), bytes);
                std::memcpy(state.state->uniform_buffer_begin + 16, initial.data(), 16);
                std::memcpy(state.state->uniform_buffer_begin + 32, replacement_bytes.data(), 16);
                Check(compiled.translator.Stats().helpers == 0 && compiled.fn(state.state) == HaltReason::CallHost &&
                      std::memcmp(state.state->uniform_buffer_begin + 16, expected.data(), 16) == 0 &&
                      std::memcmp(state.state->uniform_buffer_begin + 64, initial.data(), 16) == 0 &&
                      std::memcmp(state.state->uniform_buffer_begin + 80, initial.data() + offset, bytes) == 0 &&
                      std::memcmp(state.state->uniform_buffer_begin + 96, expected.data(), 16) == 0,
                      "scalar/vector FPR lane reads and writes preserve every untouched byte and captured value");
            }
        }
    }
    {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe100})};
        ir::Assembler as{block.get()};
        auto input = as.LoadImm(ir::Imm{u64{0x73619842abcd3591}}).SetType(T::U64);
        as.SetHostGPR(input, ir::Imm{u32{3}}, ir::Imm{u32{0}});
        auto captured = as.GetHostGPR(ir::Imm{u32{3}}, ir::Imm{u32{0}}).SetType(T::U64);
        auto captured_vector = as.GetHostFPR(ir::Imm{u32{19}}, ir::Imm{u32{0}}).SetType(T::V128);
        auto pointer = as.GetUniformAddress(ir::Imm{u32{0}}).SetType(T::U64);
        auto observed = as.CallHost(&MutateUniforms, pointer).SetType(T::U64);
        auto updated = as.GetHostGPR(ir::Imm{u32{3}}, ir::Imm{u32{0}}).SetType(T::U64);
        auto updated_vector = as.GetHostFPR(ir::Imm{u32{19}}, ir::Imm{u32{0}}).SetType(T::V128);
        as.StoreUniform(ir::Uniform{64, T::U64}, observed); as.StoreUniform(ir::Uniform{72, T::U64}, captured);
        as.StoreUniform(ir::Uniform{80, T::U64}, updated); as.StoreUniform(ir::Uniform{96, T::V128}, captured_vector);
        as.StoreUniform(ir::Uniform{112, T::V128}, updated_vector);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features, bindings};
        StateStorage state; state.Put(16, 0x8371); state.Put(24, 0xabcdef);
        clobber_vectors = vector;
        Check(compiled.fn(state.state) == HaltReason::CallHost && state.Get(64) == 0x73619842abcd3591ULL &&
              state.Get(72) == state.Get(64) && state.Get(80) == 0x31569874abcdef81ULL &&
              state.Get(96) == 0x8371 && state.Get(104) == 0xabcdef &&
              state.Get(112) == 0x123456789abcdef0ULL && state.Get(120) == 0x9876543210abcdefULL,
              "callbacks observe published pinned state, update it, and may destroy every RVV cache register");
        clobber_vectors = false;
    }
    // More bindings than available registers still preserve all logical homes.
    for (bool fpr : {false, true}) {
        std::vector<UniformMapDesc> pressure;
        for (u32 i = 0; i < 16; ++i) pressure.emplace_back(i * 16, fpr ? 16 : 8, i, fpr);
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe180})};
        ir::Assembler as{block.get()};
        std::array<ir::Value, 16> captured;
        const auto type = fpr ? T::V128 : T::U64;
        for (u32 i = 0; i < 16; ++i)
            captured[i] = ir::Value{block->AppendInst(fpr ? O::GetHostFPR : O::GetHostGPR, ir::Imm{i}, ir::Imm{u32{0}})}.SetType(type);
        for (u32 i = 0; i < 16; ++i)
            block->AppendInst(fpr ? O::SetHostFPR : O::SetHostGPR, captured[(i + 1) % 16], ir::Imm{i}, ir::Imm{u32{0}});
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features, pressure};
        StateStorage state;
        for (u32 i = 0; i < 16; ++i) { state.Put(i * 16, 0x7835198 + i); state.Put(i * 16 + 8, 0x8374169 - i); }
        Check(compiled.fn(state.state) == HaltReason::CallHost, "host-register pressure halt");
        for (u32 i = 0; i < 16; ++i)
            Check(state.Get(i * 16) == 0x7835198 + (i + 1) % 16 &&
                  (!fpr || state.Get(i * 16 + 8) == 0x8374169 - (i + 1) % 16),
                  "register pressure preserves captured logical GPR/FPR homes");
    }
    std::cout << "PASS native host GPR/FPR bindings, partial writes, captures, callbacks and pressure\n";
}

void NativePhi(bool vector) {
    rv::HostFeatures features{}; features.vector = vector;
    for (bool invert : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe200})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        auto original = as.LoadImm(ir::Imm{u64{31}}).SetType(T::U64);
        auto edge = invert ? as.NotGoto(ir::BOOL{input}) : as.Goto(ir::BOOL{input});
        auto updated = as.LoadImm(ir::Imm{u64{73}}).SetType(T::U64);
        as.BindLabel(edge);
        ir::Params inputs; inputs.Push(original); inputs.Push(updated);
        auto selected = as.AddPhi(inputs).SetType(T::U64);
        as.StoreUniform(ir::Uniform{16, T::U64}, selected);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features};
        Check(compiled.translator.Stats().helpers == 0 && compiled.translator.Stats().bytes[size_t(O::AddPhi)] <= 12,
              "forward scalar phi uses register copies on its two incoming edges");
        for (u64 value : {u64{0}, u64{1}}) {
            StateStorage state; state.Put(0, value);
            const bool taken = invert ? value == 0 : value != 0;
            Check(compiled.fn(state.state) == HaltReason::CallHost && state.Get(16) == (taken ? 31 : 73),
                  "phi chooses the incoming edge for Goto and NotGoto");
        }
    }
    // Swap two loop-carried values in parallel. The backedge is physically
    // after the phi definitions, so the two copies form a real cycle.
    for (bool packed : {false, true}) for (bool callback : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe280})};
        ir::Assembler as{block.get()};
        const auto type = packed ? T::V128 : T::U64;
        auto initial_a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto initial_b = as.LoadUniform(ir::Uniform{16, type}).SetType(type);
        auto count = as.LoadUniform(ir::Uniform{32, T::U64}).SetType(T::U64);
        auto a = as.AddPhi(ir::Params{}).SetType(type);
        auto b = as.AddPhi(ir::Params{}).SetType(type);
        auto n = as.AddPhi(ir::Params{}).SetType(T::U64);
#if defined(__riscv) && __riscv_xlen == 64
        if (callback && vector) (void)as.CallHost(&SwiftRiscvVectorRange).SetType(T::U64);
#endif
        auto next = as.Sub(n, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        auto active = as.TestNotZero(next);
        auto edge = as.Goto(active);
        as.BindLabel(edge); auto* label = &block->GetInstList().back();
        MoveBefore(block.get(), label, a.Def());
        ir::Params a_inputs; a_inputs.Push(initial_a); a_inputs.Push(b); a.Def()->SetArg(0, a_inputs);
        ir::Params b_inputs; b_inputs.Push(initial_b); b_inputs.Push(a); b.Def()->SetArg(0, b_inputs);
        ir::Params n_inputs; n_inputs.Push(count); n_inputs.Push(next); n.Def()->SetArg(0, n_inputs);
        as.StoreUniform(ir::Uniform{64, type}, a); as.StoreUniform(ir::Uniform{80, type}, b);
        as.StoreUniform(ir::Uniform{96, T::U64}, next);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features};
        Check(compiled.translator.Stats().helpers == 0 && compiled.context.ValueStats().initializations == 0,
              "loop phis have native edge assignments without undefined SSA initialization");
        if (!callback && (!packed || vector))
            Check(compiled.context.ValueStats().loads == 0 && compiled.context.ValueStats().stores == 0,
                  "loop-carried phis and their edge inputs stay in registers with zero SSA stack traffic");
        for (u64 iterations : {u64{1}, u64{2}, u64{3}, u64{31}, u64{32}}) {
            StateStorage state; state.Put(0, 7); state.Put(8, 0xabcdef);
            state.Put(16, 11); state.Put(24, 0x8374169); state.Put(32, iterations);
            Check(compiled.fn(state.state) == HaltReason::CallHost &&
                  state.Get(64) == (iterations % 2 ? 7 : 11) && state.Get(80) == (iterations % 2 ? 11 : 7) &&
                  (!packed || (state.Get(72) == (iterations % 2 ? 0xabcdef : 0x8374169) &&
                               state.Get(88) == (iterations % 2 ? 0x8374169 : 0xabcdef))) && state.Get(96) == 0,
                  "parallel phi cycles and callback clobbers preserve the full final loop iteration");
        }
    }
    {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe300})};
        ir::Assembler as{block.get()};
        auto count = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        std::array<ir::Value, 10> phis;
        for (auto& phi : phis) phi = as.AddPhi(ir::Params{}).SetType(T::U64);
        auto n = as.AddPhi(ir::Params{}).SetType(T::U64);
        auto next = as.Sub(n, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        auto edge = as.Goto(as.TestNotZero(next));
        as.BindLabel(edge); MoveBefore(block.get(), &block->GetInstList().back(), phis.front().Def());
        for (u32 i = 0; i < phis.size(); ++i) {
            ir::Params inputs; inputs.Push(ir::Imm{u64(i + 1)}); inputs.Push(phis[(i + 1) % phis.size()]);
            phis[i].Def()->SetArg(0, inputs);
            as.StoreUniform(ir::Uniform{16 + i * 8, T::U64}, phis[i]);
        }
        ir::Params inputs; inputs.Push(count); inputs.Push(next); n.Def()->SetArg(0, inputs);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features};
        for (u64 iterations : {u64{1}, u64{2}, u64{11}, u64{24}}) {
            StateStorage state; state.Put(0, iterations);
            Check(compiled.fn(state.state) == HaltReason::CallHost, "spilled phi cycle halt");
            for (u32 i = 0; i < phis.size(); ++i)
                Check(state.Get(16 + i * 8) == (i + iterations - 1) % phis.size() + 1,
                      "parallel copies preserve a cycle spanning resident and spilled phi values");
        }
    }
    for (bool callback : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe340})};
        ir::Assembler as{block.get()};
        constexpr u32 lanes = 13; // one more than the resident phi RVV pool
        std::array<ir::Value, lanes> initial, phis;
        for (u32 i = 0; i < lanes; ++i) initial[i] = as.LoadUniform(ir::Uniform{i * 16, T::V128}).SetType(T::V128);
        auto count = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
        for (auto& phi : phis) phi = as.AddPhi(ir::Params{}).SetType(T::V128);
        auto n = as.AddPhi(ir::Params{}).SetType(T::U64);
#if defined(__riscv) && __riscv_xlen == 64
        if (callback && vector) (void)as.CallHost(&SwiftRiscvVectorRange).SetType(T::U64);
#endif
        auto next = as.Sub(n, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        auto edge = as.Goto(as.TestNotZero(next));
        as.BindLabel(edge); MoveBefore(block.get(), &block->GetInstList().back(), phis.front().Def());
        for (u32 i = 0; i < lanes; ++i) {
            ir::Params inputs; inputs.Push(initial[i]); inputs.Push(phis[(i + 1) % lanes]); phis[i].Def()->SetArg(0, inputs);
            as.StoreUniform(ir::Uniform{512 + i * 16, T::V128}, phis[i]);
        }
        ir::Params inputs; inputs.Push(count); inputs.Push(next); n.Def()->SetArg(0, inputs);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features};
        for (u64 iterations : {u64{1}, u64{2}, u64{14}, u64{29}}) {
            StateStorage state;
            for (u32 i = 0; i < lanes; ++i) { state.Put(i * 16, 1001 + i); state.Put(i * 16 + 8, 0x9876543210ULL + i); }
            state.Put(256, iterations);
            Check(compiled.fn(state.state) == HaltReason::CallHost, "vector phi pressure halt");
            for (u32 i = 0; i < lanes; ++i) {
                const u32 source = (i + iterations - 1) % lanes;
                Check(state.Get(512 + i * 16) == 1001 + source && state.Get(520 + i * 16) == 0x9876543210ULL + source,
                      "vector phi cycles retain both halves across RVV/GPR spills and ABI clobbers");
            }
        }
    }
    for (auto type : {T::U8, T::U16, T::U32, T::U64, T::V128}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe380})};
        ir::Assembler as{block.get()};
        ir::Params inputs; inputs.Push(ir::Imm{UINT64_MAX});
        auto phi = as.AddPhi(inputs).SetType(type);
        as.StoreUniform(ir::Uniform{0, type}, phi);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features};
        StateStorage state;
        const auto bytes = ir::GetValueSizeByte(type);
        Check(compiled.fn(state.state) == HaltReason::CallHost &&
              state.Get(0) == (bytes >= 8 ? UINT64_MAX : (u64{1} << (bytes * 8)) - 1) && state.Get(8) == 0,
              "entry phi immediates normalize scalar widths and zero the upper vector half");
    }
    for (u32 malformed = 0; malformed < 4; ++malformed) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe400})};
        ir::Assembler as{block.get()};
        if (malformed == 0) {
            auto edge = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
            as.BindLabel(edge);
            ir::Params inputs; inputs.Push(ir::Imm{u64{7}});
            (void)as.AddPhi(inputs).SetType(T::U64); // two edges, one input
        } else if (malformed == 1) {
            auto later = as.LoadImm(ir::Imm{u64{7}}).SetType(T::U64);
            auto edge = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
            as.BindLabel(edge);
            ir::Params inputs; inputs.Push(later); inputs.Push(later);
            auto phi = as.AddPhi(inputs).SetType(T::U64);
            block->RemoveInst(later.Def()); block->InsertAfter(later.Def(), phi.Def()); block->ReIdInstr();
        } else if (malformed == 2) {
            auto value = as.LoadImm(ir::Imm{u32{7}}).SetType(T::U32);
            auto edge = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
            as.BindLabel(edge);
            ir::Params inputs; inputs.Push(value); inputs.Push(value);
            (void)as.AddPhi(inputs).SetType(T::U64); // width mismatch
        } else {
            (void)as.LoadImm(ir::Imm{u64{7}}).SetType(T::U64);
            ir::Params inputs; inputs.Push(ir::Imm{u64{11}});
            (void)as.AddPhi(inputs).SetType(T::U64); // outside the phi header
        }
        block->SetTerminal(ir::terminal::ReturnToHost{});
        bool rejected{};
        try { Compiled compiled{block.get(), true, features}; }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "malformed phi count, dominance, width or placement is rejected before code publication");
    }
    std::cout << "PASS native scalar/vector phi edges, NotGoto, loop-carried swaps and ABI clobbers\n";
}

}  // namespace swift::tests::riscv
