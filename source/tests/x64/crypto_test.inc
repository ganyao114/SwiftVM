// Independent reference uses the published FIPS-197 substitution table and
// polynomial long division, rather than the fallback's generated table.
using CryptoBytes = std::array<u8, 16>;
constexpr std::array<u8, 256> test_sbox{
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

u8 TestGfMultiply(u8 a, u8 b) {
    u32 product{};
    for (u32 bit = 0; bit < 8; ++bit) if ((a >> bit) & 1) product ^= u32(b) << bit;
    for (s32 bit = 14; bit >= 8; --bit) if ((product >> bit) & 1) product ^= 0x11bu << (bit - 8);
    return u8(product);
}

CryptoBytes TestMixColumns(const CryptoBytes& input, bool inverse) {
    constexpr u8 forward[4][4]{{2,3,1,1},{1,2,3,1},{1,1,2,3},{3,1,1,2}};
    constexpr u8 backward[4][4]{{14,11,13,9},{9,14,11,13},{13,9,14,11},{11,13,9,14}};
    CryptoBytes result{};
    for (u32 c = 0; c < 4; ++c) for (u32 row = 0; row < 4; ++row) for (u32 term = 0; term < 4; ++term)
        result[c * 4 + row] ^= TestGfMultiply(input[c * 4 + term], (inverse ? backward : forward)[row][term]);
    return result;
}

CryptoBytes TestAesRound(const CryptoBytes& data, const CryptoBytes& key, u32 mode) {
    constexpr std::array<u8, 16> forward{0,5,10,15,4,9,14,3,8,13,2,7,12,1,6,11};
    constexpr std::array<u8, 16> backward{0,13,10,7,4,1,14,11,8,5,2,15,12,9,6,3};
    std::array<u8, 256> inverse{};
    for (u32 byte = 0; byte < 256; ++byte) inverse[test_sbox[byte]] = u8(byte);
    CryptoBytes result{};
    for (u32 byte = 0; byte < 16; ++byte) {
        const auto selected = data[((mode & 2) ? backward : forward)[byte]];
        result[byte] = (mode & 2) ? inverse[selected] : test_sbox[selected];
    }
    if (!(mode & 1)) result = TestMixColumns(result, (mode & 2) != 0);
    for (u32 byte = 0; byte < 16; ++byte) result[byte] ^= key[byte];
    return result;
}

CryptoBytes TestKeygen(const CryptoBytes& input, u32 rcon) {
    constexpr std::array<u8, 16> indices{4,5,6,7,5,6,7,4,12,13,14,15,13,14,15,12};
    CryptoBytes result{};
    for (u32 byte = 0; byte < 16; ++byte) result[byte] = test_sbox[input[indices[byte]]];
    result[4] ^= u8(rcon); result[12] ^= u8(rcon);
    return result;
}

void PutCrypto(Storage& storage, u32 offset, const CryptoBytes& input) {
    std::memcpy(storage.state->uniform_buffer_begin + offset, input.data(), 16);
}
CryptoBytes GetCrypto(const Storage& storage, u32 offset) {
    CryptoBytes result{}; std::memcpy(result.data(), storage.state->uniform_buffer_begin + offset, 16); return result;
}
CryptoBytes ParseCrypto(const char* hex) {
    CryptoBytes result{};
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (u32 byte = 0; byte < 16; ++byte) result[byte] = u8(digit(hex[byte * 2]) * 16 + digit(hex[byte * 2 + 1]));
    return result;
}
ir::Value CryptoConstant(ir::Assembler& as, const CryptoBytes& bytes) {
    u64 low{}, high{}; std::memcpy(&low, bytes.data(), 8); std::memcpy(&high, bytes.data() + 8, 8);
    return as.VecLoadConst(ir::Imm{low}, ir::Imm{high}).SetType(ValueType::V128);
}

void AesNativeAndBaseline() {
    const std::array ordinary{OpCode::VecAesEnc, OpCode::VecAesEncLast, OpCode::VecAesDec, OpCode::VecAesDecLast};
    const std::array fast_ops{OpCode::VecAesEncFast, OpCode::VecAesEncLastFast, OpCode::VecAesDecFast, OpCode::VecAesDecLastFast};
    u64 seed = 0x3c6ef372fe94f82bULL;
    for (u32 mode = 0; mode < 4; ++mode) for (bool fast : {false, true}) for (u32 alias = 0; alias < 3; ++alias) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd000})}; ir::Assembler as{block.get()};
        auto data = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto key = alias == 1 ? data : as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        ir::Value result;
        if (fast) {
            auto zero = alias == 2 ? as.VecLoadConst(ir::Imm{u64{0}}, ir::Imm{u64{0}}).SetType(ValueType::V128) :
                                    as.VecSharedZero().SetType(ValueType::V128);
            result = ir::Value{block->AppendInst(fast_ops[mode], data, key, zero)}.SetType(ValueType::V128);
        } else result = ir::Value{block->AppendInst(ordinary[mode], data, key)}.SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, result);
        if (alias == 2) as.StoreUniform(ir::Uniform{48, ValueType::V128}, data);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get()}, baseline{block.get(), {}};
        for (u32 iteration = 0; iteration < 512; ++iteration) {
            CryptoBytes input{}, round_key{};
            for (u32 byte = 0; byte < 16; ++byte) {
                input[byte] = iteration < 256 ? u8(iteration) : u8(Random(seed));
                round_key[byte] = u8(Random(seed));
            }
            Storage actual, portable, interpreted;
            for (auto* storage : {&actual, &portable, &interpreted}) { PutCrypto(*storage, 0, input); PutCrypto(*storage, 16, round_key); }
            backend::interp::Interpreter reference{*interpreted.state, block.get()}; const auto wanted = reference.Run();
            Check(native.fn(actual.state) == wanted && baseline.fn(portable.state) == wanted, "AES native/baseline halt");
            const auto mathematical = TestAesRound(input, alias == 1 ? input : round_key, mode);
            Check(GetCrypto(actual, 32) == mathematical && GetCrypto(portable, 32) == mathematical &&
                  GetCrypto(interpreted, 32) == mathematical, "AES FIPS table round/math/interpreter/native/baseline");
            if (alias == 2) Check(GetCrypto(actual, 48) == input && GetCrypto(portable, 48) == input, "AES live input survives output");
        }
    }
    // Fast's third operand is a shared zero-key ABI contract, not a third
    // semantic input. Reject nonzero or unproven dynamic vectors consistently.
    for (auto op : fast_ops) for (bool dynamic : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd100})}; ir::Assembler as{block.get()};
        auto data = as.VecSharedZero().SetType(ValueType::V128);
        auto bad = dynamic ? as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128) :
                             as.VecLoadConst(ir::Imm{u64{0}}, ir::Imm{u64{1}}).SetType(ValueType::V128);
        auto output = ir::Value{block->AppendInst(op, data, data, bad)}.SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, output); block->SetTerminal(ir::terminal::ReturnToHost{});
        for (bool baseline : {false, true}) {
            bool rejected{};
            try { Compiled compiled{block.get(), baseline ? x64::HostFeatures{} : x64::HostFeatures::Detect()}; }
            catch (const std::runtime_error& error) { rejected = std::string{error.what()}.find("proven zero") != std::string::npos; }
            Check(rejected, "AES Fast rejects third operand without proof of zero");
        }
    }
    std::cout << "PASS AES rounds all bytes, random keys, live aliases, native/baseline and Fast zero contract\n";
}

