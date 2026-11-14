#include "runtime/backend/arm64/compile.h"
#include "runtime/backend/loongarch64/compile.h"
#include "runtime/backend/riscv64/compile.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/frontend/arm32/build_ir.h"
#include "runtime/frontend/arm32/interpreter.h"
#include "tests/support/check.h"
#include "tests/support/native_code.h"
#include "tests/support/test_mode.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <vector>

namespace {
namespace front = swift::arm32;
namespace arch = swift::runtime::arm32;
namespace ir = swift::runtime::ir;
namespace backend = swift::runtime::backend;
using swift::tests::NativeISA;
using U32 = std::uint32_t;
using U64 = std::uint64_t;
using U8 = std::uint8_t;
constexpr U32 Base = 0x400000;

std::vector<U8> ARM(std::initializer_list<U32> words) {
    std::vector<U8> out;
    for (auto word : words)
        for (unsigned byte = 0; byte != 4; ++byte) out.push_back(word >> (byte * 8));
    return out;
}

std::vector<U8> THUMB(std::initializer_list<std::uint16_t> words) {
    std::vector<U8> out;
    for (auto word : words) {
        out.push_back(word);
        out.push_back(word >> 8);
    }
    return out;
}

front::StepResult Step(std::span<const U8> code, arch::GuestState& state) {
    const auto decoded = front::DecodeOne(code, state.pc);
    REQUIRE(bool(decoded));
    return front::InterpretDecoded(decoded.instruction, code, state, {});
}

ir::TargetInfo Target(NativeISA isa) {
    switch (isa) {
        case NativeISA::Arm64:
            return backend::arm64::KnownTargetInfo();
        case NativeISA::X64:
            return backend::x64::KnownTargetInfo();
        case NativeISA::Riscv64:
            return backend::riscv64::KnownTargetInfo();
        case NativeISA::LoongArch64:
            return backend::loongarch64::KnownTargetInfo();
        default:
            return {};
    }
}

backend::CompileResult Compile(const ir::Program& program,
                               NativeISA isa,
                               bool relocatable = false) {
    const auto configured = [relocatable]<class Options>() {
        Options options;
        options.flags_offset = offsetof(arch::GuestState, packed_flags);
        options.pc_offset = offsetof(arch::GuestState, pc);
        options.fault_locations = true;
        options.relocatable = relocatable;
        return options;
    };
    switch (isa) {
        case NativeISA::Arm64:
            return backend::arm64::Compile(
                    program, configured.template operator()<backend::arm64::CompileOptions>());
        case NativeISA::X64:
            return backend::x64::Compile(
                    program, configured.template operator()<backend::x64::CompileOptions>());
        case NativeISA::Riscv64:
            return backend::riscv64::Compile(
                    program, configured.template operator()<backend::riscv64::CompileOptions>());
        case NativeISA::LoongArch64:
            return backend::loongarch64::Compile(
                    program,
                    configured.template operator()<backend::loongarch64::CompileOptions>());
        default:
            return {};
    }
}

template <class T> T ReadLane(const arch::GuestState& state, unsigned vector, unsigned lane = 0) {
    T out;
    std::memcpy(&out, state.q[vector] + lane * sizeof(T), sizeof(T));
    return out;
}

template <class T>
void WriteLane(arch::GuestState& state, unsigned vector, T value, unsigned lane = 0) {
    std::memcpy(state.q[vector] + lane * sizeof(T), &value, sizeof(T));
}
}  // namespace

