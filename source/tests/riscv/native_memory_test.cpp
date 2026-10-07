#include "test_support.h"

#include <atomic>
#include <thread>
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::riscv {

using T = ir::ValueType;
using O = ir::OpCode;

namespace {
struct RangeProbe { u64 pointer{}; u32 calls{}; bool vector{}; u64 rounding{}; };
#if defined(__riscv) && __riscv_xlen == 64
extern "C" u64 SwiftRiscvVectorRange(u64, u64, u64, u64, u64, u64, u64, u64);
#endif
bool ProbeRange(void* opaque, u64, u64) {
    auto& probe = *static_cast<RangeProbe*>(opaque);
    ++probe.calls;
#if defined(__riscv) && __riscv_xlen == 64
    asm volatile("frrm %0" : "=r"(probe.rounding));
    if (probe.vector) SwiftRiscvVectorRange(0, 0, 0, 0, 0, 0, 0, 0);
    // The assembly helper changes every GP/FP callee-saved register before
    // restoring them, exercising a real C ABI boundary on each oracle call.
    SwiftRiscvFaultHelper(probe.pointer);
#endif
    return true;
}
}  // namespace

void NativeMemoryFrame(bool vector) {
    rv::HostFeatures features{}; features.vector = vector;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc820})};
    ir::Assembler as{block.get()};
    auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto live = as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128);
    if (vector) live = as.VecFAdd(live, live, ir::Imm{u64{32}}).SetType(T::V128);
    auto sum = as.LoadMemory(ir::Operand{address}).SetType(T::U64);
    for (u32 i = 0; i < 2; ++i) {
        auto next = as.LoadMemoryTSO(ir::Operand{address}).SetType(T::U64);
        sum = as.Add(sum, ir::Operand{next}).SetType(T::U64);
    }
    as.StoreUniform(ir::Uniform{8, T::U64}, sum);
    as.StoreUniform(ir::Uniform{48, T::V128}, live);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{block.get(), true, features};
    Check(compiled.translator.Stats().saved_fprs == 0 && compiled.translator.Stats().lazy_saved_fprs == 12 &&
          compiled.translator.Stats().saved_gprs < 9 &&
          compiled.translator.Stats().saved_gprs + compiled.translator.Stats().lazy_saved_gprs == 9 &&
          compiled.translator.Stats().frame_size == rv::kBlockSavedFrameSize,
          "memory fast paths save only modified GPRs and no FPRs; actual calls retain full fault recovery");
    for (bool oracle : {false, true}) {
        alignas(8) u64 memory = 0x17369fcb;
        RangeProbe probe{reinterpret_cast<u64>(&memory), 0, vector};
        StateStorage input; input.Put(0, reinterpret_cast<u64>(&memory));
        input.Put(32, 0x3f8000003f800000ULL); input.Put(40, input.Get(32));
        if (oracle) { input.state->interp_range_check = &ProbeRange; input.state->interp_range_check_ctx = &probe; }
#if defined(__riscv) && __riscv_xlen == 64
        u64 previous{}, current{}; asm volatile("fsrmi %0, 2" : "=r"(previous) :: "memory");
        const auto abi = SwiftRiscvCheckABI(compiled.fn, input.state);
        asm volatile("frrm %0\nfsrm %1" : "=&r"(current) : "r"(previous) : "memory");
        Check(abi == 1 && current == 2 && (!oracle || probe.rounding == 2),
              "optional and repeated memory callbacks preserve LP64D and the caller's rounding mode");
#endif
        Check(input.state->halt_reason == HaltReason::CallHost && input.Get(8) == memory * 3 &&
              input.Get(48) == (vector ? 0x4000000040000000ULL : input.Get(32)) && input.Get(56) == input.Get(48) &&
              probe.calls == (oracle ? 3u : 0u),
              "live GPR/vector values survive zero or three conditional oracle calls");
    }
    std::cout << "PASS conditional memory ABI frames and repeated oracle calls\n";
}

