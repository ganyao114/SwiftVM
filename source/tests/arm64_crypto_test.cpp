#include <array>
#include <cstring>
#include <string_view>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;

namespace {
using Bytes = std::array<u8, 16>;
constexpr u32 kAESE = 0x4E284800, kAESD = 0x4E285800;
constexpr u32 kAESMC = 0x4E286800, kAESIMC = 0x4E287800;

// FIPS 197, Table 4. This literal table is independent of the runtime's
// polynomial-inversion S-box implementation and the shared AES IR sequence.
constexpr std::array<u8, 256> kSbox{
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

Bytes Hex(std::string_view text) {
    auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    Bytes result{};
    for (u32 i = 0; i < 16; ++i) result[i] = (digit(text[2 * i]) << 4) | digit(text[2 * i + 1]);
    return result;
}
u8 Multiply(u8 a, u8 b) {
    // Polynomial product reduced by x^8+x^4+x^3+x+1. No runtime crypto helper.
    u32 product = 0;
    for (u32 bit = 0; bit < 8; ++bit) if ((b >> bit) & 1) product ^= u32(a) << bit;
    for (s32 bit = 14; bit >= 8; --bit) if ((product >> bit) & 1) product ^= 0x11Bu << (bit - 8);
    return u8(product);
}
Bytes Reference(u32 op, Bytes old, const Bytes& source) {
    if (op == kAESE || op == kAESD) {
        std::array<u8, 256> inverse{};
        for (u32 i = 0; i < 256; ++i) inverse[kSbox[i]] = u8(i);
        Bytes result{};
        for (u32 column = 0; column < 4; ++column) for (u32 row = 0; row < 4; ++row) {
            const u32 index = 4 * ((column + (op == kAESE ? row : 4 - row)) % 4) + row;
            const u8 keyed = old[index] ^ source[index];
            result[4 * column + row] = op == kAESE ? kSbox[keyed] : inverse[keyed];
        }
        return result;
    }
    constexpr u8 forward[4][4]{{2,3,1,1},{1,2,3,1},{1,1,2,3},{3,1,1,2}};
    constexpr u8 inverse[4][4]{{14,11,13,9},{9,14,11,13},{13,9,14,11},{11,13,9,14}};
    Bytes result{};
    for (u32 column = 0; column < 4; ++column) for (u32 row = 0; row < 4; ++row)
        for (u32 term = 0; term < 4; ++term)
            result[4 * column + row] ^= Multiply(source[4 * column + term],
                op == kAESMC ? forward[row][term] : inverse[row][term]);
    return result;
}

class InstructionMemory final : public MemoryInterface {
public:
    explicit InstructionMemory(std::vector<u32> instructions) : code(std::move(instructions)) {
        code.push_back(0xD4200000);
    }
    bool Read(void* destination, size_t address, size_t size) override {
        if (address < 0x1000 || address - 0x1000 + size > code.size() * 4) return false;
        std::memcpy(destination, reinterpret_cast<u8*>(code.data()) + address - 0x1000, size);
        return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* pointer) override {
        const auto address = reinterpret_cast<size_t>(pointer);
        return address >= 0x1000 && address - 0x1000 < code.size() * 4
            ? reinterpret_cast<u8*>(code.data()) + address - 0x1000 : nullptr;
    }
    std::vector<u32> code;
};
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(swift::arm64::ThreadContext64) + 32> storage{};
    backend::State* state = new (storage.data()) backend::State{};
    Machine() {
        state->host_cpu_flags = 0xB0000000;
        Context().fpsr = 0x0800009F;
        Context().fpcr = 0x01400000;
        for (u32 reg = 0; reg < 32; ++reg) {
            Bytes value{};
            for (u32 byte = 0; byte < 16; ++byte) value[byte] = u8(reg * 7 + byte * 23);
            Vector(reg, value);
        }
    }
    ~Machine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() {
        return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin);
    }
    void Vector(u32 reg, const Bytes& value) { std::memcpy(&Context().v[reg], value.data(), 16); }
    Bytes Vector(u32 reg) { Bytes value{}; std::memcpy(value.data(), &Context().v[reg], 16); return value; }
    void Run(std::vector<u32> instructions, swift::arm64::InterruptReason reason = swift::arm64::InterruptReason::BRK) {
        InstructionMemory memory(std::move(instructions));
        ir::Block block(0, 0x1000);
        ir::Assembler assembler(&block);
        swift::arm64::A64Decoder decoder(0x1000, &memory, &assembler);
        decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto initial = Context();
        const auto initial_flags = state->host_cpu_flags;
        const auto initial_location = state->current_loc;
#endif
        backend::interp::Interpreter interpreter(*state, &block);
        interpreter.Run();
        REQUIRE(Context().padding == u32(reason));
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto expected = Context();
        const auto expected_flags = state->host_cpu_flags;
        const auto expected_location = state->current_loc;
        for (bool baseline : {false, true}) {
            Context() = initial;
            state->host_cpu_flags = initial_flags;
            state->current_loc = initial_location;
            state->halt_reason = HaltReason::None;
            backend::x64::Translator translator(&block, baseline ? backend::x64::HostFeatures{}
                                                               : backend::x64::HostFeatures::Detect());
            translator.Translate();
            translator.getCode<HaltReason (*)(backend::State*)>()(state);
            REQUIRE(std::memcmp(&Context(), &expected, sizeof(expected)) == 0);
            REQUIRE(state->host_cpu_flags == expected_flags);
            REQUIRE(state->current_loc.Value() == expected_location.Value());
        }
#endif
    }
};
}

