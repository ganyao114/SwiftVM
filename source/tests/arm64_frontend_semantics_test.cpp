#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/frontend/arm64/fp_helpers.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;

namespace {

using swift::arm64::ThreadContext64;
using swift::arm64::InterruptReason;
using namespace vixl::aarch64;

constexpr VAddr kCodeBase = 0x1000;

class CodeMemory final : public MemoryInterface {
public:
    explicit CodeMemory(std::initializer_list<u32> instructions) : code(instructions) {
        code.push_back(0xD4200000);  // BRK #0 terminates the decoded block.
    }
    bool Read(void* dest, size_t address, size_t size) override {
        if (address < kCodeBase || address - kCodeBase + size > code.size() * 4) return false;
        std::memcpy(dest, reinterpret_cast<u8*>(code.data()) + address - kCodeBase, size);
        return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* pointer) override {
        const auto address = reinterpret_cast<size_t>(pointer);
        if (address < kCodeBase || address - kCodeBase >= code.size() * 4) return nullptr;
        return reinterpret_cast<u8*>(code.data()) + address - kCodeBase;
    }
    std::vector<u32> code;
};

struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(ThreadContext64) + 32> storage{};
    backend::State* state;
    Machine() : state(new (storage.data()) backend::State{}) {}
    ~Machine() { state->~State(); }
    ThreadContext64& Context() {
        return *reinterpret_cast<ThreadContext64*>(state->uniform_buffer_begin);
    }
    void Run(std::initializer_list<u32> instructions, InterruptReason reason = InterruptReason::BRK) {
        state->halt_reason = HaltReason::None;
        CodeMemory memory(instructions);
        ir::Block block(0, kCodeBase);
        ir::Assembler assembler(&block);
        swift::arm64::A64Decoder decoder(kCodeBase, &memory, &assembler);
        decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto initial_context = Context();
        const auto initial_flags = state->host_cpu_flags;
#endif
        backend::interp::Interpreter interpreter(*state, &block);
        interpreter.Run();
        REQUIRE(Context().padding == static_cast<u32>(reason));
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto expected_context = Context();
        const auto expected_flags = state->host_cpu_flags;
        for (bool scalar_cache : {false, true}) {
            CAPTURE(scalar_cache);
            Context() = initial_context;
            state->host_cpu_flags = initial_flags;
            state->halt_reason = HaltReason::None;
            backend::x64::Translator translator(&block, backend::x64::HostFeatures::Detect(), nullptr, nullptr, scalar_cache);
            translator.Translate();
            translator.getCode<HaltReason (*)(backend::State*)>()(state);
            REQUIRE(std::memcmp(&Context(), &expected_context, sizeof(expected_context)) == 0);
            REQUIRE(state->host_cpu_flags == expected_flags);
        }
#endif
    }
    void Vector(u32 reg, u64 low, u64 high) {
        const u64 halves[] = {low, high};
        std::memcpy(&Context().v[reg], halves, sizeof(halves));
    }
    u64 Half(u32 reg, bool high = false) {
        u64 value;
        std::memcpy(&value, reinterpret_cast<u8*>(&Context().v[reg]) + (high ? 8 : 0), 8);
        return value;
    }
};

u32 Copy(u32 op, u32 bits, u32 destination_index, u32 source, u32 dest,
         bool q = true, u32 source_index = 0) {
    const u32 size = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : 3;
    const u32 imm5 = (destination_index << (size + 1)) | (1u << size);
    return op | (q ? 0x40000000u : 0u) | (imm5 << 16) |
           ((source_index << size) << 11) | (source << 5) | dest;
}

}  // namespace

TEST_CASE("ARM64 bit miscellaneous instructions count and reverse every byte") {
    for (u32 size : {2u, 3u}) for (bool q : {false, true}) {
        Machine machine;
        machine.Vector(0, 0x123456789ABCDEF0, 0xFEDCBA9876543210);
        machine.Vector(1, ~u64{0}, ~u64{0});
        machine.Context().fpsr = 0x08000091;
        machine.Run({NEON_RBIT_NOT | (q ? 1u << 30 : 0) | (size << 22) | (1u << 5)}, InterruptReason::ILL_CODE);
        REQUIRE(machine.Half(0) == 0x123456789ABCDEF0);
        REQUIRE(machine.Half(0, true) == 0xFEDCBA9876543210);
        REQUIRE(machine.Context().fpsr == 0x08000091);
    }
    for (bool q : {false, true}) for (bool alias : {false, true}) {
        for (u32 op : {u32(NEON_CNT), u32(NEON_RBIT_NOT)}) for (u32 start = 0; start < 256; start += 16) {
            Machine machine;
            std::array<u8, 16> input{}, expected{};
            for (u32 lane = 0; lane < 16; ++lane) {
                input[lane] = start + lane;
                u8 result = 0;
                for (u32 bit = 0; bit < 8; ++bit) {
                    if (op == NEON_CNT) result += (input[lane] >> bit) & 1;
                    else result |= ((input[lane] >> bit) & 1) << (7 - bit);
                }
                expected[lane] = lane < (q ? 16u : 8u) ? result : 0;
            }
            std::memcpy(&machine.Context().v[1], input.data(), 16);
            const u32 rd = alias ? 1 : 0;
            machine.Vector(0, ~u64{0}, ~u64{0});
            machine.Run({op | (q ? 1u << 30 : 0) | (op == NEON_RBIT_NOT ? 1u << 22 : 0) | (1u << 5) | rd});
            REQUIRE(std::memcmp(&machine.Context().v[rd], expected.data(), 16) == 0);
        }
        for (u32 size = 0; size < 3; ++size) for (u32 group : {16u, 32u, 64u}) {
            const u32 bytes = 1u << size;
            if (bytes * 8 >= group) continue;
            Machine machine;
            std::array<u8, 16> input{}, expected{};
            for (u32 lane = 0; lane < 16; ++lane) input[lane] = 3 + lane * 13;
            for (u32 lane = 0; lane < (q ? 16u : 8u); ++lane) {
                const u32 base = lane / (group / 8) * (group / 8);
                const u32 index = lane % (group / 8);
                expected[lane] = input[base + group / 8 - bytes - index / bytes * bytes + index % bytes];
            }
            std::memcpy(&machine.Context().v[1], input.data(), 16);
            const u32 rd = alias ? 1 : 0;
            machine.Vector(0, ~u64{0}, ~u64{0});
            const u32 op = group == 16 ? NEON_REV16 : group == 32 ? NEON_REV32 : NEON_REV64;
            machine.Run({op | (q ? 1u << 30 : 0) | (size << 22) | (1u << 5) | rd});
            REQUIRE(std::memcmp(&machine.Context().v[rd], expected.data(), 16) == 0);
        }
    }
}

