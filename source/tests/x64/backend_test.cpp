#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <boost/multiprecision/cpp_int.hpp>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xmmintrin.h>

#include "runtime/backend/address_space.h"
#include "runtime/backend/atomic_fallback.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/x87_helper.h"
#include "runtime/frontend/x86/x87.h"
#include "translator/x86/cpu.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
using ir::OpCode;
using ir::ValueType;
namespace x64 = swift::runtime::backend::x64;

namespace {
u64 checks{};
void Check(bool condition, const std::string& detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}

std::atomic<u32> fault_range_calls{};
bool FaultRangeProbe(void* pointer, u64, u64) {
    fault_range_calls.fetch_add(1, std::memory_order_relaxed);
    const auto value = *static_cast<volatile u64*>(pointer);
    return value != UINT64_MAX;
}

u64 HaltAtPublishedLocation(u64 pointer) {
    auto* state = reinterpret_cast<backend::State*>(pointer);
    state->halt_reason = HaltReason::PageFatal;
    return state->current_loc.Value();
}

struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 4096> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const { u64 value{}; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
};

struct Compiled {
    x64::Translator translator;
    using Entry = HaltReason (*)(backend::State*);
    Entry entry;
    explicit Compiled(ir::Block* block, x64::HostFeatures features = x64::HostFeatures::Detect()) : translator(block, features) {
        translator.Translate();
        entry = translator.getCode<Entry>();
    }
    HaltReason fn(backend::State* state) const {
        const auto reason = entry(state);
        // These fixtures bypass Runtime's trampoline, which consumes and
        // clears the owner-written halt reason after every native return.
        // Consume it here too, so a reused State starts its next execution
        // with no previous CallHost/PageFatal still pending. Preserve the
        // entry's returned reason, including synchronous faults, for assertions.
        state->halt_reason = HaltReason::None;
        return reason;
    }
};

Config TestConfig() {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
            .backend_isa = kX86_64, .uniform_buffer_size = 4096,
            .static_program = true, .global_opts = Optimizations::None,
            .stack_alignment = 16};
}

IntrusivePtr<ir::Block> ScalarProgram(OpCode op, ValueType type) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x1000})};
    ir::Assembler as{block.get()};
    auto left = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto right = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
    ir::Value result;
    switch (op) {
        case OpCode::Add: case OpCode::Sub: case OpCode::Adc: case OpCode::Sbb:
        case OpCode::Mul: case OpCode::Div: case OpCode::And: case OpCode::Or:
        case OpCode::Xor: case OpCode::AndNot:
            result = ir::Value{block->AppendInst(op, left, ir::Operand{right})}.SetType(type); break;
        case OpCode::LslValue: case OpCode::LsrValue: case OpCode::AsrValue: case OpCode::RorValue:
            result = ir::Value{block->AppendInst(op, left, right)}.SetType(type); break;
        case OpCode::Neg: case OpCode::Not:
            result = ir::Value{block->AppendInst(op, left)}.SetType(type); break;
        default: throw std::runtime_error("unknown scalar fixture");
    }
    as.SaveFlags(result, ir::Flags::All);
    as.StoreUniform(ir::Uniform{16, result.Type()}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

u64 Random(u64& seed) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

void ScalarDifferential() {
    const std::array ops{OpCode::Add, OpCode::Sub, OpCode::Adc, OpCode::Sbb,
        OpCode::Mul, OpCode::Div, OpCode::And, OpCode::Or, OpCode::Xor,
        OpCode::AndNot, OpCode::Neg, OpCode::Not,
        OpCode::LslValue, OpCode::LsrValue, OpCode::AsrValue, OpCode::RorValue};
    const std::array types{ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64,
        ValueType::S8, ValueType::S16, ValueType::S32, ValueType::S64};
    const std::array<u64, 12> edges{0, 1, 2, 7, 15, 31, 63, 64, 65,
        UINT64_MAX, 0x8000000000000000ULL, 0x7fffffffffffffffULL};
    u64 seed = 0x734052781692baffULL;
    for (auto type : types) for (auto op : ops) {
        auto block = ScalarProgram(op, type);
        Compiled compiled{block.get()};
        for (u32 iteration = 0; iteration < 128; ++iteration) {
            Storage actual, expected;
            const auto left = iteration < edges.size() ? edges[iteration] : Random(seed);
            const auto right = iteration < edges.size() ? edges[(iteration + 3) % edges.size()] : Random(seed);
            actual.Put(0, left); expected.Put(0, left);
            actual.Put(8, right); expected.Put(8, right);
            actual.state->host_cpu_flags = expected.state->host_cpu_flags = iteration & 1 ? UINT64_MAX : 0;
            backend::interp::Interpreter reference{*expected.state, block.get()};
            const auto wanted = reference.Run();
            const auto observed = compiled.fn(actual.state);
            const auto detail = std::string{ir::GetIRMetaInfo(op).name} + "/" + ir::ValueTypeString(type) + "/" + std::to_string(iteration);
            Check(observed == wanted, detail + " halt");
            Check(actual.Get(16) == expected.Get(16), detail + " value");
            Check(actual.state->host_cpu_flags == expected.state->host_cpu_flags, detail + " NZCV/PF/AF");
        }
    }
    std::cout << "PASS scalar differential: 128 programs / 16384 executions\n";
}

void Conditions() {
    for (u32 cond = 0; cond < 16; ++cond) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2000})};
        ir::Assembler as{block.get()};
        auto result = as.CondSet(ir::Cond(cond)).SetType(ValueType::U8);
        as.StoreUniform(ir::Uniform{0, ValueType::U8}, result);
        block->SetTerminal(ir::terminal::Condition{ir::Cond(cond),
                ir::terminal::LinkBlock{ir::Location{0x1111}}, ir::terminal::LinkBlock{ir::Location{0x2222}}});
        Compiled compiled{block.get()};
        for (u32 flags = 0; flags < 16; ++flags) {
            Storage actual, expected;
            actual.state->host_cpu_flags = expected.state->host_cpu_flags = u64(flags) << 28;
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(reference.Run() == compiled.fn(actual.state), "condition halt");
            Check(actual.Get(0) == expected.Get(0), "condition exact 0/1");
            Check(actual.state->current_loc == expected.state->current_loc, "condition branch");
        }
    }
    std::cout << "PASS all 16 conditions / 256 flag combinations\n";
}

