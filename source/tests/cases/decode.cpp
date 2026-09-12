#include "../support/case_support.h"
#include "../support/fp_environment.h"

TEST_CASE("Uniform elimination preserves rotate-by-zero carry polarity load") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    // cmp si,sp; rol r15b,cl; lahf; seto r15b; hlt
    std::array<swift::u8, 13> code{
            0x66, 0x44, 0x39, 0xe6, 0x41, 0xd2, 0xc7,
            0x9f, 0x41, 0x0f, 0x90, 0xc7, 0xf4,
    };
    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    const auto address = reinterpret_cast<VAddr>(code.data());
    const auto polarity_offset = offsetof(ThreadContext64, carry_inverted);
    struct Shape {
        size_t polarity_loads{};
        size_t polarity_stores{};
        size_t carry_inverts{};
    };
    auto decode = [&](Arm64Features arm64_features) {
        Block block{0, Location{address}};
        Assembler assembler{&block};
        X64Decoder decoder{address, &memory, &assembler, true,
                           arm64_features, false, false, FeatureSet{}};
        decoder.Decode();

        UniformInfo info{.uniform_size = sizeof(ThreadContext64)};
        UniformEliminationPass::Run(&block, info, FeatureSet{});

        Shape shape;
        for (auto& inst : block.GetInstList()) {
            if ((inst.GetOp() == OpCode::LoadUniform ||
                 inst.GetOp() == OpCode::StoreUniform) &&
                inst.GetArg<Uniform>(0).GetOffset() == polarity_offset) {
                shape.polarity_loads += inst.GetOp() == OpCode::LoadUniform;
                shape.polarity_stores += inst.GetOp() == OpCode::StoreUniform;
            }
            shape.carry_inverts += inst.GetOp() == OpCode::InvertCarry;
        }
        return shape;
    };

    REQUIRE(decode(Arm64Features::None).polarity_loads == 1);
    const auto canonical = decode(Arm64Features::FlagM);
    REQUIRE(canonical.polarity_loads == 0);
    REQUIRE(canonical.polarity_stores == 0);
    REQUIRE(canonical.carry_inverts >= 1);
}

TEST_CASE("narrow rotate compact recognizes only the verified U16 immediate-eight DAG") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    struct Shape {
        size_t byte_swaps{};
        size_t variable_shifts{};
        size_t carry_writes{};
        size_t overflow_writes{};
    };
    auto decode = [&](bool enabled, bool left, swift::u8 count) {
        // 66 C1 /0 ib = rol si,imm8; /1 = ror si,imm8.
        std::array<swift::u8, 6> code{
                0x66, 0xc1, static_cast<swift::u8>(left ? 0xc6 : 0xce), count, 0xf4, 0x90,
        };
        const auto address = reinterpret_cast<VAddr>(code.data());
        Block block{0, Location{address}};
        Assembler assembler{&block};
        FeatureSet features{};
        features.narrow_rotate_compact = enabled;
        X64Decoder decoder{address, &memory, &assembler, true,
                           Arm64Features::None, false, false, features};
        decoder.Decode();

        Shape shape{};
        for (auto& inst : block.GetInstList()) {
            shape.byte_swaps += inst.GetOp() == OpCode::ByteSwap;
            shape.variable_shifts +=
                    inst.GetOp() == OpCode::LslValue || inst.GetOp() == OpCode::LsrValue;
            shape.carry_writes += inst.GetOp() == OpCode::SetCarry;
            shape.overflow_writes += inst.GetOp() == OpCode::SetOverflow;
        }
        return shape;
    };

    for (bool left : {false, true}) {
        for (swift::u8 count = 0; count < 16; ++count) {
            INFO("direction=" << (left ? "rol" : "ror") << " count=" << unsigned(count));
            const auto off = decode(false, left, count);
            const auto on = decode(true, left, count);
            REQUIRE(off.byte_swaps == 0);
            REQUIRE(on.byte_swaps == (count == 8 ? 1 : 0));
            if (count == 8) {
                REQUIRE(on.variable_shifts == 0);
                REQUIRE(off.variable_shifts == 2);
            } else {
                REQUIRE(on.variable_shifts == off.variable_shifts);
            }
            // The compact lowering only changes the value DAG.  CF and OF
            // production stay present in both directions.
            REQUIRE(on.carry_writes == off.carry_writes);
            REQUIRE(on.overflow_writes == off.overflow_writes);
        }
    }

    // Memory destinations retain the established read/write and fault path.
    std::array<swift::u8, 6> memory_code{0x66, 0xc1, 0x00, 0x08, 0xf4, 0x90};
    const auto address = reinterpret_cast<VAddr>(memory_code.data());
    Block block{0, Location{address}};
    Assembler assembler{&block};
    FeatureSet features{};
    features.narrow_rotate_compact = true;
    X64Decoder decoder{address, &memory, &assembler, true,
                       Arm64Features::None, false, false, features};
    decoder.Decode();
    REQUIRE(std::none_of(block.GetInstList().begin(), block.GetInstList().end(),
                         [](const Inst& inst) { return inst.GetOp() == OpCode::ByteSwap; }));
}

