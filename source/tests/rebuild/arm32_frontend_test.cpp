#include "runtime/arch/arm32/service.h"
#include "runtime/exit.h"
#include "runtime/frontend/arm32/build_ir.h"
#include "runtime/frontend/arm32/interpreter.h"
#include "tests/support/check.h"

#include <array>
#include <bit>
#include <cstring>
#include <vector>

namespace {
namespace front = swift::arm32;
namespace arch = swift::runtime::arm32;
namespace ir = swift::runtime::ir;
using U64 = std::uint64_t;
using U32 = std::uint32_t;
using U8 = std::uint8_t;
constexpr U32 CodeBase = 0x400000, DataBase = 0x500000;

std::vector<U8> Arm(std::initializer_list<U32> words) {
    std::vector<U8> bytes;
    for (const auto word : words)
        for (unsigned byte = 0; byte < 4; ++byte) bytes.push_back(word >> (byte * 8));
    return bytes;
}

std::vector<U8> Thumb(std::initializer_list<std::uint16_t> words) {
    std::vector<U8> bytes;
    for (const auto word : words) {
        bytes.push_back(word);
        bytes.push_back(word >> 8);
    }
    return bytes;
}

struct Memory {
    std::span<const U8> code;
    U32 base{CodeBase};
    std::array<U8, 256> data{};
    std::vector<std::pair<U64, std::size_t>> fetches;
    unsigned reads{}, writes{};

    front::DirectServices Services() {
        return {this,
                [](void* opaque, U64 pc, std::span<U8> out) noexcept -> front::CodeReadResult {
                    auto& self = *static_cast<Memory*>(opaque);
                    self.fetches.emplace_back(pc, out.size());
                    if (pc < self.base || pc - self.base > self.code.size() ||
                        out.size() > self.code.size() - (pc - self.base))
                        return {{front::AccessStatus::Fault, pc}, 0};
                    std::memcpy(out.data(), self.code.data() + pc - self.base, out.size());
                    return {{}, out.size()};
                },
                [](void* opaque, U64 address, std::span<U8> out, bool) noexcept
                        -> front::AccessResult {
                    auto& self = *static_cast<Memory*>(opaque);
                    if (address < DataBase || address - DataBase > self.data.size() ||
                        out.size() > self.data.size() - (address - DataBase))
                        return {front::AccessStatus::Fault, address};
                    std::memcpy(out.data(), self.data.data() + address - DataBase, out.size());
                    ++self.reads;
                    return {};
                },
                [](void* opaque, U64 address, std::span<const U8> out, bool) noexcept
                        -> front::AccessResult {
                    auto& self = *static_cast<Memory*>(opaque);
                    if (address < DataBase || address - DataBase > self.data.size() ||
                        out.size() > self.data.size() - (address - DataBase))
                        return {front::AccessStatus::Fault, address};
                    std::memcpy(self.data.data() + address - DataBase, out.data(), out.size());
                    ++self.writes;
                    return {};
                }};
    }
};

front::StepResult Run(arch::GuestState& state, Memory& memory) {
    front::StepResult result;
    for (unsigned i = 0; i < 64; ++i) {
        result = front::InterpretOne(state, memory.Services());
        if (result.kind != front::StepKind::Completed) return result;
    }
    FAIL("ARM32 fixture exceeded instruction budget");
    return result;
}

void Builds(std::span<const U8> code, U64 entry) {
    const front::BlockWindow window{arch::Location{entry}.Address(), code};
    for (auto mode : {ir::TargetMode::Known, ir::TargetMode::Unknown}) {
        const ir::TargetInfo target{mode, 0xa64, 0};
        for (auto scope : {ir::Scope::Block, ir::Scope::Function}) {
            const auto result =
                    scope == ir::Scope::Function
                            ? front::BuildFunctionIR({&window, 1}, entry, {}, {}, target)
                            : front::BuildIR({&window, 1}, entry, scope, {}, target);
            INFO(static_cast<unsigned>(result.error));
            INFO(static_cast<unsigned>(result.ir_error));
            INFO(result.error_pc);
            REQUIRE(bool(result));
            CHECK(!result.program.Operations().empty());
        }
    }
}
}  // namespace