TEST_CASE("ARM64 RCpc unscaled transfers cover signs offsets unaligned addresses and aliases") {
    struct Operation { u32 word, bits; bool load, sign, wide; };
    const std::array<Operation, 13> operations{{
        {STLURB,8,false,false,false}, {STLURH,16,false,false,false},
        {STLUR_w,32,false,false,false}, {STLUR_x,64,false,false,true},
        {LDAPURB,8,true,false,false}, {LDAPURH,16,true,false,false},
        {LDAPUR_w,32,true,false,false}, {LDAPUR_x,64,true,false,true},
        {LDAPURSB_w,8,true,true,false}, {LDAPURSB_x,8,true,true,true},
        {LDAPURSH_w,16,true,true,false}, {LDAPURSH_x,16,true,true,true},
        {LDAPURSW,32,true,true,true}
    }};
    for (const auto& op : operations) for (s32 offset : {-256, -17, -1, 0, 1, 7, 255})
        for (u32 rt : {0u, 1u, 31u}) for (bool stack_base : {false, true}) {
            Machine machine;
            std::array<u8, 2048> memory{}, expected{};
            for (u32 i = 0; i < memory.size(); ++i) memory[i] = u8(0x93 + i * 13);
            expected = memory;
            machine.state->pt = memory.data();
            machine.state->guest_addr_limit = memory.size();
            machine.state->guest_addr_mask = UINT64_MAX;
            machine.Context().r[0] = 0xFEDCBA9876543210;
            machine.Context().r[1] = 512;
            machine.Context().sp = 512;
            machine.state->host_cpu_flags = 0xB0000000;
            const u32 rn = stack_base ? 31 : 1;
            const u32 address = 512 + offset;
            u64 wanted = 0;
            if (op.load) {
                std::memcpy(&wanted, memory.data() + address, op.bits / 8);
                if (op.sign && (wanted & (u64{1} << (op.bits - 1)))) wanted |= ~((u64{1} << op.bits) - 1);
                if (!op.wide) wanted = u32(wanted);
            } else {
                const u64 source = rt == 31 ? 0 : machine.Context().r[rt];
                std::memcpy(expected.data() + address, &source, op.bits / 8);
            }
            machine.Run({op.word | ((u32(offset) & 511) << 12) | (rn << 5) | rt});
            if (op.load && rt != 31) REQUIRE(machine.Context().r[rt] == wanted);
            REQUIRE(memory == expected);
            if (!op.load || rt != 1) REQUIRE(machine.Context().r[1] == 512);
            REQUIRE(machine.Context().sp == 512);
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
}

TEST_CASE("ARM64 vector half sign operations cover every encoding and alternate NaN handling") {
    for (u32 op : {u32(NEON_FABS_H), u32(NEON_FNEG_H)})
        for (bool q : {false, true}) for (bool alias : {false, true}) {
            const u32 rd = alias ? 31 : 0;
            CodeMemory code{op | (q ? 1u << 30 : 0) | (31u << 5) | rd};
            ir::Block block(0, kCodeBase);
            ir::Assembler assembler(&block);
            swift::arm64::A64Decoder decoder(kCodeBase, &code, &assembler);
            decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
            backend::x64::Translator native(&block), baseline(&block, {});
            native.Translate(); baseline.Translate();
#endif
            for (u32 mode : {0u, 1u}) for (bool alternate : {false, true})
                for (u32 start = 0; start < 65536; start += q ? 8 : 4) {
                    std::array<u16, 8> input{}, expected{};
                    for (u32 lane = 0; lane < 8; ++lane) {
                        const u16 value = u16(start + lane);
                        input[lane] = value;
                        const bool nan = (value & 0x7C00) == 0x7C00 && (value & 0x3FF);
                        if (lane < (q ? 8u : 4u))
                            expected[lane] = alternate && nan ? value :
                                op == NEON_FABS_H ? value & 0x7FFF : value ^ 0x8000;
                    }
                    Machine machine;
                    machine.Vector(0, ~u64{0}, ~u64{0});
                    std::memcpy(&machine.Context().v[31], input.data(), 16);
                    const u32 fpcr = (mode ? (3u << 22) | (1u << 24) | (1u << 25) | (1u << 19) : 0) |
                                     (alternate ? 2u : 0u);
                    machine.Context().fpcr = fpcr;
                    machine.Context().fpsr = 0x0800009F;
                    machine.state->host_cpu_flags = 0xB0000000;
                    const auto initial = machine.Context();
                    for (u32 route = 0; route < 3; ++route) {
                        machine.Context() = initial;
                        machine.state->halt_reason = HaltReason::None;
                        machine.state->host_cpu_flags = 0xB0000000;
                        if (route == 0) {
                            backend::interp::Interpreter interpreter(*machine.state, &block);
                            interpreter.Run();
                        } else {
#if defined(SWIFT_HAS_X64_BACKEND)
                            auto& translator = route == 1 ? native : baseline;
                            translator.getCode<HaltReason (*)(backend::State*)>()(machine.state);
#else
                            continue;
#endif
                        }
                        REQUIRE(machine.Context().padding == u32(InterruptReason::BRK));
                        REQUIRE(std::memcmp(&machine.Context().v[rd], expected.data(), 16) == 0);
                        if (!alias) REQUIRE(std::memcmp(&machine.Context().v[31], input.data(), 16) == 0);
                        REQUIRE(machine.Context().fpcr == fpcr);
                        REQUIRE(machine.Context().fpsr == 0x0800009F);
                        REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
                    }
                }
        }
}

TEST_CASE("ARM64 RCpc later-instruction faults preserve registers memory and instruction PC") {
    for (u32 word : {u32(LDAPUR_x), u32(STLUR_x)}) {
        CodeMemory code{0xD503201F, word | (1u << 5)};
        ir::Block block(0, kCodeBase);
        ir::Assembler assembler(&block);
        swift::arm64::A64Decoder decoder(kCodeBase, &code, &assembler);
        decoder.Decode();
        for (bool native : {false, true}) {
            Machine machine;
            std::array<u8, 8> bytes{};
            machine.state->pt = bytes.data();
            machine.state->guest_addr_limit = bytes.size();
            machine.state->guest_addr_mask = UINT64_MAX;
            machine.Context().r[0] = 0xFEDCBA9876543210;
            machine.Context().r[1] = 4;
            machine.state->host_cpu_flags = 0xB0000000;
            const auto initial = machine.Context();
            if (native) {
#if defined(SWIFT_HAS_X64_BACKEND)
                backend::x64::Translator translator(&block);
                translator.Translate();
                REQUIRE(translator.getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::PageFatal);
#else
                continue;
#endif
            } else {
                backend::interp::Interpreter interpreter(*machine.state, &block);
                REQUIRE(interpreter.Run() == HaltReason::PageFatal);
            }
            REQUIRE(std::memcmp(&machine.Context(), &initial, sizeof(initial)) == 0);
            REQUIRE(bytes == std::array<u8, 8>{});
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
            REQUIRE(machine.state->current_loc.Value() == kCodeBase + 4);
        }
    }
}

TEST_CASE("ARM64 FlagM rotates selected flags and preserves every unselected flag") {
    for (u32 shift = 0; shift < 64; ++shift) for (u32 mask = 0; mask < 16; ++mask) {
        const u64 value = 0xFEDCBA9876543210 ^ (u64{0x1020304050607081} * (shift + 1));
        const u64 rotated = shift ? (value >> shift) | (value << (64 - shift)) : value;
        for (u32 flags : {0u, 5u, 10u, 15u}) {
            Machine machine;
            machine.Context().r[1] = value;
            machine.Context().r[3] = u64(flags) << 28;
            machine.Run({0xD51B4203, RMIF | (shift << 15) | (1u << 5) | mask, 0xD53B4202});
            REQUIRE(machine.Context().r[2] == (((rotated & mask) | (flags & ~mask)) << 28));
        }
    }
    Machine zero;
    zero.Context().r[3] = u64{15} << 28;
    zero.Run({0xD51B4203, RMIF | (31u << 5) | 15, 0xD53B4202});
    REQUIRE(zero.Context().r[2] == 0);
}

TEST_CASE("ARM64 FlagM evaluates narrow signed results while preserving carry") {
    for (u32 bits : {8u, 16u}) {
        std::vector<u32> values;
        if (bits == 8) for (u32 value = 0; value < 512; ++value) values.push_back(value | 0xDEAD0000);
        else {
            values = {0, 1, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0x10000, 0x10001,
                      0x17FFF, 0x18000, 0x1FFFF, 0xFFFF0000, 0xFFFF8000, 0xFFFFFFFF};
        }
        const u32 lane_mask = (1u << bits) - 1;
        for (u32 value : values) for (u32 flags : {0u, 2u, 13u, 15u}) {
            Machine machine;
            machine.Context().r[1] = u64{value} | 0xBEEF000000000000;
            machine.Context().r[3] = u64(flags) << 28;
            machine.Run({0xD51B4203, (bits == 8 ? SETF8 : SETF16) | (1u << 5), 0xD53B4202});
            const u32 n = (value >> (bits - 1)) & 1;
            const u32 z = (value & lane_mask) == 0;
            const u32 v = n ^ ((value >> bits) & 1);
            REQUIRE(machine.Context().r[2] == u64((n << 3) | (z << 2) | (flags & 2) | v) << 28);
        }
        Machine zero;
        zero.Context().r[3] = u64{15} << 28;
        zero.Run({0xD51B4203, (bits == 8 ? SETF8 : SETF16) | (31u << 5), 0xD53B4202});
        REQUIRE(zero.Context().r[2] == u64{6} << 28);
    }
}

TEST_CASE("ARM64 bit miscellaneous CLZ and CLS handle zero and sign boundaries") {
    for (u32 size = 0; size < 3; ++size) {
        const u32 bits = 8u << size, bytes = bits / 8;
        const u64 mask = (u64{1} << bits) - 1;
        std::vector<u64> values{0, mask};
        if (bits == 8) for (u32 value = 0; value < 256; ++value) values.push_back(value);
        else for (u32 bit = 0; bit < bits; ++bit) {
            values.push_back(u64{1} << bit);
            values.push_back(mask ^ (u64{1} << bit));
        }
        for (bool q : {false, true}) for (bool alias : {false, true})
            for (u32 op : {u32(NEON_CLZ), u32(NEON_CLS)}) for (size_t start = 0; start < values.size(); start += 16 / bytes) {
                Machine machine;
                std::array<u8, 16> input{}, expected{};
                for (u32 lane = 0; lane < 16 / bytes; ++lane) {
                    const u64 value = values[(start + lane) % values.size()];
                    std::memcpy(input.data() + lane * bytes, &value, bytes);
                    u32 result = 0;
                    const u32 sign = (value >> (bits - 1)) & 1;
                    for (s32 bit = bits - (op == NEON_CLS ? 2 : 1); bit >= 0; --bit) {
                        if (((value >> bit) & 1) != (op == NEON_CLS ? sign : 0)) break;
                        ++result;
                    }
                    if (lane < (q ? 16u : 8u) / bytes) std::memcpy(expected.data() + lane * bytes, &result, bytes);
                }
                std::memcpy(&machine.Context().v[1], input.data(), 16);
                const u32 rd = alias ? 1 : 0;
                machine.Vector(0, ~u64{0}, ~u64{0});
                machine.Run({op | (q ? 1u << 30 : 0) | (size << 22) | (1u << 5) | rd});
                REQUIRE(std::memcmp(&machine.Context().v[rd], expected.data(), 16) == 0);
            }
    }
}

TEST_CASE("ARM64 bit miscellaneous narrowing clamps boundaries and preserves QC") {
    for (u32 size = 0; size < 3; ++size) {
        const u32 bits = 8u << size, source_bits = bits * 2;
        const u32 bytes = bits / 8, source_bytes = bytes * 2;
        const u64 mask = (u64{1} << bits) - 1;
        const u64 source_mask = source_bits == 64 ? ~u64{0} : (u64{1} << source_bits) - 1;
        const u64 sign = u64{1} << (bits - 1), source_sign = u64{1} << (source_bits - 1);
        const std::array<u64, 14> values{0, 1, sign - 1, sign, sign + 1, mask - 1, mask, mask + 1,
                                      source_sign - 1, source_sign, source_sign + 1,
                                      source_mask - sign, source_mask - sign + 1, source_mask};
        for (u32 op : {u32(NEON_XTN), u32(NEON_SQXTN), u32(NEON_UQXTN), u32(NEON_SQXTUN)})
            for (bool q : {false, true}) for (bool alias : {false, true}) for (bool sticky : {false, true})
                for (size_t start = 0; start < values.size(); ++start) {
                    Machine machine;
                    std::array<u8, 16> input{}, expected{};
                    for (u32 i = 0; i < 16; ++i) input[i] = 0xA0 + i;
                    bool saturated = false;
                    for (u32 lane = 0; lane < 16 / source_bytes; ++lane) {
                        const u64 raw = values[(start + lane) % values.size()];
                        std::memcpy(input.data() + lane * source_bytes, &raw, source_bytes);
                        u64 result = raw & mask;
                        if (op == NEON_UQXTN) {
                            saturated |= raw > mask;
                            result = raw > mask ? mask : raw;
                        } else if (op != NEON_XTN) {
                            const s64 signed_value = s64(raw & source_sign ? raw | ~source_mask : raw);
                            const s64 lower = op == NEON_SQXTN ? -s64(sign) : 0;
                            const s64 upper = op == NEON_SQXTN ? s64(sign - 1) : s64(mask);
                            const s64 clipped = signed_value < lower ? lower : signed_value > upper ? upper : signed_value;
                            saturated |= clipped != signed_value;
                            result = u64(clipped) & mask;
                        }
                        std::memcpy(expected.data() + (q ? 8 : 0) + lane * bytes, &result, bytes);
                    }
                    const u32 rd = alias ? 1 : 0;
                    machine.Vector(0, 0x1122334455667788, ~u64{0});
                    std::memcpy(&machine.Context().v[1], input.data(), 16);
                    if (q) std::memcpy(expected.data(), &machine.Context().v[rd], 8);
                    machine.Context().fpsr = 0x91 | (sticky ? 1u << 27 : 0);
                    machine.Run({op | (q ? 1u << 30 : 0) | (size << 22) | (1u << 5) | rd});
                    REQUIRE(std::memcmp(&machine.Context().v[rd], expected.data(), 16) == 0);
                    REQUIRE(machine.Context().fpsr == (0x91 | ((sticky || (saturated && op != NEON_XTN)) ? 1u << 27 : 0)));
                }
    }
}

TEST_CASE("ARM64 single-structure lane loads preserve inactive bits and post-index") {
    for (u32 size = 0; size < 4; ++size) {
        const u32 bytes = 1u << size;
        for (u32 lane = 0; lane < 16 / bytes; ++lane) {
            for (u32 count = 1; count <= 4; ++count) {
                Machine machine;
                std::array<u8, 64> memory{};
                for (u32 index = 0; index < memory.size(); ++index) memory[index] = u8(index * 17 + 3);
                machine.state->pt = memory.data();
                machine.state->guest_addr_limit = memory.size();
                machine.Context().r[1] = 1;
                const u32 rt = 30;
                for (u32 reg = 0; reg < count; ++reg)
                    machine.Vector((rt + reg) & 31, 0x123456789ABCDEF0, 0xFEDCBA9876543210);
                std::array<std::array<u8, 16>, 4> expected{};
                for (u32 reg = 0; reg < count; ++reg) {
                    std::memcpy(expected[reg].data(), &machine.Context().v[(rt + reg) & 31], 16);
                    std::memcpy(expected[reg].data() + lane * bytes, memory.data() + 1 + reg * bytes, bytes);
                }
                const u32 index = lane << size;
                const u32 code = 0x0D400000u | (1u << 23) | (31u << 16) |
                        (((count - 1) & 1) << 21) | (((count - 1) >> 1) << 13) |
                        ((size == 3 ? 2 : size) << 14) | ((index >> 3) << 30) |
                        (((index >> 2) & 1) << 12) | (((index & 3) | (size == 3)) << 10) |
                        (1u << 5) | rt;
                machine.Run({code});
                REQUIRE(machine.Context().r[1] == 1 + count * bytes);
                for (u32 reg = 0; reg < count; ++reg)
                    REQUIRE(std::memcmp(&machine.Context().v[(rt + reg) & 31], expected[reg].data(), 16) == 0);
            }
        }
    }
}

TEST_CASE("ARM64 replicated structure loads clear Q0 high bits and wrap registers") {
    for (u32 size = 0; size < 4; ++size) for (u32 count = 1; count <= 4; ++count)
        for (bool q : {false, true}) {
            Machine machine;
            std::array<u8, 64> memory{};
            for (u32 index = 0; index < memory.size(); ++index) memory[index] = u8(index * 13 + 5);
            machine.state->pt = memory.data();
            machine.state->guest_addr_limit = memory.size();
            machine.Context().r[1] = 1;
            machine.Context().r[2] = 33;
            const u32 rt = 31, bytes = 1u << size;
            for (u32 reg = 0; reg < count; ++reg) machine.Vector((rt + reg) & 31, ~u64{0}, ~u64{0});
            const u32 code = 0x0D40C000u | (u32(q) << 30) | (1u << 23) | (2u << 16) |
                    (((count - 1) & 1) << 21) | (((count - 1) >> 1) << 13) |
                    (size << 10) | (1u << 5) | rt;
            machine.Run({code});
            REQUIRE(machine.Context().r[1] == 34);
            for (u32 reg = 0; reg < count; ++reg) {
                std::array<u8, 16> expected{};
                for (u32 position = 0; position < (q ? 16u : 8u); position += bytes)
                    std::memcpy(expected.data() + position, memory.data() + 1 + reg * bytes, bytes);
                REQUIRE(std::memcmp(&machine.Context().v[(rt + reg) & 31], expected.data(), 16) == 0);
            }
        }
}

TEST_CASE("ARM64 single-structure fault preserves destination and base at its instruction PC") {
    Machine machine;
    machine.Context().r[1] = 3;
    machine.Vector(0, 0x123456789ABCDEF0, 0xFEDCBA9876543210);
    machine.state->guest_addr_limit = 4;
    const auto initial = machine.Context();
    CodeMemory memory({0xD503201F, 0x0DDFC020 | (3u << 10)}); // NOP; LD1R {v0.1d}, [x1], #8.
    ir::Block block(0, kCodeBase);
    ir::Assembler assembler(&block);
    swift::arm64::A64Decoder decoder(kCodeBase, &memory, &assembler);
    decoder.Decode();
    backend::interp::Interpreter interpreter(*machine.state, &block);
    REQUIRE(interpreter.Run() == HaltReason::PageFatal);
    REQUIRE(std::memcmp(&machine.Context(), &initial, sizeof(initial)) == 0);
    REQUIRE(machine.state->current_loc.Value() == kCodeBase + 4);
#if defined(SWIFT_HAS_X64_BACKEND)
    machine.state->halt_reason = HaltReason::None;
    backend::x64::Translator translator(&block);
    translator.Translate();
    REQUIRE(translator.getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::PageFatal);
    REQUIRE(std::memcmp(&machine.Context(), &initial, sizeof(initial)) == 0);
    REQUIRE(machine.state->current_loc.Value() == kCodeBase + 4);
#endif
}

TEST_CASE("ARM64 scalar floating arithmetic observes rounding and FPSR") {
    const u32 add = FADD_s | (2u << 16) | (1u << 5);
    for (u32 mode = 0; mode < 4; ++mode) {
        Machine machine;
        machine.Context().fpcr = mode << 22;
        machine.Vector(1, 0x3F800000, ~u64{0});
        machine.Vector(2, 0x33800000, ~u64{0});  // Half an ULP at 1.0f.
        machine.Run({add});
        REQUIRE(machine.Half(0) == (mode == 1 ? 0x3F800001 : 0x3F800000));
        REQUIRE(machine.Half(0, true) == 0);
        REQUIRE(machine.Context().fpsr == 16);
    }
    Machine machine;
    machine.Vector(1, 0x3F800000, 0);
    machine.Vector(2, 0, 0);
    machine.Run({FDIV_s | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x7F800000);
    REQUIRE(machine.Context().fpsr == 2);
    machine.Vector(1, 0xBF800000, 0);
    machine.Context().fpsr = 0;
    machine.Run({FSQRT_s | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x7FC00000);
    REQUIRE(machine.Context().fpsr == 1);
}

TEST_CASE("ARM64 scalar floating NaNs, flush and comparisons are architectural") {
    Machine machine;
    machine.Vector(1, 0x7F800123, 0);
    machine.Vector(2, 0x7FC00456, 0);
    machine.Run({FADD_s | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x7FC00123);
    REQUIRE(machine.Context().fpsr == 1);
    machine.Context().fpcr = 1u << 25;
    machine.Context().fpsr = 0;
    machine.Run({FADD_s | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x7FC00000);
    REQUIRE(machine.Context().fpsr == 1);
    machine.Context().fpcr = 1u << 24;
    machine.Context().fpsr = 0;
    machine.Vector(1, 1, 0);
    machine.Vector(2, 0x3F800000, 0);
    machine.Run({FADD_s | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x3F800000);
    REQUIRE(machine.Context().fpsr == 128);
    machine.Context().fpcr = 0;
    machine.Context().fpsr = 0;
    machine.Vector(1, 0x7FC00123, 0);
    machine.Run({FCMP_s | (2u << 16) | (1u << 5), 0xD53B4203});  // MRS x3,NZCV.
    REQUIRE(machine.Context().r[3] == 0x30000000);
    REQUIRE(machine.Context().fpsr == 0);
    machine.Run({FCMPE_s | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Context().fpsr == 1);
}

TEST_CASE("ARM64 scalar floating conversions, rounding and fused operations") {
    Machine machine;
    machine.Vector(1, 0x40200000, 0);  // 2.5
    machine.Run({FRINTN_s | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x40000000);
    REQUIRE(machine.Context().fpsr == 0);
    machine.Run({FRINTA_s | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x40400000);
    machine.Run({FRINTX_s | (1u << 5)});
    REQUIRE(machine.Context().fpsr == 16);
    machine.Run({FCVT_ds | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x4004000000000000);
    machine.Context().fpsr = 0;
    machine.Vector(1, 0x7FC00123, 0);
    machine.Run({FCVTZS_xs | (1u << 5)});
    REQUIRE(machine.Context().r[0] == 0);
    REQUIRE(machine.Context().fpsr == 1);
    machine.Context().r[1] = 0xFFFFFFFFFFFFFFFF;
    machine.Run({SCVTF_dx | (1u << 5)});
    REQUIRE(machine.Half(0) == 0xBFF0000000000000);
    machine.Vector(1, 0x3F800001, 0);
    machine.Vector(2, 0x3F7FFFFE, 0);
    machine.Vector(3, 0xBF800000, 0);
    machine.Context().fpsr = 0;
    machine.Run({FMADD_s | (2u << 16) | (3u << 10) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0xA8800000);  // -2^-46, one final rounding.
    REQUIRE(machine.Context().fpsr == 0);
}

TEST_CASE("ARM64 unsupported floating control modes preserve destination at fallback") {
    Machine machine;
    machine.Context().fpcr = 1u << 8;  // Trapping exceptions require host integration.
    machine.Vector(0, 0x1234, 0x5678);
    machine.Vector(1, 0x3F800000, 0);
    machine.Run({FADD_s | (1u << 16) | (1u << 5)}, InterruptReason::FALLBACK);
    REQUIRE(machine.Half(0) == 0x1234);
    REQUIRE(machine.Half(0, true) == 0x5678);
    REQUIRE(machine.Context().pc == kCodeBase);
}

TEST_CASE("ARM64 floating conditional comparisons only evaluate the selected arm") {
    Machine machine;
    machine.state->host_cpu_flags = 0;  // Z=0: EQ fails.
    machine.Context().fpcr = 1u << 8;
    machine.Vector(1, 0x7F800001, 0);
    machine.Vector(2, 0x3F800000, 0);
    machine.Run({FCCMPE_s | (2u << 16) | (1u << 5) | 10u, 0xD53B4203});
    REQUIRE(machine.Context().r[3] == 0xA0000000);
    REQUIRE(machine.Context().fpsr == 0);
    machine.Context().fpcr = 0;
    machine.state->host_cpu_flags = 0x40000000;
    machine.Run({FCCMP_s | (2u << 16) | (1u << 5), 0xD53B4203});
    REQUIRE(machine.Context().r[3] == 0x30000000);
    REQUIRE(machine.Context().fpsr == 1);
}

TEST_CASE("ARM64 floating zero signs, output flushing and invalid fused products") {
    Machine machine;
    machine.Context().fpcr = 2u << 22;
    machine.Vector(1, 0x3F800000, 0);
    machine.Vector(2, 0xBF800000, 0);
    machine.Run({FADD_s | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x80000000);
    machine.Context().fpcr = 1u << 24;
    machine.Vector(1, 0x00800000, 0);
    machine.Vector(2, 0x40000000, 0);
    machine.Run({FDIV_s | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0);
    REQUIRE(machine.Context().fpsr == 8);
    machine.Context().fpcr = 0;
    machine.Context().fpsr = 0;
    machine.Vector(1, 0x7F800000, 0);
    machine.Vector(2, 0, 0);
    machine.Vector(3, 0x7FC00123, 0);
    machine.Run({FMADD_s | (2u << 16) | (3u << 10) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x7FC00000);
    REQUIRE(machine.Context().fpsr == 1);
    machine.Context().fpsr = 0;
    machine.Vector(1, 0x7F800123, 0);
    machine.Vector(2, 0x7FC00456, 0);
    machine.Run({FNMUL_s | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0xFFC00123);
    REQUIRE(machine.Context().fpsr == 1);
}

TEST_CASE("ARM64 floating integer conversion saturates and preserves rounding contracts") {
    Machine machine;
    machine.Vector(1, 0x4F800000, 0);  // 2^32.
    machine.Run({FCVTZU_ws | (1u << 5)});
    REQUIRE(machine.Context().r[0] == 0xFFFFFFFF);
    REQUIRE(machine.Context().fpsr == 1);
    machine.Context().fpsr = 0;
    machine.Vector(1, 0xBE800000, 0);  // -0.25, round-to-zero remains representable.
    machine.Run({FCVTZU_ws | (1u << 5)});
    REQUIRE(machine.Context().r[0] == 0);
    REQUIRE(machine.Context().fpsr == 16);
    machine.Context().fpsr = 0;
    machine.Vector(1, 0x3FC00000, 0);  // 1.5.
    machine.Run({FCVTNS_ws | (1u << 5)});
    REQUIRE(machine.Context().r[0] == 2);
    REQUIRE(machine.Context().fpsr == 16);
    machine.Context().fpsr = 0;
    machine.Context().r[1] = ~u64{0};
    machine.Run({UCVTF_sx | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x5F800000);
    REQUIRE(machine.Context().fpsr == 16);
}

TEST_CASE("ARM64 fixed-point floating conversion scales every legal fractional width exactly") {
    for (u32 width : {32u, 64u}) {
        for (u32 fp_bits : {32u, 64u}) {
            const u32 fields = (width == 64 ? 0x80000000u : 0) |
                               (fp_bits == 64 ? 0x00400000u : 0) | (1u << 5);
            for (u32 fractional_bits = 1; fractional_bits <= width; ++fractional_bits) {
                INFO("width=" << width << " fp_bits=" << fp_bits << " fbits=" << fractional_bits);
                const u32 scale = (64 - fractional_bits) << 10;
                const u64 expected = fp_bits == 32 ? u64(127 - fractional_bits) << 23 :
                                                    u64(1023 - fractional_bits) << 52;
                Machine machine;
                machine.Context().r[1] = 1;
                machine.Vector(0, ~u64{0}, ~u64{0});
                machine.Run({SCVTF_fixed | fields | scale});
                REQUIRE(machine.Half(0) == expected);
                REQUIRE(machine.Half(0, true) == 0);
                REQUIRE(machine.Context().fpsr == 0);
                machine.Run({UCVTF_fixed | fields | scale});
                REQUIRE(machine.Half(0) == expected);
                machine.Vector(1, expected, ~u64{0});
                machine.Run({FCVTZS_fixed | fields | scale});
                REQUIRE(machine.Context().r[0] == 1);
                machine.Run({FCVTZU_fixed | fields | scale});
                REQUIRE(machine.Context().r[0] == 1);
                REQUIRE(machine.Context().fpsr == 0);
            }
        }
    }
}

TEST_CASE("ARM64 fixed-point conversions round only after binary scaling") {
    for (u32 width : {32u, 64u}) {
        for (u32 fp_bits : {32u, 64u}) {
            const u32 fields = (width == 64 ? 0x80000000u : 0) |
                               (fp_bits == 64 ? 0x00400000u : 0) |
                               ((64 - width) << 10) | (1u << 5);
            for (u32 mode = 0; mode < 4; ++mode) {
                Machine machine;
                machine.Context().fpcr = mode << 22;
                machine.Context().r[1] = width == 32 ? 0xFFFFFFFF : ~u64{0};
                machine.Run({UCVTF_fixed | fields});
                const u64 expected = fp_bits == 32 ? (mode < 2 ? 0x3F800000 : 0x3F7FFFFF) :
                                     width == 32 ? 0x3FEFFFFFFFE00000 :
                                     mode < 2 ? 0x3FF0000000000000 : 0x3FEFFFFFFFFFFFFF;
                REQUIRE(machine.Half(0) == expected);
                REQUIRE(machine.Context().fpsr == (fp_bits == 64 && width == 32 ? 0 : 16));
            }
            Machine machine;
            const u64 minimum = u64{1} << (width - 1);
            machine.Context().r[1] = minimum;
            machine.Run({SCVTF_fixed | fields});
            REQUIRE(machine.Half(0) == (fp_bits == 32 ? 0xBF000000 : 0xBFE0000000000000));
            REQUIRE(machine.Context().fpsr == 0);
            machine.Vector(1, machine.Half(0), 0);
            machine.Run({FCVTZS_fixed | fields});
            REQUIRE(machine.Context().r[0] == minimum);
            REQUIRE(machine.Context().fpsr == 0);
        }
    }
}

TEST_CASE("ARM64 fixed-point conversion saturates, truncates and honors input flushing") {
    for (u32 width : {32u, 64u}) {
        for (u32 fp_bits : {32u, 64u}) {
            const u32 fields = (width == 64 ? 0x80000000u : 0) |
                               (fp_bits == 64 ? 0x00400000u : 0) | (1u << 5);
            const u32 scale = (64 - width) << 10;
            const u64 one = fp_bits == 32 ? 0x3F800000 : 0x3FF0000000000000;
            Machine machine;
            machine.Vector(1, one, 0);
            machine.Run({FCVTZS_fixed | fields | scale});
            REQUIRE(machine.Context().r[0] == (u64{1} << (width - 1)) - 1);
            REQUIRE(machine.Context().fpsr == 1);
            machine.Context().fpsr = 0;
            machine.Run({FCVTZU_fixed | fields | scale});
            REQUIRE(machine.Context().r[0] == (width == 32 ? 0xFFFFFFFF : ~u64{0}));
            REQUIRE(machine.Context().fpsr == 1);
            machine.Context().fpsr = 0;
            machine.Vector(1, fp_bits == 32 ? 0xBE800000 : 0xBFD0000000000000, 0);
            machine.Run({FCVTZS_fixed | fields | (63u << 10)});  // -0.25 * 2 -> zero.
            REQUIRE(machine.Context().r[0] == 0);
            REQUIRE(machine.Context().fpsr == 16);
            machine.Context().fpsr = 0;
            machine.Run({FCVTZU_fixed | fields | (63u << 10)});
            REQUIRE(machine.Context().r[0] == 0);
            REQUIRE(machine.Context().fpsr == 16);
            machine.Context().fpcr = 1u << 24;
            machine.Context().fpsr = 0;
            machine.Vector(1, 1, 0);
            machine.Run({FCVTZS_fixed | fields | scale});
            REQUIRE(machine.Context().r[0] == 0);
            REQUIRE(machine.Context().fpsr == 128);
            machine.Context().fpcr = 0;
            machine.Context().fpsr = 0;
            const u64 infinity = fp_bits == 32 ? 0x7F800000 : 0x7FF0000000000000;
            machine.Vector(1, infinity | (fp_bits == 32 ? 0x00400123 : 0x0008000000000123), 0);
            machine.Run({FCVTZS_fixed | fields | scale});
            REQUIRE(machine.Context().r[0] == 0);
            REQUIRE(machine.Context().fpsr == 1);
            machine.Context().fpsr = 0;
            machine.Vector(1, infinity, 0);
            machine.Run({FCVTZU_fixed | fields | scale});
            REQUIRE(machine.Context().r[0] == (width == 32 ? 0xFFFFFFFF : ~u64{0}));
            REQUIRE(machine.Context().fpsr == 1);
            const u32 unsupported_scale = SCVTF_fixed | (fields & ~0x80000000u) | (31u << 10);
            REQUIRE_FALSE(swift::arm64::IsSupportedScalarFP(unsupported_scale));
        }
    }
}

TEST_CASE("ARM64 fixed-point unsupported floating control exits at the instruction") {
    Machine machine;
    machine.Context().fpcr = 1u << 8;
    machine.Vector(0, 0x1234, 0x5678);
    machine.Context().r[1] = 1;
    machine.Run({SCVTF_sw_fixed | (32u << 10) | (1u << 5)}, InterruptReason::FALLBACK);
    REQUIRE(machine.Half(0) == 0x1234);
    REQUIRE(machine.Half(0, true) == 0x5678);
    REQUIRE(machine.Context().fpsr == 0);
    REQUIRE(machine.Context().pc == kCodeBase);
}

TEST_CASE("ARM64 half floating arithmetic honors rounding, NaNs and scalar clearing") {
    for (u32 mode = 0; mode < 4; ++mode) {
        Machine machine;
        machine.Context().fpcr = mode << 22;
        machine.Vector(0, ~u64{0}, ~u64{0});
        machine.Vector(1, 0x3C00, ~u64{0});
        machine.Vector(2, 0x1000, ~u64{0});  // Half an ULP at 1.0h.
        machine.Run({FADD_h | (2u << 16) | (1u << 5)});
        REQUIRE(machine.Half(0) == (mode == 1 ? 0x3C01 : 0x3C00));
        REQUIRE(machine.Half(0, true) == 0);
        REQUIRE(machine.Context().fpsr == 16);
    }
    Machine machine;
    machine.Vector(1, 0x7C01, 0);
    machine.Vector(2, 0x7E12, 0);
    machine.Run({FADD_h | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x7E01);
    REQUIRE(machine.Context().fpsr == 1);
    machine.Context().fpsr = 0;
    machine.Run({FNMUL_h | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0xFE01);
    REQUIRE(machine.Context().fpsr == 1);
    machine.Context().fpcr = 1u << 25;
    machine.Context().fpsr = 0;
    machine.Run({FADD_h | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x7E00);
    REQUIRE(machine.Context().fpsr == 1);
    machine.Context().fpcr = 0;
    machine.Context().fpsr = 0;
    machine.Vector(1, 0xBC00, 0);
    machine.Run({FSQRT_h | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x7E00);
    REQUIRE(machine.Context().fpsr == 1);
    machine.Context().fpsr = 0;
    machine.Vector(1, 0x7BFF, 0);
    machine.Vector(2, 0x4000, 0);
    machine.Run({FMUL_h | (2u << 16) | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x7C00);
    REQUIRE(machine.Context().fpsr == 20);
}

TEST_CASE("ARM64 FZ16 controls half arithmetic independently of FZ") {
    for (u32 fpcr : {0u, 1u << 19, 1u << 24}) {
        Machine machine;
        machine.Context().fpcr = fpcr;
        machine.Vector(1, 1, 0);
        machine.Vector(2, 0x3C00, 0);
        machine.Run({FADD_h | (2u << 16) | (1u << 5)});
        REQUIRE(machine.Half(0) == 0x3C00);
        REQUIRE(machine.Context().fpsr == (fpcr == (1u << 19) ? 0 : 16));
        machine.Context().fpsr = 0;
        machine.Vector(1, 0x0400, 0);
        machine.Vector(2, 0x4000, 0);
        machine.Run({FDIV_h | (2u << 16) | (1u << 5)});
        REQUIRE(machine.Half(0) == (fpcr == (1u << 19) ? 0 : 0x0200));
        REQUIRE(machine.Context().fpsr == (fpcr == (1u << 19) ? 8 : 0));
        machine.Context().fpsr = 0;
        machine.Vector(1, 1, 0);
        machine.Run({FCVT_sh | (1u << 5)});
        REQUIRE(machine.Half(0) == 0x33800000);
        REQUIRE(machine.Context().fpsr == 0);
        machine.Run({FCVT_dh | (1u << 5)});
        REQUIRE(machine.Half(0) == 0x3E70000000000000);
        machine.Vector(1, 0x33800000, 0);
        machine.Run({FCVT_hs | (1u << 5)});
        REQUIRE(machine.Half(0) == 1);
        REQUIRE(machine.Context().fpsr == 0);
        machine.Context().fpsr = 0;
        machine.Vector(1, 0x33000000, 0);
        machine.Run({FCVT_hs | (1u << 5)});
        REQUIRE(machine.Half(0) == 0);
        REQUIRE(machine.Context().fpsr == 24);
    }
}

TEST_CASE("ARM64 half conversion round-trips every binary16 encoding") {
    Machine machine;
    for (u32 bits = 0; bits < 65536; ++bits) {
        INFO("half_bits=" << bits);
        machine.Context().fpsr = 0;
        machine.Vector(1, bits, ~u64{0});
        const u64 context = reinterpret_cast<u64>(&machine.Context());
        swift::arm64::ExecuteScalarFP(context, FCVT_sh | (1u << 5), kCodeBase);
        swift::arm64::ExecuteScalarFP(context, FCVT_hs | 2u, kCodeBase + 4);
        const bool nan = (bits & 0x7C00) == 0x7C00 && (bits & 0x03FF);
        const bool signaling = nan && !(bits & 0x0200);
        REQUIRE(machine.Half(2) == (nan ? bits | 0x0200 : bits));
        REQUIRE(machine.Half(2, true) == 0);
        REQUIRE(machine.Context().fpsr == (signaling ? 1 : 0));
    }
}

TEST_CASE("ARM64 half scalar rounding, comparisons and integer conversions") {
    Machine machine;
    machine.Vector(1, 0x4100, 0);  // 2.5h.
    machine.Run({FRINTN_h | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x4000);
    REQUIRE(machine.Context().fpsr == 0);
    machine.Run({FRINTA_h | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x4200);
    machine.Run({FRINTX_h | (1u << 5)});
    REQUIRE(machine.Context().fpsr == 16);
    machine.Context().fpsr = 0;
    machine.Vector(1, 0x7E01, 0);
    machine.Vector(2, 0x3C00, 0);
    machine.Run({FCMP_h | (2u << 16) | (1u << 5), 0xD53B4203});
    REQUIRE(machine.Context().r[3] == 0x30000000);
    REQUIRE(machine.Context().fpsr == 0);
    machine.Run({FCVTZS_xh | (1u << 5)});
    REQUIRE(machine.Context().r[0] == 0);
    REQUIRE(machine.Context().fpsr == 1);
    machine.Context().fpsr = 0;
    machine.Context().r[1] = 4097;
    machine.Run({SCVTF_hx | (1u << 5)});
    REQUIRE(machine.Half(0) == 0x6C00);
    REQUIRE(machine.Context().fpsr == 16);
    machine.Context().fpsr = 0;
    machine.Context().r[1] = u64{1} << 63;
    machine.Run({SCVTF_hx_fixed | (1u << 5)});  // X, 64 fractional bits.
    REQUIRE(machine.Half(0) == 0xB800);
    REQUIRE(machine.Context().fpsr == 0);
    machine.Vector(1, 0xB800, 0);
    machine.Run({FCVTZS_xh_fixed | (1u << 5)});
    REQUIRE(machine.Context().r[0] == (u64{1} << 63));
    REQUIRE(machine.Context().fpsr == 0);
}

TEST_CASE("ARM64 alternative half precision requests precise fallback") {
    Machine machine;
    machine.Context().fpcr = 1u << 26;
    machine.Context().fpsr = 0x08000000;
    machine.Vector(0, 0x1234, 0x5678);
    machine.Vector(1, 0x3C00, 0);
    machine.Run({FADD_h | (1u << 16) | (1u << 5)}, InterruptReason::FALLBACK);
    REQUIRE(machine.Half(0) == 0x1234);
    REQUIRE(machine.Half(0, true) == 0x5678);
    REQUIRE(machine.Context().fpsr == 0x08000000);
    REQUIRE(machine.Context().pc == kCodeBase);
}

TEST_CASE("ARM64 NEON copy preserves lane selection, aliases and signs") {
    for (u32 bits : {8u, 16u, 32u, 64u}) {
        for (u32 index = 0; index < 128 / bits; ++index) {
            INFO("bits=" << bits << " index=" << index);
            Machine machine;
            constexpr u64 low = 0xFEDCBA9876543210;
            constexpr u64 high = 0x8877665544332211;
            const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
            const u64 lane = ((index * bits >= 64 ? high : low) >> (index * bits % 64)) & mask;
            machine.Vector(1, low, high);
            machine.Run({Copy(NEON_DUP_ELEMENT, bits, index, 1, 1)});
            u64 broadcast = lane;
            for (u32 shift = bits; shift < 64; shift *= 2) broadcast |= broadcast << shift;
            REQUIRE(machine.Half(1) == broadcast);
            REQUIRE(machine.Half(1, true) == broadcast);

            machine.Vector(1, low, high);
            machine.Run({Copy(NEON_DUP_ELEMENT_scalar, bits, index, 1, 0)});
            REQUIRE(machine.Half(0) == lane);
            REQUIRE(machine.Half(0, true) == 0);

            machine.Vector(1, low, high);
            machine.Run({Copy(NEON_UMOV, bits, index, 1, 0, bits == 64)});
            REQUIRE(machine.Context().r[0] == lane);

            machine.Vector(1, low, high);
            machine.Context().r[2] = ~u64{0};
            machine.Run({Copy(NEON_INS_GENERAL, bits, index, 2, 1)});
            const u64 inserted_mask = mask << (index * bits % 64);
            REQUIRE(machine.Half(1, index * bits >= 64) ==
                    ((index * bits >= 64 ? high : low) | inserted_mask));
            REQUIRE(machine.Half(1, index * bits < 64) == (index * bits >= 64 ? low : high));

            machine.Vector(1, low, high);
            const u32 source_index = (index + 1) % (128 / bits);
            const u64 source_lane = ((source_index * bits >= 64 ? high : low) >>
                                     (source_index * bits % 64)) & mask;
            machine.Run({Copy(NEON_INS_ELEMENT, bits, index, 1, 1, true, source_index)});
            REQUIRE(machine.Half(1, index * bits >= 64) ==
                    (((index * bits >= 64 ? high : low) & ~inserted_mask) |
                     (source_lane << (index * bits % 64))));
        }
    }
    for (u32 bits : {8u, 16u, 32u}) {
        Machine machine;
        machine.Vector(1, ~u64{0}, 0);
        machine.Run({Copy(NEON_SMOV, bits, 0, 1, 0)});
        REQUIRE(machine.Context().r[0] == ~u64{0});
        if (bits < 32) {
            machine.Run({Copy(NEON_SMOV, bits, 0, 1, 0, false)});
            REQUIRE(machine.Context().r[0] == 0xFFFFFFFF);
        }
    }
    for (u32 bits : {8u, 16u, 32u}) {
        Machine machine;
        machine.Context().r[1] = 0x8877665544332211;
        machine.Vector(0, ~u64{0}, ~u64{0});
        machine.Run({Copy(NEON_DUP_GENERAL, bits, 0, 1, 0, false)});
        REQUIRE(machine.Half(0, true) == 0);
    }
}

TEST_CASE("ARM64 vector logical operations preserve NZCV and zero narrow upper halves") {
    constexpr u64 a = 0xFEDCBA9876543210, b = 0x3333555577779999, d = 0x9696969669696969;
    const u32 operations[] = {NEON_AND, NEON_ORR, NEON_ORN, NEON_EOR,
                              NEON_BIC, NEON_BIF, NEON_BIT, NEON_BSL};
    const u64 expected[] = {a & b, a | b, a | ~b, a ^ b, a & ~b,
                            (a & ~b) | (d & b), (a & b) | (d & ~b),
                            (a & d) | (b & ~d)};
    for (bool q : {false, true}) {
        for (u32 i = 0; i < 8; ++i) {
            for (u32 dest : {0u, 1u, 2u}) {
                Machine machine;
                machine.Vector(0, d, d);
                machine.Vector(1, a, a);
                machine.Vector(2, b, b);
                machine.state->host_cpu_flags = 0xD0000000;
                const u64 old = dest == 0 ? d : dest == 1 ? a : b;
                const u64 wanted = i == 5 ? (a & ~b) | (old & b)
                                 : i == 6 ? (a & b) | (old & ~b)
                                 : i == 7 ? (a & old) | (b & ~old) : expected[i];
                machine.Run({operations[i] | (q ? 0x40000000u : 0u) | (2u << 16) | (1u << 5) | dest});
                REQUIRE(machine.Half(dest) == wanted);
                REQUIRE(machine.Half(dest, true) == (q ? wanted : 0));
                REQUIRE(machine.state->host_cpu_flags == 0xD0000000);
            }
        }
    }
}

TEST_CASE("ARM64 MRS MSR NZCV roundtrip every flag combination") {
    for (u64 flags = 0; flags < 16; ++flags) {
        Machine machine;
        machine.Context().r[1] = (flags << 28) | 0xFFFFFFFF00001234;
        machine.Run({0xD51B4201, 0xD53B4200});
        REQUIRE(machine.Context().r[0] == flags << 28);
    }
}

TEST_CASE("ARM64 existing NEON compares and extrema preserve NZCV") {
    for (u32 op : {u32(NEON_CMHS), u32(NEON_UMINP), u32(NEON_UMAXP)}) {
        Machine machine;
        machine.Vector(1, 0xFE010203FA050607, 0x807F00117F801000);
        machine.Vector(2, 0xFF010205FA030608, 0x7F80FF11807F0000);
        machine.state->host_cpu_flags = 0xF0000000;
        machine.Run({op | 0x40000000u | (2u << 16) | (1u << 5)});
        REQUIRE(machine.state->host_cpu_flags == 0xF0000000);
    }
}

TEST_CASE("ARM64 scalar floating moves preserve bits and clear remaining vector lanes") {
    for (auto instruction : {u32(FMOV_s), u32(FABS_s), u32(FNEG_s),
                              u32(FMOV_d), u32(FABS_d), u32(FNEG_d)}) {
        Machine machine;
        constexpr u64 bits = 0xFFF0000180000000;
        machine.Vector(0, ~u64{0}, ~u64{0});
        machine.Vector(1, bits, ~u64{0});
        machine.Run({instruction | (1u << 5)});
        const bool narrow = instruction == FMOV_s || instruction == FABS_s || instruction == FNEG_s;
        u64 expected = narrow ? bits & 0xFFFFFFFF : bits;
        const u64 sign = narrow ? u64{1} << 31 : u64{1} << 63;
        if (instruction == FABS_s || instruction == FABS_d) expected &= ~sign;
        if (instruction == FNEG_s || instruction == FNEG_d) expected ^= sign;
        REQUIRE(machine.Half(0) == expected);
        REQUIRE(machine.Half(0, true) == 0);
    }
    Machine machine;
    machine.Vector(0, ~u64{0}, ~u64{0});
    machine.Context().r[1] = 0xFEDCBA9880000001;
    machine.Run({u32(FMOV_sw) | (1u << 5), u32(FMOV_ws) | 2u});
    REQUIRE(machine.Half(0) == 0x80000001);
    REQUIRE(machine.Half(0, true) == 0);
    REQUIRE(machine.Context().r[2] == 0x80000001);
}

TEST_CASE("ARM64 scalar SIMD loads clear every unwritten byte") {
    for (u32 i = 0; i < 4; ++i) {
        Machine machine;
        const u64 input = 0x8877665544332211;
        machine.Context().r[1] = reinterpret_cast<u64>(&input);
        machine.Vector(0, ~u64{0}, ~u64{0});
        const u32 instructions[] = {LDR_b_unsigned, LDR_h_unsigned, LDR_s_unsigned, LDR_d_unsigned};
        machine.Run({instructions[i] | (1u << 5)});
        const u64 mask = i == 3 ? ~u64{0} : (u64{1} << (8u << i)) - 1;
        REQUIRE(machine.Half(0) == (input & mask));
        REQUIRE(machine.Half(0, true) == 0);
    }
}

TEST_CASE("ARM64 scalar immediate and conditional select retain exact encodings") {
    Machine machine;
    machine.Vector(0, ~u64{0}, ~u64{0});
    machine.Run({0x1E2E1000});  // FMOV S0, #1.0
    REQUIRE(machine.Half(0) == 0x3F800000);
    REQUIRE(machine.Half(0, true) == 0);
    machine.Run({0x1E6E1000});  // FMOV D0, #1.0
    REQUIRE(machine.Half(0) == 0x3FF0000000000000);
    for (u64 flags = 0; flags < 16; ++flags) {
        for (u32 condition = 0; condition < 16; ++condition) {
            machine.Vector(1, 0x7FC12345, ~u64{0});
            machine.Vector(2, 0x80000000, ~u64{0});
            machine.state->host_cpu_flags = flags << 28;
            const bool n = flags & 8, z = flags & 4, c = flags & 2, v = flags & 1;
            const bool base[] = {z, c, n, v, c && !z, n == v, !z && n == v, true};
            const bool passed = condition >= 14 ? true : base[condition / 2] ^ (condition & 1);
            machine.Run({0x1E220C20u | (condition << 12)});  // FCSEL S0,S1,S2,cond
            REQUIRE(machine.Half(0) == (passed ? 0x7FC12345 : 0x80000000));
            REQUIRE(machine.Half(0, true) == 0);
            REQUIRE(machine.state->host_cpu_flags == flags << 28);
        }
    }
}

TEST_CASE("ARM64 modified immediates implement masks and MSL without retaining old lanes") {
    auto immediate = [](u32 cmode, bool op, u32 imm8) {
        return 0x4F000400u | (op ? 0x20000000u : 0u) | (cmode << 12) |
               ((imm8 >> 5) << 16) | ((imm8 & 31) << 5);
    };
    Machine machine;
    machine.Vector(0, ~u64{0}, ~u64{0});
    machine.Run({immediate(13, false, 0x12)});  // MOVI V0.4S,#0x12,MSL #16
    REQUIRE(machine.Half(0) == 0x0012FFFF0012FFFF);
    REQUIRE(machine.Half(0, true) == 0x0012FFFF0012FFFF);
    machine.Run({immediate(13, true, 0x12)});
    REQUIRE(machine.Half(0) == 0xFFED0000FFED0000);
    machine.Run({immediate(14, true, 0xA5)});
    REQUIRE(machine.Half(0) == 0xFF00FF0000FF00FF);
    machine.Run({immediate(15, false, 0x70)});
    REQUIRE(machine.Half(0) == 0x3F8000003F800000);
    machine.Run({immediate(15, true, 0x70)});
    REQUIRE(machine.Half(0) == 0x3FF0000000000000);
}

TEST_CASE("ARM64 barriers emit memory synchronization and ISB ends translation") {
    CodeMemory memory({0xD5033FBF, 0xD5033F9F, 0xD5033FDF, 0xD2800020});
    ir::Block block(0, kCodeBase);
    ir::Assembler assembler(&block);
    swift::arm64::A64Decoder decoder(kCodeBase, &memory, &assembler);
    decoder.Decode();
    u32 barriers = 0;
    for (auto& instruction : block.GetInstList()) {
        if (instruction.GetOp() == ir::OpCode::MemoryBarrierTSO) ++barriers;
        if (instruction.GetOp() == ir::OpCode::StoreUniform) {
            REQUIRE(instruction.GetArg<ir::Uniform>(0).GetOffset() != offsetof(ThreadContext64, r));
        }
    }
    REQUIRE(barriers == 3);
}

TEST_CASE("ARM64 EXT and SHRN handle narrow vectors and every narrowing width") {
    constexpr u64 low = 0xFEDCBA9876543210, high = 0x8877665544332211;
    for (u32 index = 0; index < 8; ++index) {
        Machine machine;
        machine.Vector(1, low, ~u64{0});
        machine.Vector(2, high, ~u64{0});
        machine.Run({u32(NEON_EXT) | (2u << 16) | (index << 11) | (1u << 5)});
        const u64 wanted = index == 0 ? low : (low >> (index * 8)) | (high << (64 - index * 8));
        REQUIRE(machine.Half(0) == wanted);
        REQUIRE(machine.Half(0, true) == 0);
    }
    for (u32 bits : {8u, 16u, 32u}) {
        for (u32 shift = 1; shift <= bits; ++shift) {
            for (bool upper : {false, true}) {
                Machine machine;
                machine.Vector(1, low, high);
                machine.Vector(0, 0xDEADBEEF, ~u64{0});
                machine.Run({u32(NEON_SHRN) | (upper ? 0x40000000u : 0u) |
                             ((2 * bits - shift) << 16) | (1u << 5)});
                u64 wanted = 0;
                const u32 source_bits = bits * 2;
                for (u32 lane = 0; lane < 64 / bits; ++lane) {
                    const u64 source = lane * source_bits >= 64 ? high : low;
                    const u64 value = (source >> ((lane * source_bits % 64) + shift)) &
                                      ((u64{1} << bits) - 1);
                    wanted |= value << (lane * bits);
                }
                REQUIRE(machine.Half(0, upper) == wanted);
                REQUIRE(machine.Half(0, !upper) == (upper ? 0xDEADBEEF : 0));
            }
        }
    }
}

TEST_CASE("ARM64 SIMD integer arithmetic confines carries to lanes and preserves flags") {
    constexpr u64 a = 0xFEDCBA98765432FF, b = 0x8877665544332211;
    for (u32 op : {u32(NEON_ADD), u32(NEON_SUB), u32(NEON_MUL)}) {
        for (u32 size = 0; size < 4; ++size) {
            if (op == NEON_MUL && size == 3) continue;
            for (bool q : {false, true}) {
                if (!q && size == 3) continue;
                for (u32 dest : {0u, 1u, 2u}) {
                    Machine machine;
                    machine.Vector(1, a, b);
                    machine.Vector(2, b, a);
                    machine.state->host_cpu_flags = 0xF0000000;
                    machine.Run({op | (size << 22) | (q ? 0x40000000u : 0u) |
                                 (2u << 16) | (1u << 5) | dest});
                    const u32 bits = 8u << size;
                    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
                    for (bool high : {false, true}) {
                        u64 expected = 0;
                        if (!high || q) {
                            for (u32 shift = 0; shift < 64; shift += bits) {
                                const u64 left = ((high ? b : a) >> shift) & mask;
                                const u64 right = ((high ? a : b) >> shift) & mask;
                                const u64 result = op == NEON_ADD ? left + right
                                                 : op == NEON_SUB ? left - right : left * right;
                                expected |= (result & mask) << shift;
                            }
                        }
                        REQUIRE(machine.Half(dest, high) == expected);
                    }
                    REQUIRE(machine.state->host_cpu_flags == 0xF0000000);
                }
            }
        }
    }
    Machine machine;
    machine.Vector(1, a, b);
    machine.Run({u32(NEON_RBIT_NOT) | 0x40000000u | (1u << 5)});
    REQUIRE(machine.Half(0) == ~a);
    REQUIRE(machine.Half(0, true) == ~b);
}

TEST_CASE("ARM64 unknown system register accesses stop with explicit fallback") {
    for (u32 instruction : {0xD5380000u, 0xD5180000u}) {  // MRS/MSR MIDR_EL1
        Machine machine;
        machine.Context().r[0] = 0xFEEDFACE;
        CodeMemory memory({instruction});
        ir::Block block(0, kCodeBase);
        ir::Assembler assembler(&block);
        swift::arm64::A64Decoder decoder(kCodeBase, &memory, &assembler);
        decoder.Decode();
        backend::interp::Interpreter interpreter(*machine.state, &block);
        interpreter.Run();
        REQUIRE(machine.Context().padding == static_cast<u32>(InterruptReason::FALLBACK));
        REQUIRE(machine.Context().r[0] == 0xFEEDFACE);
        REQUIRE(machine.state->current_loc.Value() == kCodeBase);
    }
}

TEST_CASE("ARM64 CAS and LSE atomics use exact memory widths and preserve NZCV") {
    for (u32 size = 0; size < 4; ++size) {
        const u32 width = 8u << size;
        const u64 mask = width == 64 ? ~u64{0} : (u64{1} << width) - 1;
        constexpr u64 initial = 0x88776655FFEEDDCC;
        constexpr u64 desired = 0x10203040A5B6C7D8;
        for (u32 order = 0; order < 4; ++order) {
            for (bool success : {false, true}) {
                Machine machine;
                alignas(8) u64 memory = initial;
                machine.Context().r[1] = reinterpret_cast<u64>(&memory);
                machine.Context().r[2] = success ? initial & mask : (initial ^ 1) & mask;
                machine.Context().r[3] = desired;
                machine.Context().r[4] = initial;
                machine.state->host_cpu_flags = 0xF0000000;
                const u32 code = 0x08A07C00u | (size << 30) | ((order & 1) << 22) |
                                 ((order >> 1) << 15) | (2u << 16) | (1u << 5) | 3u;
                machine.Run({0xF9000024, code, 0xF9400020});
                REQUIRE(machine.Context().r[2] == (initial & mask));
                REQUIRE(machine.Context().r[0] == (success ? (initial & ~mask) | (desired & mask) : initial));
                REQUIRE(machine.state->host_cpu_flags == 0xF0000000);
            }
            for (u32 operation : {0u, 1u, 2u, 3u, 8u}) {
                for (bool alias : {false, true}) {
                    Machine machine;
                    alignas(8) u64 memory = initial;
                    machine.Context().r[1] = reinterpret_cast<u64>(&memory);
                    machine.Context().r[2] = desired;
                    machine.Context().r[4] = initial;
                    machine.state->host_cpu_flags = 0xF0000000;
                    const u32 dest = alias ? 2 : 0;
                    const u32 code = 0x38200000u | (size << 30) | ((order & 1) << 23) |
                                     ((order >> 1) << 22) | (2u << 16) | (operation << 12) |
                                     (1u << 5) | dest;
                    machine.Run({0xF9000024, code, 0xF9400025});
                    REQUIRE(machine.Context().r[dest] == (initial & mask));
                    const u64 value = operation == 0 ? initial + desired
                                    : operation == 1 ? initial & ~desired
                                    : operation == 2 ? initial ^ desired
                                    : operation == 3 ? initial | desired : desired;
                    REQUIRE(machine.Context().r[5] == ((initial & ~mask) | (value & mask)));
                    REQUIRE(machine.state->host_cpu_flags == 0xF0000000);
                }
            }
        }
    }
}

TEST_CASE("ARM64 exclusive monitor operations explicitly reject") {
    const u32 instructions[] = {
        0xC85F7C20,  // LDXR X0,[X1]
        0xC8007C22,  // STXR W0,X2,[X1]
        0xC85FFC20,  // LDAXR X0,[X1]
        0xC800FC22,  // STLXR W0,X2,[X1]
        0xC87F0820,  // LDXP X0,X2,[X1]
        0xC8200822,  // STXP W0,X2,X2,[X1]
    };
    for (u32 instruction : instructions) {
        Machine machine;
        machine.Context().r[0] = 0xDEADBEEF;
        machine.Context().r[1] = 1;  // An unsupported operation must never access memory.
        machine.Run({instruction}, InterruptReason::FALLBACK);
        REQUIRE(machine.Context().r[0] == 0xDEADBEEF);
        REQUIRE(machine.state->current_loc.Value() == kCodeBase);
    }
}

TEST_CASE("ARM64 LDAR STLR emit ordered memory access at all scalar widths") {
    for (u32 size = 0; size < 4; ++size) {
        Machine machine;
        alignas(8) u64 memory = ~u64{0};
        constexpr u64 input = 0x10203040A5B6C7D8;
        machine.Context().r[1] = reinterpret_cast<u64>(&memory);
        machine.Context().r[2] = input;
        machine.Context().r[4] = ~u64{0};
        const u32 store = 0x089FFC22u | (size << 30);
        const u32 load = 0x08DFFC20u | (size << 30);
        machine.Run({0xF9000024, store, load});
        const u64 mask = size == 3 ? ~u64{0} : (u64{1} << (8u << size)) - 1;
        REQUIRE(machine.Context().r[0] == (input & mask));
        REQUIRE(memory == ((~u64{0} & ~mask) | (input & mask)));
    }
}

TEST_CASE("ARM64 instruction dependencies include terminating instructions") {
    for (u32 terminal : {0xD65F03C0u, 0x14000001u, 0xD4200000u}) {
        CodeMemory memory({0xD2800020, terminal, 0xD2800040});
        ir::Block block(0, kCodeBase);
        ir::Assembler assembler(&block);
        swift::arm64::A64Decoder decoder(kCodeBase, &memory, &assembler);
        decoder.Decode();
        const auto& dependencies = block.GetGuestCodeDependencies();
        REQUIRE(dependencies.size() == 2);
        REQUIRE(dependencies[0].start.Value() == kCodeBase);
        REQUIRE(dependencies[0].end.Value() == kCodeBase + 4);
        REQUIRE(dependencies[1].start.Value() == kCodeBase + 4);
        REQUIRE(dependencies[1].end.Value() == kCodeBase + 8);
    }
}

TEST_CASE("ARM64 atomic alignment faults precede every memory access and publication") {
    for (u32 size = 1; size < 4; ++size) {
        for (u32 code : {0x08A27C23u, 0x38220020u, 0x08DFFC20u, 0x089FFC22u}) {
            Machine machine;
            machine.Context().r[0] = 0xDEADBEEF;
            machine.Context().r[1] = 1;
            machine.Context().r[2] = 0xFEEDFACE;
            machine.state->current_loc = ir::Location{kCodeBase};
            CodeMemory memory({code | (size << 30)});
            ir::Block block(0, kCodeBase);
            ir::Assembler assembler(&block);
            swift::arm64::A64Decoder decoder(kCodeBase, &memory, &assembler);
            decoder.Decode();
            const auto context = machine.Context();
            backend::interp::Interpreter interpreter(*machine.state, &block);
            REQUIRE(interpreter.Run() == HaltReason::PageFatal);
            REQUIRE(std::memcmp(&machine.Context(), &context, sizeof(context)) == 0);
            REQUIRE(machine.state->current_loc.Value() == kCodeBase);
#if defined(SWIFT_HAS_X64_BACKEND)
            machine.state->halt_reason = HaltReason::None;
            backend::x64::Translator translator(&block);
            translator.Translate();
            REQUIRE(translator.getCode<HaltReason (*)(backend::State*)>()(machine.state) ==
                    HaltReason::PageFatal);
            REQUIRE(std::memcmp(&machine.Context(), &context, sizeof(context)) == 0);
            REQUIRE(machine.state->current_loc.Value() == kCodeBase);
#endif
        }
    }
}