TEST_CASE("function decoder replays at a late block entry") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    std::array<swift::u8, 9> code{
            0xb8, 0x01, 0x00, 0x00, 0x00,
            0xd1, 0xc0,
            0x75, 0xfc,
    };
    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    const auto start = reinterpret_cast<VAddr>(code.data());
    const auto boundary = start + 5;
    HIRBuilder builder{1, true, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{start});
    auto* first = function->GetCurrentBlock();

    Assembler first_assembler{&builder};
    X64Decoder first_decoder{start,
                             &memory,
                             &first_assembler,
                             true,
                             Arm64Features::None,
                             false,
                             false,
                             FeatureSet{}};
    first_decoder.Decode();
    REQUIRE(std::count_if(first->GetInstList().begin(),
                          first->GetInstList().end(),
                          [](const Inst& inst) {
                              return inst.GetOp() == OpCode::RorImm;
                          }) == 1);

    HIRBlock* second{};
    for (auto& block : function->GetHIRBlockList()) {
        if (block.GetBlock()->GetStartLocation().Value() == boundary) {
            second = &block;
            break;
        }
    }
    REQUIRE(second != nullptr);
    REQUIRE(builder.ResetDecodedBlock(first));

    Assembler replay_assembler{&builder};
    X64Decoder replay_decoder{start,
                              &memory,
                              &replay_assembler,
                              true,
                              Arm64Features::None,
                              false,
                              false,
                              FeatureSet{},
                              boundary};
    replay_decoder.Decode();
    REQUIRE(std::none_of(first->GetInstList().begin(),
                         first->GetInstList().end(),
                         [](const Inst& inst) {
                             return inst.GetOp() == OpCode::RorImm;
                         }));
    REQUIRE(VisitVariant<bool>(first->GetBlock()->GetTerminal(),
                               [boundary](const auto& value) {
                                   using T = std::decay_t<decltype(value)>;
                                   if constexpr (std::is_same_v<
                                                         T,
                                                         terminal::LinkBlock>) {
                                       return value.next.Value() == boundary;
                                   }
                                   return false;
                               }));

    builder.SetCurBlock(second);
    Assembler second_assembler{&builder};
    X64Decoder second_decoder{boundary,
                              &memory,
                              &second_assembler,
                              true,
                              Arm64Features::None,
                              false,
                              false,
                              FeatureSet{}};
    second_decoder.Decode();
    REQUIRE(std::count_if(second->GetInstList().begin(),
                          second->GetInstList().end(),
                          [](const Inst& inst) {
                              return inst.GetOp() == OpCode::RorImm;
                          }) == 1);
}

