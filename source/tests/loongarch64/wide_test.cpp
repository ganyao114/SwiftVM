#include "test_support.h"

#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {

using T = ir::ValueType;

void WideMemory() {
    constexpr u32 bytes = 32;
    for (bool ordered : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x5600})};
        ir::Assembler as{block.get()};
        auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto value = as.LoadUniform(ir::Uniform{32, T::V256}).SetType(T::V256);
        if (ordered) as.StoreMemoryTSO(ir::Operand{address}, value);
        else as.StoreMemory(ir::Operand{address}, value);
        auto loaded = ordered ? as.LoadMemoryTSO(ir::Operand{address}) : as.LoadMemory(ir::Operand{address});
        as.StoreUniform(ir::Uniform{96, T::V256}, loaded.SetType(T::V256));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (bool cache : {false, true}) {
            Compiled compiled{block.get(), cache};
            for (u64 address : {u64{0}, u64{1}, u64{31}, u64{4079}, u64{4080}, u64{4095}, u64{8160}, u64{8176}, u64{8191}}) {
                StateStorage state;
                alignas(16) std::array<u8, 8192> memory; memory.fill(0xa5);
                auto expected = memory;
                std::array<u8, bytes> input, sentinel;
                for (u32 i = 0; i < bytes; ++i) input[i] = u8(i * 19 + 7);
                sentinel.fill(0xcc);
                std::memcpy(state.state->uniform_buffer_begin + 32, input.data(), bytes);
                std::memcpy(state.state->uniform_buffer_begin + 96, sentinel.data(), bytes);
                state.Put(0, address);
                state.state->pt = memory.data(); state.state->guest_addr_limit = memory.size();
                state.state->guest_addr_mask = memory.size() - 1;
                const bool valid = address <= memory.size() - bytes;
                if (valid) std::memcpy(expected.data() + address, input.data(), bytes);
                Check(compiled.fn(state.state) == (valid ? HaltReason::CallHost : HaltReason::PageFatal), "V256 full-extent memory halt");
                Check(memory == expected, "V256 access uses distinct low/high halves and rejects the whole extent before either write");
                Check(std::memcmp(state.state->uniform_buffer_begin + 96, valid ? input.data() : sentinel.data(), bytes) == 0,
                      "V256 memory roundtrip and failed access preserve complete destination state");
                if (valid) Check(CheckABI(compiled.fn, state.state) == 1, "V256 split memory keeps LP64D");
            }
        }
    }
    std::cout << "PASS distinct V256 halves, unaligned/page-crossing accesses and full-extent failure\n";
}

}  // namespace swift::tests::loongarch64
