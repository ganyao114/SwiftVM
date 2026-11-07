#include "tests/support/check.h"
#include "tests/support/test_mode.h"
#include "tests/support/native_code.h"
#include "tests/support/guest_memory.h"
#include "runtime/frontend/x86/interpreter.h"
#include "runtime/frontend/x86/build_ir.h"
#include "runtime/arch/x86/state.h"
#include "runtime/arch/x86/flags.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
using swift::tests::Check;
using swift::tests::NativeMode;
namespace front = swift::x86;
namespace arch = swift::runtime::x86;
using u64 = std::uint64_t;

struct Machine {
    static constexpr u64 code_pc = 0x1000, memory_pc = 0x8000;
    arch::GuestState state;
    std::vector<std::uint8_t> code;
    std::array<std::uint8_t, 256> memory{};
    u64 denied_read{UINT64_MAX}, denied_write{UINT64_MAX};
    front::AccessStatus fetch_tail{front::AccessStatus::Completed};
    unsigned reads{}, writes{};

    explicit Machine(std::initializer_list<std::uint8_t> bytes) : code(bytes) {
        state.pc = code_pc;
        state.gprs[arch::RSP] = memory_pc + 128;
    }

    static bool InMemory(u64 address, std::size_t size) {
        return swift::tests::ContainsMemory(memory_pc, 256, address, size);
    }

    front::DirectServices Services() {
        return {this,
                [](void* raw,
                   u64 pc,
                   std::span<std::uint8_t> destination) noexcept -> front::CodeReadResult {
                    auto& m = *static_cast<Machine*>(raw);
                    if (pc < code_pc || pc - code_pc >= m.code.size())
                        return {{front::AccessStatus::Fault, pc}, 0};
                    const auto count =
                            std::min(destination.size(),
                                     m.code.size() - static_cast<std::size_t>(pc - code_pc));
                    std::memcpy(destination.data(), m.code.data() + pc - code_pc, count);
                    return {{count < destination.size() ? m.fetch_tail
                                                        : front::AccessStatus::Completed,
                             pc + count},
                            count};
                },
                [](void* raw, u64 address, std::span<std::uint8_t> destination, bool) noexcept
                        -> front::AccessResult {
                    auto& m = *static_cast<Machine*>(raw);
                    ++m.reads;
                    if (!InMemory(address, destination.size()) || address == m.denied_read)
                        return {front::AccessStatus::Fault, address};
                    std::memcpy(destination.data(),
                                m.memory.data() + address - memory_pc,
                                destination.size());
                    return {};
                },
                [](void* raw, u64 address, std::span<const std::uint8_t> source, bool) noexcept
                        -> front::AccessResult {
                    auto& m = *static_cast<Machine*>(raw);
                    ++m.writes;
                    if (!InMemory(address, source.size()) || address == m.denied_write)
                        return {front::AccessStatus::Fault, address};
                    std::memcpy(
                            m.memory.data() + address - memory_pc, source.data(), source.size());
                    return {};
                },
                [](void*,
                   const front::AtomicRequest&,
                   front::AtomicValue&) noexcept -> front::AccessResult {
                    return {front::AccessStatus::UnsupportedBeforeAccess};
                },
                nullptr};
    }

    front::StepResult Step() {
        const auto services = Services();
        return front::InterpretOne(state, services);
    }

    u64 Read64(u64 address) {
        u64 value;
        std::memcpy(&value, memory.data() + address - memory_pc, 8);
        return value;
    }

    void Write64(u64 address, u64 value) {
        std::memcpy(memory.data() + address - memory_pc, &value, 8);
    }
};