void VectorDifferential() {
    const std::array ops{OpCode::VecAdd, OpCode::VecSub, OpCode::VecMul, OpCode::VecCmpEq,
        OpCode::VecCmpGt, OpCode::VecMin, OpCode::VecMax, OpCode::VecZip, OpCode::VecUnzip,
        OpCode::VecShiftLeft, OpCode::VecShiftRight, OpCode::VecShiftRightArithmetic};
    u64 seed = 0x1856efae52ad7384ULL;
    for (auto op : ops) for (u32 bits : {8u, 16u, 32u, 64u}) for (u32 option : {0u, 1u}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3000})};
        ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto right = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        ir::Value result;
        if (op == OpCode::VecMin || op == OpCode::VecMax || op == OpCode::VecZip || op == OpCode::VecUnzip)
            result = ir::Value{block->AppendInst(op, left, right, ir::Imm{bits}, ir::Imm{option})}.SetType(ValueType::V128);
        else result = ir::Value{block->AppendInst(op, left, right, ir::Imm{bits})}.SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, result);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u32 iteration = 0; iteration < 32; ++iteration) {
            Storage actual, expected;
            for (u32 offset = 0; offset < 32; offset += 8) {
                auto value = Random(seed);
                if (offset == 16 && (op == OpCode::VecShiftLeft || op == OpCode::VecShiftRight || op == OpCode::VecShiftRightArithmetic))
                    value = iteration & 1 ? iteration : UINT64_MAX;
                actual.Put(offset, value); expected.Put(offset, value);
            }
            backend::interp::Interpreter reference{*expected.state, block.get()};
            const auto wanted = reference.Run();
            Check(compiled.fn(actual.state) == wanted, "vector halt");
            const auto detail = std::string{ir::GetIRMetaInfo(op).name} + "/" + std::to_string(bits);
            Check(actual.Get(32) == expected.Get(32) && actual.Get(40) == expected.Get(40), detail);
        }
    }
    std::cout << "PASS vector differential: 96 programs / 3072 executions\n";
}

void VectorCompareAndMultiplyBoundaries() {
    u64 seed = 0x3856fc96a48983ULL;
    for (auto op : {OpCode::VecCmpEq, OpCode::VecCmpGt, OpCode::VecMul}) for (u32 bits : {8u, 16u, 32u, 64u}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3300})}; ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto right = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto result = ir::Value{block->AppendInst(op, left, right, ir::Imm{bits})}.SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, result);
        auto sum = as.VecAdd(result, right, ir::Imm{bits}).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{48, ValueType::V128}, sum);
        block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get()};
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        const std::array<u64, 5> edges{0, 1, mask, u64{1} << (bits - 1), (u64{1} << (bits - 1)) - 1};
        for (u32 iteration = 0; iteration < 128; ++iteration) {
            Storage actual, expected;
            for (u32 operand = 0; operand < 2; ++operand) for (u32 lane = 0; lane < 128 / bits; ++lane) {
                const u64 value = iteration < 25 ? edges[(iteration / (operand ? 5 : 1) + lane) % 5] : Random(seed);
                const auto offset = operand * 16 + lane * bits / 8;
                std::memcpy(actual.state->uniform_buffer_begin + offset, &value, bits / 8);
                std::memcpy(expected.state->uniform_buffer_begin + offset, &value, bits / 8);
            }
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(compiled.fn(actual.state) == reference.Run(), "vector compare/multiply boundary halt");
            for (u32 offset : {32u, 40u, 48u, 56u})
                Check(actual.Get(offset) == expected.Get(offset), "vector compare/multiply boundary values and dependent add");
        }
    }
    std::cout << "PASS vector signed comparison and multiply edge/random lanes\n";
}

