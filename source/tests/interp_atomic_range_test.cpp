#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/atomic_fallback.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/ir/atomic_rmw.h"

using namespace swift;
using namespace swift::runtime;

namespace {
using Rmw = ir::AtomicRMWOp;
constexpr u64 kMax = std::numeric_limits<u64>::max();
constexpr u64 kFaultPc = 0x1234;
constexpr u64 kSentinel = 0x5A13B28C49D6E7F0;

struct Operation { ir::OpCode opcode; Rmw rmw{Rmw::Add}; };
std::vector<Operation> Operations() {
    std::vector<Operation> result{{ir::OpCode::CompareAndSwap},
                                  {ir::OpCode::AtomicExchange},
                                  {ir::OpCode::AtomicFetchAdd}};
    for (auto op : {Rmw::Add, Rmw::Sub, Rmw::And, Rmw::Or, Rmw::Xor, Rmw::Neg,
                    Rmw::AddCarry, Rmw::SubBorrow, Rmw::SignedMax, Rmw::SignedMin,
                    Rmw::UnsignedMax, Rmw::UnsignedMin})
        result.push_back({ir::OpCode::AtomicRMW, op});
    return result;
}
constexpr std::array<ir::ValueType, 4> kTypes{
    ir::ValueType::U8, ir::ValueType::U16, ir::ValueType::U32, ir::ValueType::U64};
u64 WidthMask(u32 bytes) { return bytes == 8 ? kMax : (u64{1} << (bytes * 8)) - 1; }

// Independent bit-pattern model: widened unsigned arithmetic avoids signed
// overflow, while sign/magnitude comparison covers the signed extrema.
u64 ExpectedMemory(Operation operation, u32 bytes, u64 old, u64 rhs, u64 expected, u64 carry) {
    const u64 mask = WidthMask(bytes);
    old &= mask; rhs &= mask; expected &= mask;
    if (operation.opcode == ir::OpCode::CompareAndSwap) return old == expected ? rhs : old;
    if (operation.opcode == ir::OpCode::AtomicExchange) return rhs;
    if (operation.opcode == ir::OpCode::AtomicFetchAdd) return (old + rhs) & mask;
    switch (operation.rmw) {
        case Rmw::Add: return (old + rhs) & mask;
        case Rmw::Sub: return (old - rhs) & mask;
        case Rmw::And: return old & rhs;
        case Rmw::Or: return old | rhs;
        case Rmw::Xor: return old ^ rhs;
        case Rmw::Neg: return (u64{0} - old) & mask;
        case Rmw::AddCarry: return (old + rhs + carry) & mask;
        case Rmw::SubBorrow: return (old - rhs - carry) & mask;
        default: {
            bool greater = old > rhs;
            if (operation.rmw == Rmw::SignedMax || operation.rmw == Rmw::SignedMin) {
                const u64 sign = u64{1} << (bytes * 8 - 1);
                const bool old_negative = old & sign, rhs_negative = rhs & sign;
                if (old_negative != rhs_negative) greater = !old_negative;
            }
            const bool maximum = operation.rmw == Rmw::SignedMax || operation.rmw == Rmw::UnsignedMax;
            return maximum == greater ? old : rhs;
        }
    }
}

struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 128> storage{};
    backend::State* state = new (storage.data()) backend::State{};
    Machine() {
        std::memset(state->uniform_buffer_begin, 0xD9, 128);
        state->current_loc = ir::Location{kFaultPc};
        state->host_cpu_flags = 0xB0000000;
    }
    ~Machine() { state->~State(); }
};