TEST_CASE("ARM32 register shifts and RRX preserve the architectural zero and oversized cases",
          "[compile][arm32-semantics]") {
    constexpr U32 input = 0x80000003;
    for (unsigned kind = 0; kind < 4; ++kind)
        for (unsigned amount : {0, 1, 31, 32, 33, 64, 255})
            for (bool carry : {false, true}) {
                arch::GuestState state;
                state.pc = Base;
                state.r[1] = input;
                state.r[2] = amount;
                state.packed_flags = 1 | (U64(carry) << 29);
                const auto code = ARM({0xe1b00211u | (kind << 5)});
                REQUIRE(Step(code, state).kind == front::StepKind::Completed);
                U32 result = input;
                bool expected_carry = carry;
                if (amount) {
                    if (kind == 0) {
                        result = amount < 32 ? input << amount : 0;
                        expected_carry = amount <= 32 && ((input >> (32 - amount)) & 1);
                    } else if (kind == 1) {
                        result = amount < 32 ? input >> amount : 0;
                        expected_carry = amount <= 32 && ((input >> (amount - 1)) & 1);
                    } else if (kind == 2) {
                        result = amount < 32 ? static_cast<U32>(static_cast<std::int32_t>(input) >>
                                                                amount)
                                             : UINT32_MAX;
                        expected_carry = amount < 32 ? (input >> (amount - 1)) & 1 : true;
                    } else {
                        result = std::rotr(input, amount % 32);
                        expected_carry = result >> 31;
                    }
                }
                INFO(kind);
                INFO(amount);
                INFO(carry);
                CHECK(state.r[0] == result);
                CHECK(bool(state.packed_flags & (U64{1} << 29)) == expected_carry);
                CHECK(bool(state.packed_flags & (U64{1} << 30)) == (result == 0));
            }
    for (bool carry : {false, true}) {
        arch::GuestState state;
        state.pc = Base;
        state.r[1] = input;
        state.packed_flags = U64(carry) << 29;
        REQUIRE(Step(ARM({0xe1b00061}), state).kind == front::StepKind::Completed);
        CHECK(state.r[0] == ((U32(carry) << 31) | (input >> 1)));
        CHECK(state.packed_flags & (U64{1} << 29));
    }
}

TEST_CASE("ARM32 SEL selects Rn for set GE lanes", "[compile][arm32-semantics]") {
    for (bool thumb : {false, true}) {
        const auto entry = arch::Location::Make(Base, thumb).Key();
        {
            arch::GuestState state{};
            state.pc = entry;
            state.r[1] = 0x11111111;
            state.r[2] = 0x22222222;
            state.psr_aux = 0x10 | (U64{0b1010} << 16);
            const auto code = thumb ? THUMB({0xfaa1, 0xf082}) : ARM({0xe6810fb2});
            INFO(thumb);
            REQUIRE(Step(code, state).kind == front::StepKind::Completed);
            CHECK(state.r[0] == 0x11221122u);
        }
        {
            // UADD8 with all-ones sets GE for each nonzero byte; SEL must then
            // pick 0 from Rn for those lanes and all-ones from Rm for the zero lane.
            arch::GuestState state{};
            state.pc = entry;
            state.r[1] = 0x11220033;
            state.r[2] = 0xffffffff;
            state.r[4] = 0;
            const auto code =
                    thumb ? THUMB({0xfa81, 0xf342, 0xfaa4, 0xf083}) : ARM({0xe6513f92, 0xe6840fb3});
            INFO(thumb);
            REQUIRE(Step(code, state).kind == front::StepKind::Completed);
            REQUIRE(Step(std::span<const U8>(code).subspan(4), state).kind ==
                    front::StepKind::Completed);
            CHECK(state.r[0] == 0x0000ff00u);
        }
    }
}

TEST_CASE("ARM32 SEVL encodings retain their pre-v8 NOP hint behavior",
          "[compile][arm32-semantics]") {
    const std::array codes{ARM({0xe320f005}), THUMB({0xbf50}), THUMB({0xf3af, 0x8005})};
    for (unsigned encoding = 0; encoding < codes.size(); ++encoding) {
        for (const auto version : {arch::ArchVersion::v7, arch::ArchVersion::v8}) {
            arch::GuestState state;
            state.pc = arch::Location::Make(Base, encoding != 0).Key();
            state.SetCpsr(0xa80a0010u | (encoding ? 32u : 0u));
            state.r[0] = 0x12345678;
            const auto before = state;
            arch::Profile profile;
            profile.version = version;
            const auto decoded = front::DecodeOne(codes[encoding], state.pc);
            REQUIRE(bool(decoded));
            const auto step = front::InterpretDecoded(
                    decoded.instruction, codes[encoding], state, {}, profile);
            REQUIRE(step.kind == front::StepKind::Completed);
            CHECK(state.r[0] == before.r[0]);
            CHECK(state.Cpsr() == before.Cpsr());
            CHECK(std::uint32_t(state.pc) == Base + codes[encoding].size());
        }
    }
}