void VectorShiftBoundaries() {
    u64 seed = 0x1739acfeb23567ULL;
    const std::array ops{OpCode::VecShiftLeft, OpCode::VecShiftRight, OpCode::VecShiftRightArithmetic};
    const std::array immediate_ops{OpCode::VecShiftLeftImm, OpCode::VecShiftRightImm, OpCode::VecShiftRightArithmeticImm};
    for (u32 bits : {8u, 16u, 32u, 64u}) for (u32 kind = 0; kind < 3; ++kind) for (bool immediate : {false, true}) {
        const std::array<u64, 9> counts{0, 1, bits - 1, bits, bits + 1, 255, 256, u64{1} << 32, UINT64_MAX};
        for (auto count : counts) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3400})}; ir::Assembler as{block.get()};
            auto source = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
            ir::Value result;
            if (immediate) result = ir::Value{block->AppendInst(immediate_ops[kind], source, ir::Imm{count}, ir::Imm{bits})}.SetType(ValueType::V128);
            else {
                auto amount = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
                result = ir::Value{block->AppendInst(ops[kind], source, amount, ir::Imm{bits})}.SetType(ValueType::V128);
            }
            as.StoreUniform(ir::Uniform{32, ValueType::V128}, result); block->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled compiled{block.get()};
            for (u32 iteration = 0; iteration < 16; ++iteration) {
                Storage actual, expected;
                for (u32 offset : {0u, 8u}) { const auto input = iteration == 0 ? UINT64_MAX : Random(seed); actual.Put(offset, input); expected.Put(offset, input); }
                actual.Put(16, count); expected.Put(16, count);
                actual.Put(24, UINT64_MAX); expected.Put(24, UINT64_MAX);
                backend::interp::Interpreter reference{*expected.state, block.get()};
                Check(compiled.fn(actual.state) == reference.Run(), "packed shift boundary halt");
                Check(actual.Get(32) == expected.Get(32) && actual.Get(40) == expected.Get(40),
                      std::string{ir::GetIRMetaInfo(immediate ? immediate_ops[kind] : ops[kind]).name} + "/" + std::to_string(bits) + "/" + std::to_string(count));
            }
        }
    }
    std::cout << "PASS packed shift widths and full 64-bit count boundaries\n";
}

void VectorWidenAndPack() {
    u64 seed = 0x364387efa12398ULL;
    for (u32 bits : {8u, 16u, 32u}) for (u32 sign : {0u, 1u}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3500})}; ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto right = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto result = as.VecMulWiden(left, right, ir::Imm{bits}, ir::Imm{sign}).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, result); block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        const u64 mask = (u64{1} << bits) - 1;
        const std::array<u64, 5> edges{0, 1, mask, u64{1} << (bits - 1), (u64{1} << (bits - 1)) - 1};
        for (u32 iteration = 0; iteration < 128; ++iteration) {
            Storage actual, expected;
            for (u32 operand = 0; operand < 2; ++operand) for (u32 lane = 0; lane < 128 / bits; ++lane) {
                const u64 value = iteration < 25 ? edges[(operand ? iteration / 5 : iteration % 5)] : Random(seed);
                const u64 input = lane & 1 ? ~value : value;
                const auto offset = operand * 16 + lane * bits / 8;
                std::memcpy(actual.state->uniform_buffer_begin + offset, &input, bits / 8);
                std::memcpy(expected.state->uniform_buffer_begin + offset, &input, bits / 8);
            }
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(compiled.fn(actual.state) == reference.Run(), "even-lane widening multiply halt");
            Check(actual.Get(32) == expected.Get(32) && actual.Get(40) == expected.Get(40), "even-lane widening multiply values");
        }
    }
    for (u32 bits : {16u, 32u}) for (u32 unsigned_result : {0u, 1u}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3600})}; ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto right = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto result = as.VecPack(left, right, ir::Imm{bits}, ir::Imm{unsigned_result}).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, result); block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        const std::array<s64, 15> edges{-2147483648LL, -65536, -32769, -32768, -1, 0, 1,
                                      127, 255, 256, 32767, 32768, 65535, 65536, 2147483647};
        for (u32 iteration = 0; iteration < 128; ++iteration) {
            Storage actual, expected;
            for (u32 operand = 0; operand < 2; ++operand) for (u32 lane = 0; lane < 128 / bits; ++lane) {
                const auto value = iteration < edges.size() ? static_cast<u64>(edges[(iteration + lane + operand * 3) % edges.size()]) : Random(seed);
                const auto offset = operand * 16 + lane * bits / 8;
                std::memcpy(actual.state->uniform_buffer_begin + offset, &value, bits / 8);
                std::memcpy(expected.state->uniform_buffer_begin + offset, &value, bits / 8);
            }
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(compiled.fn(actual.state) == reference.Run(), "signed-source saturating pack halt");
            Check(actual.Get(32) == expected.Get(32) && actual.Get(40) == expected.Get(40), "signed-source saturating pack values");
        }
    }
    std::cout << "PASS widening multiply and signed/unsigned saturating pack\n";
}

