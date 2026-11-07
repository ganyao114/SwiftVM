#include "tests/support/check.h"
#include "tests/support/test_mode.h"
#include "tests/support/native_code.h"
#include "tests/support/guest_memory.h"
#include "runtime/ir/direct/interpreter.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>

namespace {
using swift::tests::Check;
using swift::tests::NativeMode;
namespace ir = swift::runtime::ir;
namespace direct = ir::direct;
using direct::Operand;
using direct::Value;
using ir::OpCode;
using ir::Type;
using u64 = std::uint64_t;

Value Need(direct::Result result) {
    Check(bool(result), "direct IR operation succeeds");
    return result.GetValue();
}

struct Machine {
    static constexpr u64 memory_base = 0x8000;
    std::array<std::uint8_t, 48> uniforms{};
    std::array<std::uint8_t, 32> memory{};
    u64 flags{0xa5a5000002001481ULL}, pc{0x1200};
    std::uint32_t mxcsr{0x5f80};
    u64 denied_read{UINT64_MAX};
    direct::AccessStatus denied_status{direct::AccessStatus::Fault};
    unsigned reads{}, writes{}, ordered_reads{}, ordered_writes{};
    bool source_changed{};

    static bool InMemory(u64 address, std::size_t bytes) {
        return swift::tests::ContainsMemory(memory_base, 32, address, bytes);
    }

    direct::Services Services() {
        return {this,
                [](void* opaque,
                   u64 address,
                   std::span<std::uint8_t> destination,
                   bool ordered) noexcept -> direct::AccessResult {
                    auto& machine = *static_cast<Machine*>(opaque);
                    ++machine.reads;
                    machine.ordered_reads += ordered;
                    if (!InMemory(address, destination.size()))
                        return {direct::AccessStatus::Fault, address};
                    if (address == machine.denied_read) return {machine.denied_status, address};
                    std::memcpy(destination.data(),
                                machine.memory.data() + (address - memory_base),
                                destination.size());
                    return {};
                },
                [](void* opaque,
                   u64 address,
                   std::span<const std::uint8_t> source,
                   bool ordered) noexcept -> direct::AccessResult {
                    auto& machine = *static_cast<Machine*>(opaque);
                    ++machine.writes;
                    machine.ordered_writes += ordered;
                    if (!InMemory(address, source.size()))
                        return {direct::AccessStatus::Fault, address};
                    std::memcpy(machine.memory.data() + (address - memory_base),
                                source.data(),
                                source.size());
                    return {direct::AccessStatus::Completed, 0, machine.source_changed};
                }};
    }
};

template <class T, std::size_t size>
T Read(const std::array<std::uint8_t, size>& bytes, unsigned offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void NumericAndUniform() {
    Machine machine;
    machine.uniforms.fill(0xa5);
    const auto initial_flags = machine.flags;
    direct::Interpreter interpreter({machine.uniforms, machine.flags, machine.pc, machine.mxcsr});

    const std::array literal{Operand::Immediate(0xfedcba9876543210ULL, Type::U64)};
    const auto wide = Need(interpreter.Emit<OpCode::LoadImm>(Type::U64, std::span{literal}));
    const auto sum = Need(interpreter.Emit<OpCode::Add>(
            Type::U64, {Operand::ValueOf(wide), Operand::Immediate(0x11, Type::U64)}));
    Need(interpreter.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(3, Type::U64), Operand::ValueOf(sum, Type::U32)}));
    const auto loaded = Need(
            interpreter.Emit<OpCode::LoadUniform>(Type::U64, {Operand::Uniform(3, Type::U64)}));
    Need(interpreter.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(14, Type::U16), Operand::ValueOf(sum, Type::U8)}));

    Check(sum.number.bits == 0xfedcba9876543221ULL && interpreter.NaturalType(sum) == Type::U64,
          "direct values retain the complete numeric result and its natural type");
    Check(loaded.number.bits == 0x76543221 && Read<u64>(machine.uniforms, 3) == 0x76543221 &&
                  Read<std::uint16_t>(machine.uniforms, 14) == 0x21,
          "Uniform stores read the logical view and zero-extend to their real field width");
    Check(machine.uniforms[2] == 0xa5 && machine.uniforms[11] == 0xa5 &&
                  machine.uniforms[13] == 0xa5 && machine.uniforms[16] == 0xa5,
          "unaligned Uniform fields preserve their surrounding ordinary buffer bytes");
    Check(machine.flags == initial_flags && machine.pc == 0x1200 && machine.mxcsr == 0x5f80,
          "plain numeric operations leave the separately bound environment scalars intact");
}

