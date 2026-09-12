#include "../support/case_support.h"
#include "../support/fp_environment.h"

TEST_CASE("FEX-style vector immediate shift boundary IR") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    // PSLLQ xmm0,{0,63,64,255}; PSRLQ xmm0,{0,63,64,255};
    // PSLLDQ xmm0,{0,15,16}; PSRLDQ xmm0,{0,15,16}; HLT.
    // These are the x86 semantic cliffs which must never be reduced modulo
    // the element/vector width by an AArch64 immediate encoding.
    std::vector<swift::u8> code;
    const auto append = [&](std::initializer_list<swift::u8> bytes) {
        code.insert(code.end(), bytes.begin(), bytes.end());
    };
    for (swift::u8 count : {swift::u8{0}, swift::u8{63}, swift::u8{64}, swift::u8{255}}) {
        append({0x66, 0x0F, 0x73, 0xF0, count});  // /6 PSLLQ
    }
    for (swift::u8 count : {swift::u8{0}, swift::u8{63}, swift::u8{64}, swift::u8{255}}) {
        append({0x66, 0x0F, 0x73, 0xD0, count});  // /2 PSRLQ
    }
    for (swift::u8 count : {swift::u8{0}, swift::u8{15}, swift::u8{16}}) {
        append({0x66, 0x0F, 0x73, 0xF8, count});  // /7 PSLLDQ
    }
    for (swift::u8 count : {swift::u8{0}, swift::u8{15}, swift::u8{16}}) {
        append({0x66, 0x0F, 0x73, 0xD8, count});  // /3 PSRLDQ
    }
    code.push_back(0xF4);

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
    Block block{0, Location{address}};
    Assembler assembler{&block};
    X64Decoder decoder{address, &memory, &assembler, true,
                       Arm64Features::None, false, false, FeatureSet{}};
    decoder.Decode();

    std::vector<swift::u64> left_counts;
    std::vector<swift::u64> right_counts;
    std::vector<std::pair<swift::u64, bool>> byte_shifts;
    size_t variable_shifts = 0;
    size_t shared_zeroes = 0;
    for (auto& inst : block.GetInstList()) {
        switch (inst.GetOp()) {
            case OpCode::VecShiftLeftImm:
                REQUIRE(inst.GetArg<Imm>(2).Get() == 64);
                left_counts.push_back(inst.GetArg<Imm>(1).Get());
                break;
            case OpCode::VecShiftRightImm:
                REQUIRE(inst.GetArg<Imm>(2).Get() == 64);
                right_counts.push_back(inst.GetArg<Imm>(1).Get());
                break;
            case OpCode::VecByteShift:
                byte_shifts.emplace_back(
                        inst.GetArg<Imm>(2).Get(),
                        inst.GetArg<Imm>(3).Get() != 0);
                break;
            case OpCode::VecShiftLeft:
            case OpCode::VecShiftRight:
                ++variable_shifts;
                break;
            case OpCode::VecSharedZero:
                ++shared_zeroes;
                break;
            default:
                break;
        }
    }

    REQUIRE(left_counts == std::vector<swift::u64>{0, 63, 64, 255});
    REQUIRE(right_counts == std::vector<swift::u64>{0, 63, 64, 255});
    REQUIRE(variable_shifts == 0);
    // count=0 is direct and emits no byte-shift IR; count=16 is represented
    // by the explicit all-zero vector. Only the two count=15 cases reach EXT.
    REQUIRE((byte_shifts ==
             std::vector<std::pair<swift::u64, bool>>{{15, true}, {15, false}}));
    REQUIRE(shared_zeroes == 4);  // one for each 15 and 16 lowering before CSE
}

TEST_CASE("Constant CSE does not reuse a constant computed under a branch") {
    using namespace swift::runtime::ir;

    // ConstFoldingPass deduplicates LoadImm within a sliding window, and resets
    // its table at Goto / NotGoto / BindLabel because the x86 front end builds
    // intra-block control flow. BindLabel is the reset that carries the
    // correctness: a constant computed between a forward branch and its
    // label has NOT executed on the path that took the branch, so rewriting a
    // use after the label to that definition reads a register the block never
    // wrote. (The resets at the branch itself are redundant with it -- code
    // before a forward branch dominates everything after -- and are kept only
    // to scope facts to one straight-line region; see the comment in the pass.)
    //
    // The shape is real: DecodeShift (decoder_alu.cc) materializes LoadImm(0)
    // for SAR's OF inside exactly such a NotGoto/BindLabel region, a handful of
    // IR instructions before the label.
    Block conditional{0, Location{0x1000}};
    auto cond = conditional.LoadImm<BOOL>(Imm{1u});
    auto skip = conditional.NotGoto(cond);
    auto guarded = conditional.LoadImm(Imm{0x1234u});
    conditional.StoreUniform(Uniform{0, ValueType::U32}, guarded);
    conditional.BindLabel(skip);
    auto merged = conditional.LoadImm(Imm{0x1234u});
    conditional.StoreUniform(Uniform{8, ValueType::U32}, merged);
    conditional.SetTerminal(terminal::ReturnToDispatch{});

    ConstFoldingPass::Run(&conditional, FeatureSet{});

    auto arg_of_store = [](Block& block, std::uint32_t offset) -> Inst* {
        Inst* found = nullptr;
        for (auto& inst : block.GetInstList()) {
            if (inst.GetOp() == OpCode::StoreUniform &&
                inst.GetArg<Uniform>(0).GetOffset() == offset) {
                found = inst.GetArg<Value>(1).Def();
            }
        }
        return found;
    };

    // The store after the label must still read the constant computed
    // after the label. Removing the BindLabel reset rewrites it to `guarded`.
    REQUIRE(arg_of_store(conditional, 8) == merged.Def());
    REQUIRE(arg_of_store(conditional, 8) != guarded.Def());

    // Positive control. Without it the case above would also pass if the pass
    // did no deduplication at all -- which is exactly how a barrier ends up
    // "covered" by a test that proves nothing. Same two constants, same
    // distance, no branch between them: here the second one MUST be folded away.
    Block straight_line{1, Location{0x2000}};
    auto first = straight_line.LoadImm(Imm{0x1234u});
    straight_line.StoreUniform(Uniform{0, ValueType::U32}, first);
    auto second = straight_line.LoadImm(Imm{0x1234u});
    straight_line.StoreUniform(Uniform{8, ValueType::U32}, second);
    straight_line.SetTerminal(terminal::ReturnToDispatch{});

    ConstFoldingPass::Run(&straight_line, FeatureSet{});

    REQUIRE(arg_of_store(straight_line, 8) == first.Def());
    REQUIRE(arg_of_store(straight_line, 8) != second.Def());
}