TEST_CASE("ARM32 location keys preserve execution context independently of source address",
          "[compile][arm32-frontend]") {
    const auto arm = arch::Location::Make(CodeBase);
    const auto thumb = arch::Location::Make(CodeBase, true);
    const auto it = arch::Location::Make(CodeBase, true, 0x0c);
    const auto fp = arch::Location::Make(CodeBase, true, 0x0c, 1u << 24);
    CHECK(arm.Key() != thumb.Key());
    CHECK(thumb.Key() != it.Key());
    CHECK(it.Key() != fp.Key());
    CHECK(fp.Address() == arm.Address());
    CHECK(fp.Advance(2).AdvanceIT().IT().Value() == 0x18);
    CHECK(fp.Advance(2).AdvanceIT().FPSCR().FTZ());
    CHECK(fp.SetEFlag(true).EFlag());
    arch::GuestState state;
    state.fpscr = 1u << 24;
    state.pc = fp.Key();
    state.SetCpsr(0xa0000010 | (1u << 5) | (1u << 9) | (3u << 25) | (0xa0u << 8));
    CHECK(state.Cpsr() == (0xa0000010u | (1u << 5) | (1u << 9) | (3u << 25) | (0xa0u << 8)));
    CHECK(arch::Location{state.pc}.FPSCR().FTZ());
}

TEST_CASE("ARM32 decoder preserves ARM Thumb16 and Thumb32 encoding identity",
          "[compile][arm32-frontend]") {
    const auto arm = Arm({0xe3a00001});
    const auto a = front::DecodeOne(arm, CodeBase);
    REQUIRE(bool(a));
    CHECK(a.instruction.Code() == front::Opcode::arm_MOV_imm);
    CHECK(a.instruction.Length() == 4);
    CHECK(a.instruction.Condition() == ir::Condition::AL);
    const auto short_code = Thumb({0x2001});
    const auto t = front::DecodeOne(short_code, arch::Location::Make(CodeBase, true).Key());
    REQUIRE(bool(t));
    CHECK(t.instruction.Code() == front::Opcode::thumb16_MOV_imm);
    CHECK(t.instruction.Length() == 2);
    const auto wide_code = Thumb({0xf240, 0x0001});
    const auto wide = front::DecodeOne(wide_code, arch::Location::Make(CodeBase, true).Key());
    REQUIRE(bool(wide));
    CHECK(wide.instruction.Word() == 0xf2400001);
    CHECK(wide.instruction.Length() == 4);
    CHECK(front::DecodeOne(std::span{wide_code}.first(2), wide.instruction.PC()).error ==
          front::DecodeError::Truncated);
    CHECK(!front::DecodeOne(arm, CodeBase + 2));
    CHECK(!front::DecodeOne(short_code, arch::Location::Make(CodeBase + 1, true).Key()));
}

