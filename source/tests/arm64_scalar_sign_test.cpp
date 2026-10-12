#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
using SignVectorBits = std::array<u64, 2>;
u64 Mask(u32 bits) { return bits == 64 ? ~u64{0} : (u64{1} << bits) - 1; }
u64 Exponent(u32 bits) { return bits == 16 ? 0x7C00 : bits == 32 ? 0x7F800000 : 0x7FF0000000000000; }
u64 Reference(u64 raw, u32 bits, bool negate, bool alternate) {
    const u64 mask = Mask(bits), sign = u64{1} << (bits - 1);
    const u64 exponent = Exponent(bits), fraction = mask & ~(sign | exponent);
    const u64 input = raw & mask;
    const bool nan = (input & exponent) == exponent && (input & fraction) != 0;
    return alternate && nan ? input : negate ? input ^ sign : input & ~sign;
}
class InstructionMemory final : public MemoryInterface {
public:
    explicit InstructionMemory(u32 instruction) : code{instruction, 0xD4200000} {}
    bool Read(void* destination, size_t address, size_t size) override {
        if (address < 0x1000 || address - 0x1000 + size > sizeof(code)) return false;
        std::memcpy(destination, reinterpret_cast<u8*>(code.data()) + address - 0x1000, size);
        return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* pointer) override {
        const auto address = reinterpret_cast<size_t>(pointer);
        return address >= 0x1000 && address - 0x1000 < sizeof(code)
            ? reinterpret_cast<u8*>(code.data()) + address - 0x1000 : nullptr;
    }
    std::array<u32, 2> code;
};
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(swift::arm64::ThreadContext64) + 32> storage{};
    backend::State* state = new(storage.data()) backend::State{};
    Machine() {
        state->host_cpu_flags = 0xB0000000;
        for (u32 reg = 0; reg < 32; ++reg)
            SetVector(reg, {0xDEF0123456789ABC ^ u64(reg) * 0x100000001,
                            0xCAFEFEDCBADF00D5 ^ u64(reg) * 0x100010001});
    }
    ~Machine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() { return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin); }
    void SetVector(u32 reg, SignVectorBits value) { std::memcpy(&Context().v[reg], value.data(), 16); }
    SignVectorBits GetVector(u32 reg) { SignVectorBits result{}; std::memcpy(result.data(), &Context().v[reg], 16); return result; }
};
struct Program {
    InstructionMemory memory;
    ir::Block block{0, 0x1000};
    ir::Assembler assembler{&block};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>, 2> native;
#endif
    Program(u32 bits, bool negate, bool alias) : memory(
        (bits == 16 ? (negate ? FNEG_h : FABS_h) : bits == 32 ? (negate ? FNEG_s : FABS_s)
                                                                    : (negate ? FNEG_d : FABS_d)) |
        (31u << 5) | (alias ? 31u : 0u)) {
        swift::arm64::A64Decoder decoder(0x1000, &memory, &assembler); decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        for (u32 i = 0; i < 2; ++i) {
            native[i] = std::make_unique<backend::x64::Translator>(
                &block, i ? backend::x64::HostFeatures{} : backend::x64::HostFeatures::Detect());
            native[i]->Translate();
        }
#endif
    }
    void Run(Machine& machine) {
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto initial = machine.Context();
        const auto flags = machine.state->host_cpu_flags;
        const auto location = machine.state->current_loc;
#endif
        backend::interp::Interpreter{*machine.state, &block}.Run();
        REQUIRE(machine.Context().padding == u32(swift::arm64::InterruptReason::BRK));
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto expected = machine.Context();
        const auto expected_flags = machine.state->host_cpu_flags;
        const auto expected_location = machine.state->current_loc;
        for (auto& translator : native) {
            machine.Context() = initial; machine.state->host_cpu_flags = flags;
            machine.state->current_loc = location; machine.state->halt_reason = HaltReason::None;
            translator->getCode<HaltReason(*)(backend::State*)>()(machine.state);
            REQUIRE(std::memcmp(&machine.Context(), &expected, sizeof(expected)) == 0);
            REQUIRE(machine.state->host_cpu_flags == expected_flags);
            REQUIRE(machine.state->current_loc.Value() == expected_location.Value());
        }
#endif
    }
};
void CheckCase(Program& program, u32 bits, bool negate, bool alias, u64 input, u32 fpcr, u32 fpsr) {
    Machine machine;
    const u32 rd = alias ? 31 : 0;
    machine.SetVector(31, {(input & Mask(bits)) | (~Mask(bits) & 0xFEDCBADF01234567), 0xDEADBEEFCAFEF00D});
    machine.Context().fpcr = fpcr; machine.Context().fpsr = fpsr;
    const auto before = machine.Context();
    program.Run(machine);
    REQUIRE(machine.GetVector(rd) == SignVectorBits{Reference(input, bits, negate, fpcr & 2), 0});
    auto expected = before;
    const SignVectorBits result{Reference(input, bits, negate, fpcr & 2), 0};
    std::memcpy(&expected.v[rd], result.data(), 16);
    expected.padding = machine.Context().padding;
    expected.pc = machine.Context().pc;
    REQUIRE(std::memcmp(&machine.Context(), &expected, sizeof(expected)) == 0);
    REQUIRE(machine.Context().fpcr == fpcr); REQUIRE(machine.Context().fpsr == fpsr);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 scalar FABS FNEG preserve NaN signs under AH without processing floating status") {
    for (u32 bits : {16u, 32u, 64u}) {
        const u64 sign = u64{1} << (bits - 1), exponent = Exponent(bits);
        const u64 fraction = Mask(bits) & ~(sign | exponent), quiet = (fraction + 1) >> 1;
        // Signed zero, subnormal bounds, normal values, infinity and both
        // quiet/signaling NaN payload bounds must remain exact bit patterns.
        const std::array<u64, 13> magnitudes{0, 1, fraction, fraction + 1, exponent >> 1,
            exponent - 1, exponent, exponent | 1, exponent | (quiet - 1),
            exponent | quiet, exponent | (quiet + 1), exponent | fraction, quiet};
        for (bool negate : {false, true}) for (bool alias : {false, true}) {
            Program program(bits, negate, alias);
            for (u64 magnitude : magnitudes) for (u64 source_sign : {u64{0}, sign})
                for (u32 control : {0u, 2u, 0x03080000u, 0x03080002u, 0x03C80000u, 0x03C80002u})
                    for (u32 status : {0u, 0x0800009Fu})
                        CheckCase(program, bits, negate, alias, magnitude | source_sign, control, status);
        }
    }
}