TEST_CASE("integer immediates fold only into proven single-use ALU consumers") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    auto check_right = [](OpCode op, Imm immediate, ValueType type, Flags flags,
                          bool expected) {
        Block block{0, Location{0x2280}};
        const auto left = block.LoadUniform(Uniform{0, type});
        const auto constant = block.LoadImm(immediate).SetType(type);
        Value result;
        switch (op) {
            case OpCode::Add:
                result = block.Add(left, Operand{constant}).SetType(type);
                break;
            case OpCode::Sub:
                result = block.Sub(left, Operand{constant}).SetType(type);
                break;
            case OpCode::And:
                result = block.And(left, Operand{constant}).SetType(type);
                break;
            case OpCode::Or:
                result = block.Or(left, Operand{constant}).SetType(type);
                break;
            case OpCode::Xor:
                result = block.Xor(left, Operand{constant}).SetType(type);
                break;
            default:
                FAIL("not an integer-immediate whitelist opcode");
        }
        if (flags != Flags::None) block.SaveFlags(result, flags);
        FeatureSet features{};
        features.int_imm_fold = true;
        ConstFoldingPass::Run(&block, features);
        INFO("opcode " << static_cast<unsigned>(op) << " type "
                       << static_cast<unsigned>(type) << " flags "
                       << static_cast<swift::u64>(flags));
        REQUIRE(result.Def()->GetArg<Operand>(1).IsImm() == expected);
        REQUIRE(constant.Def()->GetUses(false) == (expected ? 0 : 1));
    };

    // Every whitelist family, including the Sub+SaveFlags shape used for Cmp.
    check_right(OpCode::Add, Imm{1u}, ValueType::U64, Flags::None, true);
    check_right(OpCode::Sub, Imm{4096u}, ValueType::U64, Flags::All, true);
    check_right(OpCode::And, Imm{0x7fu}, ValueType::U32, Flags::All, true);
    check_right(OpCode::Or, Imm{0xff00ff00u}, ValueType::U32, Flags::None, true);
    check_right(OpCode::Xor, Imm{0x80000000u}, ValueType::U32,
                Flags::Parity, true);

    // Encoder and narrow-NZCV fail closed. Narrow PF/AF-only is direct and
    // keeps bit 4 semantics without taking the sign-alignment path.
    check_right(OpCode::Add, Imm{0x12345u}, ValueType::U64,
                Flags::None, false);
    check_right(OpCode::And, Imm{0x01234567u}, ValueType::U32,
                Flags::None, false);
    check_right(OpCode::Sub, Imm{7u}, ValueType::U16, Flags::All, false);
    check_right(OpCode::Sub, Imm{7u}, ValueType::U16,
                Flags::Parity | Flags::AuxiliaryCarry, true);

    SECTION("gate OFF is byte-for-byte IR inert") {
        Block block{1, Location{0x2290}};
        const auto left = block.LoadUniform(Uniform{0, ValueType::U64});
        const auto constant = block.LoadImm(Imm{1u}).SetType(ValueType::U64);
        const auto result = block.Add(left, Operand{constant}).SetType(ValueType::U64);
        FeatureSet features{};
        features.int_imm_fold = false;
        ConstFoldingPass::Run(&block, features);
        REQUIRE_FALSE(result.Def()->GetArg<Operand>(1).IsImm());
        REQUIRE(result.Def()->GetArg<Operand>(1).GetLeft().value.Def() == constant.Def());
    }

    SECTION("multi-use and observable constants are rejected") {
        Block block{2, Location{0x22a0}};
        const auto left = block.LoadUniform(Uniform{0, ValueType::U64});
        const auto constant = block.LoadImm(Imm{1u}).SetType(ValueType::U64);
        const auto add = block.Add(left, Operand{constant}).SetType(ValueType::U64);
        const auto logical = block.And(left, Operand{constant}).SetType(ValueType::U64);
        block.StoreUniform(Uniform{8, ValueType::U64}, constant);
        FeatureSet features{};
        features.int_imm_fold = true;
        ConstFoldingPass::Run(&block, features);
        REQUIRE_FALSE(add.Def()->GetArg<Operand>(1).IsImm());
        REQUIRE_FALSE(logical.Def()->GetArg<Operand>(1).IsImm());
        REQUIRE(constant.Def()->GetUses(false) == 3);
    }

    SECTION("zero-left Sub becomes a unary Neg and keeps its pseudo flags") {
        Block block{3, Location{0x22b0}};
        const auto zero = block.LoadImm(Imm{0u}).SetType(ValueType::U16);
        const auto source = block.LoadUniform(Uniform{0, ValueType::U16});
        const auto result = block.Sub(zero, Operand{source}).SetType(ValueType::U16);
        block.SaveFlags(result, Flags::All);
        FeatureSet features{};
        features.int_imm_fold = true;
        ConstFoldingPass::Run(&block, features);
        REQUIRE(result.Def()->GetOp() == OpCode::Neg);
        REQUIRE(result.Def()->GetArg<Value>(0).Def() == source.Def());
        REQUIRE(result.Def()->GetPseudoOperations(OpCode::SaveFlags).size() == 1);
        REQUIRE(zero.Def()->GetUses(false) == 0);
    }

    SECTION("folded flags and Neg pass the real allocator and emitter contracts") {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        ModuleConfig module_config{};
        module_config.feature_overrides.Set(FeatureId::int_imm_fold, true);
        auto module = address_space.MapModule(0x22c0, 0x23c0, module_config);
        auto* block = new Block(0, Location{0x22c0});
        const auto source = block->LoadUniform(Uniform{0, ValueType::U16});
        const auto zero = block->LoadImm(Imm{0u}).SetType(ValueType::U16);
        const auto neg = block->Sub(zero, Operand{source}).SetType(ValueType::U16);
        block->SaveFlags(neg, Flags::All);
        block->StoreUniform(Uniform{8, ValueType::U16}, neg);
        block->SetTerminal(terminal::ReturnToDispatch{});

        auto features = ResolveFeatureSet(module_config);
        ConstFoldingPass::Run(block, features);
        DeadCodeEliminationPass::Run(block);
        block->ReIdInstr();
        RegAlloc alloc{block->MaxInstrId(),
                       address_space.GetTrampolines().GetGPRRegs(),
                       address_space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(block, &alloc, false, features);
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block);
        context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
    }
}

TEST_CASE("operand copy kill is a fail-closed Mul emitter fast path") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    enum class Observer { None, ResultSave, ResultBranch, RightSave };
    enum class RightShape { Value, Immediate };
    struct Emitted {
        swift::u32 bytes{};
        swift::u32 moves{};
        swift::u32 multiplies{};
        std::string text{};
    };

    auto run = [](bool enabled, OpCode op, ValueType type, Observer observer,
                  RightShape right_shape) {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        ModuleConfig module_config{};
        module_config.feature_overrides.Set(FeatureId::operand_copy_kill, enabled);
        auto module = address_space.MapModule(0x22d0, 0x23d0, module_config);
        IntrusivePtr<Block> block{new Block(0, Location{0x22d0})};
        const auto left = block->LoadUniform(Uniform{0, type});
        const auto right_value = block->LoadUniform(Uniform{8, type});
        if (observer == Observer::RightSave) {
            block->SaveFlags(right_value, Flags::Parity);
        }
        const auto right = right_shape == RightShape::Value
                ? Operand{right_value}
                : Operand{Imm{3u}};
        Value result;
        switch (op) {
            case OpCode::Mul:
                result = block->Mul(left, right).SetType(type);
                break;
            case OpCode::Sub:
                result = block->Sub(left, right).SetType(type);
                break;
            case OpCode::Or:
                result = block->Or(left, right).SetType(type);
                break;
            case OpCode::Xor:
                result = block->Xor(left, right).SetType(type);
                break;
            default:
                FAIL("not an operand-copy-kill matrix opcode");
        }
        if (observer == Observer::ResultSave) {
            block->SaveFlags(result, Flags::All);
        } else if (observer == Observer::ResultBranch) {
            block->AppendInst(OpCode::BranchOnlyFlags, result, Flags::NZCV);
        }
        block->StoreUniform(Uniform{16, type}, result);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        const auto features = ResolveFeatureSet(module_config);
        RegAlloc alloc{block->MaxInstrId(),
                       address_space.GetTrampolines().GetGPRRegs(),
                       address_space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(block.get(), &alloc, false, features);
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();

        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        std::string text;
        swift::u32 moves{};
        swift::u32 multiplies{};
        auto& masm = context.GetMasm();
        auto* first = masm.GetBuffer()->GetStartAddress<
                const vixl::aarch64::Instruction*>();
        auto* last = masm.GetBuffer()->GetEndAddress<
                const vixl::aarch64::Instruction*>();
        for (auto* instruction = first; instruction < last; ++instruction) {
            decoder.Decode(instruction);
            std::string_view line{disassembler.GetOutput()};
            const auto begin = line.find_first_not_of(" \t");
            if (begin != std::string_view::npos) line.remove_prefix(begin);
            if (line.starts_with("mov ")) ++moves;
            if (line.starts_with("mul ")) ++multiplies;
            text.append(line);
            text.push_back('\n');
        }
        return Emitted{context.CurrentBufferSize(), moves, multiplies,
                       std::move(text)};
    };

    SECTION("gate OFF is completely inert and gate ON removes one basic Mul copy") {
        const auto off = run(false, OpCode::Mul, ValueType::U32,
                             Observer::None, RightShape::Value);
        const auto off_again = run(false, OpCode::Mul, ValueType::U32,
                                   Observer::None, RightShape::Value);
        const auto on = run(true, OpCode::Mul, ValueType::U32,
                            Observer::None, RightShape::Value);
        REQUIRE(off.bytes == off_again.bytes);
        REQUIRE(off.text == off_again.text);
        REQUIRE(on.bytes + vixl::aarch64::kInstructionSize == off.bytes);
        REQUIRE(on.moves + 1 == off.moves);
        REQUIRE(on.multiplies == off.multiplies);
        REQUIRE(on.multiplies == 1);
    }

    SECTION("whitelist opcode x observer x width matrix fails closed") {
        constexpr std::array ops{OpCode::Mul, OpCode::Sub, OpCode::Or,
                                 OpCode::Xor};
        constexpr std::array observers{Observer::None, Observer::ResultSave,
                                       Observer::ResultBranch,
                                       Observer::RightSave};
        constexpr std::array widths{ValueType::U16, ValueType::U32,
                                    ValueType::U64};
        for (const auto op : ops) {
            for (const auto observer : observers) {
                for (const auto type : widths) {
                    const auto off = run(false, op, type, observer,
                                         RightShape::Value);
                    const auto on = run(true, op, type, observer,
                                        RightShape::Value);
                    const bool eligible = op == OpCode::Mul &&
                                          observer == Observer::None &&
                                          type != ValueType::U16;
                    INFO("op=" << static_cast<unsigned>(op)
                               << " observer=" << static_cast<unsigned>(observer)
                               << " type=" << static_cast<unsigned>(type));
                    REQUIRE(on.bytes +
                                    (eligible ? vixl::aarch64::kInstructionSize : 0) ==
                            off.bytes);
                    REQUIRE(on.moves + (eligible ? 1u : 0u) == off.moves);
                }
            }
        }
    }

    SECTION("immediate and induction-compatible operand shapes stay on the old path") {
        for (const auto type : {ValueType::U32, ValueType::U64}) {
            const auto off = run(false, OpCode::Mul, type, Observer::None,
                                 RightShape::Immediate);
            const auto on = run(true, OpCode::Mul, type, Observer::None,
                                RightShape::Immediate);
            REQUIRE(on.bytes == off.bytes);
            REQUIRE(on.text == off.text);
        }
    }
}

