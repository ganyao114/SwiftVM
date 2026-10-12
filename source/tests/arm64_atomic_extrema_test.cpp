#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/ir/atomic_rmw.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;

namespace {

constexpr VAddr kCodeBase = 0x1000;

class AtomicCodeMemory final : public MemoryInterface {
public:
    explicit AtomicCodeMemory(u32 opcode) : code{opcode, 0xD4200000} {}
    bool Read(void* destination, size_t address, size_t size) override {
        if (address < kCodeBase || address - kCodeBase + size > sizeof(code)) return false;
        std::memcpy(destination, reinterpret_cast<u8*>(code.data()) + address - kCodeBase, size);
        return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* pointer) override {
        const auto address = reinterpret_cast<size_t>(pointer);
        if (address < kCodeBase || address - kCodeBase >= sizeof(code)) return nullptr;
        return reinterpret_cast<u8*>(code.data()) + address - kCodeBase;
    }
private:
    std::array<u32, 2> code;
};

struct AtomicMachine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(swift::arm64::ThreadContext64) + 32> storage{};
    backend::State* state = new (storage.data()) backend::State{};
    ~AtomicMachine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() {
        return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin);
    }
};

u32 Extrema(u32 size, u32 operation, u32 ordering, u32 source = 0, u32 result = 1) {
    return 0x38200000 | (size << 30) | ((ordering & 1) << 23) |
           ((ordering >> 1) << 22) | (source << 16) | (operation << 12) | (2 << 5) | result;
}

u64 Mask(u32 bits) { return bits == 64 ? ~u64{0} : (u64{1} << bits) - 1; }

u64 Expected(u32 operation, u32 bits, u64 old, u64 rhs) {
    const u64 mask = Mask(bits);
    old &= mask; rhs &= mask;
    const bool signed_compare = operation < 6;
    const u64 sign = signed_compare ? u64{1} << (bits - 1) : 0;
    const bool greater = (old ^ sign) > (rhs ^ sign);
    return operation == 4 || operation == 6 ? (greater ? old : rhs) : (greater ? rhs : old);
}

void Decode(u32 opcode, ir::Block& block) {
    AtomicCodeMemory memory(opcode);
    ir::Assembler assembler(&block);
    swift::arm64::A64Decoder decoder(kCodeBase, &memory, &assembler);
    decoder.Decode();
}

}  // namespace

TEST_CASE("ARM64 LSE extrema are atomic for every width and ordering") {
    for (u32 size = 0; size < 4; ++size) {
        const u32 bits = 8u << size;
        const u64 sign = u64{1} << (bits - 1);
        const std::array<u64, 5> values{0, 1, sign - 1, sign, Mask(bits)};
        for (u32 operation = 4; operation <= 7; ++operation) {
            for (u32 ordering = 0; ordering < 4; ++ordering) {
                for (u64 old : values) for (u64 operand : values) {
                    INFO("bits=" << bits << " op=" << operation << " order=" << ordering
                         << " old=" << old << " operand=" << operand);
                    alignas(16) std::array<u8, 16> memory;
                    memory.fill(0xA7);
                    std::memcpy(memory.data(), &old, size_t{1} << size);
                    const auto initial_memory = memory;
                    AtomicMachine machine;
                    machine.Context().r[0] = operand | (bits == 64 ? 0 : ~Mask(bits));
                    machine.Context().r[1] = ~u64{0};
                    machine.Context().r[2] = reinterpret_cast<u64>(memory.data());
                    machine.state->host_cpu_flags = 0xB0000000;
                    const auto initial_context = machine.Context();
                    ir::Block block(0, kCodeBase);
                    Decode(Extrema(size, operation, ordering), block);
                    backend::interp::Interpreter interpreter(*machine.state, &block);
                    interpreter.Run();
                    REQUIRE(machine.Context().r[1] == old);
                    REQUIRE(machine.Context().padding == static_cast<u32>(swift::arm64::InterruptReason::BRK));
                    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
                    u64 actual{};
                    std::memcpy(&actual, memory.data(), size_t{1} << size);
                    REQUIRE(actual == Expected(operation, bits, old, operand));
                    for (size_t index = size_t{1} << size; index < memory.size(); ++index)
                        REQUIRE(memory[index] == 0xA7);
#if defined(SWIFT_HAS_X64_BACKEND)
                    const auto expected_context = machine.Context();
                    const auto expected_memory = memory;
                    machine.Context() = initial_context;
                    machine.state->host_cpu_flags = 0xB0000000;
                    machine.state->halt_reason = HaltReason::None;
                    memory = initial_memory;
                    backend::x64::Translator translator(&block);
                    translator.Translate();
                    translator.getCode<HaltReason (*)(backend::State*)>()(machine.state);
                    REQUIRE(std::memcmp(&machine.Context(), &expected_context, sizeof(expected_context)) == 0);
                    REQUIRE(memory == expected_memory);
                    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
#endif
                }
            }
        }
    }
}

