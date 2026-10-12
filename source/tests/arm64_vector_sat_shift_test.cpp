#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <boost/multiprecision/cpp_int.hpp>
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
using Big = boost::multiprecision::cpp_int;
using SatShiftVector = std::array<u64, 2>;
constexpr std::array<u32, 9> Operations{NEON_SQSHL_imm, NEON_UQSHL_imm, NEON_SQSHLU,
    NEON_SQSHRN, NEON_UQSHRN, NEON_SQSHRUN, NEON_SQRSHRN, NEON_UQRSHRN, NEON_SQRSHRUN};
u64 Mask(u32 bits) { return bits == 64 ? ~u64{0} : (u64{1} << bits) - 1; }
bool Left(u32 op) { return op == NEON_SQSHL_imm || op == NEON_UQSHL_imm || op == NEON_SQSHLU; }
u64 GetLane(SatShiftVector value, u32 bits, u32 lane) { return (value[lane * bits / 64] >> (lane * bits % 64)) & Mask(bits); }
void PutLane(SatShiftVector& value, u32 bits, u32 lane, u64 element) { value[lane * bits / 64] |= (element & Mask(bits)) << (lane * bits % 64); }
struct Expected { SatShiftVector value; bool qc; };
Expected Reference(u32 op, u32 bits, u32 shift, bool q, SatShiftVector source, SatShiftVector old) {
    const bool narrow = !Left(op);
    const u32 source_bits = narrow ? bits * 2 : bits;
    const bool signed_source = op != NEON_UQSHL_imm && op != NEON_UQSHRN && op != NEON_UQRSHRN;
    const bool signed_result = op == NEON_SQSHL_imm || op == NEON_SQSHRN || op == NEON_SQRSHRN;
    const bool round = op == NEON_SQRSHRN || op == NEON_UQRSHRN || op == NEON_SQRSHRUN;
    Expected result{{}, false};
    const Big lower = signed_result ? -(Big(1) << (bits - 1)) : Big(0);
    const Big upper = signed_result ? (Big(1) << (bits - 1)) - 1 : (Big(1) << bits) - 1;
    for (u32 lane = 0; lane < (narrow ? 64u : q ? 128u : 64u) / bits; ++lane) {
        const u64 input = GetLane(source, source_bits, lane);
        Big value = input;
        if (signed_source && (input >> (source_bits - 1))) value -= Big(1) << source_bits;
        if (Left(op)) value <<= shift;
        else {
            if (round) value += Big(1) << (shift - 1);
            value >>= shift;
        }
        if (value < lower) { value = lower; result.qc = true; }
        if (value > upper) { value = upper; result.qc = true; }
        value &= (Big(1) << bits) - 1;
        PutLane(result.value, bits, lane, value.convert_to<u64>());
    }
    if (narrow && q) result.value = {old[0], result.value[0]};
    return result;
}
u32 Encode(u32 op, u32 bits, u32 shift, bool q, u32 rd) {
    const u32 encoded = Left(op) ? bits + shift : 2 * bits - shift;
    return op | (q ? NEON_Q : 0) | (encoded << 16) | (31u << 5) | rd;
}
class InstructionMemory final : public MemoryInterface {
public:
    explicit InstructionMemory(u32 word) : code{word, 0xD4200000} {}
    bool Read(void* dst, size_t address, size_t size) override {
        if (address < 0x1000 || address - 0x1000 + size > sizeof(code)) return false;
        std::memcpy(dst, reinterpret_cast<u8*>(code.data()) + address - 0x1000, size); return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* pointer) override {
        auto address = reinterpret_cast<size_t>(pointer);
        return address >= 0x1000 && address - 0x1000 < sizeof(code) ? reinterpret_cast<u8*>(code.data()) + address - 0x1000 : nullptr;
    }
    std::array<u32, 2> code;
};
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(swift::arm64::ThreadContext64) + 32> storage{};
    backend::State* state = new(storage.data()) backend::State{};
    Machine() {
        state->host_cpu_flags = 0xB0000000; Context().fpcr = 0x01400000;
        for (u32 reg = 0; reg < 32; ++reg) Set(reg, {0xFEDCBA9876543210 ^ u64(reg) * 0x100010001, ~u64{0} ^ u64(reg) * 0x100000001});
    }
    ~Machine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() { return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin); }
    void Set(u32 reg, SatShiftVector value) { std::memcpy(&Context().v[reg], value.data(), 16); }
    SatShiftVector Get(u32 reg) { SatShiftVector value; std::memcpy(value.data(), &Context().v[reg], 16); return value; }
};
struct Program {
    InstructionMemory memory;
    ir::Block block{0, 0x1000};
    ir::Assembler assembler{&block};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>, 2> native;
#endif
    explicit Program(u32 word) : memory(word) {
        swift::arm64::A64Decoder decoder(0x1000, &memory, &assembler); decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        for (u32 i = 0; i < 2; ++i) {
            native[i] = std::make_unique<backend::x64::Translator>(&block, i ? backend::x64::HostFeatures{} : backend::x64::HostFeatures::Detect());
            native[i]->Translate();
        }
#endif
    }
    void Run(Machine& machine, swift::arm64::InterruptReason reason = swift::arm64::InterruptReason::BRK) {
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto initial = machine.Context();
        const auto flags = machine.state->host_cpu_flags;
        const auto location = machine.state->current_loc;
#endif
        backend::interp::Interpreter{*machine.state, &block}.Run();
        REQUIRE(machine.Context().padding == u32(reason));
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
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
void Check(Program& program, u32 op, u32 bits, u32 shift, bool q, bool alias, SatShiftVector input, bool sticky) {
    Machine machine; const u32 rd = alias ? 31 : 0;
    machine.Set(31, input); machine.Context().fpsr = 0x9F | (sticky ? 1u << 27 : 0);
    const auto before = machine.Context();
    const auto expected = Reference(op, bits, shift, q, input, machine.Get(rd));
    INFO("op=" << op << " bits=" << bits << " shift=" << shift << " q=" << q << " alias=" << alias);
    program.Run(machine);
    REQUIRE(machine.Get(rd) == expected.value);
    REQUIRE(machine.Context().fpsr == (before.fpsr | (expected.qc ? 1u << 27 : 0)));
    REQUIRE(machine.Context().fpcr == before.fpcr); REQUIRE(machine.Context().r == before.r);
    REQUIRE(machine.Context().sp == before.sp); REQUIRE(machine.Context().fp == before.fp); REQUIRE(machine.Context().lr == before.lr);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    for (u32 reg = 0; reg < 32; ++reg) if (reg != rd)
        REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], 16) == 0);
}
}

