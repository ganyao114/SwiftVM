// Host-register IR is deliberately not interpreted as a reference: the
// interpreter's Host handlers are placeholders. Expected values below are
// specified as independent byte updates to the canonical uniform buffer.
struct HostCompiled {
    x64::Translator translator;
    HaltReason (*fn)(backend::State*);
    HostCompiled(ir::Block* block, const Config& config) : translator(block, x64::HostFeatures::Detect(), &config) {
        translator.Translate(); fn = translator.getCode<decltype(fn)>();
    }
};

bool FixedHostGprReference(u32 reg) {
    return reg <= 9 || (reg >= 19 && reg <= 23) || reg == 29;
}

ir::Value HostGet(ir::Assembler& as, bool fpr, u64 reg, u64 offset, ValueType type) {
    return (fpr ? as.GetHostFPR(ir::Imm{reg}, ir::Imm{offset}) :
                  as.GetHostGPR(ir::Imm{reg}, ir::Imm{offset})).SetType(type);
}

void HostSet(ir::Assembler& as, bool fpr, ir::Value value, u64 reg, u64 offset) {
    if (fpr) as.SetHostFPR(value, ir::Imm{reg}, ir::Imm{offset});
    else as.SetHostGPR(value, ir::Imm{reg}, ir::Imm{offset});
}

void HostUniformSlicesAndSnapshots() {
    u64 seed = 0x91246a376723be91ULL;
    for (bool fpr : {false, true}) for (u32 reg = 0; reg < 32; ++reg)
        for (u32 size : {1u, 2u, 4u, 8u, 16u}) {
            const u32 width = fpr ? 16 : 8;
            if (size > width) continue;
            for (u32 slice = 0; slice + size <= width; slice += fpr ? size : 1) {
                const u32 base = fpr ? 128 : 64;
                const auto type = fpr ? ir::GetVecIRValueType(size) : ir::GetIRValueType(size);
                std::array maps{UniformMapDesc{base, width, reg, fpr}};
                auto config = TestConfig(); config.buffers_static_alloc = maps;
                IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd000})};
                ir::Assembler as{block.get()};
                auto old = HostGet(as, fpr, reg, slice, type);
                auto value = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
                HostSet(as, fpr, value, reg, slice);
                auto now = HostGet(as, fpr, reg, slice, type);
                auto canonical = as.LoadUniform(ir::Uniform{base + slice, type}).SetType(type);
                as.StoreUniform(ir::Uniform{256, type}, old);
                as.StoreUniform(ir::Uniform{272, type}, now);
                as.StoreUniform(ir::Uniform{288, type}, canonical);
                // A canonical write must be visible to the following Host Get.
                as.StoreUniform(ir::Uniform{base + slice, type}, old);
                auto restored = HostGet(as, fpr, reg, slice, type);
                as.StoreUniform(ir::Uniform{304, type}, restored);
                block->SetTerminal(ir::terminal::ReturnToHost{});
                HostCompiled compiled{block.get(), config};
                // Code retains a validated copy, not a borrowed descriptor.
                maps[0].offset = 4000;
                for (u32 iteration = 0; iteration < 8; ++iteration) {
                    Storage actual;
                    std::array<u8, 320> expected{};
                    for (u32 offset = 0; offset < expected.size(); offset += 8) {
                        const u64 bits = Random(seed);
                        std::memcpy(expected.data() + offset, &bits, 8); actual.Put(offset, bits);
                    }
                    std::array<u8, 16> snapshot{};
                    std::memcpy(snapshot.data(), expected.data() + base + slice, size);
                    std::memcpy(expected.data() + base + slice, expected.data() + 32, size);
                    if (!fpr && size == 4 && slice == 0 && FixedHostGprReference(reg))
                        std::memset(expected.data() + base + 4, 0, 4);
                    std::memcpy(expected.data() + 256, snapshot.data(), size);
                    std::memcpy(expected.data() + 272, expected.data() + 32, size);
                    std::memcpy(expected.data() + 288, expected.data() + 32, size);
                    std::memcpy(expected.data() + base + slice, snapshot.data(), size);
                    std::memcpy(expected.data() + 304, snapshot.data(), size);
                    actual.state->host_cpu_flags = 0x12345678fabcdef0ULL;
                    Check(compiled.fn(actual.state) == HaltReason::CallHost, "Host mapped slice halt");
                    Check(std::memcmp(actual.state->uniform_buffer_begin, expected.data(), expected.size()) == 0,
                          "Host mapped slice/old SSA/canonical write: " + std::to_string(fpr) + "/" +
                          std::to_string(reg) + "/" + std::to_string(size) + "/" + std::to_string(slice));
                    Check(actual.state->host_cpu_flags == 0x12345678fabcdef0ULL, "Host mapped accesses preserve flags");
                }
            }
        }
    std::cout << "PASS Host uniform slices, all logical registers and SSA snapshots\n";
}