TEST_CASE("ARM32 NEON overlapping permutations choose data without raising an exception",
          "[compile][arm32-semantics]") {
    const std::array arm_codes{
            ARM({0xf3b20080}),
            ARM({0xf3b660c6}),
            ARM({0xf3b20100}),
            ARM({0xf3b66146}),
            ARM({0xf3b20180}),
            ARM({0xf3b661c6}),
    };
    const std::array thumb_codes{
            THUMB({0xffb2, 0x0080}),
            THUMB({0xffb6, 0x60c6}),
            THUMB({0xffb2, 0x0100}),
            THUMB({0xffb6, 0x6146}),
            THUMB({0xffb2, 0x0180}),
            THUMB({0xffb6, 0x61c6}),
    };
    for (const bool thumb : {false, true})
        for (const auto& code : thumb ? thumb_codes : arm_codes) {
            arch::GuestState state{};
            state.pc = arch::Location::Make(Base, thumb).Key();
            state.SetCpsr(0xa80a0010u | (thumb ? 32u : 0u));
            for (unsigned reg = 0; reg < 16; ++reg) {
                state.r[reg] = 0x12345600 + reg;
                for (unsigned lane = 0; lane < 16; ++lane) state.q[reg][lane] = reg * 16 + lane;
            }
            const auto before = state;
            REQUIRE(Step(code, state).kind == front::StepKind::Completed);
            CHECK(std::memcmp(state.q, before.q, sizeof(state.q)) == 0);
            CHECK(std::memcmp(state.r, before.r, sizeof(state.r)) == 0);
            CHECK(state.Cpsr() == before.Cpsr() && state.fpscr == before.fpscr);
            CHECK(std::uint32_t(state.pc) == Base + 4);
        }
}

TEST_CASE("ARM32 VFP and NEON share the S D and Q register bank", "[compile][arm32-semantics]") {
    arch::GuestState state;
    state.pc = Base;
    // VMOV S0,R0 changes only the low 32-bit lane of D0/Q0.
    state.r[0] = 0x12345678;
    std::memset(state.q[0], 0xa5, 16);
    REQUIRE(Step(ARM({0xee000a10}), state).kind == front::StepKind::Completed);
    CHECK(ReadLane<U32>(state, 0) == 0x12345678);
    CHECK(ReadLane<U32>(state, 0, 1) == 0xa5a5a5a5);
    CHECK(ReadLane<U64>(state, 0, 1) == UINT64_C(0xa5a5a5a5a5a5a5a5));
    // VADD.I8 Q0,Q1,Q2 writes the whole Q0 bank.
    std::memset(state.q[1], 1, 16);
    std::memset(state.q[2], 2, 16);
    REQUIRE(Step(ARM({0xf2020844}), state).kind == front::StepKind::Completed);
    for (auto byte : state.q[0]) CHECK(byte == 3);
}

TEST_CASE("ARM32 VFP arithmetic reports independent FPSCR exceptional status",
          "[compile][arm32-semantics]") {
    arch::GuestState state;
    state.pc = Base;
    state.packed_flags = UINT64_C(0x90000001);
    WriteLane(state, 0, 1.5, 1);  // D1
    WriteLane(state, 1, 2.25);    // D2
    // VADD.F64 D0,D1,D2
    REQUIRE(Step(ARM({0xee310b02}), state).kind == front::StepKind::Completed);
    CHECK(ReadLane<double>(state, 0) == 3.75);
    CHECK(state.packed_flags == UINT64_C(0x90000001));
    state.pc = Base;
    WriteLane(state, 0, UINT64_C(0x7ff0000000000042), 1);
    WriteLane(state, 1, 1.0);
    REQUIRE(Step(ARM({0xee310b02}), state).kind == front::StepKind::Completed);
    CHECK(ReadLane<U64>(state, 0) == UINT64_C(0x7ff8000000000042));
    CHECK(state.fpscr & 1);
}