void MetadataAndRawViews() {
    Machine machine;
    direct::Interpreter interpreter({machine.uniforms, machine.flags, machine.pc, machine.mxcsr});
    constexpr u64 dynamic_flags = 0xf40000ff;
    const auto reserved = machine.flags & ~dynamic_flags;
    const ir::OperationMetadata full{ir::FlagBits(ir::Flag::All),
                                     ir::FlagProfile::ArithmeticCanonicalCarry};
    const ir::OperationMetadata partial{ir::FlagBits(ir::Flag::Zero),
                                        ir::FlagProfile::ArithmeticCanonicalCarry};

    const auto wide = Need(interpreter.Emit<OpCode::LoadImm>(
            Type::U64, {Operand::Immediate(0x123456789abcdeffULL, Type::U64)}));
    const auto raw =
            Need(interpreter.Emit<OpCode::BitCast>(Type::U8, {Operand::ValueOf(wide, Type::U8)}));
    const auto field = Need(interpreter.Emit<OpCode::BitExtract>(
            Type::U8, {Operand::ValueOf(raw), Operand::Immediate(8), Operand::Immediate(8)}));
    const auto sum = Need(interpreter.Emit<OpCode::Add>(
            Type::U8, {Operand::ValueOf(raw), Operand::Immediate(1, Type::U8)}, full));
    Check(raw.number.bits == 0x123456789abcdeffULL && field.number.bits == 0xde &&
                  sum.number.bits == 0,
          "raw BitCast preserves its carrier while numeric reads truncate to the U8 view");
    Check(machine.flags == (reserved | 0x64000000),
          "FF plus one publishes carry, zero, auxiliary carry and even raw parity");
    const auto snapshot = Need(interpreter.Emit<OpCode::GetFlags>(
            Type::U64, {Operand::ValueOf(sum), Operand::Flags(ir::Flag::Zero)}));
    auto producer = interpreter.ProducerOf(sum);
    Check(interpreter.Metadata(producer) == full && snapshot.number.bits == machine.flags,
          "producer metadata round-trips and GetFlags transports the complete opaque word");

    const auto one =
            Need(interpreter.Emit<OpCode::LoadImm>(Type::U8, {Operand::Immediate(1, Type::U8)}));
    Need(interpreter.Emit<OpCode::Add>(
            Type::U8, {Operand::ValueOf(one), Operand::ValueOf(one)}, partial));
    Check(machine.flags == (reserved | 0x24000000) &&
                  snapshot.number.bits == (reserved | 0x64000000),
          "a partial flag write preserves omitted fields and an earlier snapshot is immutable");
    Check(interpreter.SetMetadata(producer, full) == ir::Error::None &&
                  machine.flags == (reserved | 0x64000000),
          "late metadata publication uses the concrete producer snapshot");
    Need(interpreter.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(0, Type::U64), Operand::ValueOf(raw, Type::U8)}));
    Check(Read<u64>(machine.uniforms, 0) == 0xff,
          "raw transport does not bypass a Uniform consumer's explicit numeric view");
}

void SelectedMemory() {
    Machine machine;
    const auto initial_flags = machine.flags;
    constexpr std::uint32_t payload = 0x87654321;
    std::memcpy(machine.memory.data() + 4, &payload, sizeof(payload));
    direct::Interpreter interpreter({machine.uniforms, machine.flags, machine.pc, machine.mxcsr},
                                    machine.Services());
    const auto condition = Need(
            interpreter.Emit<OpCode::LoadImm>(Type::U64, {Operand::Immediate(0x100, Type::U64)}));
    const auto base = Need(interpreter.Emit<OpCode::LoadImm>(
            Type::U64, {Operand::Immediate(Machine::memory_base, Type::U64)}));
    const auto zero = Need(interpreter.Emit<OpCode::Zero>(Type::U32, {}));
    const auto fallback = Need(
            interpreter.Emit<OpCode::LoadImm>(Type::U32, {Operand::Immediate(0x55, Type::U32)}));
    const auto narrow_test = Need(interpreter.Emit<OpCode::TestNotZero>(
            Type::Bool, {Operand::ValueOf(condition, Type::U8)}));
    auto loaded = zero;
    if (narrow_test.number.bits != 0)
        loaded = Need(interpreter.Emit<OpCode::LoadMemoryTSO>(Type::U32, {Operand::ValueOf(base)}));
    const auto skipped =
            Need(interpreter.Emit<OpCode::Select>(Type::U32,
                                                  {Operand::ValueOf(condition, Type::U8),
                                                   Operand::ValueOf(loaded),
                                                   Operand::ValueOf(fallback)}));
    Check(machine.reads == 0 && skipped.number.bits == 0x55,
          "an unselected host branch submits no access and Select honors the condition view");

    const auto full_test = Need(interpreter.Emit<OpCode::TestNotZero>(
            Type::Bool, {Operand::ValueOf(condition, Type::U64)}));
    if (full_test.number.bits != 0)
        loaded = Need(
                interpreter.Emit<OpCode::LoadMemoryTSO>(Type::U32,
                                                        {Operand::Expression(ir::OperandOp::Add, 2),
                                                         Operand::ValueOf(base),
                                                         Operand::Immediate(4)}));
    const auto selected = Need(interpreter.Emit<OpCode::Select>(
            Type::U32,
            {Operand::ValueOf(condition), Operand::ValueOf(loaded), Operand::ValueOf(fallback)}));
    machine.source_changed = true;
    Need(interpreter.Emit<OpCode::StoreMemoryTSO>(Type::Void,
                                                  {Operand::Expression(ir::OperandOp::Add, 2),
                                                   Operand::ValueOf(base),
                                                   Operand::Immediate(12),
                                                   Operand::ValueOf(selected, Type::U16)}));
    Check(selected.number.bits == payload && Read<std::uint16_t>(machine.memory, 12) == 0x4321 &&
                  machine.reads == 1 && machine.writes == 1 && machine.ordered_reads == 1 &&
                  machine.ordered_writes == 1 && interpreter.code_changed,
          "the selected arm performs each exact-width ordered service once and retains change "
          "status");
    Check(machine.flags == initial_flags,
          "predicates, selection and memory services do not overwrite the flag environment");
}