TEST_CASE("zero store zr is value-only and fail-closed") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    enum class StoreKind { Uniform, Memory, FixedFPR };
    enum class Cardinality { Single, CompatibleStores, IncompatibleUse };
    enum class Observer { None, Save, Branch };
    struct Emitted {
        swift::u32 bytes{};
        swift::u32 moves{};
        swift::u32 zr_stores{};
        std::string text{};
    };

    auto run = [](bool enabled, StoreKind kind, ValueType type, bool zero,
                  Cardinality cardinality, Observer observer,
                  bool force_spill = false) {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        ModuleConfig module_config{};
        module_config.feature_overrides.Set(FeatureId::zero_store_zr, enabled);
        auto module = address_space.MapModule(0x23d0, 0x24d0, module_config);
        IntrusivePtr<Block> block{new Block(0, Location{0x23d0})};
        const auto stored = block->LoadImm(Imm{zero ? swift::u64{0}
                                                    : swift::u64{0x5a}})
                                    .SetType(type);
        if (observer == Observer::Save) {
            block->SaveFlags(stored, Flags::Parity);
        } else if (observer == Observer::Branch) {
            block->AppendInst(OpCode::BranchOnlyFlags, stored, Flags::NZCV);
        }
        if (cardinality == Cardinality::IncompatibleUse) {
            const auto other = block->Add(stored, Operand{Imm{1u}}).SetType(type);
            block->StoreUniform(Uniform{24, type}, other);
        }
        if (kind == StoreKind::Uniform) {
            block->StoreUniform(Uniform{8, type}, stored);
            if (cardinality == Cardinality::CompatibleStores) {
                block->StoreUniform(Uniform{24, type}, stored);
            }
        } else if (kind == StoreKind::Memory) {
            const auto address = block->LoadImm(Imm{swift::u64{0x100}})
                                         .SetType(ValueType::U64);
            block->StoreMemory(Operand{address}, stored);
            if (cardinality == Cardinality::CompatibleStores) {
                block->StoreMemory(Operand{address}, stored);
            }
        } else {
            const auto size = GetValueSizeByte(type);
            block->AppendInst(OpCode::SetHostFPR, stored, HostRegIndex(24),
                              Imm{static_cast<swift::u64>(
                                      sizeof(swift::u128) - size)});
            if (cardinality == Cardinality::CompatibleStores) {
                block->AppendInst(
                        OpCode::SetHostFPR, stored, HostRegIndex(24),
                        Imm{static_cast<swift::u64>(
                                sizeof(swift::u128) - 2 * size)});
            }
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        const auto features = ResolveFeatureSet(module_config);
        RegAlloc alloc{block->MaxInstrId(),
                       address_space.GetTrampolines().GetGPRRegs(),
                       address_space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(block.get(), &alloc, false, features);
        if (force_spill) {
            alloc.MapMemSpill(stored.Id(), SpillSlot{0});
        }
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();

        const std::string_view zr_store = [&]() -> std::string_view {
            switch (type) {
                case ValueType::U8: return "strb wzr";
                case ValueType::U16: return "strh wzr";
                case ValueType::U32: return "str wzr";
                case ValueType::U64: return "str xzr";
                default: FAIL("unexpected zero-store matrix width");
            }
        }();
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        Emitted emitted{.bytes = context.CurrentBufferSize()};
        auto& masm = context.GetMasm();
        auto* first = masm.GetBuffer()->GetStartAddress<
                const vixl::aarch64::Instruction*>();
        auto* last = masm.GetBuffer()->GetEndAddress<
                const vixl::aarch64::Instruction*>();
        for (auto* instruction = first; instruction < last; ++instruction) {
            decoder.Decode(instruction);
            std::string_view line{disassembler.GetOutput()};
            const auto begin = line.find_first_not_of(" \t");
            if (begin != std::string_view::npos) line.remove_prefix(begin);
            emitted.moves += line.starts_with("mov ");
            emitted.zr_stores += kind == StoreKind::FixedFPR
                    ? line.starts_with("mov v") &&
                              (line.find(", wzr") != std::string_view::npos ||
                               line.find(", xzr") != std::string_view::npos)
                    : line.starts_with(zr_store);
            emitted.text.append(line);
            emitted.text.push_back('\n');
        }
        return emitted;
    };

    SECTION("StoreUniform StoreMemory SetHostFPR x width x zero x cardinality x gate") {
        constexpr std::array kinds{StoreKind::Uniform, StoreKind::Memory,
                                   StoreKind::FixedFPR};
        constexpr std::array widths{ValueType::U8, ValueType::U16,
                                    ValueType::U32, ValueType::U64};
        constexpr std::array cardinalities{Cardinality::Single,
                                           Cardinality::CompatibleStores,
                                           Cardinality::IncompatibleUse};
        for (const auto kind : kinds) {
            for (const auto type : widths) {
                for (const bool zero : {false, true}) {
                    for (const auto cardinality : cardinalities) {
                        const auto off = run(false, kind, type, zero,
                                             cardinality, Observer::None);
                        const auto off_again = run(false, kind, type, zero,
                                                   cardinality, Observer::None);
                        const auto on = run(true, kind, type, zero,
                                            cardinality, Observer::None);
                        INFO("kind=" << static_cast<unsigned>(kind)
                                     << " type=" << static_cast<unsigned>(type)
                                     << " zero=" << zero
                                     << " cardinality="
                                     << static_cast<unsigned>(cardinality));
                        REQUIRE(off.bytes == off_again.bytes);
                        REQUIRE(off.text == off_again.text);
                        if (!zero) {
                            REQUIRE(on.bytes == off.bytes);
                            REQUIRE(on.text == off.text);
                            REQUIRE(on.zr_stores == 0);
                        } else if (cardinality != Cardinality::IncompatibleUse) {
                            const swift::u32 expected_stores =
                                    cardinality == Cardinality::CompatibleStores
                                    ? 2
                                    : 1;
                            REQUIRE(on.bytes + vixl::aarch64::kInstructionSize ==
                                    off.bytes);
                            REQUIRE(on.moves + 1 == off.moves);
                            REQUIRE(off.zr_stores == 0);
                            REQUIRE(on.zr_stores == expected_stores);
                        } else {
                            REQUIRE(on.bytes == off.bytes);
                            REQUIRE(on.moves == off.moves);
                            REQUIRE(on.text == off.text);
                            REQUIRE(off.zr_stores == 0);
                            REQUIRE(on.zr_stores == 0);
                        }
                    }
                }
            }
        }
    }

    SECTION("pseudo observers keep the LoadImm materialization") {
        for (const auto kind : {StoreKind::Uniform, StoreKind::Memory,
                                StoreKind::FixedFPR}) {
            for (const auto type : {ValueType::U8, ValueType::U16,
                                    ValueType::U32, ValueType::U64}) {
                for (const auto observer : {Observer::Save, Observer::Branch}) {
                    const auto off = run(false, kind, type, true,
                                         Cardinality::Single, observer);
                    const auto on = run(true, kind, type, true,
                                        Cardinality::Single, observer);
                    INFO("kind=" << static_cast<unsigned>(kind)
                                 << " type=" << static_cast<unsigned>(type)
                                 << " observer=" << static_cast<unsigned>(observer));
                    REQUIRE(on.bytes == off.bytes);
                    REQUIRE(on.moves == off.moves);
                    REQUIRE(on.text == off.text);
                    REQUIRE(off.zr_stores == 0);
                    REQUIRE(on.zr_stores == 0);
                }
            }
        }
    }

    SECTION("spilled zero stays byte-identical on the old path") {
        for (const auto kind : {StoreKind::Uniform, StoreKind::Memory,
                                StoreKind::FixedFPR}) {
            const auto off = run(false, kind, ValueType::U64, true,
                                 Cardinality::Single, Observer::None, true);
            const auto on = run(true, kind, ValueType::U64, true,
                                Cardinality::Single, Observer::None, true);
            REQUIRE(on.bytes == off.bytes);
            REQUIRE(on.text == off.text);
            REQUIRE(on.zr_stores == 0);
        }
    }
}

TEST_CASE("MMX-form shared opcodes are refused instead of run on the XMM file") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    // MMX and SSE share opcode numbers: distorm reports the same insn.opcode
    // for `paddb mm0,mm1` (0F FC C1) and `paddb xmm0,xmm1` (66 0F FC C1), and
    // only the operand register class distinguishes them. x86_regs_table maps
    // R_MM0..R_MM7 onto X86RegInfo::Xmm0..Xmm7, so before the guard in
    // DecodeSwitch an MMX-form instruction did not fail -- it read and wrote
    // the guest's XMM0-XMM7 and produced silently wrong data. This runtime
    // implements no MMX register file and does not advertise MMX (CPUID leaf 1
    // EDX bit 23 is clear), so the only correct answer is to refuse the
    // instruction: InterruptReason::FALLBACK, which the runtime turns into
    // IllegalCode.
    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    constexpr auto kInterruptOffset = offsetof(ThreadContext64, interrupt);
    constexpr auto kXmmBase = offsetof(ThreadContext64, xmms);
    constexpr auto kXmmEnd = kXmmBase + sizeof(ThreadContext64::xmms);

    struct Outcome {
        bool refused;      // a FALLBACK interrupt was raised
        bool touched_xmm;  // the XMM register file was read or written
    };
    auto decode = [&](std::vector<swift::u8> bytes) {
        bytes.push_back(0xF4);  // hlt, so Decode() terminates
        const auto address = reinterpret_cast<VAddr>(bytes.data());
        Block block{0, Location{address}};
        Assembler assembler{&block};
        X64Decoder decoder{address, &memory, &assembler, true,
                           Arm64Features::None, false, false, FeatureSet{}};
        decoder.Decode();

        Outcome out{false, false};
        for (auto& inst : block.GetInstList()) {
            const bool is_store = inst.GetOp() == OpCode::StoreUniform;
            const bool is_load = inst.GetOp() == OpCode::LoadUniform;
            if (!is_store && !is_load) {
                continue;
            }
            const auto offset = inst.GetArg<Uniform>(0).GetOffset();
            if (offset >= kXmmBase && offset < kXmmEnd) {
                out.touched_xmm = true;
            }
            if (is_store && offset == kInterruptOffset) {
                auto reason = inst.GetArg<Value>(1);
                if (reason.Defined() && reason.Def()->GetOp() == OpCode::LoadImm &&
                    reason.Def()->GetArg<Imm>(0).Get() ==
                            static_cast<swift::u32>(InterruptReason::FALLBACK)) {
                    out.refused = true;
                }
            }
        }
        return out;
    };

    struct Case {
        const char* name;
        std::vector<swift::u8> mmx;  // no 66 prefix: MM operands
        std::vector<swift::u8> sse;  // 66-prefixed: XMM operands
    };
    // The seven named in the original report, plus a spread across the rest of
    // the shared space (the basic MMX/SSE2 integer set, the SSSE3 additions and
    // the GPR<->vector movers) -- the defect was never specific to seven
    // opcodes, it was a property of the dispatch.
    const std::vector<Case> cases = {
            {"pshufb", {0x0F, 0x38, 0x00, 0xC1}, {0x66, 0x0F, 0x38, 0x00, 0xC1}},
            {"palignr", {0x0F, 0x3A, 0x0F, 0xC1, 0x03}, {0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x03}},
            {"pextrw", {0x0F, 0xC5, 0xC1, 0x02}, {0x66, 0x0F, 0xC5, 0xC1, 0x02}},
            {"pmovmskb", {0x0F, 0xD7, 0xC1}, {0x66, 0x0F, 0xD7, 0xC1}},
            {"pmuludq", {0x0F, 0xF4, 0xC1}, {0x66, 0x0F, 0xF4, 0xC1}},
            {"psadbw", {0x0F, 0xF6, 0xC1}, {0x66, 0x0F, 0xF6, 0xC1}},
            {"pminub", {0x0F, 0xDA, 0xC1}, {0x66, 0x0F, 0xDA, 0xC1}},
            {"paddb", {0x0F, 0xFC, 0xC1}, {0x66, 0x0F, 0xFC, 0xC1}},
            {"psubb", {0x0F, 0xF8, 0xC1}, {0x66, 0x0F, 0xF8, 0xC1}},
            {"pand", {0x0F, 0xDB, 0xC1}, {0x66, 0x0F, 0xDB, 0xC1}},
            {"pxor", {0x0F, 0xEF, 0xC1}, {0x66, 0x0F, 0xEF, 0xC1}},
            {"pcmpeqb", {0x0F, 0x74, 0xC1}, {0x66, 0x0F, 0x74, 0xC1}},
            {"punpcklbw", {0x0F, 0x60, 0xC1}, {0x66, 0x0F, 0x60, 0xC1}},
            {"packsswb", {0x0F, 0x63, 0xC1}, {0x66, 0x0F, 0x63, 0xC1}},
            {"pmullw", {0x0F, 0xD5, 0xC1}, {0x66, 0x0F, 0xD5, 0xC1}},
            {"pmaddwd", {0x0F, 0xF5, 0xC1}, {0x66, 0x0F, 0xF5, 0xC1}},
            {"pavgb", {0x0F, 0xE0, 0xC1}, {0x66, 0x0F, 0xE0, 0xC1}},
            {"pmaxsw", {0x0F, 0xEE, 0xC1}, {0x66, 0x0F, 0xEE, 0xC1}},
            {"paddusb", {0x0F, 0xDC, 0xC1}, {0x66, 0x0F, 0xDC, 0xC1}},
            {"psllw imm", {0x0F, 0x71, 0xF0, 0x03}, {0x66, 0x0F, 0x71, 0xF0, 0x03}},
            {"psllw reg", {0x0F, 0xF1, 0xC1}, {0x66, 0x0F, 0xF1, 0xC1}},
            {"pinsrw", {0x0F, 0xC4, 0xC0, 0x02}, {0x66, 0x0F, 0xC4, 0xC0, 0x02}},
            {"movd", {0x0F, 0x6E, 0xC0}, {0x66, 0x0F, 0x6E, 0xC0}},
            {"pabsb", {0x0F, 0x38, 0x1C, 0xC1}, {0x66, 0x0F, 0x38, 0x1C, 0xC1}},
            {"pmulhrsw", {0x0F, 0x38, 0x0B, 0xC1}, {0x66, 0x0F, 0x38, 0x0B, 0xC1}},
            // paddq is the one shared opcode distorm reports with XMM operand
            // indices in BOTH forms, so the register-class test cannot see it
            // and DecodeSwitch reads the 0x66 prefix instead. It is here as a
            // register form and as a memory form, because the prefix scan walks
            // the encoding and the memory form has a different byte layout.
            {"paddq", {0x0F, 0xD4, 0xC1}, {0x66, 0x0F, 0xD4, 0xC1}},
            {"paddq mem", {0x0F, 0xD4, 0x01}, {0x66, 0x0F, 0xD4, 0x01}},
            {"paddq rex", {0x0F, 0xD4, 0xC1}, {0x66, 0x44, 0x0F, 0xD4, 0xC1}},
            // paddq mm0,[rcx+0x66]: the displacement byte IS 0x66. A prefix
            // scan that does not stop at the 0x0F escape reads it as an
            // operand-size prefix and lets this MMX instruction through.
            {"paddq disp=0x66",
             {0x0F, 0xD4, 0x41, 0x66},
             {0x66, 0x0F, 0xD4, 0x41, 0x66}},
    };

    for (auto& c : cases) {
        INFO("MMX form of " << c.name);
        auto mmx = decode(c.mmx);
        // Refusing is the point; not touching the XMM file is what refusing
        // BUYS, and is the assertion that fails if the guard is removed.
        REQUIRE_FALSE(mmx.touched_xmm);
        REQUIRE(mmx.refused);
    }
    for (auto& c : cases) {
        // The 66-prefixed twin must be unaffected: a guard that rejected both
        // forms would pass every assertion above while breaking SSE2.
        INFO("SSE form of " << c.name);
        auto sse = decode(c.sse);
        REQUIRE_FALSE(sse.refused);
        REQUIRE(sse.touched_xmm);
    }
}