void Scalar() {
    Machine m{0xb8, 0xff, 0xff, 0xff, 0xff, 0x83, 0xc0, 1, 0xff, 0xc0, 0x0f, 0x92, 0xc3};
    Check(m.Step().kind == front::StepKind::Completed && m.state.gprs[arch::RAX] == UINT32_MAX,
          "EAX clears the high half");
    Check(m.Step().kind == front::StepKind::Completed && m.state.gprs[arch::RAX] == 0 &&
                  arch::flags::ToRflags(m.state.packed_flags) == 0x257,
          "ADD publishes x86 carry/parity/AF/zero");
    Check(m.Step().kind == front::StepKind::Completed && m.state.gprs[arch::RAX] == 1 &&
                  arch::flags::ToRflags(m.state.packed_flags) == 0x203,
          "INC preserves carry");
    Check(m.Step().kind == front::StepKind::Completed && m.state.gprs[arch::RBX] == 1,
          "SETC observes the shared snapshot");
    Machine load{0x48, 0x8b, 0x00};
    load.state.gprs[arch::RAX] = 0x7000;
    Check(load.Step().kind == front::StepKind::Fault && load.state.pc == Machine::code_pc &&
                  load.state.gprs[arch::RAX] == 0x7000,
          "failed load does not commit destination or PC");
}

void DecodedConsumers() {
    Machine machine{0xb8, 0x2a, 0, 0, 0};
    const auto bytes = std::span<const std::uint8_t>{machine.code};
    const auto decoded = front::DecodeOne(bytes, machine.state.pc);
    Check(bool(decoded), "one owned decoder record is available to either consumer");
    const front::DecodedBlock block{machine.state.pc, 0, 1};
    const front::BlockWindow window{machine.state.pc, bytes};
    const auto built = front::BuildDecodedIR(
            front::InstructionRange{std::span<const front::Instruction>{&decoded.instruction, 1}},
            {&block, 1},
            {&window, 1},
            swift::runtime::ir::Scope::Block,
            {offsetof(arch::GuestState, gprs), offsetof(arch::GuestState, pc), 8});
    Check(bool(built) && machine.state.pc == Machine::code_pc && machine.state.gprs[arch::RAX] == 0,
          "the IR-building consumer submits a Program without executing guest effects");
    auto services = machine.Services();
    services.fetch = nullptr;
    const auto step = front::InterpretDecoded(decoded.instruction, bytes, machine.state, services);
    Check(step.kind == front::StepKind::Completed && step.instructions == 1 &&
                  machine.state.gprs[arch::RAX] == 42 && machine.state.pc == Machine::code_pc + 5,
          "the same decoded record executes through IR direct without fetching or decoding");
}

void DirectBlockBoundaries() {
    Machine ready{0xb8, 1, 0, 0, 0, 0xbb, 2, 0, 0, 0};
    const front::ReadyEntryLookup entry{
            nullptr, [](void*, u64 pc) noexcept { return pc == Machine::code_pc + 5; }};
    const auto first = front::InterpretBlock(ready.state, ready.Services(), 256, nullptr, entry);
    Check(first.kind == front::StepKind::Completed && first.instructions == 1 &&
                  ready.state.pc == Machine::code_pc + 5 && ready.state.gprs[arch::RAX] == 1 &&
                  ready.state.gprs[arch::RBX] == 0,
          "a cold linear fragment stops before a ready native entry");
    const auto second = front::InterpretBlock(ready.state, ready.Services(), 1);
    Check(second.kind == front::StepKind::Completed && second.instructions == 1 &&
                  ready.state.gprs[arch::RBX] == 2 && ready.state.pc == Machine::code_pc + 10,
          "the completed-instruction budget ends at the actual next PC");
    Machine branch{0x31, 0xc0, 0x75, 2, 0xbb, 3, 0, 0, 0};
    const auto untaken = front::InterpretBlock(branch.state, branch.Services());
    Check(untaken.kind == front::StepKind::Completed && untaken.instructions == 2 &&
                  branch.state.pc == Machine::code_pc + 4 && branch.state.gprs[arch::RBX] == 0,
          "an untaken conditional is still a block boundary");
    Machine repeat{0xf3, 0xaa};
    repeat.state.gprs[arch::RCX] = 2;
    repeat.state.gprs[arch::RDI] = Machine::memory_pc;
    repeat.state.gprs[arch::RAX] = 0x44;
    const auto iteration = front::InterpretBlock(repeat.state, repeat.Services());
    Check(iteration.kind == front::StepKind::Completed && iteration.instructions == 1 &&
                  repeat.state.pc == Machine::code_pc && repeat.state.gprs[arch::RCX] == 1 &&
                  repeat.writes == 1,
          "one restartable REP iteration returns before another entry visit");
}