void AesNistFullCipher() {
    // FIPS-197 Appendix C.1, AES-128, published plaintext/ciphertext and keys.
    const std::array keys{
        "000102030405060708090a0b0c0d0e0f", "d6aa74fdd2af72fadaa678f1d6ab76fe",
        "b692cf0b643dbdf1be9bc5006830b3fe", "b6ff744ed2c2c9bf6c590cbf0469bf41",
        "47f7f7bc95353e03f96c32bcfd058dfd", "3caaa3e8a99f9deb50f3af57adf622aa",
        "5e390f7df7a69296a7553dc10aa31f6b", "14f9701ae35fe28c440adf4d4ea9c026",
        "47438735a41c65b9e016baf4aebf7ad2", "549932d1f08557681093ed9cbe2c974e",
        "13111d7fe3944a17f307a78b4d2b30c5"};
    const auto plaintext = ParseCrypto("00112233445566778899aabbccddeeff");
    const auto ciphertext = ParseCrypto("69c4e0d86a7b0430d8cdb78070b4c55a");
    for (bool inverse : {false, true}) for (bool fast : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd200})}; ir::Assembler as{block.get()};
        auto state = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        state = as.VecXor(state, CryptoConstant(as, ParseCrypto(keys[inverse ? 10 : 0]))).SetType(ValueType::V128);
        auto zero = as.VecSharedZero().SetType(ValueType::V128);
        for (u32 round = 1; round <= 10; ++round) {
            auto round_key = ParseCrypto(keys[inverse ? 10 - round : round]);
            if (inverse && round < 10) round_key = TestMixColumns(round_key, true);
            auto key = CryptoConstant(as, round_key);
            const auto op = inverse ? (round == 10 ? (fast ? OpCode::VecAesDecLastFast : OpCode::VecAesDecLast) :
                                                     (fast ? OpCode::VecAesDecFast : OpCode::VecAesDec)) :
                                      (round == 10 ? (fast ? OpCode::VecAesEncLastFast : OpCode::VecAesEncLast) :
                                                     (fast ? OpCode::VecAesEncFast : OpCode::VecAesEnc));
            state = fast ? ir::Value{block->AppendInst(op, state, key, zero)}.SetType(ValueType::V128) :
                           ir::Value{block->AppendInst(op, state, key)}.SetType(ValueType::V128);
        }
        as.StoreUniform(ir::Uniform{16, ValueType::V128}, state); block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get()}, baseline{block.get(), {}};
        Storage actual, portable;
        PutCrypto(actual, 0, inverse ? ciphertext : plaintext); PutCrypto(portable, 0, inverse ? ciphertext : plaintext);
        Check(native.fn(actual.state) == baseline.fn(portable.state), "NIST native/baseline halt");
        Check(GetCrypto(actual, 16) == (inverse ? plaintext : ciphertext) && GetCrypto(portable, 16) == (inverse ? plaintext : ciphertext),
              "FIPS-197 full AES128 encryption/decryption native/baseline");
    }
    std::cout << "PASS FIPS-197 AES128 complete encryption/decryption ordinary/Fast native/baseline\n";
}