TEST_CASE("ARM32 Thumb fetch reads only the required halfwords across page edges",
          "[compile][arm32-frontend]") {
    const auto short_code = Thumb({0x2007});
    Memory short_memory{short_code, 0xffe};
    arch::GuestState short_state;
    short_state.pc = arch::Location::Make(0xffe, true).Key();
    const auto short_step = front::InterpretOne(short_state, short_memory.Services());
    CHECK(short_step.kind == front::StepKind::Completed);
    CHECK(short_state.r[0] == 7);
    REQUIRE(short_memory.fetches.size() == 1);
    CHECK(short_memory.fetches[0] == std::pair<U64, std::size_t>{0xffe, 2});

    const auto wide_code = Thumb({0xf240, 0x0001});
    Memory wide_memory{wide_code, 0xffe};
    arch::GuestState wide_state;
    wide_state.pc = arch::Location::Make(0xffe, true).Key();
    const auto wide_step = front::InterpretOne(wide_state, wide_memory.Services());
    CHECK(wide_step.kind == front::StepKind::Completed);
    CHECK(wide_state.r[0] == 1);
    REQUIRE(wide_memory.fetches.size() == 2);
    CHECK(wide_memory.fetches[1] == std::pair<U64, std::size_t>{0x1000, 2});
    const std::array<front::BlockWindow, 2> windows{
            {{0xffe, std::span{wide_code}.first(2)}, {0x1000, std::span{wide_code}.subspan(2)}}};
    front::DecodeTiming timing;
    const auto function =
            front::DecodeFunction(windows, arch::Location::Make(0xffe, true).Key(), {}, &timing);
    REQUIRE(bool(function));
    CHECK(function.instructions.size() == 1);
    CHECK(function.decoded_bytes == 4);
    CHECK(timing.calls == 1);
    CHECK(front::BuildFunctionIR(windows, arch::Location::Make(0xffe, true).Key()));

    Memory missing{std::span{wide_code}.first(2), 0xffe};
    arch::GuestState fault;
    fault.pc = arch::Location::Make(0xffe, true).Key();
    const auto fault_step = front::InterpretOne(fault, missing.Services());
    CHECK(fault_step.kind == front::StepKind::Fault);
    CHECK(fault_step.fault_address == 0x1000);
    CHECK(fault.r[0] == 0);
}

TEST_CASE("ARM32 predicates and no-borrow flags use the same frontend with both sinks",
          "[compile][arm32-frontend]") {
    const auto code = Arm({0xe3a00005, 0xe2801003, 0xe2512008, 0x13a03009, 0x03a03007, 0xef000000});
    Builds(code, CodeBase);
    arch::GuestState state;
    state.pc = CodeBase;
    Memory memory{code};
    const auto step = Run(state, memory);
    CHECK(step.kind == front::StepKind::Service);
    CHECK(step.service == swift::runtime::ExitValue(swift::runtime::NativeExit::Syscall));
    CHECK(state.r[0] == 5 && state.r[1] == 8 && state.r[2] == 0 && state.r[3] == 7);
    CHECK(state.packed_flags & (U64{1} << 30));
    CHECK(state.packed_flags & (U64{1} << 29));
}

TEST_CASE("ARM32 Thumb IT advances through skipped instructions and suppresses implicit flags",
          "[compile][arm32-frontend]") {
    const auto code = Thumb({0x2000, 0xbf0c, 0x2107, 0x2109, 0xdf00});
    const auto entry = arch::Location::Make(CodeBase, true).Key();
    Builds(code, entry);
    arch::GuestState state;
    state.pc = entry;
    Memory memory{code};
    const auto step = Run(state, memory);
    CHECK(step.kind == front::StepKind::Service);
    CHECK(state.r[1] == 7);
    CHECK(arch::Location{state.pc}.IT().Value() == 0);
    CHECK(state.packed_flags & (U64{1} << 30));
}

TEST_CASE("ARM32 direct branches preserve context and mode switches", "[compile][arm32-frontend]") {
    const auto check = [](std::span<const U8> bytes, U64 entry, U32 address, bool thumb) {
        const auto decoded = front::DecodeOne(bytes, entry);
        REQUIRE(bool(decoded));
        U64 target{};
        REQUIRE(decoded.instruction.RelativeTarget(target));
        CHECK(arch::Location{target}.Address() == address);
        CHECK(arch::Location{target}.Thumb() == thumb);
        CHECK(arch::Location{target}.FPSCR().FTZ());
    };
    check(Arm({0xeafffffe}),
          arch::Location::Make(CodeBase, false, 0, 1u << 24).Key(),
          CodeBase,
          false);
    check(Arm({0xfa000000}),
          arch::Location::Make(CodeBase, false, 0, 1u << 24).Key(),
          CodeBase + 8,
          true);
    check(Thumb({0xe7fe}), arch::Location::Make(CodeBase, true, 0, 1u << 24).Key(), CodeBase, true);
    check(Thumb({0xf000, 0xb800}),
          arch::Location::Make(CodeBase, true, 0, 1u << 24).Key(),
          CodeBase + 4,
          true);
    check(Thumb({0xf000, 0xe800}),
          arch::Location::Make(CodeBase + 2, true, 0, 1u << 24).Key(),
          CodeBase + 4,
          false);
}