TEST_CASE("function decoder links an unconditional jump only to an existing block") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    std::array<swift::u8, 32> code{};
    code[0] = 0xeb;
    code[1] = 0x02;
    code[4] = 0xf4;
    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    const auto start = reinterpret_cast<VAddr>(code.data());
    const auto target = Location{start + 4};
    auto decode = [&](bool create_target) {
        HIRBuilder builder{1, true, false, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{start});
        auto* entry = function->GetCurrentBlock();
        if (create_target) {
            function->CreateOrGetBlock(target);
        }
        Assembler assembler{&builder};
        X64Decoder decoder{start,
                           &memory,
                           &assembler,
                           true,
                           Arm64Features::None,
                           false,
                           false,
                           FeatureSet{}};
        decoder.Decode();
        const bool has_set_location = std::any_of(
                entry->GetInstList().begin(), entry->GetInstList().end(),
                [](const Inst& inst) { return inst.GetOp() == OpCode::SetLocation; });
        const bool links_target = VisitVariant<bool>(
                entry->GetBlock()->GetTerminal(), [&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, terminal::LinkBlock>) {
                        return value.next == target;
                    }
                    return false;
                });
        const bool links_external = VisitVariant<bool>(
                entry->GetBlock()->GetTerminal(), [&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T,
                                                 terminal::ExternalLinkBlock>) {
                        return value.next == target;
                    }
                    return false;
                });
        const bool records_external =
                function->GetExternalDirectLinks().size() == 1 &&
                function->GetExternalDirectLinks().front().source == entry &&
                function->GetExternalDirectLinks().front().target == target;
        return std::tuple{has_set_location, links_target, links_external,
                          records_external};
    };

    const auto external = decode(false);
    const auto internal = decode(true);
    REQUIRE(external == std::tuple{true, false, false, true});
    REQUIRE(internal == std::tuple{false, true, false, false});
}

TEST_CASE("structured V128 address wraps inside the 4GB guest window") {
    using namespace swift::translator;
    using namespace swift::translator::x86;

    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<size_t>(page_long);
    auto* window = static_cast<swift::u8*>(
            mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(window != MAP_FAILED);

    const std::array<swift::u8, 16> expected{
            0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
            0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f,
    };
    std::memcpy(window + 0x10, expected.data(), expected.size());

    // movaps xmm0,[rax+0x18]; hlt
    // 0xfffffff8 + 0x18 = 0x100000010, whose 4GB-window address is 0x10.
    constexpr swift::u64 code_guest = 0x1000;
    const std::array<swift::u8, 5> code{0x0f, 0x28, 0x40, 0x18, 0xf4};
    REQUIRE(code_guest + code.size() < page * 2);
    std::memcpy(window + code_guest, code.data(), code.size());

    swift::runtime::backend::SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make(window, UINT32_MAX);
    auto* core = X86Core::Make(instance);
    auto& context = core->GetContext();
    context.rip.qword = code_guest;
    context.rax.qword = UINT64_C(0xfffffff8);

    const auto exit = core->Run();
    INFO("SVM_ADDRMODE_STRUCT="
         << (swift::runtime::GetRawSvmConfigEnvForTest("SVM_ADDRMODE_STRUCT")
                     ? swift::runtime::GetRawSvmConfigEnvForTest("SVM_ADDRMODE_STRUCT")
                     : "<unset>"));
    REQUIRE(exit == ExitReason::None);
    REQUIRE(std::memcmp(&context.xmm0, expected.data(), expected.size()) == 0);

    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(window, page * 2);
}

TEST_CASE("A1 host-base fold gate is exact and Q UXTW encoding is valid") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend::arm64;
    using namespace vixl::aarch64;

    REQUIRE(HostBaseFoldEligible(true, true, UINT32_MAX,
                                 ir::ValueType::V128, true, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(false, true, UINT32_MAX,
                                       ir::ValueType::V128, true, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, false, UINT32_MAX,
                                       ir::ValueType::V128, true, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, true, 0,
                                       ir::ValueType::V128, true, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, true, UINT32_MAX,
                                       ir::ValueType::U64, true, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, true, UINT32_MAX,
                                       ir::ValueType::V128, false, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, true, UINT32_MAX,
                                       ir::ValueType::V128, true, false, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, true, UINT32_MAX,
                                       ir::ValueType::V128, true, true, true));

    MacroAssembler masm;
    masm.Ldr(q0, MemOperand{x24, w10, UXTW});
    masm.Str(q1, MemOperand{x24, w11, UXTW});
    masm.FinalizeCode();
    auto* first = masm.GetBuffer()->GetStartAddress<const Instruction*>();
    auto* second = first->GetNextInstruction();
    REQUIRE(first[0].GetInstructionBits() == 0x3cea4b00u);
    REQUIRE(second->GetInstructionBits() == 0x3cab4b01u);

    Decoder decoder;
    Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    decoder.Decode(&first[0]);
    REQUIRE(std::string_view(disassembler.GetOutput()).find(
                    "ldr q0, [x24, w10, uxtw]") != std::string_view::npos);
    decoder.Decode(second);
    REQUIRE(std::string_view(disassembler.GetOutput()).find(
                    "str q1, [x24, w11, uxtw]") != std::string_view::npos);
}

TEST_CASE("SwiftVM VIXL scratch contract rejects hidden V-register leases") {
    using namespace vixl::aarch64;

    MacroAssembler stock_masm;
    {
        UseScratchRegisterScope temps(&stock_masm);
        const auto stock = temps.AcquireD();
        REQUIRE(stock.GetCode() == d31.GetCode());
    }

    MacroAssembler masm;
    masm.SvmBeginScratchContract(x16.GetBit(), 0);
    {
        UseScratchRegisterScope temps(&masm);
        const auto acquired = temps.AcquireX();
        REQUIRE(acquired.GetCode() == x16.GetCode());
    }
    const auto clean = masm.SvmEndScratchContract();
    REQUIRE(clean.gpr == x16.GetBit());
    REQUIRE(clean.vreg == 0);

    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        std::signal(SIGABRT, SIG_DFL);
        MacroAssembler child_masm;
        child_masm.SvmBeginScratchContract(
                child_masm.GetScratchRegisterList()->GetList(), 0);
        child_masm.Fcmp(d0, 1.0);
        _exit(0);
    }

    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFSIGNALED(status));
    REQUIRE(WTERMSIG(status) == SIGABRT);
}

