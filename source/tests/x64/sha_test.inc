using ShaWords = std::array<u32, 4>;
u32 IndependentRotate(u32 word, u32 count) {
    u32 result{};
    for (u32 bit = 0; bit < 32; ++bit) result |= ((word >> ((bit + count) % 32)) & 1u) << bit;
    return result;
}
u32 IndependentShaSigma(u32 word, bool small, bool second) {
    if (small) return second ? IndependentRotate(word, 17) ^ IndependentRotate(word, 19) ^ (word >> 10) :
                              IndependentRotate(word, 7) ^ IndependentRotate(word, 18) ^ (word >> 3);
    return second ? IndependentRotate(word, 6) ^ IndependentRotate(word, 11) ^ IndependentRotate(word, 25) :
                    IndependentRotate(word, 2) ^ IndependentRotate(word, 13) ^ IndependentRotate(word, 22);
}
ShaWords IndependentShaOperation(OpCode op, const ShaWords& destination, const ShaWords& source, const ShaWords& key) {
    ShaWords result{};
    if (op == OpCode::VecSha256Msg1) {
        for (u32 i = 0; i < 4; ++i) result[i] = destination[i] + IndependentShaSigma(i == 3 ? source[0] : destination[i + 1], true, false);
    } else if (op == OpCode::VecSha256Msg2) {
        for (u32 i = 0; i < 4; ++i) result[i] = destination[i] + IndependentShaSigma(i < 2 ? source[i + 2] : result[i - 2], true, true);
    } else {
        std::array<u32, 8> state{source[3], source[2], destination[3], destination[2], source[1], source[0], destination[1], destination[0]};
        for (u32 round = 0; round < 2; ++round) {
            u32 choose{}, majority{};
            for (u32 bit = 0; bit < 32; ++bit) {
                const auto a = (state[0] >> bit) & 1, b = (state[1] >> bit) & 1, c = (state[2] >> bit) & 1;
                majority |= u32(a + b + c >= 2) << bit;
                choose |= ((state[4] >> bit) & 1 ? (state[5] >> bit) & 1 : (state[6] >> bit) & 1) << bit;
            }
            const u32 first = state[7] + IndependentShaSigma(state[4], false, true) + choose + key[round];
            const u32 second = IndependentShaSigma(state[0], false, false) + majority;
            state = {first + second, state[0], state[1], state[2], state[3] + first, state[4], state[5], state[6]};
        }
        result = {state[5], state[4], state[1], state[0]};
    }
    return result;
}
ShaWords GetShaWords(const Storage& storage, u32 offset) {
    ShaWords result{}; std::memcpy(result.data(), storage.state->uniform_buffer_begin + offset, 16); return result;
}
void PutShaWords(Storage& storage, u32 offset, const ShaWords& words) {
    std::memcpy(storage.state->uniform_buffer_begin + offset, words.data(), 16);
}
ir::Value ShaConstant(ir::Assembler& as, const ShaWords& words) {
    return as.VecLoadConst(ir::Imm{u64(words[0]) | (u64(words[1]) << 32)},
                           ir::Imm{u64(words[2]) | (u64(words[3]) << 32)}).SetType(ValueType::V128);
}
void ShaNativeAndBaseline() {
    u64 seed = 0x9b05688c2b3e6c1fULL;
    for (auto op : {OpCode::VecSha256Msg1, OpCode::VecSha256Msg2, OpCode::VecSha256Rnds2}) for (u32 alias = 0; alias < 5; ++alias) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe000})}; ir::Assembler as{block.get()};
        auto destination = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto source = alias == 1 ? destination : as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto key = alias == 2 ? destination : alias == 3 ? source : as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
        auto result = (op == OpCode::VecSha256Rnds2 ? ir::Value{block->AppendInst(op, destination, source, key)} :
                                                                   ir::Value{block->AppendInst(op, destination, source)}).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{48, ValueType::V128}, result);
        if (alias == 4) { as.StoreUniform(ir::Uniform{64, ValueType::V128}, destination); as.StoreUniform(ir::Uniform{80, ValueType::V128}, source); }
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get()}, baseline{block.get(), {}};
        for (u32 iteration = 0; iteration < 512; ++iteration) {
            ShaWords data{}, other{}, keys{};
            constexpr std::array<u32, 6> edges{0, 1, UINT32_MAX, 0x80000000, 0x7fffffff, 0xaaaaaaaa};
            for (u32 i = 0; i < 4; ++i) {
                data[i] = iteration < 36 ? edges[(iteration / 6 + i) % 6] : u32(Random(seed));
                other[i] = iteration < 36 ? edges[(iteration % 6 + i) % 6] : u32(Random(seed));
                keys[i] = u32(Random(seed));
            }
            const auto selected_source = alias == 1 ? data : other;
            const auto selected_keys = alias == 2 ? data : alias == 3 ? selected_source : keys;
            const auto mathematical = IndependentShaOperation(op, data, selected_source, selected_keys);
            Storage actual, portable, interpreted;
            for (auto* storage : {&actual, &portable, &interpreted}) {
                PutShaWords(*storage, 0, data); PutShaWords(*storage, 16, other); PutShaWords(*storage, 32, keys);
                storage->state->host_cpu_flags = 0xb00a5510;
            }
            backend::interp::Interpreter reference{*interpreted.state, block.get()}; const auto wanted = reference.Run();
            Check(native.fn(actual.state) == wanted && baseline.fn(portable.state) == wanted, "SHA native/baseline halt");
            Check(GetShaWords(actual, 48) == mathematical && GetShaWords(portable, 48) == mathematical && GetShaWords(interpreted, 48) == mathematical,
                  "SHA independent bit rotation/boolean math, carry boundaries and aliases");
            Check(actual.state->host_cpu_flags == 0xb00a5510 && portable.state->host_cpu_flags == 0xb00a5510, "SHA guest flags preservation");
            if (alias == 4) Check(GetShaWords(actual, 64) == data && GetShaWords(actual, 80) == other &&
                                  GetShaWords(portable, 64) == data && GetShaWords(portable, 80) == other, "SHA live inputs survive reused output homes");
            if (op == OpCode::VecSha256Rnds2 && alias == 0) {
                actual.state->halt_reason = portable.state->halt_reason = HaltReason::None;
                actual.Put(40, ~actual.Get(40)); portable.Put(40, ~portable.Get(40));
                Check(native.fn(actual.state) == wanted && baseline.fn(portable.state) == wanted &&
                      GetShaWords(actual, 48) == mathematical && GetShaWords(portable, 48) == mathematical, "SHA RNDS2 ignores high key words");
            }
        }
    }
    std::cout << "PASS SHA256 message/round primitives independent math, flags, aliases and native/baseline\n";
}