TEST_CASE("ARM64 LSE extrema respect zero-register and destination aliases") {
    for (u32 operation = 4; operation <= 7; ++operation) {
        alignas(16) u64 memory = 0x8000000000000000;
        AtomicMachine machine;
        machine.Context().r[2] = reinterpret_cast<u64>(&memory);
        ir::Block block(0, kCodeBase);
        Decode(Extrema(3, operation, 3, 31, 31), block);
        backend::interp::Interpreter interpreter(*machine.state, &block);
        interpreter.Run();
        REQUIRE(memory == Expected(operation, 64, 0x8000000000000000, 0));
        memory = 5;
        machine.Context().r[0] = 7;
        machine.state->halt_reason = HaltReason::None;
        ir::Block aliased(1, kCodeBase);
        Decode(Extrema(3, operation, 3, 0, 0), aliased);
        backend::interp::Interpreter aliased_interpreter(*machine.state, &aliased);
        aliased_interpreter.Run();
        REQUIRE(machine.Context().r[0] == 5);
        REQUIRE(memory == Expected(operation, 64, 5, 7));
    }
}

TEST_CASE("ARM64 CASP word pairs compare and update as one 64-bit atomic") {
    for (u32 ordering = 0; ordering < 4; ++ordering) for (bool success : {false, true}) {
        alignas(16) u64 memory = 0xFEDCBA9876543210;
        AtomicMachine machine;
        machine.Context().r[0] = 0xFFFFFFFF76543210;
        machine.Context().r[1] = success ? 0x11111111FEDCBA98 : 0x11111111FEDCBA99;
        machine.Context().r[2] = 0xABCDEF0101234567;
        machine.Context().r[3] = 0xABCDEF0189ABCDEF;
        machine.Context().r[4] = reinterpret_cast<u64>(&memory);
        machine.state->host_cpu_flags = 0xB0000000;
        const auto initial_context = machine.Context();
        const u64 initial_memory = memory;
        const u32 opcode = 0x08207C82 | ((ordering & 1) << 22) | ((ordering >> 1) << 15);
        ir::Block block(0, kCodeBase);
        Decode(opcode, block);  // CASP W0,W1,W2,W3,[X4]
        backend::interp::Interpreter interpreter(*machine.state, &block);
        interpreter.Run();
        REQUIRE(machine.Context().r[0] == 0x76543210);
        REQUIRE(machine.Context().r[1] == 0xFEDCBA98);
        REQUIRE(memory == (success ? 0x89ABCDEF01234567 : initial_memory));
        REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto expected_context = machine.Context();
        machine.Context() = initial_context;
        machine.state->halt_reason = HaltReason::None;
        memory = initial_memory;
        backend::x64::Translator translator(&block);
        translator.Translate();
        translator.getCode<HaltReason (*)(backend::State*)>()(machine.state);
        REQUIRE(std::memcmp(&machine.Context(), &expected_context, sizeof(expected_context)) == 0);
        REQUIRE(memory == (success ? 0x89ABCDEF01234567 : initial_memory));
#endif
    }
}

TEST_CASE("ARM64 CASP odd pair encodings and misalignment stop before writes") {
    for (u32 opcode : {0x08217C82u, 0x08207C83u}) {
        alignas(16) std::array<u64, 2> memory{0xFEDCBA9876543210, 0x0123456789ABCDEF};
        const auto initial_memory = memory;
        AtomicMachine machine;
        machine.Context().r[4] = reinterpret_cast<u64>(memory.data());
        ir::Block block(0, kCodeBase);
        Decode(opcode, block);
        backend::interp::Interpreter interpreter(*machine.state, &block);
        interpreter.Run();
        const auto reason = machine.Context().padding;
        REQUIRE(reason == static_cast<u32>(swift::arm64::InterruptReason::ILL_CODE));
        REQUIRE(memory == initial_memory);
    }
    alignas(16) std::array<u64, 2> memory{0xFEDCBA9876543210, 0x0123456789ABCDEF};
    const auto initial_memory = memory;
    AtomicMachine machine;
    machine.Context().r[4] = reinterpret_cast<u64>(memory.data()) + 4;
    ir::Block block(0, kCodeBase);
    Decode(0x08207C82, block);
    backend::interp::Interpreter interpreter(*machine.state, &block);
    interpreter.Run();
    REQUIRE(machine.state->halt_reason == HaltReason::PageFatal);
    REQUIRE(memory == initial_memory);
}

#if defined(SWIFT_HAS_X64_BACKEND)
TEST_CASE("ARM64 LSE extrema converge atomically under x64 contention") {
    alignas(16) u64 shared = 0;
    constexpr u32 kThreads = 4;
    constexpr u32 kIterations = 2000;
    ir::Block block(0, kCodeBase);
    Decode(Extrema(3, 6, 3), block);  // LDUMAXAL X0,X1,[X2]
    backend::x64::Translator translator(&block);
    translator.Translate();
    const auto run = translator.getCode<HaltReason (*)(backend::State*)>();
    std::atomic<bool> start{false};
    std::atomic<bool> monotonic{true};
    std::vector<std::thread> workers;
    for (u32 thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&, thread] {
            AtomicMachine machine;
            machine.Context().r[2] = reinterpret_cast<u64>(&shared);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            u64 previous{};
            for (u32 iteration = 0; iteration < kIterations; ++iteration) {
                machine.Context().r[0] = u64{iteration} * kThreads + thread + 1;
                machine.state->halt_reason = HaltReason::None;
                run(machine.state);
                const u64 old = machine.Context().r[1];
                if (old < previous) monotonic.store(false, std::memory_order_relaxed);
                previous = old;
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    REQUIRE(shared == u64{kThreads} * kIterations);
    REQUIRE(monotonic.load(std::memory_order_relaxed));
}
#endif