void PartialServiceStop() {
    Machine machine{0xb8, 7, 0, 0, 0, 0xc8, 0x10, 0, 2};  // MOV; ENTER 16,2.
    const auto stack = machine.state.gprs[arch::RSP];
    const auto base = Machine::memory_pc + 160;
    machine.state.gprs[arch::RBP] = base;
    auto services = machine.Services();
    services.read =
            [](void* raw, u64, std::span<std::uint8_t>, bool) noexcept -> front::AccessResult {
        ++static_cast<Machine*>(raw)->reads;
        return {front::AccessStatus::UnsupportedBeforeAccess};
    };
    const auto step = front::InterpretBlock(machine.state, services);
    Check(step.kind == front::StepKind::Incomplete && machine.writes == 1 && machine.reads == 1 &&
                  machine.Read64(stack - 8) == base && machine.state.pc == Machine::code_pc + 5 &&
                  step.pc == Machine::code_pc + 5 && step.instructions == 1 &&
                  machine.state.gprs[arch::RAX] == 7 && machine.state.gprs[arch::RSP] == stack &&
                  machine.state.gprs[arch::RBP] == base,
          "service unavailability after an ENTER write preserves progress and forbids replay");
}

void Model() {
    constexpr std::uint8_t endbr[]{0xf3, 0x0f, 0x1e, 0xfa};
    const auto disabled = front::DecodeOne(endbr, Machine::code_pc);
    const auto enabled = front::DecodeOne(endbr, Machine::code_pc, nullptr, {true, false});
    Check(disabled && disabled.instruction.View().Size() == 4 &&
                  enabled.error == front::DecodeError::UnsupportedGuestFeature,
          "CET belongs to the guest model; enabled unimplemented semantics cannot become a NOP");
}

void FetchBoundary() {
    Machine complete{0x90};
    complete.fetch_tail = front::AccessStatus::Fault;
    Check(complete.Step().kind == front::StepKind::Completed &&
                  complete.state.pc == Machine::code_pc + 1,
          "an inaccessible speculative tail cannot fault a complete short instruction");
    for (const auto bytes : {std::initializer_list<std::uint8_t>{0x48},
                             std::initializer_list<std::uint8_t>{0xb8, 0x2a},
                             std::initializer_list<std::uint8_t>{0x0f}}) {
        Machine short_code{bytes};
        short_code.fetch_tail = front::AccessStatus::Fault;
        short_code.state.gprs[arch::RAX] = 0x1234;
        const auto step = short_code.Step();
        Check(step.kind == front::StepKind::Fault &&
                      step.fault_address == Machine::code_pc + bytes.size() &&
                      short_code.state.pc == Machine::code_pc &&
                      short_code.state.gprs[arch::RAX] == 0x1234,
              "a genuinely missing instruction tail preserves the fetch fault and guest state");
    }
    Machine unavailable{0xb8};
    unavailable.fetch_tail = front::AccessStatus::UnsupportedBeforeAccess;
    Check(unavailable.Step().kind == front::StepKind::Unsupported &&
                  unavailable.state.pc == Machine::code_pc,
          "unqualified instruction backing remains unsupported before execution");
    Machine invalid{0x06};
    invalid.fetch_tail = front::AccessStatus::Fault;
    Check(invalid.Step().kind == front::StepKind::InvalidEncoding,
          "an invalid long-mode opcode is not reclassified as a missing fetch tail");
    Machine truncated{0xb8, 0x2a};
    Check(truncated.Step().kind == front::StepKind::Truncated,
          "an explicitly finite decode buffer without a fetch fault remains truncated");
    Machine too_long{};
    too_long.code.assign(15, 0x66);
    Check(too_long.Step().kind == front::StepKind::InvalidEncoding,
          "a 15-byte prefix stream cannot request a sixteenth instruction byte");
}