TEST_CASE("GPR immediate shifts specialize only nonzero in-range counts") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    struct ShiftFastScope {
        bool had_old{};
        std::string old;
        ShiftFastScope() {
            if (const char* value = swift::runtime::GetRawSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST")) {
                had_old = true;
                old = value;
            }
            swift::runtime::SetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST", "1", 1);
        }
        ~ShiftFastScope() {
            if (had_old) swift::runtime::SetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST", old.c_str(), 1);
            else swift::runtime::UnsetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST");
        }
    } shift_fast_scope;

    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    struct Kind {
        swift::u8 group;
        OpCode imm;
        OpCode value;
    };
    const std::array kinds{
            Kind{4, OpCode::LslImm, OpCode::LslValue},
            Kind{5, OpCode::LsrImm, OpCode::LsrValue},
            Kind{7, OpCode::AsrImm, OpCode::AsrValue},
    };
    const std::array widths{8u, 16u, 32u, 64u};

    auto decode = [&](swift::u32 width, swift::u8 group, swift::u8 count, bool by_cl) {
        std::vector<swift::u8> code;
        if (width == 16) code.push_back(0x66);
        if (width == 64) code.push_back(0x48);
        code.push_back(by_cl ? (width == 8 ? 0xD2 : 0xD3)
                             : (width == 8 ? 0xC0 : 0xC1));
        code.push_back(
                static_cast<swift::u8>(0xC0 | (group << 3)));  // r/m = AL/AX/EAX/RAX
        if (!by_cl) code.push_back(count);
        code.push_back(0xF4);  // hlt terminates Decode

        const auto address = reinterpret_cast<VAddr>(code.data());
        auto block = std::make_unique<Block>(0, Location{address});
        Assembler assembler{block.get()};
        X64Decoder decoder{address, &memory, &assembler, true,
                           Arm64Features::None, false, false, FeatureSet{}};
        decoder.Decode();
        return block;
    };

    for (auto kind : kinds) {
        for (swift::u32 width : widths) {
            const std::array counts{0u, 1u, width - 1, width, width + 1};
            for (swift::u32 raw_count : counts) {
                INFO("group=" << unsigned(kind.group) << " width=" << width
                              << " count=" << raw_count);
                auto block =
                        decode(width, kind.group, static_cast<swift::u8>(raw_count), false);
                bool has_imm = false;
                bool has_value = false;
                bool has_guard = false;
                bool has_flag_write = false;
                for (auto& inst : block->GetInstList()) {
                    has_imm |= inst.GetOp() == kind.imm;
                    has_value |= inst.GetOp() == kind.value;
                    has_guard |= inst.GetOp() == OpCode::NotGoto;
                    has_flag_write |= inst.GetOp() == OpCode::SaveFlags ||
                                      inst.GetOp() == OpCode::SetCarry ||
                                      inst.GetOp() == OpCode::SetOverflow;
                }

                const swift::u32 effective = raw_count & (width == 64 ? 0x3Fu : 0x1Fu);
                if (effective == 0) {
                    REQUIRE_FALSE(has_imm);
                    REQUIRE_FALSE(has_value);
                    REQUIRE_FALSE(has_guard);
                    REQUIRE_FALSE(has_flag_write);
                } else if (effective < width) {
                    REQUIRE(has_imm);
                    REQUIRE_FALSE(has_value);
                    REQUIRE_FALSE(has_guard);
                    REQUIRE(has_flag_write);
                } else {
                    // 8/16-bit counts at or above the operand width retain the
                    // generic 32-bit-container path and its zero-count guard.
                    REQUIRE(has_value);
                    REQUIRE(has_guard);
                    REQUIRE(has_flag_write);
                }
            }

            // CL is runtime data even if the test initializes it later; it must
            // retain x86's 5/6-bit mask and zero-count flag guard.
            auto cl_block = decode(width, kind.group, 0, true);
            bool has_value = false;
            bool has_guard = false;
            for (auto& inst : cl_block->GetInstList()) {
                has_value |= inst.GetOp() == kind.value;
                has_guard |= inst.GetOp() == OpCode::NotGoto;
            }
            REQUIRE(has_value);
            REQUIRE(has_guard);
        }
    }
}

