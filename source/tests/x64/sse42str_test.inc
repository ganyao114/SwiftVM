// Included in backend_test.cpp's anonymous namespace. This SDM model shares
// no evaluator or result-packing helper with the runtime implementation.
using StringBytes = std::array<u8, 16>;

u64 IndependentStringResult(const StringBytes& a, const StringBytes& b, u32 control) {
    const bool words = control & 1, signed_elements = control & 2;
    const u32 count = words ? 8 : 16, aggregation = (control >> 2) & 3;
    std::array<s32, 16> left{}, right{};
    for (u32 i = 0; i < count; ++i) {
        const u32 av = words ? u32(a[2 * i]) | (u32(a[2 * i + 1]) << 8) : a[i];
        const u32 bv = words ? u32(b[2 * i]) | (u32(b[2 * i + 1]) << 8) : b[i];
        const u32 sign = words ? 0x8000 : 0x80, range = words ? 0x10000 : 0x100;
        left[i] = signed_elements && (av & sign) ? s32(av) - s32(range) : s32(av);
        right[i] = signed_elements && (bv & sign) ? s32(bv) - s32(range) : s32(bv);
    }
    u32 first_length{}, second_length{};
    while (first_length < count && left[first_length]) ++first_length;
    while (second_length < count && right[second_length]) ++second_length;
    u32 mask{};
    for (u32 position = 0; position < count; ++position) {
        bool match{};
        if (aggregation == 0) {
            if (position < second_length)
                for (u32 i = 0; i < first_length; ++i) match |= left[i] == right[position];
        } else if (aggregation == 1) {
            if (position < second_length)
                for (u32 i = 0; i + 1 < first_length; i += 2)
                    match |= left[i] <= right[position] && right[position] <= left[i + 1];
        } else if (aggregation == 2) {
            match = position >= first_length && position >= second_length;
            if (position < first_length && position < second_length)
                match = left[position] == right[position];
        } else {
            match = true;
            // The architecture compares only cells within the 128-bit window.
            // An exhausted needle succeeds; an exhausted text fails while
            // a valid needle element remains.
            for (u32 i = 0; i + position < count; ++i)
                if (i < first_length &&
                    (i + position >= second_length || left[i] != right[i + position])) match = false;
        }
        const u32 polarity = (control >> 4) & 3;
        if (polarity == 1 || (polarity == 3 && position < second_length)) match = !match;
        if (match) mask |= 1u << position;
    }
    u32 index = count;
    if (control & 0x40) {
        for (u32 i = 0; i < count; ++i) if (mask & (1u << i)) index = i;
    } else {
        for (u32 i = count; i-- > 0;) if (mask & (1u << i)) index = i;
    }
    return u64(mask) | (u64(index) << 16) | (u64(first_length < count) << 24) |
           (u64(second_length < count) << 25) | (u64(mask != 0) << 26) | (u64(mask & 1) << 27);
}

u64 IndependentStringFlags(u64 old, u64 packed, ir::Flags requested) {
    const std::array<ir::Flags, 6> flags{ir::Flags::Negate, ir::Flags::Zero, ir::Flags::Carry,
                                      ir::Flags::Overflow, ir::Flags::AuxiliaryCarry, ir::Flags::Parity};
    const std::array<u32, 4> destinations{31, 30, 29, 28};
    for (u32 i = 0; i < 4; ++i) if (True(requested & flags[i])) {
        const u64 bit = u64{1} << destinations[i];
        old = (old & ~bit) | (((packed >> (24 + i)) & 1) ? bit : 0);
    }
    if (True(requested & flags[4])) old &= ~(u64{1} << 26);
    if (True(requested & flags[5])) old = (old & ~u64{255}) | 1;
    return old;
}

StringBytes StringPattern(u32 control, u32 variant, u32 length, bool second) {
    StringBytes bytes{};
    const bool words = control & 1;
    const u32 count = words ? 8 : 16;
    const std::array<u16, 16> values{
        1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x100,
        0xff00, 2, 3, 0x81, 0xfe, 0x1234, 0x5678, 1};
    for (u32 i = 0; i < count; ++i) {
        u32 value = values[(i + (second ? (variant ? 0 : 3) : 0)) % values.size()];
        if (variant && i > 3) value = values[i % 4];
        if (!words) { value &= 255; if (!value) value = 0x80; }
        if (i == length) value = 0;
        if (words) { bytes[2 * i] = u8(value); bytes[2 * i + 1] = u8(value >> 8); }
        else bytes[i] = u8(value);
    }
    return bytes;
}

struct StringMxcsrRestore {
    u32 saved = _mm_getcsr();
    ~StringMxcsrRestore() { _mm_setcsr(saved); }
};