TEST_CASE("ARM64 scalar half sign handling covers every encoding with AH toggled") {
    for (bool negate : {false, true}) for (bool alias : {false, true}) {
        Program program(16, negate, alias);
        for (u32 input = 0; input <= 0xFFFF; ++input) {
            const u32 poison = 0x03080000 | ((input & 3) << 22);
            for (u32 alternate : {0u, 2u})
                CheckCase(program, 16, negate, alias, input, poison | alternate, 0x0800009F);
        }
    }
}

TEST_CASE("ARM64 scalar single double sign operations preserve random and NaN payload bits") {
    u64 random = 0xB58257EAD321A829;
    for (u32 bits : {32u, 64u}) for (bool negate : {false, true}) for (bool alias : {false, true}) {
        Program program(bits, negate, alias);
        const u64 sign = u64{1} << (bits - 1), exponent = Exponent(bits), fraction = Mask(bits) & ~(sign | exponent);
        for (u32 sample = 0; sample < 256; ++sample) {
            const u64 raw = Random(random), nan = (raw & (sign | fraction)) | exponent | 1;
            for (u32 alternate : {0u, 2u}) {
                const u32 control = 0x03080000 | ((sample & 3) << 22) | alternate;
                CheckCase(program, bits, negate, alias, raw, control, 0x0800009F);
                CheckCase(program, bits, negate, alias, nan, control, 0x0800009F);
            }
        }
    }
}