TEST_CASE("ARM32 scalar Thumb IT VFP and NEON IR compiles on every backend",
          "[compile][arm32-semantics]") {
    const std::array code{
            ARM({0xe3a00005, 0xe2801003, 0xe2512008, 0x13a03009, 0x03a03007, 0xef000000}),
            THUMB({0x2000, 0xbf0c, 0x2107, 0x2109, 0xdf00}),
            ARM({0xe1b00211, 0xe1b03231, 0xe1b04251, 0xe1b05271, 0xef000000}),
            ARM({0xee310b02, 0xeeb40b41, 0xef000000}),
            ARM({0xf2020844, 0xf2220844, 0xf2020054, 0xef000000}),
            ARM({0xf3bb0542, 0xef000000})};
    for (auto isa : {NativeISA::Arm64, NativeISA::X64, NativeISA::Riscv64, NativeISA::LoongArch64})
        for (std::size_t index = 0; index < code.size(); ++index)
            for (auto scope : {ir::Scope::Block, ir::Scope::Function}) {
                const auto entry = arch::Location::Make(Base, index == 1).Key();
                const front::BlockWindow window{Base, code[index]};
                const auto built =
                        scope == ir::Scope::Function
                                ? front::BuildFunctionIR({&window, 1}, entry, {}, {}, Target(isa))
                                : front::BuildIR({&window, 1}, entry, scope, {}, Target(isa));
                INFO(static_cast<unsigned>(isa));
                INFO(index);
                INFO(static_cast<unsigned>(built.error));
                INFO(static_cast<unsigned>(built.ir_error));
                REQUIRE(bool(built));
                const auto compiled = Compile(built.program, isa, index >= 3);
                INFO(static_cast<unsigned>(compiled.error));
                INFO(compiled.node.value);
                REQUIRE(bool(compiled));
                CHECK(!compiled.words.empty());
                if (index >= 3) CHECK(compiled.relocatable);
                if (index == 3 || index == 5) {
                    CHECK(std::any_of(compiled.relocations.begin(),
                                      compiled.relocations.end(),
                                      [](const auto& relocation) {
                                          const auto id = static_cast<unsigned>(relocation.symbol);
                                          return id >= 256 && id <= 703;
                                      }));
                }
            }
}

TEST_CASE("ARM32 Direct and native JIT preserve the same integer flags and IT results",
          "[native][arm32-semantics]") {
    const auto isa = swift::tests::HostISA();
    REQUIRE(isa != NativeISA::None);
    for (bool thumb : {false, true}) {
        const auto code = thumb ? THUMB({0x2000, 0xbf0c, 0x2107, 0x2109, 0xdf00})
                                : ARM({0xe3a00005,
                                       0xe2801003,
                                       0xe2512008,
                                       0x13a03009,
                                       0x03a03007,
                                       0xef000000});
        const auto entry = arch::Location::Make(Base, thumb).Key();
        arch::GuestState expected;
        expected.pc = entry;
        for (std::size_t offset = 0; offset < code.size();) {
            const auto decoded = front::DecodeOne(std::span{code}.subspan(offset), expected.pc);
            REQUIRE(bool(decoded));
            const auto result = front::InterpretDecoded(
                    decoded.instruction, std::span{code}.subspan(offset), expected, {});
            offset += decoded.instruction.Length();
            if (result.kind == front::StepKind::Service) break;
            REQUIRE(result.kind == front::StepKind::Completed);
        }
        const front::BlockWindow window{Base, code};
        const auto built = front::BuildFunctionIR({&window, 1}, entry, {}, {}, Target(isa));
        REQUIRE(bool(built));
        const auto compiled = Compile(built.program, isa);
        REQUIRE(bool(compiled));
        swift::tests::NativeCode published{compiled.words, isa};
        arch::GuestState actual;
        actual.pc = entry;
        CHECK(published.Run(reinterpret_cast<U8*>(&actual)) == 1);
        CHECK(std::memcmp(actual.r, expected.r, sizeof(actual.r)) == 0);
        CHECK(actual.pc == expected.pc);
        CHECK(actual.Cpsr() == expected.Cpsr());
    }
}