TEST_CASE("ARM64 AES matches FIPS 197 round stages and preserves architectural flags") {
    const Bytes plaintext = Hex("00112233445566778899aabbccddeeff");
    const Bytes key = Hex("000102030405060708090a0b0c0d0e0f");
    const Bytes added = Hex("00102030405060708090a0b0c0d0e0f0");
    const Bytes shifted = Hex("6353e08c0960e104cd70b751bacad0e7");
    const Bytes mixed = Hex("5f72641557f5bc92f7be3b291db9f91a");
    struct Case { u32 op; Bytes old, source, expected; };
    for (const auto& test : std::array<Case, 4>{{{kAESE, plaintext, key, shifted},
             {kAESD, shifted, {}, added}, {kAESMC, plaintext, shifted, mixed},
             {kAESIMC, plaintext, mixed, shifted}}}) {
        Machine machine;
        machine.Vector(0, test.old);
        machine.Vector(31, test.source);
        machine.Run({test.op | (31u << 5)});
        REQUIRE(machine.Vector(0) == test.expected);
        REQUIRE(machine.Vector(31) == test.source);
        REQUIRE(machine.Context().fpsr == 0x0800009F);
        REQUIRE(machine.Context().fpcr == 0x01400000);
        REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    }
}

TEST_CASE("ARM64 AES round and column transforms cover every byte and aliased registers") {
    for (u32 op : {kAESE, kAESD, kAESMC, kAESIMC}) for (u32 alias : {0u, 1u})
        for (u32 seed = 0; seed < 256; ++seed) {
            INFO("op=" << op << " alias=" << alias << " seed=" << seed);
            Machine machine;
            Bytes old{}, source{};
            for (u32 byte = 0; byte < 16; ++byte) {
                old[byte] = u8(seed + byte * 13);
                source[byte] = u8(seed * 3 + byte * 29 + 1);
            }
            const u32 rd = alias ? 31 : 0;
            machine.Vector(rd, old);
            machine.Vector(31, source);
            const auto before = machine.Context();
            const auto expected = Reference(op, machine.Vector(rd), source);
            machine.Run({op | (31u << 5) | rd});
            REQUIRE(machine.Vector(rd) == expected);
            for (u32 reg = 0; reg < 32; ++reg) if (reg != rd)
                REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], 16) == 0);
            REQUIRE(machine.Context().fpsr == before.fpsr);
            REQUIRE(machine.Context().fpcr == before.fpcr);
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
}

TEST_CASE("ARM64 AES composes a complete FIPS 197 AES128 encryption and decryption") {
    const std::array<Bytes, 11> keys{
        Hex("000102030405060708090a0b0c0d0e0f"), Hex("d6aa74fdd2af72fadaa678f1d6ab76fe"),
        Hex("b692cf0b643dbdf1be9bc5006830b3fe"), Hex("b6ff744ed2c2c9bf6c590cbf0469bf41"),
        Hex("47f7f7bc95353e03f96c32bcfd058dfd"), Hex("3caaa3e8a99f9deb50f3af57adf622aa"),
        Hex("5e390f7df7a69296a7553dc10aa31f6b"), Hex("14f9701ae35fe28c440adf4d4ea9c026"),
        Hex("47438735a41c65b9e016baf4aebf7ad2"), Hex("549932d1f08557681093ed9cbe2c974e"),
        Hex("13111d7fe3944a17f307a78b4d2b30c5")};
    const auto plaintext = Hex("00112233445566778899aabbccddeeff");
    const auto ciphertext = Hex("69c4e0d86a7b0430d8cdb78070b4c55a");
    for (bool decrypt : {false, true}) {
        Machine machine;
        machine.Vector(0, decrypt ? ciphertext : plaintext);
        machine.Vector(31, {});
        for (u32 i = 0; i < keys.size(); ++i) machine.Vector(i + 1, keys[i]);
        auto xor_key = [](u32 key_reg) { return u32(vixl::aarch64::NEON_EOR) | (1u << 30) | (key_reg << 16); };
        std::vector<u32> code;
        if (decrypt) {
            code.push_back(xor_key(11));
            for (u32 key_reg = 10; key_reg >= 2; --key_reg) {
                code.push_back(kAESD | (31u << 5));
                code.push_back(xor_key(key_reg));
                code.push_back(kAESIMC);
            }
            code.push_back(kAESD | (31u << 5));
            code.push_back(xor_key(1));
        } else {
            for (u32 key_reg = 1; key_reg <= 10; ++key_reg) {
                code.push_back(kAESE | (key_reg << 5));
                if (key_reg < 10) code.push_back(kAESMC);
            }
            code.push_back(xor_key(11));
        }
        machine.Run(code);
        REQUIRE(machine.Vector(0) == (decrypt ? plaintext : ciphertext));
        for (u32 i = 0; i < keys.size(); ++i) REQUIRE(machine.Vector(i + 1) == keys[i]);
        REQUIRE(machine.Context().fpsr == 0x0800009F);
        REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    }
}

TEST_CASE("ARM64 AES rejects reserved opcode and size fields before register mutation") {
    std::vector<u32> reserved;
    for (u32 op = 0; op < 16; ++op) if (op < 4 || op > 7) reserved.push_back(0x4E280800 | (op << 12));
    for (u32 op : {kAESE, kAESD, kAESMC, kAESIMC})
        for (u32 size = 1; size < 4; ++size) reserved.push_back(op | (size << 22));
    for (u32 code : reserved) {
        Machine machine;
        const auto before = machine.Context();
        machine.Run({code | (31u << 5)}, swift::arm64::InterruptReason::ILL_CODE);
        for (u32 reg = 0; reg < 32; ++reg)
            REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], 16) == 0);
        REQUIRE(machine.state->current_loc.Value() == 0x1000);
        REQUIRE(machine.Context().fpsr == before.fpsr);
        REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    }
}