void MemoryBounds() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4000})};
    ir::Assembler as{block.get()};
    auto address = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto value = as.LoadMemory(ir::Operand{address}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{8, ValueType::U64}, value);
    as.StoreMemory(ir::Operand{address}, value);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{block.get()};
    std::array<u64, 4> memory{0x123456789abcdef0ULL, 22, 33, 44};
    Storage actual;
    actual.state->pt = memory.data(); actual.state->guest_addr_limit = sizeof(memory);
    actual.Put(0, 0);
    Check(compiled.fn(actual.state) == HaltReason::CallHost && actual.Get(8) == memory[0], "biased native memory load/store");
    actual.state->halt_reason = HaltReason::None; actual.Put(0, sizeof(memory) - 7);
    Check(compiled.fn(actual.state) == HaltReason::PageFatal, "full memory extent checked");
    actual.state->halt_reason = HaltReason::None; actual.state->guest_addr_mask = 31; actual.Put(0, UINT64_MAX - 31);
    Check(compiled.fn(actual.state) == HaltReason::CallHost && actual.Get(8) == memory[0], "guest mask precedes memory bias");
    std::cout << "PASS native memory mapping and boundaries\n";
}

void MemoryAlignment() {
    for (u64 mask : {u64{0}, u64{1}, u64{3}, u64{7}, u64{15}}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4200})};
        ir::Assembler as{block.get()};
        auto address = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        as.CheckMemoryAlignment(address, ir::Imm{mask});
        auto committed = as.LoadImm(ir::Imm{u64{42}}).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{8, ValueType::U64}, committed);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u64 offset = 0; offset < 32; ++offset) {
            Storage actual; actual.Put(0, 0x1000 + offset);
            const bool aligned = ((0x1000 + offset) & mask) == 0;
            Check(compiled.fn(actual.state) == (aligned ? HaltReason::CallHost : HaltReason::PageFatal),
                  "alignment immediate is an address mask");
            Check(actual.Get(8) == (aligned ? 42 : 0), "unaligned guard rejects before committing following IR");
            Check(actual.state->current_loc == 0x4200, "alignment rejection retains faulting block PC");
        }
    }
    std::cout << "PASS native alignment masks and rejected accesses\n";
}

void PreciseFaultLocations() {
    for (u32 advances : {1u, 2u}) for (u32 kind = 0; kind < 5; ++kind) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4300})}; ir::Assembler as{block.get()};
        auto address = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto value = as.LoadImm(ir::Imm{u64{1}}).SetType(ValueType::U64);
        for (u32 index = 0; index < advances; ++index) as.AdvancePC(ir::Imm{u32{4}});
        if (kind == 0) (void)as.LoadMemory(ir::Operand{address}).SetType(ValueType::U64);
        else if (kind == 1) as.StoreMemory(ir::Operand{address}, value);
        else if (kind == 2) (void)as.AtomicFetchAdd(address, value).SetType(ValueType::U64);
        else if (kind == 3) as.CheckMemoryAlignment(address, ir::Imm{u64{7}});
        else {
            auto state = as.LoadUniform(ir::Uniform{16, ValueType::U64}).SetType(ValueType::U64);
            (void)as.CallHost(&HaltAtPublishedLocation, state).SetType(ValueType::U64);
        }
        as.StoreUniform(ir::Uniform{8, ValueType::U64}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get()};
        Storage actual; actual.Put(0, 9); actual.Put(16, reinterpret_cast<u64>(actual.state));
        actual.state->guest_addr_limit = 8;
        Check(compiled.fn(actual.state) == HaltReason::PageFatal, "later guest instruction halts before committing output");
        Check(actual.Get(8) == 0, "fault exit prevents following IR commits");
        Check(actual.state->current_loc == 0x4300 + advances * 4, "second/third instruction publishes precise fault PC");
    }
    for (bool dynamic : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4380})}; ir::Assembler as{block.get()};
        as.AdvancePC(ir::Imm{u32{4}});
        auto explicit_location = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        if (dynamic) as.SetLocation(ir::Lambda{explicit_location});
        else as.SetLocation(ir::Lambda{ir::Imm{u64{0x9988}}});
        auto address = as.LoadImm(ir::Imm{u64{9}}).SetType(ValueType::U64);
        (void)as.LoadMemory(ir::Operand{address}).SetType(ValueType::U64);
        block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get()};
        Storage actual; actual.Put(0, 0x9988); actual.state->guest_addr_limit = 8;
        Check(compiled.fn(actual.state) == HaltReason::PageFatal && actual.state->current_loc == 0x9988,
              "explicit immediate/dynamic SetLocation supersedes pending metadata publication");
    }
    std::cout << "PASS precise later-instruction bounds, alignment, atomic and helper PCs\n";
}