struct Program {
    ir::Block block{0, kFaultPc};
    ir::Inst* atomic{};
    std::array<u64, 32> values{};
    Program(Operation operation, ir::ValueType type, u64 address, u64 rhs, u64 expected, u64 carry,
            bool fault_boundary = false) {
        ir::Assembler assembler{&block};
        const auto addr = assembler.LoadImm<ir::U64>(ir::Imm{address}).SetType(ir::ValueType::U64);
        const auto input = assembler.LoadImm<ir::U64>(ir::Imm{rhs}).SetType(type);
        const auto compare = assembler.LoadImm<ir::U64>(ir::Imm{expected}).SetType(type);
        const auto carry_value = assembler.LoadImm<ir::U64>(ir::Imm{carry}).SetType(ir::ValueType::U64);
        if (fault_boundary) {
            const auto committed = assembler.LoadImm<ir::U64>(ir::Imm{kSentinel}).SetType(ir::ValueType::U64);
            assembler.StoreUniform(ir::Uniform{32, ir::ValueType::U64}, committed);
            assembler.AdvancePC(ir::Imm{u32{4}});
            // The existing interpreter publishes PC through SetLocation;
            // AdvancePC itself is only a decoder progress marker.
            assembler.SetLocation(ir::Lambda{ir::Imm{kFaultPc}});
        }
        switch (operation.opcode) {
            case ir::OpCode::CompareAndSwap:
                atomic = assembler.CompareAndSwap(addr, compare, input).SetType(type).Def(); break;
            case ir::OpCode::AtomicExchange:
                atomic = assembler.AtomicExchange(addr, input).SetType(type).Def(); break;
            case ir::OpCode::AtomicFetchAdd:
                atomic = assembler.AtomicFetchAdd(addr, input).SetType(type).Def(); break;
            default:
                atomic = assembler.AtomicRMW(ir::Imm{static_cast<u32>(operation.rmw)}, addr,
                                             input, carry_value).SetType(type).Def(); break;
        }
        assembler.ReturnToHost();
        values.fill(kSentinel);
    }
    void Prime(backend::State& state) {
        backend::interp::Interpreter interpreter{state, &block};
        backend::interp::InterpStack stack{values};
        for (auto& inst : block.GetInstList()) if (&inst != atomic) interpreter.Run(&inst, stack);
    }
    void Execute(backend::State& state) {
        backend::interp::Interpreter interpreter{state, &block};
        backend::interp::InterpStack stack{values};
        interpreter.Run(atomic, stack);
    }
    u64 Result() const { return values[size_t(atomic->Id()) * 2]; }
};

struct GuardSnapshot {
    unsigned readers = backend::riscv64::anonymous_memory_readers.load();
    unsigned depth = backend::riscv64::memory_kernel_depth.load();
    u32 gate = backend::unaligned_atomic_lock.load();
    bool owns = backend::owns_unaligned_atomic_guard.load();
    bool Unchanged() const {
        return readers == backend::riscv64::anonymous_memory_readers.load() &&
               depth == backend::riscv64::memory_kernel_depth.load() &&
               gate == backend::unaligned_atomic_lock.load() &&
               owns == backend::owns_unaligned_atomic_guard.load();
    }
};
struct Oracle {
    GuardSnapshot guards;
    bool allow{true}, throws{}, guards_unchanged{true};
    unsigned calls{};
    u64 address{}, size{};
    static bool Check(void* opaque, u64 address, u64 size) {
        auto& self = *static_cast<Oracle*>(opaque);
        ++self.calls; self.address = address; self.size = size;
        self.guards_unchanged &= self.guards.Unchanged();
        if (self.throws) throw std::runtime_error("test mapping oracle failure");
        return self.allow;
    }
};

struct Pages {
    size_t page = size_t(sysconf(_SC_PAGESIZE));
    u8* data = static_cast<u8*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Pages() {
        REQUIRE(data != MAP_FAILED);
        for (size_t i = 0; i < page; ++i) data[i] = u8(i * 37 + 11);
        REQUIRE(mprotect(data + page, page, PROT_NONE) == 0);
    }
    ~Pages() { if (data != MAP_FAILED) munmap(data, page * 2); }
};