u64 HostUniformMutation(u64 pointer) {
    auto* state = reinterpret_cast<backend::State*>(pointer);
    auto* bytes = state->uniform_buffer_begin;
    u64 observed{}; std::memcpy(&observed, bytes + 64, 8);
    std::memcpy(bytes + 96, bytes + 64, 8);
    std::memcpy(bytes + 160, bytes + 128, 16);
    const u64 scalar = 0xbcd1234567890abcULL;
    const std::array<u64, 2> vector{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    std::memcpy(bytes + 64, &scalar, 8); std::memcpy(bytes + 128, vector.data(), 16);
    return observed;
}

void HostUniformHelpersAndHalt() {
    std::array maps{UniformMapDesc{64, 8, 0, false}, UniformMapDesc{128, 16, 0, true}};
    auto config = TestConfig(); config.buffers_static_alloc = maps;
    for (bool halt : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd100})}; ir::Assembler as{block.get()};
        auto old = HostGet(as, false, 0, 0, ValueType::U64);
        auto scalar = as.LoadUniform(ir::Uniform{32, ValueType::U32}).SetType(ValueType::U32);
        auto vector = as.LoadUniform(ir::Uniform{40, ValueType::V128}).SetType(ValueType::V128);
        HostSet(as, false, scalar, 0, 0); HostSet(as, true, vector, 0, 0);
        as.AdvancePC(ir::Imm{u32{4}});
        auto pointer = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto result = as.CallHost(&HostUniformMutation, pointer).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{104, ValueType::U64}, result);
        if (halt) {
            (void)as.CallHost(&HaltAtPublishedLocation, pointer).SetType(ValueType::U64);
        }
        auto after_gpr = HostGet(as, false, 0, 0, ValueType::U64);
        auto after_fpr = HostGet(as, true, 0, 0, ValueType::V128);
        as.StoreUniform(ir::Uniform{192, ValueType::U64}, old);
        as.StoreUniform(ir::Uniform{200, ValueType::U64}, after_gpr);
        as.StoreUniform(ir::Uniform{208, ValueType::V128}, after_fpr);
        block->SetTerminal(ir::terminal::ReturnToHost{}); HostCompiled compiled{block.get(), config};
        Storage actual; actual.Put(0, reinterpret_cast<u64>(actual.state));
        actual.Put(32, 0xdeadbeef98765432ULL); actual.Put(40, 0xabcddcba32100123ULL);
        actual.Put(48, 0x4567765410fffeeeULL); actual.Put(64, UINT64_MAX);
        actual.Put(192, 0x8888777766665555ULL); actual.Put(200, 0x8888777766665555ULL);
        Check(compiled.fn(actual.state) == (halt ? HaltReason::PageFatal : HaltReason::CallHost), "Host helper halt contract");
        Check(actual.Get(96) == 0x98765432ULL && actual.Get(104) == 0x98765432ULL,
              "helper observes committed fixed GPR U32 zero extension");
        Check(actual.Get(160) == actual.Get(40) && actual.Get(168) == actual.Get(48), "helper observes committed FPR");
        Check(actual.Get(64) == 0xbcd1234567890abcULL, "helper mutation remains canonical on exit");
        Check(actual.state->current_loc == 0xd104, "mapped helper publishes precise PC");
        if (halt) {
            Check(actual.Get(192) == 0x8888777766665555ULL && actual.Get(200) == 0x8888777766665555ULL,
                  "helper halt suppresses following Host loads and stores");
        } else {
            Check(actual.Get(192) == UINT64_MAX && actual.Get(200) == actual.Get(64), "old SSA survives helper and fresh Get sees mutation");
            Check(actual.Get(208) == 0x0123456789abcdefULL && actual.Get(216) == 0xfedcba9876543210ULL,
                  "FPR Get sees canonical helper mutations");
        }
    }
    std::cout << "PASS Host helper canonical state, mutations and early halt\n";
}

