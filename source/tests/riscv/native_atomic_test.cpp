#include "test_support.h"

#include <atomic>
#include <thread>
#include "runtime/frontend/ir_assembler.h"
#include "runtime/ir/atomic_rmw.h"

namespace swift::tests::riscv {

using T = ir::ValueType;
using O = ir::OpCode;
using R = ir::AtomicRMWOp;

void NativeAtomics() {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) {
        const auto size = ir::GetValueSizeByte(type);
        const u64 mask = size == 8 ? UINT64_MAX : (u64{1} << (size * 8)) - 1;
        for (u32 operation = 0; operation < 11; ++operation) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xb000})};
            ir::Assembler as{block.get()};
            auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
            auto input = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
            auto desired = as.LoadUniform(ir::Uniform{16, type}).SetType(type);
            auto carry = as.LoadUniform(ir::Uniform{24, T::U8}).SetType(T::U8);
            ir::Value result;
            if (operation == 0) result = as.AtomicExchange(address, input);
            else if (operation == 1) result = as.AtomicFetchAdd(address, input);
            else if (operation == 2) result = as.CompareAndSwap(address, input, desired);
            else result = as.AtomicRMW(ir::Imm{u64(operation - 3)}, address, input, carry);
            result = result.SetType(type);
            as.StoreUniform(ir::Uniform{32, type}, result);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled cached{block.get()}, uncached{block.get(), false};
            Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0,
                  "all scalar atomic widths and RMW operations bypass interpreter dispatch");
            for (u32 offset = 0; offset < 8; ++offset) {
                for (u64 initial : {u64{0}, u64{1}, mask, mask >> 1, mask ^ (mask >> 1)}) {
                    for (u32 cin = 0; cin < 2; ++cin) {
                        const u64 input_value = offset & 1 ? initial : 0xf234fb67525b8395ULL & mask;
                        const u64 desired_value = 0x89318a565283764bULL & mask;
                        u64 wanted{};
                        switch (operation) {
                            case 0: wanted = input_value; break;
                            case 1: wanted = initial + input_value; break;
                            case 2: wanted = initial == input_value ? desired_value : initial; break;
                            case 3: wanted = initial + input_value; break;
                            case 4: wanted = initial - input_value; break;
                            case 5: wanted = initial & input_value; break;
                            case 6: wanted = initial | input_value; break;
                            case 7: wanted = initial ^ input_value; break;
                            case 8: wanted = u64{0} - initial; break;
                            case 9: wanted = initial + input_value + cin; break;
                            case 10: wanted = initial - input_value - cin; break;
                        }
                        wanted &= mask;
                        for (auto* compiled : {&cached, &uncached}) {
                            alignas(16) std::array<u8, 32> memory;
                            memory.fill(0xa5);
                            std::memcpy(memory.data() + offset + 8, &initial, size);
                            auto expected = memory;
                            std::memcpy(expected.data() + offset + 8, &wanted, size);
                            StateStorage state;
                            state.Put(0, reinterpret_cast<u64>(memory.data() + offset + 8));
                            state.Put(8, input_value); state.Put(16, desired_value); state.Put(24, cin);
                            Check(compiled->fn(state.state) == HaltReason::CallHost, "native atomic halt");
                            Check(state.Get(32) == initial && memory == expected,
                                  "atomic observed value, replacement and untouched neighbors: width=" +
                                  std::to_string(size * 8) + " operation=" + std::to_string(operation) +
                                  " offset=" + std::to_string(offset));
                        }
                    }
                }
            }
        }
    }
    // Exercise competing reservations and AMOs, not just single-thread values.
    for (auto type : {T::U16, T::U32, T::U64}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xb100})};
        ir::Assembler as{block.get()};
        auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto one = as.LoadImm(ir::Imm{u64{1}}).SetType(type);
        (void)as.AtomicFetchAdd(address, one).SetType(type);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        alignas(16) u64 memory{};
        std::atomic<bool> valid{true};
        std::array<std::thread, 4> workers;
        for (auto& worker : workers) worker = std::thread{[&] {
            StateStorage state;
            state.Put(0, reinterpret_cast<u64>(&memory));
            for (u32 i = 0; i < 1000; ++i)
                if (compiled.fn(state.state) != HaltReason::CallHost) valid.store(false);
        }};
        for (auto& worker : workers) worker.join();
        Check(valid.load() && memory == 4000, "concurrent native subword LR/SC and word AMO updates");
    }
    std::cout << "PASS native atomics: all widths/RMW operations, alignment, neighbors and contention\n";
}

}  // namespace swift::tests::riscv