void Sse42StringNativeAndBaseline() {
    u64 seed = 0x27bdfaa98397d621ULL;
    for (u32 control = 0; control < 256; ++control) for (u32 alias = 0; alias < 3; ++alias) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe800})}; ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto b = alias == 1 ? a : as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto result = as.Sse42Str(alias == 2 ? b : a, alias == 2 ? a : b,
                                 ir::Imm{control | (alias == 2 ? 0xa500u : 0u)}).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{32, ValueType::U64}, result);
        as.PublishSse42StrFlags(result, ir::Flags::All);
        auto published = as.GetFlags(result, ir::Flags::All).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{40, ValueType::U64}, published);
        // Keep both SSA inputs alive across the native implicit XMM0 output
        // and the helper call, and overwrite a source uniform afterwards.
        as.StoreUniform(ir::Uniform{64, ValueType::V128}, a);
        as.StoreUniform(ir::Uniform{80, ValueType::V128}, b);
        as.StoreUniform(ir::Uniform{alias == 2 ? 16u : 0u, ValueType::U64}, result);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get()}, baseline{block.get(), {}};
        const u32 count = control & 1 ? 8 : 16;
        const u32 iterations = alias ? 64 : 2 * (count + 1) * (count + 1);
        for (u32 iteration = 0; iteration < iterations; ++iteration) {
            const u32 la = iteration % (count + 1), lb = (iteration / (count + 1)) % (count + 1);
            const u32 variant = iteration / ((count + 1) * (count + 1));
            StringBytes a_bytes = StringPattern(control, variant, la, false);
            StringBytes b_bytes = StringPattern(control, variant, lb, true);
            if (alias) {
                for (u32 i = 0; i < 16; ++i) { a_bytes[i] = u8(Random(seed)); b_bytes[i] = u8(Random(seed)); }
                if (control & 1) {
                    if (la < count) a_bytes[2 * la] = a_bytes[2 * la + 1] = 0;
                    if (lb < count) b_bytes[2 * lb] = b_bytes[2 * lb + 1] = 0;
                } else {
                    if (la < count) a_bytes[la] = 0;
                    if (lb < count) b_bytes[lb] = 0;
                }
            }
            if (alias == 1) b_bytes = a_bytes;
            const u64 expected = IndependentStringResult(alias == 2 ? b_bytes : a_bytes,
                                                         alias == 2 ? a_bytes : b_bytes, control);
            const u64 old_flags = iteration & 1 ? UINT64_MAX : 0x9a7185e307080402ULL;
            const std::string detail = "SSE42 string/" + std::to_string(control) + "/" +
                                       std::to_string(alias) + "/" + std::to_string(iteration);
            for (auto* compiled : {&native, &baseline}) {
                Storage actual;
                std::memcpy(actual.state->uniform_buffer_begin, a_bytes.data(), 16);
                std::memcpy(actual.state->uniform_buffer_begin + 16, b_bytes.data(), 16);
                actual.Put(96, 0xa55a01028040ffffULL); actual.Put(104, 0x1234abcdef987654ULL);
                actual.state->host_cpu_flags = old_flags;
                StringMxcsrRestore restore;
                const u32 host_mxcsr = (restore.saved | 0x9fc1u) & ~0x6000u;
                const u32 changed = host_mxcsr | ((iteration & 3) << 13);
                _mm_setcsr(changed);
                const auto reason = compiled->fn(actual.state);
                const auto after = _mm_getcsr();
                Check(reason == HaltReason::CallHost, detail + " halt");
                Check(actual.Get(32) == expected, detail + " packed mask/index/SZCO");
                const auto flags = IndependentStringFlags(old_flags, expected, ir::Flags::All);
                Check(actual.Get(40) == flags && actual.state->host_cpu_flags == flags, detail + " published flags");
                Check(std::memcmp(actual.state->uniform_buffer_begin + 64, a_bytes.data(), 16) == 0 &&
                      std::memcmp(actual.state->uniform_buffer_begin + 80, b_bytes.data(), 16) == 0,
                      detail + " source aliases survive");
                Check(actual.Get(96) == 0xa55a01028040ffffULL && actual.Get(104) == 0x1234abcdef987654ULL,
                      detail + " unrelated guest FP/control uniforms preserved");
                Check(after == changed, detail + " host MXCSR preserves rounding/DAZ/FTZ/status");
            }
        }
    }
    std::cout << "PASS SSE42 strings: all 256 imm8 / all NUL pairs / signed byte-word / aliases / native-baseline\n";
}

void Sse42StringPartialFlags() {
    const std::array<ir::Flags, 6> flag_bits{ir::Flags::Negate, ir::Flags::Zero, ir::Flags::Carry,
        ir::Flags::Overflow, ir::Flags::AuxiliaryCarry, ir::Flags::Parity};
    u64 seed = 0xbc65e94a58d21907ULL;
    for (u32 subset = 0; subset < 64; ++subset) {
        auto requested = ir::Flags::None;
        for (u32 i = 0; i < 6; ++i) if (subset & (1u << i)) requested |= flag_bits[i];
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe900})}; ir::Assembler as{block.get()};
        auto packed = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        as.PublishSse42StrFlags(packed, requested);
        auto flags = as.GetFlags(packed, requested).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{8, ValueType::U64}, flags);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u32 iteration = 0; iteration < 128; ++iteration) {
            // Cover all sixteen SZCO combinations, plus poisoned unused bits.
            const u64 value = (Random(seed) & ~u64{0xf000000}) | (u64(iteration & 15) << 24);
            const u64 old = iteration < 2 ? (iteration ? UINT64_MAX : 0) : Random(seed);
            Storage actual; actual.Put(0, value); actual.state->host_cpu_flags = old;
            const auto expected = IndependentStringFlags(old, value, requested);
            Check(compiled.fn(actual.state) == HaltReason::CallHost, "SSE42 partial flags halt");
            Check(actual.Get(8) == expected && actual.state->host_cpu_flags == expected,
                  "SSE42 partial flags preserve every unrequested bit/subset " + std::to_string(subset));
        }
    }
    std::cout << "PASS SSE42 flags: all 64 subsets / poisoned packed-high and state-high bits\n";
}