void FaultKeepsCompletedEffects() {
    Machine machine;
    machine.uniforms.fill(0xa5);
    machine.denied_read = Machine::memory_base + 8;
    machine.source_changed = true;
    const auto initial_flags = machine.flags;
    direct::Interpreter interpreter({machine.uniforms, machine.flags, machine.pc, machine.mxcsr},
                                    machine.Services());
    const auto address = Need(interpreter.Emit<OpCode::LoadImm>(
            Type::U64, {Operand::Immediate(Machine::memory_base, Type::U64)}));
    const auto value = Need(
            interpreter.Emit<OpCode::LoadImm>(Type::U16, {Operand::Immediate(0x3412, Type::U16)}));
    Need(interpreter.Emit<OpCode::StoreMemoryTSO>(
            Type::Void, {Operand::ValueOf(address), Operand::ValueOf(value)}));
    bool faulted{};
    try {
        const auto loaded = Need(interpreter.Emit<OpCode::LoadMemoryTSO>(
                Type::U32, {Operand::Immediate(machine.denied_read, Type::U64)}));
        Need(interpreter.Emit<OpCode::StoreUniform>(
                Type::Void, {Operand::Uniform(0, Type::U32), Operand::ValueOf(loaded)}));
    } catch (const direct::Exit& exit) {
        faulted = exit.kind == direct::ExitKind::Fault && exit.address == machine.denied_read;
    }
    Check(faulted && machine.reads == 1 && machine.writes == 1 &&
                  Read<std::uint16_t>(machine.memory, 0) == 0x3412 && interpreter.code_changed,
          "a fault is reported once without replaying or rolling back a completed write");
    Check(Read<std::uint32_t>(machine.uniforms, 0) == 0xa5a5a5a5 &&
                  machine.flags == initial_flags && machine.pc == 0x1200 && machine.mxcsr == 0x5f80,
          "a failed load leaves its unsubmitted destination and environment scalars intact");

    machine.denied_status = direct::AccessStatus::UnsupportedBeforeAccess;
    bool unsupported{};
    try {
        Need(interpreter.Emit<OpCode::LoadMemory>(
                Type::U32, {Operand::Immediate(machine.denied_read, Type::U64)}));
    } catch (const direct::Exit& exit) {
        unsupported = exit.kind == direct::ExitKind::Unsupported;
    }
    Check(unsupported && interpreter.observed_effects && machine.reads == 2 &&
                  machine.writes == 1 && Read<std::uint16_t>(machine.memory, 0) == 0x3412,
          "an unsupported later access cannot erase the completed-effect marker or replay a write");

    direct::Interpreter fresh({machine.uniforms, machine.flags, machine.pc, machine.mxcsr},
                              machine.Services());
    unsupported = false;
    try {
        Need(fresh.Emit<OpCode::LoadMemory>(Type::U32,
                                            {Operand::Immediate(machine.denied_read, Type::U64)}));
    } catch (const direct::Exit& exit) {
        unsupported = exit.kind == direct::ExitKind::Unsupported;
    }
    Check(unsupported && !fresh.observed_effects && machine.reads == 3 && machine.writes == 1,
          "unsupported before the first access leaves the concrete consumer safe to replace");
}
}  // namespace

TEST_CASE("NumericAndUniform", "[compile][ir-direct]") { CHECK_NOTHROW(NumericAndUniform()); }

TEST_CASE("MetadataAndRawViews", "[compile][ir-direct]") { CHECK_NOTHROW(MetadataAndRawViews()); }

TEST_CASE("SelectedMemory", "[compile][ir-direct]") { CHECK_NOTHROW(SelectedMemory()); }

TEST_CASE("FaultKeepsCompletedEffects", "[compile][ir-direct]") {
    CHECK_NOTHROW(FaultKeepsCompletedEffects());
}