TEST_CASE(
        "ARM32 function formation splits Thumb at decoded instruction boundaries without "
        "redecoding",
        "[compile][arm32-frontend]") {
    const auto code = Thumb({0x2003, 0x3801, 0xd1fd, 0xdf00});
    const auto entry = arch::Location::Make(CodeBase, true).Key();
    const front::BlockWindow window{CodeBase, code};
    front::DecodeTiming timing;
    const auto decoded = front::DecodeFunction({&window, 1}, entry, {}, &timing);
    REQUIRE(bool(decoded));
    CHECK(decoded.instructions.size() == 4);
    CHECK(decoded.blocks.size() == 3);
    CHECK(decoded.splits == 1);
    CHECK(timing.calls == 4);
    std::vector<swift::runtime::SourceSpan> spans;
    const auto result = front::BuildFunctionIR(
            {&window, 1}, entry, {}, {}, {}, nullptr, nullptr, nullptr, {}, &spans);
    REQUIRE(bool(result));
    CHECK(result.instruction_count == 4 && result.decoded_bytes == 8);
    for (const auto& span : spans) CHECK(span.pc <= UINT32_MAX);
    arch::GuestState state;
    state.pc = entry;
    Memory memory{code};
    CHECK(Run(state, memory).kind == front::StepKind::Service);
    CHECK(state.r[0] == 0);
}

TEST_CASE("ARM32 failed predicate performs neither faulting access nor service",
          "[compile][arm32-frontend]") {
    const auto code = Arm({0x15910000, 0x14b10000, 0x1f000000, 0xe3a02009, 0xef000000});
    Builds(code, CodeBase);
    arch::GuestState state;
    state.pc = CodeBase;
    state.packed_flags = U64{1} << 30;
    state.r[1] = 1;
    Memory memory{code};
    CHECK(front::InterpretOne(state, memory.Services()).kind == front::StepKind::Completed);
    CHECK(front::InterpretOne(state, memory.Services()).kind == front::StepKind::Completed);
    CHECK(front::InterpretOne(state, memory.Services()).kind == front::StepKind::Completed);
    CHECK(memory.reads == 0);
    CHECK(Run(state, memory).kind == front::StepKind::Service);
    CHECK(state.r[2] == 9);
}

TEST_CASE("ARM32 undefined instruction and disabled profiles raise guest exceptions",
          "[compile][arm32-frontend]") {
    const auto undefined = Arm({0xe7f000f0});
    arch::GuestState state;
    state.pc = CodeBase;
    Memory memory{undefined};
    const auto step = front::InterpretOne(state, memory.Services());
    CHECK(step.kind == front::StepKind::Service);
    CHECK(step.service == swift::runtime::SemanticExceptionExit(
                                  swift::runtime::GuestExceptionKind::IllegalInstruction, 4));
    CHECK(state.pc == CodeBase);
    Builds(undefined, CodeBase);
}

TEST_CASE("ARM32 operand-invalid instructions submit their valid prefix and exception",
          "[compile][arm32-frontend]") {
    const auto code = Arm({0xe3a02005, 0xe000019f, 0xe3a03009});
    const front::BlockWindow window{CodeBase, code};
    const auto result = front::BuildFunctionIR({&window, 1}, CodeBase);
    REQUIRE(bool(result));
    CHECK(result.instruction_count == 2);
    CHECK(result.decoded_bytes == 8);
    arch::GuestState state;
    state.pc = CodeBase;
    Memory memory{code};
    const auto step = Run(state, memory);
    CHECK(step.kind == front::StepKind::Service);
    CHECK(state.r[2] == 5);
    CHECK(state.r[3] == 0);
    CHECK(state.pc == CodeBase + 4);
}
