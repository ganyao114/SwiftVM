#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "arm64_vector_doubling_long_reference.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
using HighVectorBits = std::array<u64, 2>;
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
        Context().fpcr = 0x01400000;
        for (u32 reg = 0; reg < Context().r.size(); ++reg)
            Context().r[reg] = 0xA0123456789BCDEF ^ u64(reg) * 0x100000001;
        Context().fp = 0x8123456789ABCDEF;
        Context().lr = 0x76543210FEDCBA98;
        Context().sp = 0x1234567890ABCDEF;
        for (u32 reg = 0; reg < 32; ++reg)
            SetVector(reg, {0xDEF0123456789ABC ^ u64(reg) * 0x100000001,
                            0xCAFEFEDCBADF00D5 ^ u64(reg) * 0x100010001});
    }
    ~Machine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() { return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin); }
    void SetVector(u32 reg, HighVectorBits value) { std::memcpy(&Context().v[reg], value.data(), 16); }
    HighVectorBits GetVector(u32 reg) { HighVectorBits result{}; std::memcpy(result.data(), &Context().v[reg], 16); return result; }
};
struct Program {
    InstructionMemory memory;
    ir::Block block{0, 0x1000};
    ir::Assembler assembler{&block};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>, 2> native;
#endif
    explicit Program(u32 code) : memory(code) {
        swift::arm64::A64Decoder decoder(0x1000, &memory, &assembler);
        decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        for (u32 i = 0; i < 2; ++i) {
            native[i] = std::make_unique<backend::x64::Translator>(
                &block, i ? backend::x64::HostFeatures{} : backend::x64::HostFeatures::Detect());
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
            machine.Context() = initial;
            machine.state->host_cpu_flags = flags;
            machine.state->current_loc = location;
            machine.state->halt_reason = HaltReason::None;
            translator->getCode<HaltReason(*)(backend::State*)>()(machine.state);
            REQUIRE(std::memcmp(&machine.Context(), &expected, sizeof(expected)) == 0);
            REQUIRE(machine.state->host_cpu_flags == expected_flags);
            REQUIRE(machine.state->current_loc.Value() == expected_location.Value());
        }
#endif
    }
};