TEST_CASE("ARM32 native floating and vector paths preserve FPSCR and shared D Q state",
          "[native][arm32-semantics]") {
    const auto isa = swift::tests::HostISA();
    REQUIRE(isa != NativeISA::None);
    const std::array fixtures{ARM({0xee310b02, 0xeeb40b41, 0xef000000}),
                              ARM({0xf2020844, 0xf2026054, 0xf209880a, 0xef000000}),
                              ARM({0xeea10b02, 0xf3bb6548, 0xef000000})};
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
        arch::GuestState initial;
        initial.pc = Base;
        initial.packed_flags = UINT64_C(0x90000001);
        if (index == 0) {
            WriteLane(initial, 0, 1.5, 1);  // D1
            WriteLane(initial, 1, 2.25);    // D2
        } else if (index == 1) {
            std::memset(initial.q[1], 0x7f, 16);
            std::memset(initial.q[2], 1, 16);
            std::memset(initial.q[4], 0xee, 8);
            std::memset(initial.q[4] + 8, 3, 8);  // D9 aliases Q4's upper half.
            std::memset(initial.q[5], 4, 8);      // D10
        } else {
            initial.fpscr = 1u << 4;
            WriteLane(initial, 0, 0.5);  // FMA addend D0
            WriteLane(initial, 0, 1.5, 1);
            WriteLane(initial, 1, 2.25);
            WriteLane(initial, 4, U32{0}, 0);
            WriteLane(initial, 4, std::bit_cast<U32>(2.0f), 1);
            WriteLane(initial, 4, U32{0x7f800042}, 2);  // signaling NaN
            WriteLane(initial, 4, std::bit_cast<U32>(4.0f), 3);
        }
        auto expected = initial;
        const auto& code = fixtures[index];
        U64 service{};
        for (std::size_t offset = 0; offset < code.size();) {
            const auto decoded = front::DecodeOne(std::span{code}.subspan(offset), expected.pc);
            REQUIRE(bool(decoded));
            const auto result = front::InterpretDecoded(
                    decoded.instruction, std::span{code}.subspan(offset), expected, {});
            offset += decoded.instruction.Length();
            if (result.kind == front::StepKind::Service) {
                service = result.service;
                break;
            }
            REQUIRE(result.kind == front::StepKind::Completed);
        }
        INFO(index);
        CHECK(expected.packed_flags == initial.packed_flags);
        if (index == 0) {
            CHECK(ReadLane<double>(expected, 0) == 3.75);
            CHECK((expected.fpscr & 0xf0000000) == 0x20000000);
        } else if (index == 1) {
            for (unsigned lane = 0; lane < 16; ++lane) {
                CHECK(expected.q[0][lane] == 0x80);
                CHECK(expected.q[3][lane] == 0x7f);
            }
            for (unsigned lane = 0; lane < 8; ++lane) {
                CHECK(expected.q[4][lane] == 7);
                CHECK(expected.q[4][lane + 8] == 3);
            }
            CHECK(expected.fpscr & (1u << 27));
        } else {
            CHECK(ReadLane<double>(expected, 0) == 3.875);
            CHECK(ReadLane<U32>(expected, 3, 0) == 0x7f800000);
            CHECK(ReadLane<U32>(expected, 3, 2) == 0x7fc00000);
            CHECK((expected.fpscr & 0x13) == 0x13);
        }
        const front::BlockWindow window{Base, code};
        for (auto scope : {ir::Scope::Block, ir::Scope::Function}) {
            const auto built =
                    scope == ir::Scope::Function
                            ? front::BuildFunctionIR({&window, 1}, Base, {}, {}, Target(isa))
                            : front::BuildIR({&window, 1}, Base, scope, {}, Target(isa));
            REQUIRE(bool(built));
            const auto compiled = Compile(built.program, isa);
            REQUIRE(bool(compiled));
            swift::tests::NativeCode published{compiled.words, isa};
            auto actual = initial;
            CHECK(published.Run(reinterpret_cast<U8*>(&actual)) == service);
            CHECK(std::memcmp(actual.q, expected.q, sizeof(actual.q)) == 0);
            CHECK(actual.fpscr == expected.fpscr);
            CHECK(actual.packed_flags == expected.packed_flags);
            CHECK(actual.pc == expected.pc);
        }
    }
}

