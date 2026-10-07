#include "test_support.h"
#include "runtime/backend/atomic_fallback.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/guest_memory_scope.h"
#include "runtime/frontend/x86/x87.h"
#include <thread>

namespace swift::tests::riscv {
using T = ir::ValueType;
namespace {
struct AtomicFixture {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf800})};
    ir::Assembler as{block.get()};
    ir::Value address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    void Finish() { block->SetTerminal(ir::terminal::ReturnToHost{}); }
};
}

void MemoryProtocol() {
    AtomicFixture wide;
    std::array<ir::Value, 4> args;
    for (u32 i = 0; i < 4; ++i) args[i] = wide.as.LoadUniform(ir::Uniform{8 + i * 8, T::U64}).SetType(T::U64);
    auto old = wide.as.CompareAndSwap128(wide.address, args[0], args[1], args[2], args[3]).SetType(T::V128);
    wide.as.StoreUniform(ir::Uniform{40, T::V128}, old); wide.Finish();
    auto software_features = rv::HostFeatures::Detect(); software_features.zacas = false;
    Compiled compare{wide.block.get(), true, software_features};
    AtomicFixture narrow;
    auto amount = narrow.as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    (void)narrow.as.AtomicFetchAdd(narrow.address, amount).SetType(T::U64); narrow.Finish();
    Compiled increment{narrow.block.get()};
    alignas(16) std::array<u64, 2> memory{};
    constexpr u32 count = 1000;
    std::atomic<bool> start{};
    std::atomic<bool> failed{};
    std::thread writer{[&] {
        StateStorage state; state.state->pt = memory.data(); state.Put(0, 0);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        u64 low{}, high{};
        for (u32 i = 0; i < count;) {
            state.Put(8, low); state.Put(16, high); state.Put(24, low + 1); state.Put(32, high + 1);
            if (compare.fn(state.state) != HaltReason::CallHost) { failed = true; break; }
            const auto seen_low = state.Get(40), seen_high = state.Get(48);
            if (seen_low == low && seen_high == high) { ++i; ++low; ++high; }
            else { low = seen_low; high = seen_high; }
        }
        if (state.memory_participant.active || state.state->spill_area[backend::kRiscvMemoryOwnedSlot]) failed = true;
    }};
    std::thread adder{[&] {
        StateStorage state; state.state->pt = memory.data(); state.Put(0, 0); state.Put(8, 1);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (u32 i = 0; i < count; ++i) if (increment.fn(state.state) != HaltReason::CallHost) failed = true;
    }};
    start.store(true, std::memory_order_release); writer.join(); adder.join();
    Check(!failed && memory[0] == count * 2 && memory[1] == count,
          "software CAS128 and overlapping native AMO64 have no lost updates");
    if (rv::HostFeatures::Detect().zacas) {
        Compiled native_compare{wide.block.get()};
        memory = {}; start = false; failed = false;
        const auto update = [&](Compiled& compiled) {
            StateStorage state; state.state->pt = memory.data(); state.Put(0, 0);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            u64 low{}, high{};
            for (u32 i = 0; i < count;) {
                state.Put(8, low); state.Put(16, high); state.Put(24, low + 1); state.Put(32, high + 1);
                if (compiled.fn(state.state) != HaltReason::CallHost) { failed = true; break; }
                const auto seen_low = state.Get(40), seen_high = state.Get(48);
                if (seen_low == low && seen_high == high) { ++i; ++low; ++high; }
                else { low = seen_low; high = seen_high; }
            }
        };
        std::thread software{[&] { update(compare); }}, hardware{[&] { update(native_compare); }};
        start.store(true, std::memory_order_release); software.join(); hardware.join();
        Check(!failed && memory[0] == count * 2 && memory[1] == count * 2,
              "software CAS128 and native Zacas share the same atomic update protocol");
    }

    AtomicFixture reader;
    auto observed = reader.as.LoadMemory(ir::Operand{reader.address}).SetType(T::V128);
    reader.as.StoreUniform(ir::Uniform{16, T::V128}, observed); reader.Finish();
    Compiled read{reader.block.get()};
    memory = {}; start = false; failed = false;
    std::atomic<bool> complete{};
    std::thread publisher{[&] {
        StateStorage state; state.state->pt = memory.data(); state.Put(0, 0);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (u32 i = 0; i < count; ++i) {
            state.Put(8, i); state.Put(16, i); state.Put(24, i + 1); state.Put(32, i + 1);
            if (compare.fn(state.state) != HaltReason::CallHost || state.Get(40) != i || state.Get(48) != i) failed = true;
        }
        complete.store(true, std::memory_order_release);
    }};
    std::thread observer{[&] {
        StateStorage state; state.state->pt = memory.data(); state.Put(0, 0);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        u32 samples{};
        do {
            state.state->halt_reason = HaltReason::None;
            const auto reason = samples & 1 ? backend::interp::Interpreter{*state.state, reader.block.get()}.Run()
                                           : read.fn(state.state);
            if (reason != HaltReason::CallHost || state.Get(16) != state.Get(24)) failed = true;
            ++samples;
        } while (!complete.load(std::memory_order_acquire) || samples < count);
    }};
    start.store(true, std::memory_order_release); publisher.join(); observer.join();
    Check(!failed, "native/interpreter V128 reads cannot observe a partial software CAS128 update");

    // An x87 Float80 load spans two host reads. Both must observe the same
    // software CAS128 publication even though X87Op releases the JIT lease.
    AtomicFixture x87;
    auto context = x87.as.GetUniformAddress(ir::Imm{u64{256}}).SetType(T::U64);
    auto output = x87.as.LoadImm(ir::Imm{u64{32}}).SetType(T::U64);
    (void)x87.as.X87Op(context, ir::Imm{swift::x86::MakeX87Command(swift::x86::X87Action::Init)}, x87.address);
    (void)x87.as.X87Op(context, ir::Imm{swift::x86::MakeX87Command(
            swift::x86::X87Action::LoadFloat, swift::x86::X87Format::Float80)}, x87.address);
    (void)x87.as.X87Op(context, ir::Imm{swift::x86::MakeX87Command(
            swift::x86::X87Action::StoreFloat, swift::x86::X87Format::Float80, 0, 0,
            swift::x86::X87Pop)}, output);
    x87.Finish();
    Compiled x87_read{x87.block.get()};
    constexpr u64 significand = 0xc000000000000000ULL;
    const auto exponent = [](u64 sequence) { return u64{0x3fff} + sequence % 32; };
    alignas(16) std::array<u64, 8> x87_memory{significand, exponent(0)};
    start = false; failed = false; complete = false;
    std::thread x87_publisher{[&] {
        StateStorage state; state.state->pt = x87_memory.data(); state.Put(0, 0);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (u32 i = 0; i < count; ++i) {
            state.Put(8, significand + i); state.Put(16, exponent(i));
            state.Put(24, significand + i + 1); state.Put(32, exponent(i + 1));
            if (compare.fn(state.state) != HaltReason::CallHost || state.Get(40) != significand + i ||
                state.Get(48) != exponent(i)) failed = true;
        }
        complete.store(true, std::memory_order_release);
    }};
    std::thread x87_observer{[&] {
        StateStorage state; state.state->pt = x87_memory.data(); state.Put(0, 0);
        backend::GuestMemoryScope mapping{x87_memory.data(), UINT64_MAX};
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        u32 samples{};
        do {
            const auto reason = x87_read.fn(state.state);
            const auto sequence = x87_memory[4] - significand;
            if (reason != HaltReason::CallHost || sequence > count ||
                x87_memory[5] != exponent(sequence)) failed = true;
            ++samples;
        } while (!complete.load(std::memory_order_acquire) || samples < count);
    }};
    start.store(true, std::memory_order_release); x87_publisher.join(); x87_observer.join();
    Check(!failed, "native X87Op Float80 reads cannot observe a partial software CAS128 update");

    // The unaligned halfword and aligned word add to the very same bits;
    // neighboring bytes must survive both protocols.
    AtomicFixture halfword;
    auto half_amount = halfword.as.LoadImm(ir::Imm{u16{1}}).SetType(T::U16);
    (void)halfword.as.AtomicFetchAdd(halfword.address, half_amount).SetType(T::U16); halfword.Finish();
    Compiled unaligned{halfword.block.get()};
    AtomicFixture word;
    auto word_amount = word.as.LoadImm(ir::Imm{u32{256}}).SetType(T::U32);
    (void)word.as.AtomicFetchAdd(word.address, word_amount).SetType(T::U32); word.Finish();
    Compiled aligned{word.block.get()};
    memory[0] = 0xa000007f; start = false; failed = false;
    const auto run = [&](Compiled& compiled, u64 address) {
        StateStorage state; state.state->pt = memory.data(); state.Put(0, address);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (u32 i = 0; i < count; ++i) if (compiled.fn(state.state) != HaltReason::CallHost) failed = true;
    };
    std::thread a{[&] { run(unaligned, 1); }}, b{[&] { run(aligned, 0); }};
    start.store(true, std::memory_order_release); a.join(); b.join();
    Check(!failed && memory[0] == 0xa000007f + u64(count * 2) * 256,
          "unaligned atomic halfword and overlapping native AMO32 preserve updates and neighbors");
    Check(backend::riscv64::anonymous_memory_readers == 0 && backend::unaligned_atomic_lock == 0,
          "mixed-width operations release all memory protocol ownership");
    {
        StateStorage state; state.state->spill_area[backend::kRiscvMemoryParticipantSlot] = 0;
        state.state->pt = memory.data(); state.Put(0, 0); state.Put(8, 1);
        Check(increment.fn(state.state) == HaltReason::CallHost && backend::riscv64::anonymous_memory_readers == 0 &&
              state.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0,
              "unregistered State uses and releases its anonymous memory lease");
    }
    std::cout << "PASS mixed-width native/software atomics and ordinary guest readers\n";
}
} // namespace swift::tests::riscv