TEST_CASE("GPR immediate shift boundary results and defined flags execute correctly") {
    using namespace swift::x86;

    struct ShiftFastScope {
        bool had_old{};
        std::string old;
        ShiftFastScope() {
            if (const char* value = swift::runtime::GetRawSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST")) {
                had_old = true;
                old = value;
            }
            swift::runtime::SetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST", "1", 1);
        }
        ~ShiftFastScope() {
            if (had_old) swift::runtime::SetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST", old.c_str(), 1);
            else swift::runtime::UnsetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST");
        }
    } shift_fast_scope;

    constexpr swift::u64 input = UINT64_C(0x8123456789ab8181);
    struct Kind {
        swift::u8 group;
    };
    const std::array kinds{Kind{4}, Kind{5}, Kind{7}};
    const std::array widths{8u, 16u, 32u, 64u};
    constexpr size_t stride = 32;
    constexpr size_t case_count = 3 * 4 * 5;

    void* guest_code = mmap(nullptr, stride * case_count, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    auto* code_base = static_cast<swift::u8*>(guest_code);
    size_t case_index = 0;
    for (auto kind : kinds) {
        for (swift::u32 width : widths) {
            const std::array counts{0u, 1u, width - 1, width, width + 1};
            for (swift::u32 raw_count : counts) {
                auto* p = code_base + case_index++ * stride;
                // cmp rbx,rbx; stc gives a known pre-shift state:
                // CF=PF=ZF=1, SF=OF=0. AF is intentionally not checked.
                *p++ = 0x48;
                *p++ = 0x39;
                *p++ = 0xDB;
                *p++ = 0xF9;
                if (width == 16) *p++ = 0x66;
                if (width == 64) *p++ = 0x48;
                *p++ = width == 8 ? 0xC0 : 0xC1;
                *p++ = static_cast<swift::u8>(0xC0 | (kind.group << 3));
                *p++ = static_cast<swift::u8>(raw_count);
                // Preserve the shifted value before LAHF overwrites AH, then
                // capture OF separately in DL.
                *p++ = 0x49;
                *p++ = 0x89;
                *p++ = 0xC0;  // mov r8,rax
                *p++ = 0x9F;  // lahf
                *p++ = 0x0F;
                *p++ = 0x90;
                *p++ = 0xC2;  // seto dl
                *p++ = 0xF4;  // hlt
            }
        }
    }
    REQUIRE(case_index == case_count);

    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();
    auto* core = swift::translator::x86::X86Core::Make(instance);
    auto& context = core->GetContext();
    case_index = 0;
    for (auto kind : kinds) {
        for (swift::u32 width : widths) {
            const std::array counts{0u, 1u, width - 1, width, width + 1};
            const swift::u64 width_mask =
                    width == 64 ? UINT64_MAX : ((UINT64_C(1) << width) - 1);
            const swift::u64 original = input & width_mask;
            const bool original_sign = ((original >> (width - 1)) & 1) != 0;
            for (swift::u32 raw_count : counts) {
                INFO("group=" << unsigned(kind.group) << " width=" << width
                              << " count=" << raw_count);
                const swift::u32 count = raw_count & (width == 64 ? 0x3Fu : 0x1Fu);
                swift::u64 lane = original;
                if (count != 0) {
                    if (kind.group == 4) {
                        lane = count >= width ? 0 : (original << count) & width_mask;
                    } else if (kind.group == 5) {
                        lane = count >= width ? 0 : original >> count;
                    } else if (count >= width) {
                        lane = original_sign ? width_mask : 0;
                    } else {
                        lane = original >> count;
                        if (original_sign) {
                            const auto retained = (UINT64_C(1) << (width - count)) - 1;
                            lane |= width_mask & ~retained;
                        }
                    }
                }
                const swift::u64 expected_result =
                        width == 64 ? lane
                                    : (width == 32 ? lane
                                                   : ((input & ~width_mask) | lane));

                context.rip.qword =
                        reinterpret_cast<swift::u64>(code_base + case_index++ * stride);
                context.rax.qword = input;
                context.rbx.qword = UINT64_C(0x1122334455667788);
                context.rdx.qword = 0;
                REQUIRE(core->Run() == swift::translator::ExitReason::None);
                REQUIRE(context.r8.qword == expected_result);

                const swift::u8 captured = context.rax.low.low.high;
                swift::u8 flag_mask = 0xC4;  // SF/ZF/PF
                swift::u8 expected = 0;
                if (count == 0) {
                    flag_mask |= 0x01;
                    expected = 0x45;  // preserved CF/PF/ZF
                } else {
                    expected |= lane == 0 ? 0x40 : 0;
                    expected |= ((lane >> (width - 1)) & 1) ? 0x80 : 0;
                    expected |= (std::popcount(static_cast<swift::u8>(lane)) & 1) == 0
                                        ? 0x04
                                        : 0;
                    if (count < width) {
                        flag_mask |= 0x01;
                        const swift::u64 cf =
                                kind.group == 4 ? (original >> (width - count)) & 1
                                                : (original >> (count - 1)) & 1;
                        expected |= static_cast<swift::u8>(cf);
                    }
                }
                REQUIRE((captured & flag_mask) == (expected & flag_mask));

                if (count == 0 || count == 1) {
                    const swift::u8 expected_of =
                            count == 0
                                    ? 0
                                    : (kind.group == 4
                                               ? static_cast<swift::u8>(
                                                         ((lane >> (width - 1)) & 1) ^
                                                         ((original >> (width - 1)) & 1))
                                               : (kind.group == 5 ? original_sign : false));
                    REQUIRE(context.rdx.low.low.low == expected_of);
                }
            }
        }
    }

    swift::translator::x86::X86Core::Destroy(core);
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(guest_code, stride * case_count);
}

TEST_CASE("Uniform elimination uses the latest full GPR store for a narrow load") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    // sub rbx,rdx; popcnt r10w,bx; lahf; seto r15b; hlt
    std::array<swift::u8, 15> code{
            0x48, 0x29, 0xd3, 0xf3, 0x66, 0x44, 0x0f, 0xb8,
            0xd3, 0x9f, 0x41, 0x0f, 0x90, 0xc7, 0xf4,
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
    Block block{0, Location{address}};
    Assembler assembler{&block};
    X64Decoder decoder{address, &memory, &assembler, true,
                       Arm64Features::None, false, false, FeatureSet{}};
    decoder.Decode();

    const auto rbx_offset = offsetof(ThreadContext64, rbx);
    Value latest_rbx_store{};
    Inst* narrow_rbx_load = nullptr;
    for (auto& inst : block.GetInstList()) {
        if (inst.GetOp() == OpCode::StoreUniform &&
            inst.GetArg<Uniform>(0).GetOffset() == rbx_offset) {
            latest_rbx_store = inst.GetArg<Value>(1);
        } else if (inst.GetOp() == OpCode::LoadUniform) {
            auto uniform = inst.GetArg<Uniform>(0);
            if (uniform.GetOffset() == rbx_offset &&
                uniform.GetType() == ValueType::U16) {
                narrow_rbx_load = &inst;
            }
        }
    }
    REQUIRE(latest_rbx_store.Defined());
    REQUIRE(narrow_rbx_load != nullptr);

    UniformInfo info{.uniform_size = sizeof(ThreadContext64)};
    UniformEliminationPass::Run(&block, info, FeatureSet{});

    REQUIRE(narrow_rbx_load->GetOp() == OpCode::BitExtract);
    REQUIRE(narrow_rbx_load->GetArg<Value>(0) == latest_rbx_store);

    // Execute the same sequence without Unicorn. The old bug passed the full
    // post-sub RBX value to Popcnt64 after the narrow load was folded.
    void* guest_code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    std::memcpy(guest_code, code.data(), code.size());
    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();
    auto* core = swift::translator::x86::X86Core::Make(instance);
    auto& context = core->GetContext();
    context.rip.qword = reinterpret_cast<VAddr>(guest_code);
    context.rbx.qword = UINT64_MAX;
    // RBX - RDX = 0xffffffffffff0003. Its full-width popcount is 50,
    // while popcnt BX must see only 0x0003 and return 2.
    context.rdx.qword = 0xFFFC;
    context.r10.qword = 0xFFFF0000;
    core->Run();
    const auto popcount = context.r10.qword & 0xFFFF;
    swift::translator::x86::X86Core::Destroy(core);
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(guest_code, 4096);
    REQUIRE(popcount == 2);
}

TEST_CASE("JIT BitExtract feeds an exact U16 value to CallLambda") {
    using namespace swift::runtime;
    using namespace swift::x86;

    // add cx,r8w; popcnt bx,cx; lahf; seto r15b; hlt
    std::array<swift::u8, 15> code{
            0x66, 0x44, 0x01, 0xc1, 0xf3, 0x66, 0x0f, 0xb8,
            0xd9, 0x9f, 0x41, 0x0f, 0x90, 0xc7, 0xf4,
    };
    void* guest_code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    std::memcpy(guest_code, code.data(), code.size());
    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();

    bool matched = true;
    swift::u64 failed_rcx{};
    swift::u64 failed_r8{};
    swift::u16 expected{};
    swift::u16 actual{};
    swift::u64 random = 0x9E3779B97F4A7C15ull;
    for (int iteration = 0; iteration < 1024; ++iteration) {
        random ^= random << 7;
        random ^= random >> 9;
        auto rcx = random;
        random ^= random << 8;
        auto r8 = random;

        auto* core = swift::translator::x86::X86Core::Make(instance);
        auto& context = core->GetContext();
        context.rip.qword = reinterpret_cast<VAddr>(guest_code);
        context.rcx.qword = rcx;
        context.r8.qword = r8;
        context.rbx.qword = 0xA4297CCD266D0000ull;
        core->Run();
        expected = static_cast<swift::u16>(
                std::popcount(static_cast<swift::u16>(rcx + r8)));
        actual = static_cast<swift::u16>(context.rbx.qword);
        swift::translator::x86::X86Core::Destroy(core);
        if (actual != expected) {
            matched = false;
            failed_rcx = rcx;
            failed_r8 = r8;
            break;
        }
    }
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(guest_code, 4096);

    INFO(fmt::format("rcx={:#x}, r8={:#x}, expected={}, actual={}",
                     failed_rcx, failed_r8, expected, actual));
    REQUIRE(matched);
}

TEST_CASE("Test x86 translator") {
    using namespace swift::x86;
    using namespace swift::translator::x86;
    auto instance = X86Instance::Make();
    auto core1 = X86Core::Make(instance);

    core1->Run();
}

TEST_CASE("IR meta consistency") {
    using namespace swift::runtime::ir;

    // (1) The constexpr projection (ir.inc via meta::kPhys) must agree with the
    // runtime table summed through the single slot rule, PhysicalSlots.
    constexpr int kPhysByOp[] = {
            0,  // OpCode::Void
#define INST(name, ret, ...) meta::kPhys_##name,
#include "runtime/ir/ir.inc"
#undef INST
    };
    for (int op = 1; op < (int)OpCode::BASE_COUNT; op++) {
        auto& info = GetIRMetaInfo((OpCode)op);
        int runtime_slots = 0;
        for (auto t : info.arg_types) {
            runtime_slots += PhysicalSlots(t);
        }
        REQUIRE(runtime_slots == kPhysByOp[op]);
        REQUIRE(runtime_slots <= Inst::max_args);
    }

    // (2) StoreMemory(Operand, Value): GetArg<Value>(1) must resolve to the right
    // physical slot without the old PublicIndex out-of-bounds read.
    Block block{0, Location{0x1000}};
    auto base = block.LoadImm(Imm{0x1000u});
    auto idx = block.LoadImm(Imm{0x8u});
    auto stored = block.LoadImm(Imm{42u});
    Operand operand{base, idx};
    auto* store = block.AppendInst(OpCode::StoreMemory, operand, stored);
    REQUIRE(store->GetArg<Value>(1) == stored);

    // (3) Operand round-trip: GetArg<Operand>(0) rebuilds op/left/right.
    Operand got = store->GetArg<Operand>(0);
    REQUIRE(got.GetOp() == operand.GetOp());
    REQUIRE(got.GetLeft().IsValue());
    REQUIRE(got.GetLeft().value == base);
    REQUIRE(got.GetRight().IsValue());
    REQUIRE(got.GetRight().value == idx);
}

TEST_CASE("Flag elimination keeps live pseudo masks and unlinks dead pseudos") {
    using namespace swift::runtime::ir;

    // Dead-pseudo removal, exercised on a carry-free mask. A SaveFlags whose
    // bits are all rewritten before any read is dropped, and the surviving
    // write keeps its original (un-narrowed) mask.
    Block overwritten{0, Location{0x1000}};
    auto lhs = overwritten.LoadImm(Imm{1u});
    auto rhs = overwritten.LoadImm(Imm{2u});
    auto old_result = overwritten.Add(lhs, Operand{rhs});
    overwritten.AppendInst(OpCode::SaveFlags, old_result, Flags::NZ);
    auto new_result = overwritten.Add(lhs, Operand{rhs});
    auto* new_save = overwritten.AppendInst(OpCode::SaveFlags, new_result, Flags::NZ);

    FlagsEliminationPass::Run(&overwritten, nullptr, FeatureSet{});

    REQUIRE(old_result.Def()->GetPseudoOperations(OpCode::SaveFlags).empty());
    auto new_pseudos = new_result.Def()->GetPseudoOperations(OpCode::SaveFlags);
    REQUIRE(new_pseudos.size() == 1);
    REQUIRE(new_pseudos[0] == new_save);
    REQUIRE(new_pseudos[0]->GetArg<Flags>(1) == Flags::NZ);
    size_t save_count = 0;
    for (auto& inst : overwritten.GetInstList()) {
        save_count += inst.GetOp() == OpCode::SaveFlags;
    }
    REQUIRE(save_count == 1);

    // A carry write covered by a later carry write in the same block is dead.
    // SVM_FLAG_CARRY_ELIM=0 retains the old Gate B behavior for bisection.
    Block carry_overwritten{0, Location{0x1800}};
    auto c_lhs = carry_overwritten.LoadImm(Imm{1u});
    auto c_rhs = carry_overwritten.LoadImm(Imm{2u});
    auto c_old = carry_overwritten.Add(c_lhs, Operand{c_rhs});
    carry_overwritten.AppendInst(OpCode::SaveFlags, c_old, Flags::All);
    auto c_new = carry_overwritten.Add(c_lhs, Operand{c_rhs});
    carry_overwritten.AppendInst(OpCode::SaveFlags, c_new, Flags::All);

    FlagsEliminationPass::Run(&carry_overwritten, nullptr, FeatureSet{});

    const bool carry_elim_off =
            !swift::runtime::GetSvmConfig().flag_carry_elim;
    REQUIRE(c_old.Def()->GetPseudoOperations(OpCode::SaveFlags).size() ==
            (carry_elim_off ? 1 : 0));
    REQUIRE(c_new.Def()->GetPseudoOperations(OpCode::SaveFlags).size() == 1);
    size_t carry_save_count = 0;
    for (auto& inst : carry_overwritten.GetInstList()) {
        carry_save_count += inst.GetOp() == OpCode::SaveFlags;
    }
    REQUIRE(carry_save_count == (carry_elim_off ? 2 : 1));

    Block carry_read{0, Location{0x2000}};
    auto carry_lhs = carry_read.LoadImm(Imm{5u});
    auto carry_rhs = carry_read.LoadImm(Imm{3u});
    auto carry_source = carry_read.Add(carry_lhs, Operand{carry_rhs});
    auto* carry_save = carry_read.AppendInst(OpCode::SaveFlags, carry_source, Flags::All);
    auto sbb_result = carry_read.Sbb(carry_lhs, Operand{carry_rhs});
    carry_read.AppendInst(OpCode::SaveFlags, sbb_result, Flags::All);

    FlagsEliminationPass::Run(&carry_read, nullptr, FeatureSet{});

    // Sbb reads the preceding carry, so its producer stays live. Its mask must
    // remain whole: the JIT cannot safely turn this into a C-only pseudo.
    auto carry_pseudos = carry_source.Def()->GetPseudoOperations(OpCode::SaveFlags);
    REQUIRE(carry_pseudos.size() == 1);
    REQUIRE(carry_pseudos[0] == carry_save);
    REQUIRE(carry_pseudos[0]->GetArg<Flags>(1) == Flags::All);

    // A reader between two full writes needs only its tested bit from the
    // earlier producer. W14 narrows the surviving pseudo to that bit; the
    // module switch restores the old full mask exactly.
    Block partial{0, Location{0x2800}};
    auto p_old = partial.Add(lhs, Operand{rhs});
    auto* p_old_save = partial.AppendInst(OpCode::SaveFlags, p_old, Flags::All);
    partial.AppendInst(OpCode::TestFlags, Flags::Zero);
    auto p_new = partial.Add(lhs, Operand{rhs});
    partial.AppendInst(OpCode::SaveFlags, p_new, Flags::All);

    FlagsEliminationPass::Run(&partial, nullptr, FeatureSet{});

    const bool full_elim_on = swift::runtime::GetSvmConfig().flag_full_elim;
    REQUIRE(p_old_save->GetArg<Flags>(1) ==
            (full_elim_on ? Flags::Zero : Flags::NZCV));
}

TEST_CASE("Flag elimination removes only overwritten in-block carry writes") {
    using namespace swift::runtime::ir;

    auto count_op = [](Block& block, OpCode op) {
        size_t count = 0;
        for (auto& inst : block.GetInstList()) {
            count += inst.GetOp() == op;
        }
        return count;
    };
    auto contains = [](Block& block, Inst* wanted) {
        for (auto& inst : block.GetInstList()) {
            if (&inst == wanted) {
                return true;
            }
        }
        return false;
    };
    auto append_carry_save = [](Block& block, Value lhs, Value rhs) {
        auto result = block.Add(lhs, Operand{rhs});
        return block.AppendInst(OpCode::SaveFlags, result, Flags::Carry);
    };

    const bool carry_elim_off =
            !swift::runtime::GetSvmConfig().flag_carry_elim;
    if (carry_elim_off) {
        // The bisect switch covers every deletion introduced by this change.
        // Carry writes of all three forms survive exactly as under old Gate B;
        // unrelated non-carry DSE remains enabled.
        Block disabled{0, Location{0x3000}};
        auto lhs = disabled.LoadImm(Imm{1u});
        auto rhs = disabled.LoadImm(Imm{2u});
        auto* old_save = append_carry_save(disabled, lhs, rhs);
        auto* old_clear = disabled.AppendInst(OpCode::ClearFlags, Flags::Carry);
        auto carry_value = disabled.LoadImm<BOOL>(Imm{1u});
        auto* old_set = disabled.AppendInst(OpCode::SetCarry, carry_value);
        auto* old_invert = disabled.AppendInst(OpCode::InvertCarry);
        auto* old_noncarry =
                disabled.AppendInst(OpCode::ClearFlags, Flags::Overflow);
        auto* last_save = append_carry_save(disabled, lhs, rhs);
        auto* last_noncarry =
                disabled.AppendInst(OpCode::ClearFlags, Flags::Overflow);

        FeatureSet disabled_features{};
        disabled_features.flag_carry_elim = false;
        FlagsEliminationPass::Run(&disabled, nullptr, disabled_features);

        REQUIRE(contains(disabled, old_save));
        REQUIRE(contains(disabled, old_clear));
        REQUIRE(contains(disabled, old_set));
        REQUIRE(contains(disabled, old_invert));
        REQUIRE_FALSE(contains(disabled, old_noncarry));
        REQUIRE(contains(disabled, last_save));
        REQUIRE(contains(disabled, last_noncarry));
        REQUIRE(count_op(disabled, OpCode::SaveFlags) == 2);
        REQUIRE(count_op(disabled, OpCode::ClearFlags) == 2);
        REQUIRE(count_op(disabled, OpCode::SetCarry) == 1);
        REQUIRE(count_op(disabled, OpCode::InvertCarry) == 1);
        return;
    }

    // (1) Straight line, no intervening reader: the earlier C write is dead.
    Block overwritten{1, Location{0x3100}};
    auto lhs = overwritten.LoadImm(Imm{1u});
    auto rhs = overwritten.LoadImm(Imm{2u});
    auto* first = append_carry_save(overwritten, lhs, rhs);
    auto* last = append_carry_save(overwritten, lhs, rhs);

    FlagsEliminationPass::Run(&overwritten, nullptr, FeatureSet{});

    REQUIRE_FALSE(contains(overwritten, first));
    REQUIRE(contains(overwritten, last));
    REQUIRE(count_op(overwritten, OpCode::SaveFlags) == 1);

    // (2) A C reader between two writers keeps both writers live.
    Block read_between{2, Location{0x3200}};
    lhs = read_between.LoadImm(Imm{3u});
    rhs = read_between.LoadImm(Imm{4u});
    first = append_carry_save(read_between, lhs, rhs);
    read_between.AppendInst(OpCode::TestFlags, Flags::Carry);
    last = append_carry_save(read_between, lhs, rhs);

    FlagsEliminationPass::Run(&read_between, nullptr, FeatureSet{});

    REQUIRE(contains(read_between, first));
    REQUIRE(contains(read_between, last));
    REQUIRE(count_op(read_between, OpCode::SaveFlags) == 2);

    // (3) Flags::All at block exit makes a lone final C write live.
    Block live_out{3, Location{0x3300}};
    lhs = live_out.LoadImm(Imm{5u});
    rhs = live_out.LoadImm(Imm{6u});
    auto* only = append_carry_save(live_out, lhs, rhs);

    FlagsEliminationPass::Run(&live_out, nullptr, FeatureSet{});

    REQUIRE(contains(live_out, only));
    REQUIRE(count_op(live_out, OpCode::SaveFlags) == 1);

    // (4) On one path C is read before the later covering write; on the other
    // path the branch skips directly to that write. The earlier writer is live
    // on the union of both backward paths.
    Block branched{4, Location{0x3400}};
    lhs = branched.LoadImm(Imm{7u});
    rhs = branched.LoadImm(Imm{8u});
    first = append_carry_save(branched, lhs, rhs);
    auto condition = branched.LoadImm<BOOL>(Imm{1u});
    auto skip_read = branched.NotGoto(condition);
    branched.AppendInst(OpCode::TestFlags, Flags::Carry);
    branched.BindLabel(skip_read);
    last = append_carry_save(branched, lhs, rhs);

    FlagsEliminationPass::Run(&branched, nullptr, FeatureSet{});

    REQUIRE(contains(branched, first));
    REQUIRE(contains(branched, last));
    REQUIRE(count_op(branched, OpCode::SaveFlags) == 2);

    // (5) ClearFlags(C) and SetCarry are ordinary C writers for liveness.
    Block clear_covered{5, Location{0x3500}};
    lhs = clear_covered.LoadImm(Imm{9u});
    rhs = clear_covered.LoadImm(Imm{10u});
    auto* clear = clear_covered.AppendInst(OpCode::ClearFlags, Flags::Carry);
    last = append_carry_save(clear_covered, lhs, rhs);

    FlagsEliminationPass::Run(&clear_covered, nullptr, FeatureSet{});

    REQUIRE_FALSE(contains(clear_covered, clear));
    REQUIRE(contains(clear_covered, last));
    REQUIRE(count_op(clear_covered, OpCode::ClearFlags) == 0);

    Block set_covered{6, Location{0x3600}};
    lhs = set_covered.LoadImm(Imm{11u});
    rhs = set_covered.LoadImm(Imm{12u});
    auto carry_value = set_covered.LoadImm<BOOL>(Imm{1u});
    auto* set = set_covered.AppendInst(OpCode::SetCarry, carry_value);
    last = append_carry_save(set_covered, lhs, rhs);

    FlagsEliminationPass::Run(&set_covered, nullptr, FeatureSet{});

    REQUIRE_FALSE(contains(set_covered, set));
    REQUIRE(contains(set_covered, last));
    REQUIRE(count_op(set_covered, OpCode::SetCarry) == 0);

    // (6) A carry-representation transform follows the same liveness rule.
    Block invert_covered{7, Location{0x3700}};
    lhs = invert_covered.LoadImm(Imm{13u});
    rhs = invert_covered.LoadImm(Imm{14u});
    first = append_carry_save(invert_covered, lhs, rhs);
    auto* invert = invert_covered.AppendInst(OpCode::InvertCarry);
    last = append_carry_save(invert_covered, lhs, rhs);

    FlagsEliminationPass::Run(&invert_covered, nullptr, FeatureSet{});

    REQUIRE_FALSE(contains(invert_covered, first));
    REQUIRE_FALSE(contains(invert_covered, invert));
    REQUIRE(contains(invert_covered, last));

    Block invert_read{8, Location{0x3800}};
    lhs = invert_read.LoadImm(Imm{15u});
    rhs = invert_read.LoadImm(Imm{16u});
    first = append_carry_save(invert_read, lhs, rhs);
    invert = invert_read.AppendInst(OpCode::InvertCarry);
    invert_read.AppendInst(OpCode::TestFlags, Flags::Carry);
    last = append_carry_save(invert_read, lhs, rhs);

    FlagsEliminationPass::Run(&invert_read, nullptr, FeatureSet{});

    REQUIRE(contains(invert_read, first));
    REQUIRE(contains(invert_read, invert));
    REQUIRE(contains(invert_read, last));

    // (7) A condition keeps only the NZCV bits it actually reads.
    Block zero_condition{9, Location{0x3900}};
    lhs = zero_condition.LoadImm(Imm{17u});
    rhs = zero_condition.LoadImm(Imm{18u});
    auto result = zero_condition.Sub(lhs, Operand{rhs});
    first = zero_condition.AppendInst(OpCode::SaveFlags, result, Flags::All);
    invert = zero_condition.AppendInst(OpCode::InvertCarry);
    zero_condition.LocalCondSet(Cond::EQ);
    last = append_carry_save(zero_condition, lhs, rhs);

    FlagsEliminationPass::Run(&zero_condition, nullptr, FeatureSet{});

    REQUIRE(contains(zero_condition, first));
    REQUIRE_FALSE(contains(zero_condition, invert));
    REQUIRE(contains(zero_condition, last));

    Block carry_condition{10, Location{0x3a00}};
    lhs = carry_condition.LoadImm(Imm{19u});
    rhs = carry_condition.LoadImm(Imm{20u});
    result = carry_condition.Sub(lhs, Operand{rhs});
    first = carry_condition.AppendInst(OpCode::SaveFlags, result, Flags::All);
    invert = carry_condition.AppendInst(OpCode::InvertCarry);
    carry_condition.LocalCondSet(Cond::CS);
    last = append_carry_save(carry_condition, lhs, rhs);

    FlagsEliminationPass::Run(&carry_condition, nullptr, FeatureSet{});

    REQUIRE(contains(carry_condition, first));
    REQUIRE(contains(carry_condition, invert));
    REQUIRE(contains(carry_condition, last));

    // (8) A carry consumer protects its guest region and producer region. A
    // one-region block therefore remains untouched.
    Block gate_a{11, Location{0x3b00}};
    lhs = gate_a.LoadImm(Imm{13u});
    rhs = gate_a.LoadImm(Imm{14u});
    first = append_carry_save(gate_a, lhs, rhs);
    clear = gate_a.AppendInst(OpCode::ClearFlags, Flags::Carry);
    carry_value = gate_a.LoadImm<BOOL>(Imm{1u});
    set = gate_a.AppendInst(OpCode::SetCarry, carry_value);
    invert = gate_a.AppendInst(OpCode::InvertCarry);
    gate_a.Sbb(lhs, Operand{rhs});
    last = append_carry_save(gate_a, lhs, rhs);

    FlagsEliminationPass::Run(&gate_a, nullptr, FeatureSet{});

    REQUIRE(contains(gate_a, first));
    REQUIRE(contains(gate_a, clear));
    REQUIRE(contains(gate_a, set));
    REQUIRE(contains(gate_a, invert));
    REQUIRE(contains(gate_a, last));
    REQUIRE(count_op(gate_a, OpCode::SaveFlags) == 2);
    REQUIRE(count_op(gate_a, OpCode::ClearFlags) == 1);
    REQUIRE(count_op(gate_a, OpCode::SetCarry) == 1);
    REQUIRE(count_op(gate_a, OpCode::InvertCarry) == 1);

    Block segmented{12, Location{0x3c00}};
    lhs = segmented.LoadImm(Imm{21u});
    rhs = segmented.LoadImm(Imm{22u});
    auto* protected_save = append_carry_save(segmented, lhs, rhs);
    segmented.AppendInst(OpCode::AdvancePC, Imm{1u});
    segmented.LoadImm(Imm{23u});
    segmented.AppendInst(OpCode::AdvancePC, Imm{1u});
    auto adc_result = segmented.Adc(lhs, Operand{rhs});
    auto* adc_save = segmented.AppendInst(
            OpCode::SaveFlags, adc_result, Flags::All);
    segmented.AppendInst(OpCode::AdvancePC, Imm{1u});
    auto* tail_old = append_carry_save(segmented, lhs, rhs);
    auto* tail_new = append_carry_save(segmented, lhs, rhs);

    FlagsEliminationPass::Run(&segmented, nullptr, FeatureSet{});

    REQUIRE(contains(segmented, protected_save));
    REQUIRE(contains(segmented, adc_save));
    REQUIRE_FALSE(contains(segmented, tail_old));
    REQUIRE(contains(segmented, tail_new));
}