TEST_CASE("ARM32 VCNT and variable VSHL lower without helper calls and preserve D/Q lanes",
          "[native][arm32-semantics]") {
    const auto isa = swift::tests::HostISA();
    REQUIRE(isa != NativeISA::None);
    auto verify_run = [&](const std::vector<U8>& code, const arch::GuestState& initial) {
        auto expected = initial;
        for (std::size_t off = 0; off < code.size();) {
            const auto decoded = front::DecodeOne(std::span{code}.subspan(off), expected.pc);
            REQUIRE(bool(decoded));
            const auto step = front::InterpretDecoded(
                    decoded.instruction, std::span{code}.subspan(off), expected, {});
            off += decoded.instruction.Length();
            if (step.kind == front::StepKind::Service) break;
            REQUIRE(step.kind == front::StepKind::Completed);
        }
        const front::BlockWindow window{Base, code};
        const auto built = front::BuildIR({&window, 1}, Base, ir::Scope::Block, {}, Target(isa));
        REQUIRE(bool(built));
        for (const auto& op : built.program.Operations()) CHECK(op.opcode != ir::OpCode::CallLambda);
        const auto compiled = Compile(built.program, isa);
        REQUIRE(bool(compiled));
        swift::tests::NativeCode published{compiled.words, isa};
        auto actual = initial;
        (void)published.Run(reinterpret_cast<U8*>(&actual));
        CHECK(std::memcmp(actual.q, expected.q, sizeof(actual.q)) == 0);
        CHECK(actual.fpscr == expected.fpscr);
    };

    arch::GuestState pop_init{};
    pop_init.pc = Base;
    const U8 sample[16]{0x00, 0x01, 0x03, 0x55, 0xaa, 0x80, 0x7f, 0xff,
                        0x0f, 0xf0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
    std::memcpy(pop_init.q[0] + 8, sample, 8);  // D1 (upper half of Q0; must stay intact!)
    std::memcpy(pop_init.q[3], sample, 16);     // Q3
    verify_run(ARM({0xf3b00501, 0xf3b04546, 0xef000000}), pop_init);

    constexpr int shifts[]{0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 127,
                           -1, -7, -8, -15, -16, -31, -32, -63, -64, -127, -128};
    for (unsigned sz = 0; sz < 4; ++sz) {
        for (unsigned u = 0; u < 2; ++u) {
            arch::GuestState s_init{};
            s_init.pc = Base;
            std::memset(s_init.q[0] + 8, 0xc3, 8);  // D1 sentinel
            for (unsigned b = 0; b < 16; ++b) {
                s_init.q[1][b] = static_cast<U8>(0x85u + b * 17u);
                s_init.q[2][b] = static_cast<U8>(shifts[(b + sz * 3 + u) % 22]);
            }
            const U32 d_word = 0xf2040402u | (u << 24) | (sz << 20);
            const U32 q_word = 0xf2040442u | (u << 24) | (sz << 20);
            verify_run(ARM({d_word, 0xef000000}), s_init);
            verify_run(ARM({q_word, 0xef000000}), s_init);
        }
    }
}

TEST_CASE("ARM32 scalar VFP and NEON float ops emit x64 native FP IR and match interpreter",
          "[native][arm32-semantics]") {
    const auto code = ARM({
            0xee300a81, 0xee701ac1, 0xee202a81, 0xeec02a81, 0xeeb13ae0,
            0xeeb74ae0, 0xeef73bc4, 0xeeb84ae4, 0xeeb85a64, 0xeefd5ae0,
            0xeebc6ae0, 0xeefa6acc, 0xeebe7acc, 0xeee07a81, 0xeea08ac1,
            0xee3bab0c, 0xee3bdb4c, 0xee2beb0c, 0xee8bfb0c, 0xeef10bcb,
            0xeeeb1b0c, 0xeeeb2b4c,
            0xf2420de4, 0xf2626de4, 0xf3428df4, 0xf242acf4, 0xf262ccf4,
            0xf3fbe662, 0xf3fb0762, 0xf3fc0e32, 0xf3fc1f33, 0xef000000});
    const front::BlockWindow window{Base, code};
    const auto x64_ir =
            front::BuildIR({&window, 1}, Base, ir::Scope::Block, {}, Target(NativeISA::X64));
    REQUIRE(bool(x64_ir));
    for (const auto& op : x64_ir.program.Operations()) CHECK(op.opcode != ir::OpCode::CallLambda);

    const auto isa = swift::tests::HostISA();
    REQUIRE(isa != NativeISA::None);
    for (bool exceptional : {false, true}) {
        arch::GuestState init{};
        init.pc = Base;
        init.packed_flags = UINT64_C(0xa0000001);
        init.fpscr = (1u << 27) | 0x30000000u;  // QC=1 and FPSCR NZCV preserved
        WriteLane(init, 0, exceptional ? U32{0x00400000} : std::bit_cast<U32>(6.25f), 1);  // S1
        WriteLane(init, 0, exceptional ? U32{0x00000000} : std::bit_cast<U32>(2.5f), 2);   // S2
        WriteLane(init, 2, std::int32_t{-42}, 1);                                          // S9
        WriteLane(init, 3, std::int32_t{-384}, 1);                                         // S13
        WriteLane(init, 3, -3.75f, 2);                                                     // S14
        WriteLane(init, 3, 1.25f, 3);                                                      // S15
        WriteLane(init, 4, 10.0f, 0);                                                      // S16
        WriteLane(init, 5, exceptional ? -1.0 : 9.0, 1);                                   // D11
        WriteLane(init, 6, exceptional ? 1e308 : 1.5, 0);                                  // D12
        WriteLane(init, 8, 2.0, 1);                                                        // D17
        WriteLane(init, 9, 5.0, 0);                                                        // D18
        for (unsigned lane = 0; lane < 4; ++lane) {
            const U32 a = (exceptional && lane == 1) ? 0x00400000u
                          : (exceptional && lane == 2) ? 0x7f800011u
                                                       : std::bit_cast<U32>(1.5f + lane);
            WriteLane(init, 9, a, lane);                            // Q9
            WriteLane(init, 10, std::bit_cast<U32>(-2.0f + lane), lane);  // Q10
            WriteLane(init, 13, 0.5f * lane, lane);                 // Q13
            WriteLane(init, 14, 4.0f - lane, lane);                 // Q14
        }
        auto expected = init;
        for (std::size_t off = 0; off < code.size();) {
            const auto decoded = front::DecodeOne(std::span{code}.subspan(off), expected.pc);
            REQUIRE(bool(decoded));
            const auto step = front::InterpretDecoded(
                    decoded.instruction, std::span{code}.subspan(off), expected, {});
            off += decoded.instruction.Length();
            if (step.kind == front::StepKind::Service) break;
            REQUIRE(step.kind == front::StepKind::Completed);
        }
        const auto built = front::BuildIR({&window, 1}, Base, ir::Scope::Block, {}, Target(isa));
        REQUIRE(bool(built));
        const auto compiled = Compile(built.program, isa);
        REQUIRE(bool(compiled));
        swift::tests::NativeCode published{compiled.words, isa};
        auto actual = init;
        (void)published.Run(reinterpret_cast<U8*>(&actual));
        CHECK(std::memcmp(actual.q, expected.q, sizeof(actual.q)) == 0);
        CHECK(actual.fpscr == expected.fpscr);
        CHECK(actual.packed_flags == expected.packed_flags);
    }
}