TEST_CASE("ARM64 vector saturating immediate shifts cover every legal shape immediate alias and bigint boundary") {
    u64 random = 0x173513AF926698FA;
    for (u32 op : Operations) for (u32 bits : {8u, 16u, 32u, 64u}) for (bool q : {false, true}) {
        if ((!Left(op) && bits == 64) || (Left(op) && bits == 64 && !q)) continue;
        const u32 source_bits = Left(op) ? bits : bits * 2;
        const std::array<u64, 8> edges{0, 1, Mask(source_bits), u64{1} << (source_bits - 1),
            (u64{1} << (source_bits - 1)) - 1, (u64{1} << (source_bits - 1)) + 1, Mask(bits), Mask(bits) + 1};
        const u32 first = Left(op) ? 0 : 1, last = Left(op) ? bits - 1 : bits;
        for (u32 shift = first; shift <= last; ++shift) for (bool alias : {false, true}) {
            Program program(Encode(op, bits, shift, q, alias ? 31 : 0));
            for (u32 sample = 0; sample < 8; ++sample) {
                SatShiftVector input{};
                for (u32 lane = 0; lane < 128 / source_bits; ++lane)
                    PutLane(input, source_bits, lane, edges[(sample + lane) % edges.size()]);
                Check(program, op, bits, shift, q, alias, input, sample & 1);
            }
            Check(program, op, bits, shift, q, alias, {Random(random), Random(random)}, false);
        }
    }
}

TEST_CASE("ARM64 vector saturating shifts ignore inactive high lanes and retain narrowing destination low half") {
    for (u32 op : {u32(NEON_SQSHL_imm), u32(NEON_UQSHL_imm), u32(NEON_SQSHLU)}) for (u32 bits : {8u, 16u, 32u}) {
        Program program(Encode(op, bits, 1, false, 0));
        Check(program, op, bits, 1, false, false, {0, ~u64{0}}, false);
    }
    for (u32 op : Operations) if (!Left(op)) for (u32 bits : {8u, 16u, 32u}) for (bool alias : {false, true}) {
        Program program(Encode(op, bits, bits, true, alias ? 31 : 0));
        Check(program, op, bits, bits, true, alias, {0xFEDCBA9876543210, ~u64{0}}, false);
    }
}

TEST_CASE("ARM64 vector saturating immediate shifts reject unallocated widths without guest mutation") {
    std::vector<u32> invalid;
    // Immh=0 normally selects allocated modified-immediate encodings.
    // Only the rounded narrowing bases retain unallocated bit 11 there.
    for (u32 op : {u32(NEON_SQRSHRN), u32(NEON_UQRSHRN), u32(NEON_SQRSHRUN)})
        for (bool q : {false, true}) invalid.push_back(op | (q ? NEON_Q : 0) | (31u << 5));
    for (u32 op : Operations) {
        if (Left(op)) invalid.push_back(Encode(op, 64, 0, false, 0));
        else for (bool q : {false, true}) invalid.push_back(Encode(op, 64, 1, q, 0));
    }
    for (u32 word : invalid) {
        Program program(word); Machine machine; machine.Context().fpsr = 0x0800009F;
        const auto before = machine.Context(); program.Run(machine, swift::arm64::InterruptReason::ILL_CODE);
        REQUIRE(std::memcmp(&machine.Context().v, &before.v, sizeof(before.v)) == 0);
        REQUIRE(machine.Context().fpsr == before.fpsr); REQUIRE(machine.Context().fpcr == before.fpcr);
        REQUIRE(machine.state->current_loc.Value() == 0x1000);
    }
}