void Control() {
    Machine call{0xff, 0xd0};
    call.state.gprs[arch::RAX] = 0x1234;
    const auto sp = call.state.gprs[arch::RSP];
    call.denied_write = sp - 8;
    Check(call.Step().kind == front::StepKind::Fault && call.state.gprs[arch::RSP] == sp &&
                  call.state.pc == Machine::code_pc,
          "CALL stack write must succeed before RSP/PC commit");
    call.denied_write = UINT64_MAX;
    Check(call.Step().kind == front::StepKind::Completed && call.state.pc == 0x1234 &&
                  call.state.gprs[arch::RSP] == sp - 8 &&
                  call.Read64(sp - 8) == Machine::code_pc + 2,
          "CALL commits its real return address");
    Machine ret{0xc3};
    ret.denied_read = ret.state.gprs[arch::RSP];
    Check(ret.Step().kind == front::StepKind::Fault && ret.state.pc == Machine::code_pc &&
                  ret.state.gprs[arch::RSP] == sp,
          "RET cannot commit before a successful read");
    Machine pop{0x8f, 0x44, 0x24, 0x08};
    pop.Write64(sp, 0x12345678);
    pop.denied_write = sp + 16;
    Check(pop.Step().kind == front::StepKind::Fault && pop.state.gprs[arch::RSP] == sp &&
                  pop.state.pc == Machine::code_pc,
          "POP uses post-increment SP for EA without committing SP on store fault");
    pop.denied_write = UINT64_MAX;
    Check(pop.Step().kind == front::StepKind::Completed && pop.state.gprs[arch::RSP] == sp + 8 &&
                  pop.Read64(sp + 16) == 0x12345678,
          "POP memory destination uses the new SP");
}

void Repeat() {
    Machine m{0xf3, 0xaa};
    m.state.gprs[arch::RCX] = 0;
    m.state.gprs[arch::RDI] = Machine::memory_pc;
    m.denied_write = Machine::memory_pc;
    Check(m.Step().kind == front::StepKind::Completed && m.writes == 0 &&
                  m.state.pc == Machine::code_pc + 2,
          "zero REP never visits the store body");
    m.state.pc = Machine::code_pc;
    m.state.gprs[arch::RCX] = 2;
    m.state.gprs[arch::RAX] = 0x5a;
    m.denied_write = UINT64_MAX;
    Check(m.Step().kind == front::StepKind::Completed && m.memory[0] == 0x5a &&
                  m.state.gprs[arch::RCX] == 1 &&
                  m.state.gprs[arch::RDI] == Machine::memory_pc + 1 &&
                  m.state.pc == Machine::code_pc,
          "REP commits one iteration");
    m.denied_write = Machine::memory_pc + 1;
    Check(m.Step().kind == front::StepKind::Fault && m.state.gprs[arch::RCX] == 1 &&
                  m.state.gprs[arch::RDI] == Machine::memory_pc + 1 && m.memory[0] == 0x5a,
          "REP fault retains successful prefix only");
}

void UnsupportedAndService() {
    Machine segment_move{0x8e, 0x20};
    segment_move.state.gprs[arch::RAX] = Machine::memory_pc;
    Check(segment_move.Step().kind == front::StepKind::Unsupported && segment_move.reads == 0 &&
                  segment_move.state.pc == Machine::code_pc,
          "unsupported MOV segment target is admitted before any guest access");
    Machine segment_pop{0x0f, 0xa1};
    const auto sp = segment_pop.state.gprs[arch::RSP];
    Check(segment_pop.Step().kind == front::StepKind::Unsupported && segment_pop.reads == 0 &&
                  segment_pop.state.gprs[arch::RSP] == sp,
          "unsupported POP segment cannot read the guest stack first");
    Machine locked{0xf0, 0x48, 0x01, 0x03};
    locked.state.gprs[arch::RBX] = Machine::memory_pc;
    locked.state.gprs[arch::RAX] = 1;
    locked.Write64(Machine::memory_pc, 9);
    const auto flags = locked.state.packed_flags;
    Check(locked.Step().kind == front::StepKind::Unsupported &&
                  locked.Read64(Machine::memory_pc) == 9 && locked.state.pc == Machine::code_pc &&
                  locked.state.packed_flags == flags,
          "unsupported atomic service has no guest effects");
    Machine syscall{0x0f, 0x05};
    const auto result = syscall.Step();
    Check(result.kind == front::StepKind::Service && result.service == 1 &&
                  syscall.state.pc == Machine::code_pc + 2,
          "syscall returns the actual next PC and service event");
    Machine x87{0xd9, 0xe8};
    x87.state.x87.tags = 0;
    x87.state.x87.control = 0x037e;
    const auto failure = x87.Step();
    Check(failure.kind == front::StepKind::Service && failure.service == 4 &&
                  (x87.state.x87.status & 0x41) == 0x41 && x87.state.pc == Machine::code_pc,
          "unmasked x87 is a service with committed status, never unsupported fallback");
}