using Registers = std::array<u32, 3>;
using vector_doubling_long_reference::Mask;
constexpr u32 QC = 1u << 27;
std::array<Registers, 5> Aliases(u32 rm) { return {{{0,31,rm},{31,31,rm},{rm,31,rm},{0,rm,rm},{rm,rm,rm}}}; }
u64 Element(HighVectorBits vector, u32 bits, u32 lane) {
    return (vector[lane * bits / 64] >> (lane * bits % 64)) & Mask(bits);
}
void PutElement(HighVectorBits& vector, u32 bits, u32 lane, u64 value) {
    const u32 half = lane * bits / 64, shift = lane * bits % 64;
    vector[half] = (vector[half] & ~(Mask(bits) << shift)) | ((value & Mask(bits)) << shift);
}
u32 Encode(u32 bits, u32 operation, bool upper, bool indexed, u32 index, Registers regs) {
    const std::array<u32, 3> bases{NEON_SQDMULL,NEON_SQDMLAL,NEON_SQDMLSL};
    const std::array<u32, 3> indexed_bases{NEON_SQDMULL_byelement,NEON_SQDMLAL_byelement,NEON_SQDMLSL_byelement};
    u32 code = (indexed ? indexed_bases : bases)[operation] | ((bits == 16 ? 1u : 2u) << 22) |
        (upper ? NEON_Q : 0) | (regs[2] << 16) | (regs[1] << 5) | regs[0];
    if (indexed) {
        code |= ((index >> (bits == 16 ? 2 : 1)) << 11) | (((index >> (bits == 16 ? 1 : 0)) & 1) << 21);
        if (bits == 16) code |= (index & 1) << 20;
    }
    return code;
}
void CheckCase(Program& program, u32 bits, u32 operation, bool upper, bool indexed, u32 index, Registers regs,
               HighVectorBits lhs, HighVectorBits rhs, HighVectorBits accumulator, bool sticky) {
    Machine machine;
    machine.SetVector(regs[0],accumulator); machine.SetVector(regs[1],lhs); machine.SetVector(regs[2],rhs);
    machine.Context().fpcr = 0xFFFFFFFF; machine.Context().fpsr = 0x91 | (sticky ? QC : 0);
    const auto before = machine.Context();
    lhs = machine.GetVector(regs[1]); rhs = machine.GetVector(regs[2]); accumulator = machine.GetVector(regs[0]);
    HighVectorBits expected{};
    u32 flags = before.fpsr;
    for (u32 lane = 0; lane < 64/bits; ++lane) {
        const u32 selected = lane + (upper ? 64/bits : 0);
        const auto result = vector_doubling_long_reference::Evaluate(Element(lhs,bits,selected),
            Element(rhs,bits,indexed ? index : selected),Element(accumulator,bits*2,lane),bits,operation);
        PutElement(expected,bits*2,lane,result.value);
        if (result.product_saturated || result.accumulator_saturated) flags |= QC;
    }
    program.Run(machine);
    REQUIRE(machine.GetVector(regs[0]) == expected); REQUIRE(machine.Context().fpsr == flags);
    REQUIRE(machine.Context().fpcr == before.fpcr); REQUIRE(machine.Context().r == before.r);
    REQUIRE(machine.Context().fp == before.fp); REQUIRE(machine.Context().lr == before.lr); REQUIRE(machine.Context().sp == before.sp);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000); REQUIRE(machine.state->current_loc.Value() == 0x1004);
    for (u32 reg = 0; reg < 32; ++reg) if (reg != regs[0])
        REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], sizeof(before.v[reg])) == 0);
}
using Triple = std::array<u64, 3>;
std::vector<Triple> Edges(u32 bits) {
    const u64 sign = u64{1} << (bits-1), limit = u64{1} << (2*bits-1);
    return {{sign,sign,Mask(bits*2)},{sign,sign,0},{sign,sign,limit-1},{sign,sign,limit},
        {1,1,limit-1},{1,1,limit},{Mask(bits),1,limit},{Mask(bits),1,limit-1},
        {sign,sign-1,limit},{sign+1,sign+1,0},{0,sign,Mask(bits*2)},{0,0,0}};
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 vector doubling long register multiply clamps product before accumulator and ORs either QC") {
    for (u32 bits : {16u,32u}) {
        const u64 sign = u64{1} << (bits-1), limit = u64{1} << (2*bits-1);
        const std::array<u64, 7> values{0,1,sign-1,sign,sign+1,Mask(bits)-1,Mask(bits)};
        const std::array<u64, 8> accumulators{0,1,2,limit-1,limit,limit+1,Mask(2*bits)-1,Mask(2*bits)};
        for (bool upper : {false,true}) for (u32 operation = 0; operation < 3; ++operation) for (auto regs : Aliases(30)) {
            Program program(Encode(bits,operation,upper,false,0,regs));
            for (u64 left : values) for (u64 right : values) for (u64 old : accumulators) {
                HighVectorBits lhs{},rhs{},acc{};
                for (u32 lane = 0; lane < 128/bits; ++lane) {
                    const bool active = lane/(64/bits) == u32(upper);
                    PutElement(lhs,bits,lane,active ? left : sign);
                    PutElement(rhs,bits,lane,active ? right : sign);
                }
                for (u32 lane = 0; lane < 64/bits; ++lane) PutElement(acc,2*bits,lane,old);
                for (bool sticky : {false,true}) CheckCase(program,bits,operation,upper,false,0,regs,lhs,rhs,acc,sticky);
            }
        }
        const auto first = vector_doubling_long_reference::Evaluate(sign,sign,Mask(2*bits),bits,1);
        REQUIRE(first.value == limit-2); REQUIRE(first.product_saturated); REQUIRE_FALSE(first.accumulator_saturated);
        const auto subtract = vector_doubling_long_reference::Evaluate(sign,sign,0,bits,2);
        REQUIRE(subtract.value == limit+1); REQUIRE(subtract.product_saturated); REQUIRE_FALSE(subtract.accumulator_saturated);
    }
}

TEST_CASE("ARM64 indexed vector doubling long all H L M elements half selectors and aliases match independent BigInt") {
    for (u32 bits : {16u,32u}) for (bool upper : {false,true}) for (u32 operation = 0; operation < 3; ++operation)
        for (u32 index = 0; index < 128/bits; ++index) for (auto regs : Aliases(bits == 16 ? 15 : 30)) {
            Program program(Encode(bits,operation,upper,true,index,regs));
            for (auto values : Edges(bits)) {
                HighVectorBits lhs{},rhs{},acc{};
                for (u32 lane = 0; lane < 128/bits; ++lane) {
                    PutElement(lhs,bits,lane,lane/(64/bits) == u32(upper) ? values[0] : u64{1}<<(bits-1));
                    PutElement(rhs,bits,lane,lane == index ? values[1] : u64{1}<<(bits-1));
                }
                for (u32 lane = 0; lane < 64/bits; ++lane) PutElement(acc,2*bits,lane,values[2]);
                for (bool sticky : {false,true}) CheckCase(program,bits,operation,upper,true,index,regs,lhs,rhs,acc,sticky);
            }
        }
}