void HostUniformValidation() {
    const auto reject = [](std::span<UniformMapDesc> maps, bool fpr, bool set, u64 reg, u64 slice,
                           ValueType type, u32 limit = 4096, bool supply_config = true) {
        auto config = TestConfig(); config.buffers_static_alloc = maps; config.uniform_buffer_size = limit;
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd200})}; ir::Assembler as{block.get()};
        if (set) {
            auto value = as.LoadUniform(ir::Uniform{0, type}).SetType(type); HostSet(as, fpr, value, reg, slice);
        } else (void)HostGet(as, fpr, reg, slice, type);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        bool rejected{};
        try { x64::Translator translator{block.get(), x64::HostFeatures::Detect(), supply_config ? &config : nullptr}; }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "invalid Host binding rejected in translator construction");
    };
    std::array gpr{UniformMapDesc{64, 8, 0, false}};
    std::array fpr{UniformMapDesc{128, 16, 0, true}};
    for (bool set : {false, true}) {
        reject(gpr, false, set, 1, 0, ValueType::U64);
        reject(gpr, true, set, 0, 0, ValueType::V64);
        reject(gpr, false, set, 0, 1, ValueType::U64);
        reject(gpr, false, set, 0, UINT64_MAX, ValueType::U8);
        reject(gpr, false, set, u64{1} << 32, 0, ValueType::U64);
        reject(gpr, false, set, 32, 0, ValueType::U64);
        reject(gpr, false, set, 0, 0, ValueType::V128);
        reject(gpr, false, set, 0, 0, ValueType::VOID);
        reject(gpr, false, set, 0, 0, ValueType::U64, 0);
        reject(gpr, false, set, 0, 0, ValueType::U64, 4096, false);
        reject(fpr, true, set, 0, 1, ValueType::V32);
        reject(fpr, true, set, 0, 8, ValueType::V128);
        reject(fpr, true, set, 0, 0, ValueType::V256);
        reject(fpr, true, set, 0, u64{1} << 32, ValueType::V8);
    }
    for (auto desc : {UniformMapDesc{64, 0, 0, false}, UniformMapDesc{64, 3, 0, false},
                      UniformMapDesc{64, 16, 0, false}, UniformMapDesc{64, 32, 0, true},
                      UniformMapDesc{64, 8, 32, false}, UniformMapDesc{64, 8, 65535, false},
                      UniformMapDesc{4093, 8, 0, false}, UniformMapDesc{4096, 1, 0, false},
                      UniformMapDesc{UINT32_MAX - 1, 8, 0, false}}) {
        std::array maps{desc}; reject(maps, desc.is_float, false, 0, 0, ValueType::U8);
    }
    std::array duplicate{UniformMapDesc{64, 8, 0, false}, UniformMapDesc{128, 8, 0, false}};
    reject(duplicate, false, false, 0, 0, ValueType::U64);
    std::array overlap{UniformMapDesc{64, 8, 0, false}, UniformMapDesc{68, 16, 0, true}};
    reject(overlap, false, false, 0, 0, ValueType::U64);
    std::array displacement{UniformMapDesc{UINT32_MAX / 2, 8, 0, false}};
    reject(displacement, false, false, 0, 0, ValueType::U64, UINT32_MAX);

    // A binding may end exactly at the buffer boundary and share its logical
    // number with the other class. Adjacent ranges do not overlap.
    std::array edge{UniformMapDesc{4072, 8, 0, false}, UniformMapDesc{4080, 16, 0, true}};
    auto config = TestConfig(); config.buffers_static_alloc = edge;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd300})}; ir::Assembler as{block.get()};
    auto a = HostGet(as, false, 0, 7, ValueType::U8);
    auto b = HostGet(as, true, 0, 8, ValueType::V64);
    as.StoreUniform(ir::Uniform{0, ValueType::U8}, a); as.StoreUniform(ir::Uniform{8, ValueType::V64}, b);
    block->SetTerminal(ir::terminal::ReturnToHost{}); HostCompiled compiled{block.get(), config};
    Storage actual; actual.Put(4072, 0xaabbccddeeff1234ULL); actual.Put(4088, 0x1234abcd9876ffffULL);
    Check(compiled.fn(actual.state) == HaltReason::CallHost, "Host exact-end mappings compile and execute");
    Check(actual.Get(0) == 0xaa && actual.Get(8) == 0x1234abcd9876ffffULL, "Host exact-end slices and class separation");
    IntrusivePtr<ir::Block> phi{new ir::Block(ir::Location{0xd400})}; ir::Assembler pa{phi.get()};
    auto value = pa.AddPhi(ir::Params{}).SetType(ValueType::U64);
    pa.StoreUniform(ir::Uniform{0, ValueType::U64}, value); phi->SetTerminal(ir::terminal::ReturnToHost{});
    bool rejected{};
    try { x64::Translator translator{phi.get(), x64::HostFeatures::Detect(), &config}; }
    catch (const std::runtime_error& error) { rejected = std::string{error.what()}.find("phi input count") != std::string::npos; }
    Check(rejected, "host-entry Phi rejects missing incoming-edge input");
    std::cout << "PASS Host descriptor/slice validation and malformed Phi rejection\n";
}

