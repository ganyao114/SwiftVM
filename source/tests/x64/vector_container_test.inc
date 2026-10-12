// Generic vector operations consume complete initialized 128-bit containers.
// Narrow vector annotations and casts select memory/scalar access widths;
// they do not insert an implicit truncation into a raw vector computation.
using ContainerBytes = std::array<u8, 16>;
constexpr std::array<ValueType, 5> ContainerTypes{ValueType::V8, ValueType::V16, ValueType::V32, ValueType::V64, ValueType::V128};
ContainerBytes ContainerIndependentXor(ContainerBytes a, ContainerBytes b) {
    ContainerBytes result{};
    for (u32 byte = 0; byte < result.size(); ++byte) result[byte] = a[byte] ^ b[byte];
    return result;
}
ContainerBytes ContainerIndependentOp(ContainerBytes a, ContainerBytes b, u32 op) {
    ContainerBytes result{};
    for (u32 byte = 0; byte < result.size(); ++byte)
        result[byte] = op == 0 ? a[byte] ^ b[byte] : op == 1 ? (a[byte] == b[byte] ? 255 : 0) : u8(u32(a[byte]) * b[byte]);
    return result;
}
ir::Value ContainerEmit(ir::Assembler& as, ir::Value a, ir::Value b, u32 op, ValueType output) {
    return op == 0 ? as.VecXor(a, b).SetType(output) : op == 1 ? as.VecCmpEq(a, b, ir::Imm{u32{8}}).SetType(output) :
                                                             as.VecMul(a, b, ir::Imm{u32{8}}).SetType(output);
}
void VectorContainerActualAndViewCrossproduct() {
    u64 random = 0xf190ef532b60498dULL;
    for (auto actual : ContainerTypes) for (auto view : ContainerTypes) for (auto result : ContainerTypes)
    for (u32 op = 0; op < 3; ++op) for (bool alias : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12600})}; ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto b = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto first = as.VecXor(a, b).SetType(actual);
        auto value = ContainerEmit(as, first.SetCastType(view), b.SetCastType(view), op, result);
        // Widening a view exposes the same complete container, including
        // bits beyond both the producer's narrow annotation and its view.
        const u32 destination = alias ? 0 : 64;
        as.StoreUniform(ir::Uniform{destination, ValueType::V128}, value.SetCastType(ValueType::V128));
        as.StoreUniform(ir::Uniform{96, ValueType::V128}, first.SetCastType(ValueType::V128));
        as.StoreUniform(ir::Uniform{112, ValueType::V128}, a);
        // Narrow stores write only the view extent and leave neighbors intact.
        as.StoreUniform(ir::Uniform{160, ValueType::V128}, value.SetCastType(view));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            for (u32 sample = 0; sample < 8; ++sample) {
                ContainerBytes a{}, b{};
                for (u32 byte = 0; byte < 16; ++byte) {
                    a[byte] = sample < 4 ? u8(byte * 19 + sample * 37) : u8(Random(random));
                    b[byte] = sample < 4 ? u8(byte * 31 + sample * 11) : u8(Random(random));
                }
                const auto first = ContainerIndependentXor(a, b), wanted = ContainerIndependentOp(first, b, op);
                Storage actual, expected;
                for (auto* storage : {&actual, &expected}) {
                    std::fill_n(storage->state->uniform_buffer_begin, 256, 0xa7);
                    std::memcpy(storage->state->uniform_buffer_begin, a.data(), 16);
                    std::memcpy(storage->state->uniform_buffer_begin + 16, b.data(), 16);
                }
                const u64 flags = Random(random); actual.state->host_cpu_flags = expected.state->host_cpu_flags = flags;
                const auto mxcsr = _mm_getcsr(); backend::interp::Interpreter reference{*expected.state, block.get()};
                Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, "raw vector actual/view crossproduct halt");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + destination, wanted.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + destination, wanted.data(), 16), "raw vector complete result independent byte oracle");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + 96, first.data(), 16) &&
                      !std::memcmp(actual.state->uniform_buffer_begin + 112, a.data(), 16) &&
                      !std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 256), "raw vector retained SSA and full uniform snapshot");
                const u32 stored = ir::GetValueSizeByte(view);
                bool store_ok = !std::memcmp(actual.state->uniform_buffer_begin + 160, wanted.data(), stored);
                for (u32 byte = stored; byte < 32; ++byte) store_ok &= actual.state->uniform_buffer_begin[160 + byte] == 0xa7;
                Check(store_ok && actual.state->host_cpu_flags == flags && expected.state->host_cpu_flags == flags && _mm_getcsr() == mxcsr,
                      "raw vector store extent/neighbors and flags/MXCSR");
            }
        }
    }
    std::cout << "PASS raw vector actual/view/result width crossproduct and independent complete-byte results\n";
}

