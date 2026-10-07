#include "test_support.h"

#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::riscv {

void NativeLocals(bool vector) {
    using T = ir::ValueType;
    rv::HostFeatures features{}; features.vector = vector;
    for (auto type : {T::U8, T::U16, T::U32, T::U64, T::V8, T::V16, T::V32, T::V64, T::V128}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc500})};
        ir::Assembler as{block.get()};
        ir::Local local{3, type};
        as.DefineLocal(local);
        auto input = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        as.StoreLocal(local, input);
        auto loaded = as.LoadLocal(local).SetType(type);
        as.StoreUniform(ir::Uniform{32, type}, loaded);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled cached{block.get(), true, features}, uncached{block.get(), false, features};
        Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0,
              "typed locals are emitted directly for scalar and vector shapes");
        for (u32 offset = 0; offset <= 8; ++offset) {
            StateStorage a, b, reference;
            a.Put(0, 0xf51972538e046acdULL); a.Put(8, 0x951823bdfa548711ULL);
            b.Put(0, a.Get(0)); b.Put(8, a.Get(8)); reference.Put(0, a.Get(0)); reference.Put(8, a.Get(8));
            std::array<u8, 64> ma, mb, mr;
            ma.fill(0x7b); mb = ma; mr = ma;
            a.state->local_buffer = offset == 8 ? nullptr : ma.data() + offset;
            b.state->local_buffer = offset == 8 ? nullptr : mb.data() + offset;
            reference.state->local_buffer = offset == 8 ? nullptr : mr.data() + offset;
            backend::interp::Interpreter interpreter{*reference.state, block.get()};
            const auto expected = interpreter.Run();
            Check(cached.fn(a.state) == expected && uncached.fn(b.state) == expected && ma == mr && mb == mr &&
                  std::memcmp(a.state->uniform_buffer_begin + 32, reference.state->uniform_buffer_begin + 32, 16) == 0 &&
                  std::memcmp(b.state->uniform_buffer_begin + 32, reference.state->uniform_buffer_begin + 32, 16) == 0,
                  "local round trip, adjacent bytes, unaligned buffer and null storage");
        }
    }
    for (u64 mask : {u64{0}, u64{7}, u64{65535}, u64{0x8000000000000000}}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc510})};
        ir::Assembler as{block.get()};
        auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        as.CheckMemoryAlignment(address, ir::Imm{mask});
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), true, features};
        Check(compiled.translator.Stats().helpers == 0, "memory alignment checks have no runtime dispatcher");
        for (u64 value : {u64{0}, u64{1}, u64{8}, u64{65536}, u64{1} << 63, UINT64_MAX}) {
            StateStorage state; state.Put(0, value);
            Check(compiled.fn(state.state) == ((value & mask) ? HaltReason::PageFatal : HaltReason::CallHost),
                  "alignment masks fault exactly when requested");
        }
    }
    std::cout << "PASS native locals and memory-alignment guards\n";
}

}  // namespace swift::tests::riscv