void CheckFailure(Operation operation, ir::ValueType type, Pages& pages, u64 address,
                  u64 limit, u64 mask, bool oracle_reject = false, bool oracle_throw = false) {
    const auto initial_memory = std::vector<u8>(pages.data, pages.data + pages.page);
    Machine machine;
    machine.state->pt = pages.data;
    machine.state->guest_addr_limit = limit;
    machine.state->guest_addr_mask = mask;
    Oracle oracle; oracle.allow = !oracle_reject; oracle.throws = oracle_throw;
    machine.state->interp_range_check = &Oracle::Check;
    machine.state->interp_range_check_ctx = &oracle;
    Program program{operation, type, address, 0x89ABCDEF01234567, 0, 1};
    program.Prime(*machine.state);
    const auto initial_values = program.values;
    std::array<u8, 128> initial_uniforms;
    std::memcpy(initial_uniforms.data(), machine.state->uniform_buffer_begin, initial_uniforms.size());
    const GuardSnapshot guards;
    program.Execute(*machine.state);
    REQUIRE(machine.state->halt_reason == (oracle_throw ? HaltReason::IllegalCode : HaltReason::PageFatal));
    REQUIRE(program.values == initial_values);
    REQUIRE(std::memcmp(pages.data, initial_memory.data(), pages.page) == 0);
    REQUIRE(std::memcmp(machine.state->uniform_buffer_begin, initial_uniforms.data(), initial_uniforms.size()) == 0);
    REQUIRE(machine.state->current_loc.Value() == kFaultPc);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    REQUIRE(guards.Unchanged());
    REQUIRE(oracle.guards_unchanged);
    REQUIRE(oracle.calls == (oracle_reject || oracle_throw ? 1 : 0));
    if (oracle.calls) {
        REQUIRE(oracle.address == (address & mask));
        REQUIRE(oracle.size == ir::GetValueSizeByte(type));
    }
}
}  // namespace

TEST_CASE("interpreter scalar atomics reject complete guest extents before acquiring guards") {
    Pages pages;
    for (auto operation : Operations()) for (auto type : kTypes) {
        const u64 bytes = ir::GetValueSizeByte(type);
        INFO("opcode=" << u32(operation.opcode) << " rmw=" << u32(operation.rmw) << " bytes=" << bytes);
        // The next page is genuinely inaccessible; an unchecked path crashes.
        CheckFailure(operation, type, pages, pages.page, pages.page, kMax);
        CheckFailure(operation, type, pages, pages.page + 1, pages.page, kMax);
        CheckFailure(operation, type, pages, pages.page - bytes + 1, pages.page, kMax);
        CheckFailure(operation, type, pages, 0, 0, kMax);
        CheckFailure(operation, type, pages, kMax - bytes + 1, kMax, kMax);
        if (bytes > 1) {
            CheckFailure(operation, type, pages, kMax - bytes + 2, kMax, kMax);
            CheckFailure(operation, type, pages, pages.page - bytes + 1, kMax, pages.page - 1);
        }
        // Numeric bounds permit this page; the oracle rejects before dereference.
        CheckFailure(operation, type, pages, pages.page, pages.page * 2, kMax, true);
        CheckFailure(operation, type, pages, pages.page, pages.page * 2, kMax, false, true);
    }
}