void HostUniformNarrowBindings() {
    for (bool fpr : {false, true}) for (u32 width : {1u, 2u, 4u, 8u, 16u}) {
        if (!fpr && width > 8) continue;
        std::array maps{UniformMapDesc{64, width, 0, fpr}};
        auto config = TestConfig(); config.buffers_static_alloc = maps; config.uniform_buffer_size = 64 + width;
        // Scalar integer views of FPRs are bit-container accesses too. A
        // 4-byte fixed GPR binding must not clear bytes outside its own range.
        const auto type = width == 16 ? ValueType::V128 : ir::GetIRValueType(width);
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd480})}; ir::Assembler as{block.get()};
        auto value = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        HostSet(as, fpr, value, 0, 0);
        auto result = HostGet(as, fpr, 0, 0, type);
        as.StoreUniform(ir::Uniform{0, type}, result);
        block->SetTerminal(ir::terminal::ReturnToHost{}); HostCompiled compiled{block.get(), config};
        Storage actual; std::array<u8, 96> expected{};
        for (u32 offset = 0; offset < expected.size(); ++offset) expected[offset] = u8(offset * 23 + 17);
        std::memcpy(actual.state->uniform_buffer_begin, expected.data(), expected.size());
        std::memcpy(expected.data() + 64, expected.data() + 32, width);
        std::memcpy(expected.data(), expected.data() + 32, width);
        Check(compiled.fn(actual.state) == HaltReason::CallHost, "all supported Host binding widths execute");
        Check(std::memcmp(actual.state->uniform_buffer_begin, expected.data(), expected.size()) == 0,
              "narrow binding preserves adjacent bytes and accepts integer FPR views");
    }
    std::cout << "PASS all Host binding widths and integer FPR views\n";
}