TEST_CASE("ARM64 doubling long mixed lane random data preserves source accumulator alias snapshots and full state") {
    u64 random = 0xEC23FADF87D34B61;
    for (u32 bits : {16u,32u}) for (bool upper : {false,true}) for (u32 operation = 0; operation < 3; ++operation)
        for (bool indexed : {false,true}) for (u32 index = 0; index < (indexed ? 128/bits : 1); ++index)
            for (auto regs : Aliases(indexed && bits == 16 ? 15 : 30)) {
                Program program(Encode(bits,operation,upper,indexed,index,regs));
                for (u32 sample = 0; sample < 64; ++sample) {
                    const HighVectorBits lhs{Random(random),Random(random)},rhs{Random(random),Random(random)},acc{Random(random),Random(random)};
                    CheckCase(program,bits,operation,upper,indexed,index,regs,lhs,rhs,acc,sample & 1);
                }
            }
}

TEST_CASE("ARM64 doubling long ignores other source half and unselected indexed lanes but rejects reserved widths") {
    for (u32 bits : {16u,32u}) for (bool upper : {false,true}) for (u32 operation = 0; operation < 3; ++operation)
        for (bool indexed : {false,true}) for (u32 index = 0; index < (indexed ? 128/bits : 1); ++index) {
            const Registers regs{0,31,indexed && bits == 16 ? 15u : 30u};
            Program program(Encode(bits,operation,upper,indexed,index,regs));
            HighVectorBits lhs{},rhs{},acc{};
            for (u32 lane = 0; lane < 128/bits; ++lane) {
                PutElement(lhs,bits,lane,lane/(64/bits) == u32(upper) ? 1 : u64{1}<<(bits-1));
                PutElement(rhs,bits,lane,(indexed ? lane == index : lane/(64/bits) == u32(upper)) ? 1 : u64{1}<<(bits-1));
            }
            CheckCase(program,bits,operation,upper,indexed,index,regs,lhs,rhs,acc,false);
        }
    // One lane saturates, followed by clean lanes. Its QC must survive the
    // other scalar-IR lane helpers and both accumulator cancellation cases.
    for (u32 bits : {16u,32u}) for (bool upper : {false,true}) for (u32 operation = 0; operation < 3; ++operation)
        for (bool indexed : {false,true}) for (u32 saturated_lane = 0; saturated_lane < 64/bits; ++saturated_lane) {
            const u32 index = 128/bits-1;
            const Registers regs{0,31,indexed && bits == 16 ? 15u : 30u};
            Program program(Encode(bits,operation,upper,indexed,index,regs));
            HighVectorBits lhs{},rhs{},acc{};
            for (u32 lane = 0; lane < 128/bits; ++lane) {
                const bool selected = lane == saturated_lane + (upper ? 64/bits : 0);
                PutElement(lhs,bits,lane,selected ? u64{1}<<(bits-1) : 1);
                PutElement(rhs,bits,lane,(indexed ? lane == index : selected) ? u64{1}<<(bits-1) : 1);
            }
            if (operation == 1) PutElement(acc,2*bits,saturated_lane,Mask(2*bits));
            CheckCase(program,bits,operation,upper,indexed,index,regs,lhs,rhs,acc,false);
        }
    for (u32 size : {0u,3u}) for (u32 operation = 0; operation < 3; ++operation)
        for (bool upper : {false,true}) for (bool indexed : {false,true}) {
            const u32 word = (Encode(16,operation,upper,indexed,0,{0,31,15}) & ~(3u<<22)) | (size<<22);
            Program program(word); Machine machine; machine.Context().pc = 0xCAFE0123456789AB;
            const auto before = machine.Context();
            program.Run(machine,swift::arm64::InterruptReason::ILL_CODE);
            REQUIRE(std::memcmp(&machine.Context().v,&before.v,sizeof(before.v)) == 0);
            REQUIRE(machine.Context().fpsr == before.fpsr); REQUIRE(machine.Context().r == before.r);
            REQUIRE(machine.Context().pc == before.pc); REQUIRE(machine.state->current_loc.Value() == 0x1000);
        }
}