void AesKeygenNativeAndBaseline() {
    u64 seed = 0xa54ff53a5f1d36f1ULL;
    for (u32 rcon = 0; rcon < 256; ++rcon) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd300})}; ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto output = as.VecAesKeygenAssist(input, ir::Imm{rcon | 0xAB0000u}).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{16, ValueType::V128}, output); block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get()}, baseline{block.get(), {}};
        for (u32 iteration = 0; iteration < 5; ++iteration) {
            CryptoBytes bytes{};
            for (u32 byte = 0; byte < 16; ++byte) bytes[byte] = iteration == 0 ? u8(rcon) : u8(Random(seed));
            Storage actual, portable, interpreted;
            for (auto* storage : {&actual, &portable, &interpreted}) PutCrypto(*storage, 0, bytes);
            backend::interp::Interpreter reference{*interpreted.state, block.get()}; const auto wanted = reference.Run();
            Check(native.fn(actual.state) == wanted && baseline.fn(portable.state) == wanted, "AESKEYGENASSIST native/baseline halt");
            const auto mathematical = TestKeygen(bytes, rcon);
            Check(GetCrypto(actual, 16) == mathematical && GetCrypto(portable, 16) == mathematical && GetCrypto(interpreted, 16) == mathematical,
                  "AESKEYGENASSIST all rcon/ignored high immediate bits/FIPS substitutions");
        }
    }
    std::cout << "PASS AES keygen assist all rcon/substitution bytes and ignored upper control bits\n";
}

unsigned __int128 TestPolynomialMultiply(u64 a, u64 b) {
    // Count contributing coefficient pairs; no shifted multiplicand matches
    // the implementation's loop. The high result bit is always zero.
    unsigned __int128 result{};
    for (u32 degree = 0; degree < 127; ++degree) {
        u32 coefficient{};
        for (u32 i = 0; i < 64; ++i) if (degree >= i && degree - i < 64)
            coefficient ^= u32((a >> i) & (b >> (degree - i)) & 1);
        result |= static_cast<unsigned __int128>(coefficient) << degree;
    }
    return result;
}

void PolynomialNativeAndBaseline() {
    u64 seed = 0x510e527fade682d1ULL;
    const std::array boundaries{u64{0}, u64{1}, u64{2}, u64{1} << 63, (u64{1} << 63) - 1,
                               UINT64_MAX, u64{0xaaaaaaaaaaaaaaaa}, u64{0x5555555555555555}};
    for (u32 control : {0u, 1u, 16u, 17u, 0xEEu, 0xEFu, 0xFEu, 0xFFu}) for (bool alias : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd400})}; ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto b = alias ? a : as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto output = as.VecPclMul(a, b, ir::Imm{control}).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, output); as.StoreUniform(ir::Uniform{48, ValueType::V128}, a);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get()}, baseline{block.get(), {}};
        for (u32 iteration = 0; iteration < 128; ++iteration) {
            Storage actual, portable, interpreted;
            for (u32 offset : {0u, 8u, 16u, 24u}) {
                const u64 value = iteration < 64 ? boundaries[((offset < 16 ? iteration / 8 : iteration % 8) + offset / 8) % 8] : Random(seed);
                for (auto* storage : {&actual, &portable, &interpreted}) storage->Put(offset, value);
            }
            const auto left = actual.Get((control & 1) ? 8 : 0), right = actual.Get((alias ? 0 : 16) + ((control & 16) ? 8 : 0));
            const auto mathematical = TestPolynomialMultiply(left, right);
            backend::interp::Interpreter reference{*interpreted.state, block.get()}; const auto wanted = reference.Run();
            Check(native.fn(actual.state) == wanted && baseline.fn(portable.state) == wanted, "PCLMUL native/baseline halt");
            Check(actual.Get(32) == u64(mathematical) && actual.Get(40) == u64(mathematical >> 64) &&
                  portable.Get(32) == actual.Get(32) && portable.Get(40) == actual.Get(40) &&
                  interpreted.Get(32) == actual.Get(32) && interpreted.Get(40) == actual.Get(40), "GF2 coefficient multiplication/select/ignored control bits");
            Check(actual.Get(48) == actual.Get(0) && actual.Get(56) == actual.Get(8) && portable.Get(48) == portable.Get(0) && portable.Get(56) == portable.Get(8),
                  "PCLMUL preserves live aliased operands");
        }
    }
    std::cout << "PASS carryless polynomial multiplication all selectors, boundaries/random and native/baseline\n";
}