void VectorContainerBitCastAndForwarding() {
    for (auto narrow : ContainerTypes) for (u32 path = 0; path < 4; ++path) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12700})}; ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto b = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        ir::Value intermediate;
        if (path == 0) intermediate = as.BitCast(a).SetType(narrow);
        else if (path == 1) intermediate = as.BitCast(a).SetType(ValueType::U8);
        else intermediate = as.VecXor(a, b).SetType(path == 2 ? ValueType::V128 : narrow);
        // Paths 2/3 exercise cached V128 and non-cached narrow producers
        // through the same narrow consumer view, without retained users.
        auto final = as.VecXor(intermediate.SetCastType(narrow), b).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{64, ValueType::V128}, final);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            for (u32 sample = 0; sample < 32; ++sample) {
                ContainerBytes a{}, b{};
                for (u32 byte = 0; byte < 16; ++byte) { a[byte] = u8(byte * 43 + sample); b[byte] = u8(byte * 7 + 3 * sample); }
                const auto wanted = path < 2 ? ContainerIndependentXor(a, b) : a; Storage actual, expected;
                for (auto* storage : {&actual, &expected}) {
                    std::memcpy(storage->state->uniform_buffer_begin, a.data(), 16);
                    std::memcpy(storage->state->uniform_buffer_begin + 16, b.data(), 16);
                }
                const u64 flags = 0xcafebabefedc4321ULL; actual.state->host_cpu_flags = expected.state->host_cpu_flags = flags;
                const auto mxcsr = _mm_getcsr(); backend::interp::Interpreter reference{*expected.state, block.get()};
                Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, "raw vector bitcast/forwarding halt");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + 64, wanted.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + 64, wanted.data(), 16), "raw vector narrow bitcast re-widening and forwarded view preserve full container");
                Check(actual.state->host_cpu_flags == flags && expected.state->host_cpu_flags == flags && _mm_getcsr() == mxcsr, "raw vector bitcast/forward flags");
            }
        }
    }
    std::cout << "PASS raw vector narrow/scalar bitcasts and cached versus home-based view forwarding\n";
}

void VectorContainerNarrowLoadsAndScalarHomes() {
    const std::array<ValueType, 12> types{ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64,
        ValueType::S8, ValueType::S16, ValueType::S32, ValueType::S64, ValueType::V8, ValueType::V16, ValueType::V32, ValueType::V64};
    for (auto type : types) for (bool memory : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12800})}; ir::Assembler as{block.get()};
        ir::Value loaded;
        if (memory) loaded = as.LoadMemory(ir::Operand{as.LoadImm(ir::Imm{u64{0}}).SetType(ValueType::U64)}).SetType(type);
        else loaded = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto widened = as.BitCast(loaded).SetType(ValueType::V128);
        auto ones = as.VecLoadConst(ir::Imm{UINT64_MAX}, ir::Imm{UINT64_MAX}).SetType(ValueType::V128);
        auto final = as.VecXor(widened, ones).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{64, ValueType::V128}, widened);
        as.StoreUniform(ir::Uniform{80, ValueType::V128}, final);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            for (u32 sample = 0; sample < 16; ++sample) {
                ContainerBytes source{}, wanted{}, inverted{};
                for (u32 byte = 0; byte < 16; ++byte) source[byte] = u8(0x80 + byte * 7 + sample);
                std::copy_n(source.begin(), ir::GetValueSizeByte(type), wanted.begin());
                for (u32 byte = 0; byte < 16; ++byte) inverted[byte] = wanted[byte] ^ 255;
                Storage actual, expected;
                for (auto* storage : {&actual, &expected}) {
                    std::memcpy(storage->state->uniform_buffer_begin, source.data(), 16);
                    storage->state->pt = source.data(); storage->state->guest_addr_limit = 16;
                }
                backend::interp::Interpreter reference{*expected.state, block.get()};
                Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, "vector narrow load/scalar bitcast halt");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + 64, wanted.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + 64, wanted.data(), 16), "narrow loads initialize all unused bytes zero including scalar source homes");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + 80, inverted.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + 80, inverted.data(), 16), "vector consumes fully initialized widened scalar/narrow loaded container");
            }
        }
    }
    std::cout << "PASS narrow memory/uniform loads and initialized scalar homes widened to vector containers\n";
}