void Atomics() {
    const std::array ops{OpCode::CompareAndSwap, OpCode::AtomicExchange, OpCode::AtomicFetchAdd};
    u64 seed = 0x128489240127394ULL;
    for (auto op : ops) for (auto type : {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4400})}; ir::Assembler as{block.get()};
        auto address = as.LoadImm(ir::Imm{u64{0}}).SetType(ValueType::U64);
        auto operand = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto desired = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        ir::Value result;
        if (op == OpCode::CompareAndSwap) result = as.CompareAndSwap(address, operand, desired).SetType(type);
        else result = ir::Value{block->AppendInst(op, address, operand)}.SetType(type);
        as.StoreUniform(ir::Uniform{16, type}, result); block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u32 iteration = 0; iteration < 64; ++iteration) {
            alignas(16) u64 actual_memory = Random(seed), expected_memory = actual_memory;
            Storage actual, expected; actual.state->pt = &actual_memory; expected.state->pt = &expected_memory;
            for (u32 offset : {0u, 8u}) {
                const auto value = offset == 0 && iteration & 1 ? actual_memory : Random(seed);
                actual.Put(offset, value); expected.Put(offset, value);
            }
            backend::interp::Interpreter reference{*expected.state, block.get()}; const auto wanted = reference.Run();
            Check(compiled.fn(actual.state) == wanted, "native atomic halt");
            Check(actual_memory == expected_memory && actual.Get(16) == expected.Get(16), "native atomic observed and replacement values");
        }
    }
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4500})}; ir::Assembler as{block.get()};
    auto address = as.LoadImm(ir::Imm{u64{0}}).SetType(ValueType::U64);
    auto one = as.LoadImm(ir::Imm{u64{1}}).SetType(ValueType::U64);
    (void)as.AtomicFetchAdd(address, one).SetType(ValueType::U64); block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{block.get()}; alignas(16) u64 counter{};
    std::array<std::thread, 4> threads;
    for (auto& thread : threads) thread = std::thread{[&] {
        Storage actual; actual.state->pt = &counter;
        for (u32 iteration = 0; iteration < 2000; ++iteration) {
            actual.state->halt_reason = HaltReason::None; compiled.fn(actual.state);
        }
    }};
    for (auto& thread : threads) thread.join();
    Check(counter == 8000, "four concurrent native LOCK XADD threads retain all increments");
    std::cout << "PASS native atomics and concurrent LOCK XADD\n";
}

void FloatDifferential() {
    const std::array ops{OpCode::VecFAdd, OpCode::VecFSub, OpCode::VecFMul, OpCode::VecFDiv, OpCode::VecFCmpMask};
    const std::array<u64, 12> single{0, 0x80000000, 0x3f800000, 0xbf800000, 1, 0x7f7fffff,
        0x7f800000, 0xff800000, 0x7fc12345, 0x7f812345, 0x3f000000, 0xc0800000};
    const std::array<u64, 12> dual{0, 0x8000000000000000ULL, 0x3ff0000000000000ULL, 0xbff0000000000000ULL,
        1, 0x7fefffffffffffffULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
        0x7ff80123456789abULL, 0x7ff00123456789abULL, 0x3fe0000000000000ULL, 0xc010000000000000ULL};
    for (auto op : ops) for (u32 bits : {32u, 64u}) for (u32 relation = 0; relation < (op == OpCode::VecFCmpMask ? 16u : 1u); ++relation) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4600})}; ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto right = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        ir::Value result;
        if (op == OpCode::VecFCmpMask) result = as.VecFCmpMask(left, right, ir::Imm{bits}, ir::Imm{relation}, ir::Imm{u32{0}}).SetType(ValueType::V128);
        else result = ir::Value{block->AppendInst(op, left, right, ir::Imm{bits})}.SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, result); block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u32 iteration = 0; iteration < 144; ++iteration) {
            Storage actual, expected;
            for (u32 operand = 0; operand < 2; ++operand) for (u32 lane = 0; lane < 128 / bits; ++lane) {
                const auto index = (operand ? iteration / 12 : iteration % 12);
                const auto value = bits == 32 ? single[index] : dual[index];
                const auto offset = operand * 16 + lane * bits / 8;
                std::memcpy(actual.state->uniform_buffer_begin + offset, &value, bits / 8);
                std::memcpy(expected.state->uniform_buffer_begin + offset, &value, bits / 8);
            }
            backend::interp::Interpreter reference{*expected.state, block.get()}; const auto wanted = reference.Run();
            Check(compiled.fn(actual.state) == wanted, "native FP halt");
            Check(actual.Get(32) == expected.Get(32) && actual.Get(40) == expected.Get(40),
                std::string{ir::GetIRMetaInfo(op).name} + "/" + std::to_string(bits) + "/" + std::to_string(iteration));
        }
    }
    std::cout << "PASS packed FP edge cases and all 16 IEEE relation sets\n";
}

#include "floating_test.inc"
#include "wide_test.inc"
#include "crc_test.inc"
#include "crc_ieee_test.inc"
#include "scalar_view_test.inc"
#include "uniform_store_view_test.inc"
#include "sat_shift_test.inc"
#include "compare64_test.inc"
#include "mulpacked_test.inc"
#include "vector_container_test.inc"
#include "unsigned_estimate_test.inc"
#include "sataddsub_test.inc"
#include "crypto_test.inc"
#include "sha_test.inc"
#include "sse42str_test.inc"
#include "memory_copy_test.inc"
#include "atomic128_test.inc"
#include "host_uniform_test.inc"
#include "dot_test.inc"
#include "sat_mul_test.inc"
#include "pseudo_test.inc"
#include "x87_test.inc"