void ShaNistFullHashes() {
    constexpr std::array<u32, 64> constants{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    constexpr std::array<const char*, 3> messages{"", "abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"};
    constexpr std::array<const char*, 3> digests{
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"};
    for (u32 test = 0; test < messages.size(); ++test) {
        std::vector<u8> padded(messages[test], messages[test] + std::strlen(messages[test]));
        const u64 bit_length = padded.size() * 8;
        padded.push_back(0x80); while (padded.size() % 64 != 56) padded.push_back(0);
        for (s32 byte = 7; byte >= 0; --byte) padded.push_back(u8(bit_length >> (byte * 8)));
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe100})}; ir::Assembler as{block.get()};
        // x86 SHA groups: source={F,E,B,A}, destination={H,G,D,C}.
        auto abef = ShaConstant(as, {0x9b05688c,0x510e527f,0xbb67ae85,0x6a09e667});
        auto cdgh = ShaConstant(as, {0x5be0cd19,0x1f83d9ab,0xa54ff53a,0x3c6ef372});
        for (u32 offset = 0; offset < padded.size(); offset += 64) {
            const auto saved_abef = abef, saved_cdgh = cdgh;
            std::array<ir::Value, 16> schedule;
            for (u32 group = 0; group < 4; ++group) {
                ShaWords words{};
                for (u32 lane = 0; lane < 4; ++lane) for (u32 byte = 0; byte < 4; ++byte)
                    words[lane] = (words[lane] << 8) | padded[offset + group * 16 + lane * 4 + byte];
                schedule[group] = ShaConstant(as, words);
            }
            for (u32 group = 4; group < 16; ++group) {
                auto partial = as.VecSha256Msg1(schedule[group - 4], schedule[group - 3]).SetType(ValueType::V128);
                auto low = as.VecByteShift(schedule[group - 2], schedule[group - 2], ir::Imm{u32{4}}, ir::Imm{u32{0}}).SetType(ValueType::V128);
                auto high = as.VecByteShift(schedule[group - 1], schedule[group - 1], ir::Imm{u32{12}}, ir::Imm{u32{1}}).SetType(ValueType::V128);
                auto middle = as.VecOr(low, high).SetType(ValueType::V128);
                partial = as.VecAdd(partial, middle, ir::Imm{u32{32}}).SetType(ValueType::V128);
                schedule[group] = as.VecSha256Msg2(partial, schedule[group - 1]).SetType(ValueType::V128);
            }
            for (u32 group = 0; group < 16; ++group) {
                auto key = as.VecAdd(schedule[group], ShaConstant(as, {constants[group * 4], constants[group * 4 + 1],
                                                                     constants[group * 4 + 2], constants[group * 4 + 3]}), ir::Imm{u32{32}}).SetType(ValueType::V128);
                cdgh = as.VecSha256Rnds2(cdgh, abef, key).SetType(ValueType::V128);
                key = as.VecByteShift(key, key, ir::Imm{u32{8}}, ir::Imm{u32{0}}).SetType(ValueType::V128);
                abef = as.VecSha256Rnds2(abef, cdgh, key).SetType(ValueType::V128);
            }
            abef = as.VecAdd(abef, saved_abef, ir::Imm{u32{32}}).SetType(ValueType::V128);
            cdgh = as.VecAdd(cdgh, saved_cdgh, ir::Imm{u32{32}}).SetType(ValueType::V128);
        }
        as.StoreUniform(ir::Uniform{0, ValueType::V128}, abef); as.StoreUniform(ir::Uniform{16, ValueType::V128}, cdgh);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get()}, baseline{block.get(), {}};
        Storage actual, portable, interpreted;
        backend::interp::Interpreter reference{*interpreted.state, block.get()}; const auto wanted = reference.Run();
        Check(native.fn(actual.state) == wanted && baseline.fn(portable.state) == wanted, "NIST SHA256 complete hash halt");
        const auto expected = [&](const Storage& storage) {
            const auto a = GetShaWords(storage, 0), b = GetShaWords(storage, 16);
            const std::array<u32, 8> state{a[3],a[2],b[3],b[2],a[1],a[0],b[1],b[0]};
            constexpr char digits[] = "0123456789abcdef";
            std::string digest;
            for (auto word : state) for (s32 nibble = 7; nibble >= 0; --nibble) digest += digits[(word >> (nibble * 4)) & 15];
            return digest == digests[test];
        };
        Check(expected(actual) && expected(portable) && expected(interpreted), "NIST SHA256 empty/abc/multiblock full message schedule and compression");
    }
    std::cout << "PASS NIST SHA256 empty, abc and multiblock complete hashes native/baseline\n";
}