void HostUniformRuntimeFaultCommit() {
    std::array maps{UniformMapDesc{64, 8, 0, false}, UniformMapDesc{128, 16, 0, true}};
    auto config = TestConfig(); config.buffers_static_alloc = maps;
    backend::AddressSpace space{config}; auto module = space.GetDefaultModule();
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd500})}; ir::Assembler as{block.get()};
    auto scalar = as.LoadImm(ir::Imm{u64{0x123456789abcdeff}}).SetType(ValueType::U64);
    auto vector = as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
    HostSet(as, false, scalar, 0, 0); HostSet(as, true, vector, 0, 0);
    as.AdvancePC(ir::Imm{u32{4}});
    auto address = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    (void)as.LoadMemory(ir::Operand{address}).SetType(ValueType::U64);
    auto zero = as.LoadImm(ir::Imm{u64{0}}).SetType(ValueType::U64); HostSet(as, false, zero, 0, 0);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(block.get()) && backend::TranslateIR(module, block), "module Config reaches Host legalization");
    Runtime runtime{&space};
    auto* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(page != MAP_FAILED, "allocate Host commit fault page");
    const auto pointer = reinterpret_cast<u64>(page);
    auto uniforms = runtime.GetUniformBuffer();
    std::memcpy(uniforms.data(), &pointer, 8);
    const std::array<u64, 2> vector_bits{0x8899aabbccddeeffULL, 0x1234abcdef908765ULL};
    std::memcpy(uniforms.data() + 32, vector_bits.data(), 16);
    runtime.GetState()->guest_addr_limit = UINT64_MAX; runtime.SetLocation(0xd500);
    Check(runtime.Run() == HaltReason::PageFatal && runtime.GetLocation() == 0xd504, "Host commit real fault and precise PC");
    u64 observed{}; std::memcpy(&observed, uniforms.data() + 64, 8);
    Check(observed == 0x123456789abcdeffULL, "mapped GPR committed before real fault, following Set suppressed");
    Check(std::memcmp(uniforms.data() + 128, vector_bits.data(), 16) == 0, "mapped FPR committed before real fault");
    Check(backend::riscv64::anonymous_memory_readers.load() == 0, "Host mapped fault releases memory lease");
    munmap(page, 4096);
    std::cout << "PASS Host module Config and real fault commit\n";
}

void HostUniformFunctionConfig() {
    std::array maps{UniformMapDesc{64, 8, 0, false}};
    auto config = TestConfig(); config.buffers_static_alloc = maps;
    backend::AddressSpace space{config}; auto module = space.GetDefaultModule();
    ir::HIRBuilder builder;
    auto* function = builder.AppendFunction(ir::Location{0xd600});
    ir::Assembler as{&builder};
    auto value = as.LoadImm(ir::Imm{u64{0xabcdef0112345678}}).SetType(ValueType::U64);
    HostSet(as, false, value, 0, 0);
    auto* next = builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xd610}});
    builder.SetCurBlock(next);
    auto incoming = HostGet(as, false, 0, 0, ValueType::U64);
    as.StoreUniform(ir::Uniform{0, ValueType::U64}, incoming);
    builder.ReturnToHost();
    const std::array<ir::HIRFunction*, 1> functions{function};
    Check(x64::CompileFunctions(module, functions) != nullptr, "function compilation accepts module Host bindings");
    Runtime runtime{&space}; runtime.SetLocation(0xd600);
    Check(runtime.Run() == HaltReason::CallHost && runtime.GetLocation() == 0xd610, "mapped independent function blocks dispatch");
    u64 observed{}; std::memcpy(&observed, runtime.GetUniformBuffer().data(), 8);
    Check(observed == 0xabcdef0112345678ULL, "function block transitions preserve canonical Host state");
    std::cout << "PASS Host function Config and independent-block transitions\n";
}