TEST_CASE("two structured V128 accesses fault on the second page after the first commits") {
    using namespace swift::translator;
    using namespace swift::translator::x86;

    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<size_t>(page_long);
    auto* data = static_cast<swift::u8*>(
            mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* code_page = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(data != MAP_FAILED);
    REQUIRE(code_page != MAP_FAILED);

    const std::array<swift::u8, 16> first{
            0x01, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78,
            0x89, 0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf0,
    };
    std::memcpy(data + page - first.size(), first.data(), first.size());
    REQUIRE(mprotect(data + page, page, PROT_NONE) == 0);

    // movaps xmm0,[rax]; movaps xmm1,[rax+0x10]; hlt
    const std::array<swift::u8, 8> code{
            0x0f, 0x28, 0x00, 0x0f, 0x28, 0x48, 0x10, 0xf4,
    };
    std::memcpy(code_page, code.data(), code.size());

    swift::runtime::backend::SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make();
    auto* core = X86Core::Make(instance);
    auto& context = core->GetContext();
    context.rip.qword = reinterpret_cast<swift::u64>(code_page);
    context.rax.qword = reinterpret_cast<swift::u64>(data + page - first.size());
    constexpr swift::u64 rsi_before = UINT64_C(0x9b8a796857463524);
    context.rsi.qword = rsi_before;
    std::memset(&context.xmm0, 0x5a, sizeof(context.xmm0));
    std::memset(&context.xmm1, 0xa5, sizeof(context.xmm1));
    std::array<swift::u8, 16> second_before{};
    std::memcpy(second_before.data(), &context.xmm1, second_before.size());

    REQUIRE(core->Run() == ExitReason::PageFatal);
    // The two guest memory nodes stay separate and ordered: the first value is
    // architecturally visible, while the faulting second load commits nothing.
    REQUIRE(std::memcmp(&context.xmm0, first.data(), first.size()) == 0);
    REQUIRE(std::memcmp(&context.xmm1,
                        second_before.data(),
                        second_before.size()) == 0);
    REQUIRE(context.rsi.qword == rsi_before);

    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    REQUIRE(mprotect(data + page, page, PROT_READ | PROT_WRITE) == 0);
    munmap(data, page * 2);
    munmap(code_page, page);
}

TEST_CASE("structured address reloads RAX after every partial alias write") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::translator;
    using namespace swift::translator::x86;
    using namespace swift::x86;

    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<size_t>(page_long);
    auto* window = static_cast<swift::u8*>(
            mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(window != MAP_FAILED);

    const std::array<swift::u8, 16> first{
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    const std::array<swift::u8, 16> second{
            0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
            0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
    };
    std::memcpy(window + 0x100, first.data(), first.size());
    std::memcpy(window + 0x180, second.data(), second.size());

    struct AliasCase {
        const char* name;
        std::vector<swift::u8> write;
    };
    const std::array cases{
            AliasCase{"AL", {0xb0, 0x80}},
            AliasCase{"AX", {0x66, 0xb8, 0x80, 0x01}},
            AliasCase{"EAX", {0xb8, 0x80, 0x01, 0x00, 0x00}},
    };

    struct WindowMemory final : MemoryInterface {
        explicit WindowMemory(swift::u8* base) : base(base) {}
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, base + addr, size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(base + addr, src, size);
        }
        void* GetPointer(void* src) override {
            return base + reinterpret_cast<uintptr_t>(src);
        }
        swift::u8* base;
    } decode_memory{window};

    swift::runtime::backend::SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make(window, UINT32_MAX);
    auto* core = X86Core::Make(instance);
    for (size_t i = 0; i < cases.size(); ++i) {
        INFO("partial alias " << cases[i].name);
        const swift::u64 code_guest = 0x1000 + i * 0x100;
        std::vector<swift::u8> code{0x0f, 0x28, 0x00};  // movaps xmm0,[rax]
        code.insert(code.end(), cases[i].write.begin(), cases[i].write.end());
        code.insert(code.end(), {0x0f, 0x28, 0x08, 0xf4});  // movaps xmm1,[rax]; hlt
        REQUIRE(code_guest + code.size() < page * 2);
        std::memcpy(window + code_guest, code.data(), code.size());

        // Structural half of the test: in ON mode both memory nodes carry the
        // address state LoadUniform directly, and the alias write forces two
        // distinct definitions rather than reusing the first capture.
        Block block{0, Location{code_guest}};
        Assembler assembler{&block};
        X64Decoder decoder{code_guest, &decode_memory, &assembler, true,
                           Arm64Features::None, false, false, FeatureSet{}};
        decoder.Decode();
        std::vector<Inst*> v128_loads;
        for (auto& inst : block.GetInstList()) {
            if (inst.GetOp() == OpCode::LoadMemory &&
                inst.ReturnType() == ValueType::V128) {
                v128_loads.push_back(&inst);
            }
        }
        REQUIRE(v128_loads.size() == 2);
        const bool structured = swift::runtime::GetSvmConfig().addrmode_struct;
        if (structured) {
            const auto first_addr = v128_loads[0]->GetArg<Operand>(0);
            const auto second_addr = v128_loads[1]->GetArg<Operand>(0);
            REQUIRE(first_addr.GetRight().Null());
            REQUIRE(second_addr.GetRight().Null());
            REQUIRE(first_addr.GetLeft().IsValue());
            REQUIRE(second_addr.GetLeft().IsValue());
            REQUIRE(first_addr.GetLeft().value.Def()->GetOp() == OpCode::LoadUniform);
            REQUIRE(second_addr.GetLeft().value.Def()->GetOp() == OpCode::LoadUniform);
            REQUIRE(first_addr.GetLeft().value != second_addr.GetLeft().value);
        }

        auto& context = core->GetContext();
        context.rip.qword = code_guest;
        context.rax.qword = 0x100;
        std::memset(&context.xmm0, 0, sizeof(context.xmm0));
        std::memset(&context.xmm1, 0, sizeof(context.xmm1));
        REQUIRE(core->Run() == ExitReason::None);
        REQUIRE(std::memcmp(&context.xmm0, first.data(), first.size()) == 0);
        REQUIRE(std::memcmp(&context.xmm1, second.data(), second.size()) == 0);
    }
    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(window, page * 2);
}