void RuntimeDispatchAndFaults() {
    auto config = TestConfig();
    backend::AddressSpace space{config}; auto module = space.GetDefaultModule();
    IntrusivePtr<ir::Block> first{new ir::Block(ir::Location{0x5000})};
    first->SetTerminal(ir::terminal::LinkBlock{ir::Location{0x6000}});
    IntrusivePtr<ir::Block> last{new ir::Block(ir::Location{0x6000})};
    ir::Assembler as{last.get()};
    auto result = as.LoadImm(ir::Imm{u64{42}}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{0, ValueType::U64}, result);
    last->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(first.get()) && module->Push(last.get()), "publish runtime blocks");
    Check(backend::TranslateIR(module, first) && backend::TranslateIR(module, last), "compile native x64 runtime blocks");
    Runtime runtime{&space}; runtime.SetLocation(0x5000);
    Check(runtime.Run() == HaltReason::CallHost, "runtime dispatches x64 blocks");
    u64 observed{}; std::memcpy(&observed, runtime.GetUniformBuffer().data(), 8);
    Check(observed == 42 && runtime.GetLocation() == 0x6000, "runtime result and location");
    runtime.SignalInterrupt(); Check(runtime.Run() == HaltReason::Signal, "stopped runtime interrupt"); runtime.ClearInterrupt();
    runtime.SetLocation(0x7000); Check(runtime.Run() == HaltReason::CodeMiss, "runtime missing entry");

    IntrusivePtr<ir::Block> fault{new ir::Block(ir::Location{0x8000})};
    ir::Assembler fa{fault.get()};
    auto published = fa.LoadImm(ir::Imm{u64{0x72134}}).SetType(ValueType::U64);
    fa.StoreUniform(ir::Uniform{16, ValueType::U64}, published);
    fa.AdvancePC(ir::Imm{u32{4}});
    auto address = fa.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    fa.AdvancePC(ir::Imm{u32{4}});
    auto loaded = fa.LoadMemory(ir::Operand{address}).SetType(ValueType::U64);
    fa.StoreUniform(ir::Uniform{8, ValueType::U64}, loaded);
    fault->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(fault.get()) && backend::TranslateIR(module, fault), "publish native fault block");
    auto* inaccessible = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(inaccessible != MAP_FAILED, "allocate fault page");
    runtime.GetState()->guest_addr_limit = UINT64_MAX;
    const auto pointer = reinterpret_cast<u64>(inaccessible);
    std::memcpy(runtime.GetUniformBuffer().data(), &pointer, 8); runtime.SetLocation(0x8000);
    Check(runtime.Run() == HaltReason::PageFatal, "native SIGSEGV restores both block and dispatcher frames");
    Check(runtime.GetLocation() == 0x8008, "third-instruction native SIGSEGV retains precise PC");
    std::memcpy(&observed, runtime.GetUniformBuffer().data() + 16, 8);
    Check(observed == 0x72134, "architectural state committed before fault");
    Check(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0 &&
          runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0, "fault recovery slots cleared");
    Check(runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 &&
          backend::riscv64::anonymous_memory_readers.load() == 0, "fault recovery releases native memory reader lease");
    runtime.SetLocation(0x5000); Check(runtime.Run() == HaltReason::CallHost, "runtime reusable after fault");
    alignas(16) u64 accessible{};
    for (u32 path = 0; path < 2; ++path) {
        runtime.GetState()->interp_range_check = &FaultRangeProbe;
        runtime.GetState()->interp_range_check_ctx = path ? static_cast<void*>(&accessible) : inaccessible;
        std::memcpy(runtime.GetUniformBuffer().data(), &pointer, 8); runtime.SetLocation(0x8000);
        const auto original = _mm_getcsr(); const auto changed = (original & ~0x6000u) | 0x6000u;
        fault_range_calls.store(0, std::memory_order_relaxed);
        _mm_setcsr(changed); const auto reason = runtime.Run(); const auto after = _mm_getcsr(); _mm_setcsr(original);
        Check(reason == HaltReason::PageFatal, "fault recovers inside and after a host range callback");
        Check(runtime.GetLocation() == 0x8008, "third-instruction range callback fault retains precise PC");
        Check(fault_range_calls.load(std::memory_order_relaxed) == 1,
              "native fault path enters range callback before protection fault");
        Check(after == changed, "host MXCSR recovered when abandoning a helper frame");
        Check(backend::riscv64::anonymous_memory_readers.load() == 0, "range callback fault leaves no memory readers");
    }
    runtime.GetState()->interp_range_check = nullptr; runtime.GetState()->interp_range_check_ctx = nullptr;
    IntrusivePtr<ir::Block> second_fault{new ir::Block(ir::Location{0x8100})}; ir::Assembler sf{second_fault.get()};
    auto second_address = sf.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    sf.AdvancePC(ir::Imm{u32{4}});
    (void)sf.LoadMemory(ir::Operand{second_address}).SetType(ValueType::U64);
    second_fault->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(second_fault.get()) && backend::TranslateIR(module, second_fault), "publish second-instruction native fault block");
    runtime.SetLocation(0x8100);
    Check(runtime.Run() == HaltReason::PageFatal && runtime.GetLocation() == 0x8104,
          "second-instruction native SIGSEGV retains precise PC");
    munmap(inaccessible, 4096);
    std::cout << "PASS Runtime dispatch, interruption and native fault recovery\n";
}