TEST_CASE("interpreter scalar atomics preserve masked aligned and unaligned update semantics") {
    Pages pages;
    for (auto operation : Operations()) for (auto type : kTypes) {
        const u32 bytes = ir::GetValueSizeByte(type);
        for (u64 address : {u64{32}, u64{33}, u64(pages.page - bytes)})
        for (bool masked : {false, true})
        for (bool oracle_enabled : {false, true}) for (bool match : {false, true})
        for (u64 carry : {u64{0}, u64{1}}) {
            INFO("opcode=" << u32(operation.opcode) << " rmw=" << u32(operation.rmw)
                 << " bytes=" << bytes << " address=" << address << " oracle=" << oracle_enabled);
            const u64 old = 0xFEDCBA9876543281 & WidthMask(bytes);
            const u64 rhs = 0x0123456789ABCDE7;
            const u64 expected = old ^ (match ? 0 : 1);
            std::memcpy(pages.data + address, &old, bytes);
            auto expected_memory = std::vector<u8>(pages.data, pages.data + pages.page);
            const auto wanted = ExpectedMemory(operation, bytes, old, rhs, expected, carry);
            std::memcpy(expected_memory.data() + address, &wanted, bytes);
            Machine machine;
            machine.state->pt = pages.data;
            machine.state->guest_addr_limit = pages.page;
            machine.state->guest_addr_mask = masked ? pages.page - 1 : kMax;
            Oracle oracle;
            if (oracle_enabled) {
                machine.state->interp_range_check = &Oracle::Check;
                machine.state->interp_range_check_ctx = &oracle;
            }
            // High guest bits must be removed before bounds and oracle checks.
            Program program{operation, type, address + (masked ? pages.page * 17 : 0), rhs, expected, carry};
            program.Prime(*machine.state);
            const GuardSnapshot guards;
            program.Execute(*machine.state);
            REQUIRE(machine.state->halt_reason == HaltReason::None);
            REQUIRE(program.Result() == old);
            REQUIRE(std::memcmp(pages.data, expected_memory.data(), pages.page) == 0);
            for (size_t i = 0; i < 128; ++i) REQUIRE(machine.state->uniform_buffer_begin[i] == 0xD9);
            REQUIRE(machine.state->current_loc.Value() == kFaultPc);
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
            REQUIRE(guards.Unchanged());
            REQUIRE(oracle.guards_unchanged);
            REQUIRE(oracle.calls == unsigned(oracle_enabled));
            if (oracle_enabled) {
                REQUIRE(oracle.address == address);
                REQUIRE(oracle.size == bytes);
            }
        }
    }
}

TEST_CASE("interpreter atomic faults stop a block before result and following uniform stores") {
    Pages pages;
    for (auto operation : Operations()) for (auto type : kTypes) {
        Machine machine;
        machine.state->current_loc = ir::Location{kFaultPc - 4};
        machine.state->pt = pages.data;
        machine.state->guest_addr_limit = pages.page * 2;
        Oracle oracle; oracle.allow = false;
        machine.state->interp_range_check = &Oracle::Check;
        machine.state->interp_range_check_ctx = &oracle;
        Program program{operation, type, pages.page, 0x89ABCDEF01234567, 0, 1, true};
        ir::Assembler assembler{&program.block};
        assembler.StoreUniform(ir::Uniform{0, type}, ir::Value{program.atomic}.SetType(type));
        const auto marker = assembler.LoadImm<ir::U64>(ir::Imm{kSentinel}).SetType(ir::ValueType::U64);
        assembler.StoreUniform(ir::Uniform{16, ir::ValueType::U64}, marker);
        assembler.ReturnToHost();
        program.block.ReIdInstr();
        const auto initial_memory = std::vector<u8>(pages.data, pages.data + pages.page);
        std::array<u8, 128> expected_uniforms;
        expected_uniforms.fill(0xD9);
        std::memcpy(expected_uniforms.data() + 32, &kSentinel, sizeof(kSentinel));
        const GuardSnapshot guards;
        backend::interp::Interpreter interpreter{*machine.state, &program.block};
        REQUIRE(interpreter.Run() == HaltReason::PageFatal);
        REQUIRE(std::memcmp(machine.state->uniform_buffer_begin, expected_uniforms.data(), expected_uniforms.size()) == 0);
        REQUIRE(std::memcmp(pages.data, initial_memory.data(), pages.page) == 0);
        REQUIRE(machine.state->current_loc.Value() == kFaultPc);
        REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        REQUIRE(guards.Unchanged());
        REQUIRE(oracle.guards_unchanged);
        REQUIRE(oracle.calls == 1);
        REQUIRE(oracle.address == pages.page);
        REQUIRE(oracle.size == ir::GetValueSizeByte(type));
    }
}
