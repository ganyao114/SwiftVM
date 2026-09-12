#include "../support/case_support.h"
#include "../support/fp_environment.h"

static swift::runtime::ir::Block* BuildMulCarryOverflowBlock() {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x7000});
    auto a = block->LoadUniform<TypedValue<ValueType::U32>>(Uniform{0, ValueType::U32});
    auto b = block->LoadUniform<TypedValue<ValueType::U32>>(Uniform{4, ValueType::U32});
    // 32x32 unsigned multiply: EmitMul widens with Umull and asks SaveCV
    // whether the upper half is nonzero, i.e. the x86 `mul` CF=OF rule.
    auto product = block->Mul(a, Operand{b});
    block->SaveFlags(product, Flags::Carry | Flags::Overflow);
    block->StoreUniform(Uniform{8, ValueType::U32}, product);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

TEST_CASE("SaveCV commits x86 CF/OF into the flags register") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    swift::runtime::Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    const auto gprs = address_space.GetTrampolines().GetGPRRegs();
    const auto fprs = address_space.GetTrampolines().GetFPRRegs();

    swift::runtime::IntrusivePtr<Block> block{BuildMulCarryOverflowBlock()};
    RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &reg_alloc, false, FeatureSet{});

    arm64::JitContext context{module, reg_alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);

    auto& masm = context.GetMasm();
    auto* buffer = masm.GetBuffer();
    auto* first = buffer->GetStartAddress<const vixl::aarch64::Instruction*>();
    auto* last = buffer->GetEndAddress<const vixl::aarch64::Instruction*>();

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::string text;
    for (const auto* instr = first; instr < last; instr = instr->GetNextInstruction()) {
        decoder.Decode(instr);
        text += disassembler.GetOutput();
        text += '\n';
    }
    INFO(text);

    // HostFlagsBit::C = 29, HostFlagsBit::V = 28 -> (3u << 28) = 0x30000000,
    // and `flags` is x26 (backend/arm64/defines.h).
    REQUIRE(text.find("orr x26, x26, #0x30000000") != std::string::npos);
    // ...and it must NOT be stashed in host NZCV, where nothing collects it.
    // Nothing else in this block has any reason to write NZCV: the only guest
    // flag producer here is the multiply.
    REQUIRE(text.find("msr nzcv") == std::string::npos);
    REQUIRE(text.find("msr NZCV") == std::string::npos);
}

static swift::runtime::ir::Block* BuildCondSetArithmeticBlock(
        swift::runtime::ir::Cond cond) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x7100});
    auto flags_value = block->LoadImm(Imm{std::uint64_t(0ull)}).SetType(ValueType::U64);
    block->SaveFlags(flags_value, Flags::NZCV);
    auto one = block->CondSet(cond).SetType(ValueType::U64);
    auto base = block->LoadImm(Imm{std::uint64_t(41ull)}).SetType(ValueType::U64);
    auto sum = block->Add(base, Operand{one});
    block->StoreUniform(Uniform{0, ValueType::U64}, sum);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

static std::string TranslateCondSetArithmeticBlock(swift::runtime::ir::Cond cond) {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    swift::runtime::Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    const auto gprs = address_space.GetTrampolines().GetGPRRegs();
    const auto fprs = address_space.GetTrampolines().GetFPRRegs();

    swift::runtime::IntrusivePtr<Block> block{BuildCondSetArithmeticBlock(cond)};
    RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &reg_alloc, false, FeatureSet{});

    arm64::JitContext context{module, reg_alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    auto& masm = context.GetMasm();
    auto* buffer = masm.GetBuffer();
    auto* first = buffer->GetStartAddress<const vixl::aarch64::Instruction*>();
    auto* last = buffer->GetEndAddress<const vixl::aarch64::Instruction*>();

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::string text;
    for (const auto* instr = first; instr < last; instr = instr->GetNextInstruction()) {
        decoder.Decode(instr);
        text += disassembler.GetOutput();
        text += '\n';
    }
    return text;
}

TEST_CASE("CondSet extracts simple conditions directly from saved flags") {
    using swift::runtime::ir::Cond;
    struct Case {
        Cond cond;
        std::uint32_t bit;
        bool inverted;
    };
    constexpr std::array cases{
            Case{Cond::EQ, 30, false},
            Case{Cond::NE, 30, true},
            Case{Cond::CS, 29, false},
            Case{Cond::CC, 29, true},
            Case{Cond::MI, 31, false},
            Case{Cond::PL, 31, true},
            Case{Cond::VS, 28, false},
            Case{Cond::VC, 28, true},
    };

    for (const auto& test_case : cases) {
        const auto text = TranslateCondSetArithmeticBlock(test_case.cond);
        INFO(text);
        REQUIRE(text.find("ubfx ") != std::string::npos);
        REQUIRE(text.find("x26") != std::string::npos);
        REQUIRE(text.find(fmt::format("#{}, #1", test_case.bit)) != std::string::npos);
        REQUIRE(text.find("msr nzcv") == std::string::npos);
        REQUIRE(text.find("cset ") == std::string::npos);
        REQUIRE(text.find("csetm ") == std::string::npos);
        REQUIRE(text.find("add ") != std::string::npos);
        if (test_case.inverted) {
            REQUIRE(text.find("eor ") != std::string::npos);
        }
    }

    const auto composite = TranslateCondSetArithmeticBlock(Cond::HI);
    INFO(composite);
    REQUIRE(composite.find("msr nzcv") != std::string::npos);
    REQUIRE(composite.find("cset ") != std::string::npos);
}