void RunningInterrupt() {
    auto config = TestConfig(); backend::AddressSpace space{config}; auto module = space.GetDefaultModule();
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x9000})};
    ir::Assembler as{block.get()};
    auto one = as.LoadImm(ir::Imm{u64{1}}).SetType(ValueType::U8);
    auto jump = as.Goto(ir::BOOL{one}); as.BindLabel(jump);
    auto* label = &block->GetInstList().back(); block->RemoveInst(label); block->InsertBefore(label, one.Def());
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(block.get()) && backend::TranslateIR(module, block), "compile local backedge");
    Runtime runtime{&space}; runtime.SetLocation(0x9000);
    std::thread interrupter{[&] { std::this_thread::sleep_for(std::chrono::milliseconds{10}); runtime.SignalInterrupt(); }};
    const auto reason = runtime.Run(); interrupter.join();
    Check(reason == HaltReason::Signal, "running native local loop polls interrupt");
    runtime.ClearInterrupt();
    std::cout << "PASS running local-loop interruption\n";
}

void NativeAbi() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa000})};
    block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get()}; Storage actual;
    const auto original = _mm_getcsr(); const auto changed = (original & ~0x6000u) | 0x6000u;
    _mm_setcsr(changed); compiled.fn(actual.state);
    const auto after = _mm_getcsr(); _mm_setcsr(original);
    Check(after == changed, "native block preserves host MXCSR rounding and exception status");
    actual.state->halt_reason = HaltReason::None; actual.state->exit_request = 1;
    Check(compiled.fn(actual.state) == HaltReason::CodeMiss, "low exit-request bits preserve SMC dispatch reason");
    std::cout << "PASS host FP environment and SMC exit reason\n";
}

void UnsupportedIR() {
    IntrusivePtr<ir::Block> wide{new ir::Block(ir::Location{0xb000})};
    ir::Assembler wa{wide.get()};
    (void)wa.LoadUniform(ir::Uniform{0, ValueType::V256}).SetType(ValueType::V256);
    wide->SetTerminal(ir::terminal::ReturnToHost{});
    bool rejected{};
    try { Compiled compiled{wide.get()}; } catch (const std::runtime_error& error) {
        rejected = std::string{error.what()}.find("V256") != std::string::npos;
    }
    Check(rejected, "V256 rejected before publishing native code");

    IntrusivePtr<ir::Block> source{new ir::Block(ir::Location{0xb100})};
    ir::Assembler sa{source.get()};
    auto foreign = sa.LoadImm(ir::Imm{u64{42}}).SetType(ValueType::U64);
    IntrusivePtr<ir::Block> consumer{new ir::Block(ir::Location{0xb200})};
    ir::Assembler ca{consumer.get()};
    ca.StoreUniform(ir::Uniform{0, ValueType::U64}, foreign);
    consumer->SetTerminal(ir::terminal::ReturnToHost{});
    rejected = false;
    try { Compiled compiled{consumer.get()}; } catch (const std::runtime_error& error) {
        rejected = std::string{error.what()}.find("block-local SSA") != std::string::npos;
    }
    Check(rejected, "cross-block SSA rejected instead of reading an uninitialized stack home");
    std::cout << "PASS unsupported native IR rejection\n";
}

void ReusedStackHomes() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc000})};
    ir::Assembler as{block.get()};
    auto first = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto condition = as.TestNotZero(first).SetType(ValueType::U8);
    auto producer = as.Sub(first, ir::Operand{ir::Imm{u64{1}}}).SetType(ValueType::U64);
    auto value = producer;
    for (u32 i = 0; i < 256; ++i)
        value = as.Add(value, ir::Operand{ir::Imm{u64{17}}}).SetType(ValueType::U64);
    as.SaveFlags(producer, ir::Flags::All);
    as.StoreUniform(ir::Uniform{8, ValueType::U64}, value);
    as.StoreUniform(ir::Uniform{16, ValueType::U64}, first);
    block->SetTerminal(ir::terminal::If{ir::BOOL{condition}, ir::terminal::LinkBlock{ir::Location{0xc100}},
                                      ir::terminal::LinkBlock{ir::Location{0xc200}}});
    Compiled compiled{block.get()};
    Check(compiled.translator.FrameSize() <= 256, "straight-line stack frame follows live values instead of instruction count");
    for (u64 input : {u64{0}, u64{1}, u64{2}, UINT64_MAX, u64{1} << 63}) {
        Storage actual, expected; actual.Put(0, input); expected.Put(0, input);
        actual.state->host_cpu_flags = expected.state->host_cpu_flags = UINT64_MAX;
        backend::interp::Interpreter reference{*expected.state, block.get()};
        Check(compiled.fn(actual.state) == reference.Run(), "reused stack terminal halt");
        Check(actual.Get(8) == expected.Get(8) && actual.Get(16) == expected.Get(16), "long-lived scalar survives recycled homes");
        Check(actual.state->host_cpu_flags == expected.state->host_cpu_flags, "delayed producer RFLAGS survives recycled homes");
        Check(actual.state->current_loc == expected.state->current_loc, "terminal-only condition survives recycled homes");
    }
    std::cout << "PASS liveness stack reuse and deferred flags\n";
}