void Floating() {
    for (const bool scalar : {false, true}) {
        Machine m{static_cast<std::uint8_t>(scalar ? 0xf2 : 0x66), 0x0f, 0x58, 0xc1};
        const double left[2]{1, 2}, right[2]{3, 4};
        std::memcpy(m.state.xmm[0], left, 16);
        std::memcpy(m.state.xmm[1], right, 16);
        Check(m.Step().kind == front::StepKind::Completed, "SSE floating instruction executes");
        double value[2];
        std::memcpy(value, m.state.xmm[0], 16);
        Check(value[0] == 4 && value[1] == (scalar ? 2 : 6), "scalar/packed FP lane semantics");
    }
    const auto host_rounding = std::fegetround();
    for (unsigned mode : {1u, 2u}) {
        Machine m{0xf2, 0x0f, 0x58, 0xc1};
        const double left = 1, right = 0x1p-53;
        std::memcpy(m.state.xmm[0], &left, 8);
        std::memcpy(m.state.xmm[1], &right, 8);
        m.state.mxcsr = 0x1f80 | (mode << 13);
        Check(m.Step().kind == front::StepKind::Completed, "rounded add executes");
        double value;
        std::memcpy(&value, m.state.xmm[0], 8);
        Check(value == (mode == 1 ? 1 : std::nextafter(1.0, 2.0)) &&
                      std::fegetround() == host_rounding,
              "guest RC and host FP environment remain separate");
    }
#if defined(__aarch64__)
    std::uint64_t host_fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(host_fpcr));
    const auto ambient = host_fpcr | (std::uint64_t{1} << 25);
    asm volatile("msr fpcr, %0" ::"r"(ambient));
    Machine nan{0xf2, 0x0f, 0x58, 0xc1};
    const std::uint64_t payload = UINT64_C(0x7ff8000000004321);
    std::memcpy(nan.state.xmm[0], &payload, 8);
    const auto step = nan.Step();
    std::uint64_t bits, after;
    std::memcpy(&bits, nan.state.xmm[0], 8);
    asm volatile("mrs %0, fpcr" : "=r"(after));
    asm volatile("msr fpcr, %0" ::"r"(host_fpcr));
    Check(step.kind == front::StepKind::Completed && bits == payload && after == ambient,
          "caller default-NaN control cannot change guest arithmetic or leak across a step");
#endif
}
}  // namespace

TEST_CASE("Model", "[compile][x86-interpreter]") { CHECK_NOTHROW(Model()); }

TEST_CASE("DecodedConsumers", "[compile][x86-interpreter]") { CHECK_NOTHROW(DecodedConsumers()); }

TEST_CASE("PartialServiceStop", "[compile][x86-interpreter]") {
    CHECK_NOTHROW(PartialServiceStop());
}

TEST_CASE("DirectBlockBoundaries", "[compile][x86-interpreter]") {
    CHECK_NOTHROW(DirectBlockBoundaries());
}

TEST_CASE("FetchBoundary", "[compile][x86-interpreter]") { CHECK_NOTHROW(FetchBoundary()); }

TEST_CASE("Scalar", "[compile][x86-interpreter]") { CHECK_NOTHROW(Scalar()); }

TEST_CASE("Control", "[compile][x86-interpreter]") { CHECK_NOTHROW(Control()); }

TEST_CASE("Repeat", "[compile][x86-interpreter]") { CHECK_NOTHROW(Repeat()); }

TEST_CASE("UnsupportedAndService", "[compile][x86-interpreter]") {
    CHECK_NOTHROW(UnsupportedAndService());
}

TEST_CASE("Floating", "[compile][x86-interpreter]") { CHECK_NOTHROW(Floating()); }
