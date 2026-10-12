// Forward polynomial division with explicit reflection is independent of
// the backend's reflected byte table and SSE4.2 instruction.
u32 ReferenceCastagnoli(u32 initial, u64 data, u32 width) {
    auto reverse = [](u32 value) {
        u32 result{};
        for (u32 i = 0; i < 32; ++i) result |= ((value >> i) & 1u) << (31 - i);
        return result;
    };
    u32 value = reverse(initial);
    for (u32 bit = 0; bit < width; ++bit) {
        const bool top = ((value >> 31) ^ (data >> bit)) & 1;
        value <<= 1;
        if (top) value ^= 0x1EDC6F41;
    }
    return reverse(value);
}

void Crc32cNativeAndBaseline() {
    for (u32 width : {8u, 16u, 32u, 64u}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x5A00})}; ir::Assembler as{block.get()};
        auto initial = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto data = as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
        auto result = as.Crc32c(initial, data, ir::Imm{width}).SetType(ValueType::U32);
        as.StoreUniform(ir::Uniform{16, ValueType::U64}, as.ZeroExtend32(result).SetType(ValueType::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get()}, baseline{block.get(), x64::HostFeatures{}};
        u64 seed = 0xCECB53855164ULL;
        for (u32 iteration = 0; iteration < 512; ++iteration) {
            const u64 data_value = iteration < 256 ? iteration * u64{0x0101010101010101} : Random(seed);
            const u64 initial_value = iteration < 4 ? u64{0xDEADBEEF00000000} | (iteration & 1 ? UINT32_MAX : 0) : Random(seed);
            Storage actual, portable, expected;
            for (auto* storage : {&actual, &portable, &expected}) {
                storage->Put(0, initial_value); storage->Put(8, data_value);
                storage->state->host_cpu_flags = 0xB00A5510;
            }
            backend::interp::Interpreter reference{*expected.state, block.get()};
            const auto halt = reference.Run();
            Check(native.fn(actual.state) == halt && baseline.fn(portable.state) == halt, "CRC32C halt");
            const u64 independent = ReferenceCastagnoli(u32(initial_value), data_value, width);
            Check(actual.Get(16) == independent && portable.Get(16) == independent, "CRC32C independent forward polynomial");
            Check(actual.Get(16) == expected.Get(16), "CRC32C interpreter equality and zero extension");
            Check(actual.state->host_cpu_flags == 0xB00A5510 && portable.state->host_cpu_flags == 0xB00A5510, "CRC32C guest flags preservation");
        }
    }
    u32 known = UINT32_MAX;
    for (u8 byte : std::array<u8, 9>{'1','2','3','4','5','6','7','8','9'}) known = ReferenceCastagnoli(known, byte, 8);
    Check((known ^ UINT32_MAX) == 0xE3069283, "CRC32C standard check value");
    std::cout << "PASS CRC32C every width, independent polynomial, native and table baseline\n";
}