void ForwardedVectors() {
    for (u32 bits : {8u, 16u, 32u, 64u}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc300})}; ir::Assembler as{block.get()};
        auto value = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto other = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        for (u32 i = 0; i < 32; ++i) {
            value = as.VecAdd(value, other, ir::Imm{bits}).SetType(ValueType::V128);
            value = as.VecXor(value, other).SetType(ValueType::V128);
            value = as.VecSub(value, other, ir::Imm{bits}).SetType(ValueType::V128);
        }
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, value); block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()}; u64 seed = 0x87ac1300159844ULL;
        for (u32 iteration = 0; iteration < 32; ++iteration) {
            Storage actual, expected;
            for (u32 offset : {0u, 8u, 16u, 24u}) { const auto input = Random(seed); actual.Put(offset, input); expected.Put(offset, input); }
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(compiled.fn(actual.state) == reference.Run(), "forwarded SSE chain halt");
            Check(actual.Get(32) == expected.Get(32) && actual.Get(40) == expected.Get(40), "forwarded SSE chain matches interpreter");
        }
    }
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc400})}; ir::Assembler as{block.get()};
    auto left = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto right = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    auto mask = as.VecFCmpMask(left, right, ir::Imm{u32{32}}, ir::Imm{u32{1}}, ir::Imm{u32{0}}).SetType(ValueType::V128);
    auto value = as.VecAdd(mask, right, ir::Imm{u32{32}}).SetType(ValueType::V128);
    as.StoreUniform(ir::Uniform{32, ValueType::V128}, value); block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{block.get()}; Storage actual, expected;
    for (u32 offset : {0u, 8u}) { actual.Put(offset, 0x3f8000003f800000ULL); expected.Put(offset, 0x3f8000003f800000ULL); }
    for (u32 offset : {16u, 24u}) { actual.Put(offset, 0x4000000040000000ULL); expected.Put(offset, 0x4000000040000000ULL); }
    backend::interp::Interpreter reference{*expected.state, block.get()};
    Check(compiled.fn(actual.state) == reference.Run(), "lane-written comparison result halt");
    Check(actual.Get(32) == expected.Get(32) && actual.Get(40) == expected.Get(40), "forwarding excludes intermediate vector writes");
    std::cout << "PASS chained vector forwarding\n";
}
}

int main() {
    try {
        ScalarDifferential(); Conditions(); VectorDifferential(); VectorCompareAndMultiplyBoundaries(); VectorShiftBoundaries(); VectorWidenAndPack(); FloatDifferential(); MemoryBounds(); MemoryAlignment(); PreciseFaultLocations(); Atomics();
        NativeAbi(); UnsupportedIR(); ReusedStackHomes(); ForwardedVectors(); RoundIntegralNativeAndBaseline(); FusedMultiplyAddNativeAndBaseline();
        CpuidPairAndDelayedUpper(); WideDivisionNativeAndFallback(); Crc32cNativeAndBaseline(); AesNativeAndBaseline(); AesNistFullCipher(); AesKeygenNativeAndBaseline(); PolynomialNativeAndBaseline();
        IeeeCrcEveryWidthAndAliases(); IeeeCrcByteOrderAndKnownValue(); IeeeCrcScalarViews(); IeeeCrcInvalidIR();
        ScalarInputViewsArithmeticAndPredicates(); ScalarInputViewsLocalJumpsAndTerminals();
        UniformStoreScalarViewsAndNeighbors(); UniformStoreVectorViewsAndNeighbors(); UniformStoreSetTypeRetainsInputView();
        SatShiftAllShapesAndBoundaries(); SatShiftProducerTimeAndPressure(); SatShiftInvalidIR(); SatShiftForwardedVectors();
        Compare64BoundariesAndAliases(); Compare64ForwardedChains(); CompareWidthWholeBlockValidation();
        MulPackedAllBytesAndDwordBoundaries(); MulPackedForwardingAndPressure(); MulPackedWidthValidation();
        VectorContainerActualAndViewCrossproduct(); VectorContainerBitCastAndForwarding(); VectorContainerNarrowLoadsAndScalarHomes();
        UnsignedEstimateBoundariesAndForwarding(); UnsignedEstimateMalformedIR();
        SatAddSubBigIntBoundariesForwardingAndPressure(); SatAddSubAllBytePairs(); SatAddSubMalformedWholeBlock();
        SatAddSubRawContainerViews();
        ShaNativeAndBaseline(); ShaNistFullHashes(); Sse42StringNativeAndBaseline(); Sse42StringPartialFlags();
        DotBytePairsNativeAndBaseline(); DotWrapAndAliases(); DotInvalidIR();
        SatMulDirectedAndRandom(); SatMulInactiveAndAliases(); SatMulInvalidIR();
        ProducerTimePseudoResults(); ProducerTimePseudoValidation();
        X87RegisterHardware(); X87HostEnvironmentAndStack(); X87BinaryRoundingBoundariesAndNaNs(); X87InvalidIR();
        MemoryCopyOverlapAndLambdas(); MemoryCopyPreflightAndZero();
        MemoryCopyRealFaultRecovery(); MemoryCopyWriterGate(); Cas128ObservedAndRanges(); Cas128ActualFaultsAndReadonlyMismatch();
        Cas128CrossPageMatchingFaults(); Cas128InterpreterCrossPageMatchingFaults();
        HostUniformSlicesAndSnapshots(); HostUniformHelpersAndHalt(); HostUniformValidation(); HostUniformNarrowBindings(); HostUniformRuntimeFaultCommit(); HostUniformFunctionConfig();
        Cas128ContentionAndReaders(); RuntimeDispatchAndFaults(); RunningInterrupt();
        std::cout << "PASS " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