void NativeMemoryCopy(bool vector, bool zacas) {
    rv::HostFeatures features{}; features.vector = vector; features.zacas = zacas;
    for (auto op : {O::MemoryCopy, O::MemoryCopyTSO}) for (u32 size : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 13u, 15u, 16u, 17u, 31u, 32u, 33u, 127u, 128u, 129u, 255u, 256u, 257u, 4096u}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc800})};
        ir::Assembler as{block.get()};
        auto live = as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128);
        auto dst = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto src = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        block->AppendInst(op, ir::Lambda{dst}, ir::Lambda{src}, ir::Imm{u64(size)});
        as.StoreUniform(ir::Uniform{48, T::V128}, live);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled cached{block.get(), true, features}, uncached{block.get(), false, features};
        Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0, "memory copies use native small/scalable-vector paths or a direct libc boundary");
        for (u32 alignment = 0; alignment < 16; ++alignment) for (s32 distance : {-129, -17, -1, 0, 1, 17, 129}) {
            for (auto* compiled : {&cached, &uncached}) {
                std::array<u8, 8192> memory{};
                for (u32 i = 0; i < memory.size(); ++i) memory[i] = u8(i * 31 + (i >> 3));
                auto expected = memory;
                const u32 source = 256 + alignment, destination = source + distance;
                std::memmove(expected.data() + destination, expected.data() + source, size);
                StateStorage state;
                state.state->pt = reinterpret_cast<u64*>(memory.data()); state.state->guest_addr_limit = memory.size();
                state.Put(0, destination); state.Put(8, source);
                state.Put(32, 0x92ace834df678910ULL); state.Put(40, 0x715642098fb67cdaULL);
                Check(compiled->fn(state.state) == HaltReason::CallHost && memory == expected && state.Get(48) == state.Get(32) && state.Get(56) == state.Get(40),
                      "memmove overlap, boundaries, alignment, scalable VLEN and live vector preservation: size=" + std::to_string(size));
            }
        }
        if (size) {
            std::array<u8, 64> memory{}; StateStorage state;
            state.state->pt = reinterpret_cast<u64*>(memory.data()); state.state->guest_addr_limit = 64;
            state.Put(0, 64); state.Put(8, 0);
            Check(cached.fn(state.state) == HaltReason::PageFatal, "memory copy validates the entire destination range");
            state.state->halt_reason = HaltReason::None; state.Put(0, 0); state.Put(8, 64);
            Check(cached.fn(state.state) == HaltReason::PageFatal, "memory copy validates the entire source range");
        }
    }
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc810})};
    ir::Assembler as{block.get()};
    auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto low = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto high = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    auto new_low = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
    auto new_high = as.LoadUniform(ir::Uniform{32, T::U64}).SetType(T::U64);
    auto old = as.CompareAndSwap128(address, low, high, new_low, new_high).SetType(T::V128);
    as.StoreUniform(ir::Uniform{48, T::V128}, old); block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled cached{block.get(), true, features}, uncached{block.get(), false, features};
    Check(cached.translator.Stats().helpers == 0, "wide CAS calls a fixed atomic kernel without IR dispatch");
    for (u32 offset = 0; offset < 16; ++offset) for (bool match : {false, true}) for (auto* compiled : {&cached, &uncached}) {
        alignas(16) std::array<u8, 48> memory; memory.fill(0xa5);
        const std::array<u64, 2> original{0x986410983f764815ULL, 0x83bfd12475629831ULL}, replacement{0x397af27867be0215ULL, 0x249afb6274657289ULL};
        std::memcpy(memory.data() + offset, original.data(), 16); auto expected = memory;
        if (match) std::memcpy(expected.data() + offset, replacement.data(), 16);
        StateStorage state; state.Put(0, reinterpret_cast<u64>(memory.data() + offset)); state.Put(8, original[0]); state.Put(16, original[1] ^ !match);
        state.Put(24, replacement[0]); state.Put(32, replacement[1]);
        Check(compiled->fn(state.state) == HaltReason::CallHost && memory == expected && state.Get(48) == original[0] && state.Get(56) == original[1], "CAS128 observed pair, match/mismatch, alignment and neighboring bytes");
    }
    for (u32 offset : {0u, 1u}) {
        alignas(16) std::array<u8, 32> memory{};
        constexpr u64 tag = 0x793145298fab7861ULL;
        std::memcpy(memory.data() + offset + 8, &tag, 8);
        std::array<std::thread, 4> workers; std::atomic<bool> valid{true};
        for (auto& worker : workers) worker = std::thread{[&] {
            StateStorage input; input.Put(0, reinterpret_cast<u64>(memory.data() + offset));
            u64 observed = 0;
            for (u32 success = 0; success < 1000;) {
                input.Put(8, observed); input.Put(16, observed ^ tag); input.Put(24, observed + 1); input.Put(32, (observed + 1) ^ tag);
                if (cached.fn(input.state) != HaltReason::CallHost || input.Get(56) != (input.Get(48) ^ tag)) { valid.store(false); break; }
                if (input.Get(48) == observed) { ++success; ++observed; }
                else observed = input.Get(48);
            }
        }};
        for (auto& worker : workers) worker.join();
        u64 low{}, high{}; std::memcpy(&low, memory.data() + offset, 8); std::memcpy(&high, memory.data() + offset + 8, 8);
        Check(valid.load() && low == 4000 && high == (low ^ tag), "four concurrent CAS128 writers preserve the pair on aligned and unaligned paths");
    }
    std::cout << "PASS native memory copy and direct CAS128 kernel\n";
}

}  // namespace swift::tests::riscv
