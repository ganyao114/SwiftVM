#include "../support/case_support.h"
#include "../support/fp_environment.h"

TEST_CASE("resident XMM ABI maps the configured register bank") {
    using namespace swift::runtime;
    using namespace swift::translator::x86;

    if (!GetSvmConfig().xmm_resident) {
        SUCCEED("SVM_XMM_RESIDENT=0 has no resident XMM ABI");
        return;
    }

    auto* instance = X86Instance::Make();
    const auto mappings = instance->GetAddressSpace()->GetConfig().buffers_static_alloc;
    const auto xmm_base = offsetof(swift::x86::ThreadContext64, xmms);
    auto mapped = [&](swift::u32 index, swift::u32 host) {
        return std::ranges::any_of(mappings, [&](const UniformMapDesc& desc) {
            return desc.is_float && desc.offset == xmm_base + index * sizeof(swift::x86::Xmm) &&
                   desc.size == sizeof(swift::x86::Xmm) && desc.reg == host;
        });
    };
    const swift::u32 resident_count = GetSvmConfig().xmm_resident_hi ? 16 : 8;
    std::array<bool, 16> resident_mappings{};
    for (swift::u32 index = 0; index < 16; ++index) {
        resident_mappings[index] = mapped(index, 16 + index);
    }
    X86Instance::Destroy(instance);

    for (swift::u32 index = 0; index < 16; ++index) {
        INFO("XMM" << index);
        REQUIRE(resident_mappings[index] == (index < resident_count));
    }
}

TEST_CASE("resident XMM coalescing preserves captures and fixed-home windows") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    auto gprs = GPRSMask{~((1u << 8) - 1u)};
    auto resident_fprs = [](swift::u32 target = 16) {
        FPRSMask mask{0};
        mask.Mark(target);
        return mask;
    };

    struct Case {
        IntrusivePtr<Block> block;
        Value produced;
        Value conflict;
        Inst* publish;
    };
    auto make_case = [&](OpCode op, swift::u64 location, swift::u32 target = 16,
                         bool with_conflict = false) {
        IntrusivePtr<Block> block{new Block(0, Location{location})};
        auto left = block->LoadUniform(Uniform{0, ValueType::V128});
        auto right = block->LoadUniform(Uniform{16, ValueType::V128});
        Value produced{};
        switch (op) {
            case OpCode::LoadUniform:
                produced = left;
                break;
            case OpCode::LoadMemory: {
                auto address = block->LoadUniform(Uniform{64, ValueType::U64});
                produced = block->LoadMemory(Operand{address}).SetType(ValueType::V128);
                break;
            }
            case OpCode::VecAnd: produced = block->VecAnd(left, right); break;
            case OpCode::VecOr: produced = block->VecOr(left, right); break;
            case OpCode::VecXor: produced = block->VecXor(left, right); break;
            case OpCode::VecAdd: produced = block->VecAdd(left, right, Imm{32u}); break;
            case OpCode::VecSub: produced = block->VecSub(left, right, Imm{32u}); break;
            case OpCode::VecCmpEq: produced = block->VecCmpEq(left, right, Imm{8u}); break;
            case OpCode::VecCmpGt: produced = block->VecCmpGt(left, right, Imm{8u}); break;
            case OpCode::VecMul: produced = block->VecMul(left, right, Imm{32u}); break;
            case OpCode::VecShuffle32Indexed:
                produced = block->VecShuffle32Indexed(left, right);
                break;
            case OpCode::VecExtractBytes:
                produced = block->VecExtractBytes(left, right, Imm{7u});
                break;
            case OpCode::VecZip: produced = block->VecZip(left, right, Imm{64u}, Imm{0u}); break;
            case OpCode::VecFAdd: produced = block->VecFAdd(left, right, Imm{32u}); break;
            case OpCode::VecFSub: produced = block->VecFSub(left, right, Imm{32u}); break;
            case OpCode::VecFMul: produced = block->VecFMul(left, right, Imm{32u}); break;
            case OpCode::VecFDiv: produced = block->VecFDiv(left, right, Imm{32u}); break;
            default: FAIL("missing resident XMM producer test case");
        }
        produced = produced.SetType(ValueType::V128);
        Value conflict{};
        if (with_conflict) {
            conflict = block->VecOr(left, right).SetType(ValueType::V128);
        }
        auto* publish = block->AppendInst(
                OpCode::SetHostFPR, produced, HostRegIndex(target), Imm{0u});
        if (conflict.Defined()) {
            block->StoreUniform(Uniform{32, ValueType::V128}, conflict);
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        return Case{std::move(block), produced, conflict, publish};
    };

    auto allocate = [&](Block* block, bool enabled, swift::u32 target = 16) {
        auto alloc = std::make_unique<RegAlloc>(
                block->MaxInstrId(), gprs, resident_fprs(target), FeatureSet{});
        RegisterAllocTestSupport::RunForXmmResidentTest(block, alloc.get(), enabled);
        return alloc;
    };
    auto emit_size = [&](Block* block, RegAlloc& alloc, swift::u64 location) {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        auto module = address_space.MapModule(
                LocationDescriptor{location}, LocationDescriptor{location + 0x10},
                ModuleConfig{});
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block);
        context.Finish();
        return context.CurrentBufferSize();
    };

    constexpr std::array producers{
            OpCode::LoadUniform, OpCode::LoadMemory,
            OpCode::VecAnd, OpCode::VecOr, OpCode::VecXor,
            OpCode::VecAdd, OpCode::VecSub, OpCode::VecCmpEq, OpCode::VecCmpGt,
            OpCode::VecMul,
            OpCode::VecShuffle32Indexed, OpCode::VecExtractBytes, OpCode::VecZip,
            OpCode::VecFAdd, OpCode::VecFSub, OpCode::VecFMul, OpCode::VecFDiv,
    };
    std::size_t index = 0;
    for (swift::u32 target : {16u, 24u}) {
        for (auto op : producers) {
            CAPTURE(op, target);
            auto item = make_case(op, 0xa000 + index++ * 0x20, target);
            auto off = allocate(item.block.get(), false, target);
            REQUIRE_FALSE(off->IsHostWriteCoalesced(item.publish->Id()));
            const auto off_size = emit_size(
                    item.block.get(), *off, 0xb000 + index * 0x20);
            auto on = allocate(item.block.get(), true, target);
            REQUIRE(on->ValueFPR(item.produced).id == target);
            REQUIRE(on->IsHostWriteCoalesced(item.publish->Id()));
            const auto on_size = emit_size(
                    item.block.get(), *on, 0xc000 + index * 0x20);
            REQUIRE(on_size + vixl::aarch64::kInstructionSize == off_size);

            auto negative = make_case(
                    op, 0xd000 + index * 0x20, target, true);
            RegAlloc conflict_alloc{negative.block->MaxInstrId(), gprs,
                                    resident_fprs(target), FeatureSet{}};
            RegisterAllocTestSupport::RunForXmmResidentConflictTest(
                    negative.block.get(), &conflict_alloc,
                    negative.conflict.Id(), target);
            REQUIRE(conflict_alloc.ValueFPR(negative.conflict).id == target);
            REQUIRE_FALSE(conflict_alloc.IsHostWriteCoalesced(
                    negative.publish->Id()));
        }
    }

    SECTION("a crossing SetHostFPR forces a real capture copy") {
        for (swift::u32 target : {16u, 24u}) {
            CAPTURE(target);
            IntrusivePtr<Block> block{new Block(0, Location{0xa400 + target})};
            auto capture = block->GetHostFPR(HostRegIndex(target), Imm{0u})
                                    .SetType(ValueType::V128);
            auto replacement = block->LoadUniform(Uniform{16, ValueType::V128});
            block->SetHostFPR(replacement, HostRegIndex(target), Imm{0u});
            block->StoreUniform(Uniform{32, ValueType::V128}, capture);
            block->SetTerminal(terminal::ReturnToDispatch{});
            block->ReIdInstr();
            auto alloc = allocate(block.get(), true, target);
            REQUIRE(alloc->ValueFPR(capture).id != target);
            REQUIRE_FALSE(alloc->IsHostReadCoalesced(capture.Id()));
        }
    }

    SECTION("a third-party fixed-home interval rejects early publication") {
        for (swift::u32 target : {16u, 24u}) {
            CAPTURE(target);
            IntrusivePtr<Block> block{new Block(0, Location{0xa420 + target})};
            auto left = block->LoadUniform(Uniform{0, ValueType::V128});
            auto right = block->LoadUniform(Uniform{16, ValueType::V128});
            auto candidate = block->VecXor(left, right).SetType(ValueType::V128);
            auto tied = block->VecOr(left, right).SetType(ValueType::V128);
            auto* publish = block->AppendInst(
                    OpCode::SetHostFPR, candidate, HostRegIndex(target), Imm{0u});
            block->StoreUniform(Uniform{32, ValueType::V128}, tied);
            block->SetTerminal(terminal::ReturnToDispatch{});
            block->ReIdInstr();
            RegAlloc alloc{block->MaxInstrId(), gprs,
                           resident_fprs(target), FeatureSet{}};
            RegisterAllocTestSupport::RunForXmmResidentConflictTest(
                    block.get(), &alloc, tied.Id(), target);
            REQUIRE(alloc.ValueFPR(tied).id == target);
            REQUIRE_FALSE(alloc.IsHostWriteCoalesced(publish->Id()));
        }
    }

    SECTION("partial lane and GPR views retain their explicit bridges") {
        IntrusivePtr<Block> block{new Block(0, Location{0xa440})};
        auto lane = block->LoadUniform(Uniform{0, ValueType::V64});
        auto* partial = block->AppendInst(
                OpCode::SetHostFPR, lane, HostRegIndex(24), Imm{8u});
        auto scalar = block->GetHostFPR(HostRegIndex(24), Imm{8u})
                               .SetType(ValueType::U64);
        block->StoreUniform(Uniform{32, ValueType::U64}, scalar);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        auto alloc = allocate(block.get(), true, 24);
        REQUIRE_FALSE(alloc->IsHostWriteCoalesced(partial->Id()));
        REQUIRE_FALSE(alloc->IsHostReadCoalesced(scalar.Id()));
    }

    SECTION("resident mappings are finalized after XMM store sinking") {
        if (!GetSvmConfig().xmm_resident) {
            SUCCEED("integration shape is enabled by SVM_XMM_RESIDENT=1");
        } else {
            UniformInfo info{};
            info.uniform_size = 64;
            info.xmm_uniform_ranges.push_back({0, 16});
            UniformRegister mapped{.uniform = Uniform{0, ValueType::V128}};
            mapped.host_reg.is_fpr = true;
            mapped.host_reg.fpr = HostFPR{24};
            info.uni_fprs.Mark(24);
            info.uniform_regs_map.Map(0, 16, mapped);

            IntrusivePtr<Block> block{new Block(0, Location{0xa460})};
            auto left = block->LoadUniform(Uniform{16, ValueType::V128});
            auto right = block->LoadUniform(Uniform{32, ValueType::V128});
            block->StoreUniform(Uniform{0, ValueType::V128}, block->VecAnd(left, right));
            block->StoreUniform(Uniform{0, ValueType::V128}, block->VecOr(left, right));
            block->SetTerminal(terminal::ReturnToDispatch{});
            block->ReIdInstr();

            UniformEliminationPass::Run(block.get(), info, FeatureSet{});
            UniformStoreSinkPass::Run(block.get(), info, FeatureSet{});
            UniformEliminationPass::FinalizeStaticFPRMappings(block.get(), info);
            std::size_t uniform_stores = 0;
            std::size_t fixed_stores = 0;
            for (auto& inst : block->GetInstList()) {
                uniform_stores += inst.GetOp() == OpCode::StoreUniform;
                fixed_stores += inst.GetOp() == OpCode::SetHostFPR;
            }
            REQUIRE(uniform_stores == 0);
            REQUIRE(fixed_stores == 1);
        }
    }

    SECTION("helper captures honor resident FPR preservation contracts") {
        std::vector<UniformMapDesc> descriptors;
        constexpr swift::u32 descriptor_count = 12;
        for (swift::u32 index = 0; index < descriptor_count; ++index) {
            descriptors.emplace_back(index * sizeof(swift::u128), sizeof(swift::u128),
                                     16 + index, true);
        }
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .uniform_buffer_size = static_cast<swift::u32>(
                        descriptor_count * sizeof(swift::u128)),
                .buffers_static_alloc = descriptors,
        };
        auto capture_counts = [&](HostRegisterEffect effect) {
            AddressSpace address_space{config};
            IntrusivePtr<Block> block{new Block(0, Location{0xa480})};
            (void)block->CallLambda(Lambda{
                    DataClass{Imm{swift::u64{reinterpret_cast<swift::VAddr>(
                            FptrCast(&ReadFPCRFromHostHelper))}}},
                    HelperCallTraits{.host_registers = effect}});
            block->SetTerminal(terminal::ReturnToDispatch{});
            block->ReIdInstr();
            RegAlloc alloc{block->MaxInstrId(),
                           address_space.GetTrampolines().GetGPRRegs(),
                           address_space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
            RegisterAllocPass::Run(block.get(), &alloc, false, FeatureSet{});
            arm64::JitContext context{address_space.GetDefaultModule(), alloc};
            arm64::JitTranslator translator{context};
            translator.Translate(block.get());
            context.Finish();

            auto& masm = context.GetMasm();
            auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
            auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
            vixl::aarch64::Decoder decoder;
            vixl::aarch64::Disassembler disassembler;
            decoder.AppendVisitor(&disassembler);
            std::array<std::size_t, 4> counts{};
            for (auto* instruction = first; instruction < last;
                 instruction = instruction->GetNextInstruction()) {
                decoder.Decode(instruction);
                const std::string_view line = disassembler.GetOutput();
                const bool low_store = line.find("stp q16, q17") != std::string_view::npos ||
                                       line.find("stp q18, q19") != std::string_view::npos ||
                                       line.find("stp q20, q21") != std::string_view::npos ||
                                       line.find("stp q22, q23") != std::string_view::npos;
                const bool low_load = line.find("ldp q16, q17") != std::string_view::npos ||
                                      line.find("ldp q18, q19") != std::string_view::npos ||
                                      line.find("ldp q20, q21") != std::string_view::npos ||
                                      line.find("ldp q22, q23") != std::string_view::npos;
                const bool high_store = line.find("stp q24, q25") != std::string_view::npos ||
                                        line.find("stp q26, q27") != std::string_view::npos;
                const bool high_load = line.find("ldp q24, q25") != std::string_view::npos ||
                                       line.find("ldp q26, q27") != std::string_view::npos;
                counts[0] += low_store;
                counts[1] += low_load;
                counts[2] += high_store;
                counts[3] += high_load;
            }
            return counts;
        };

        REQUIRE(capture_counts(HostRegisterEffect::MayTouchSIMD) ==
                std::array<std::size_t, 4>{4, 4, 2, 2});
        REQUIRE(capture_counts(HostRegisterEffect::PreservesPinnedState) ==
                std::array<std::size_t, 4>{0, 0, 0, 0});
    }
}

TEST_CASE("AES resident ownership ties only complete verified chains") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    constexpr swift::u32 kTarget = 18;
    auto gprs = GPRSMask{~((1u << 8) - 1u)};
    FPRSMask resident_fprs{0};
    resident_fprs.Mark(kTarget);

    struct Chain {
        IntrusivePtr<Block> block;
        Value first;
        Value last;
        Value conflict;
        Inst* publish;
    };
    auto make_chain = [&](swift::u64 location, bool last_round,
                          bool with_store_observer, bool with_extra_use,
                          bool with_conflict) {
        IntrusivePtr<Block> block{new Block(0, Location{location})};
        auto home = block->GetHostFPR(HostRegIndex(kTarget), Imm{0u})
                            .SetType(ValueType::V128);
        auto key0 = block->LoadUniform(Uniform{0, ValueType::V128});
        auto key1 = block->LoadUniform(Uniform{16, ValueType::V128});
        auto zero = block->LoadUniform(Uniform{32, ValueType::V128});
        auto first = block->VecAesEncFast(home, key0, zero)
                             .SetType(ValueType::V128);
        if (with_store_observer) {
            auto address = block->LoadUniform(Uniform{64, ValueType::U64});
            auto payload = block->LoadUniform(Uniform{72, ValueType::U64});
            block->StoreMemory(Operand{address}, payload);
        }
        if (with_extra_use) {
            block->StoreUniform(Uniform{96, ValueType::V128}, first);
        }
        Value last = last_round
                ? block->VecAesEncLastFast(first, key1, zero)
                          .SetType(ValueType::V128)
                : block->VecAesEncFast(first, key1, zero)
                          .SetType(ValueType::V128);
        Value conflict{};
        if (with_conflict) {
            conflict = block->VecOr(key0, key1).SetType(ValueType::V128);
        }
        auto* publish = block->AppendInst(
                OpCode::SetHostFPR, last, HostRegIndex(kTarget), Imm{0u});
        if (conflict.Defined()) {
            block->StoreUniform(Uniform{112, ValueType::V128}, conflict);
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        return Chain{std::move(block), first, last, conflict, publish};
    };

    auto allocate = [&](Block* block, bool enabled) {
        auto alloc = std::make_unique<RegAlloc>(
                block->MaxInstrId(), gprs, resident_fprs, FeatureSet{});
        RegisterAllocTestSupport::RunForAesChainTieTest(block, alloc.get(), enabled);
        return alloc;
    };
    auto emit_size = [&](Block* block, RegAlloc& alloc, swift::u64 location,
                         bool enabled) {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        ModuleConfig module_config{};
        module_config.feature_overrides.Set(
                FeatureId::ra_aes_chain_tie, enabled);
        auto module = address_space.MapModule(
                LocationDescriptor{location}, LocationDescriptor{location + 0x10},
                module_config);
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block);
        context.Finish();
        return context.CurrentBufferSize();
    };

    for (bool last_round : {false, true}) {
        for (bool fault_observer : {false, true}) {
            CAPTURE(last_round, fault_observer);
            auto item = make_chain(0xaa00 + last_round * 0x40 + fault_observer * 0x20,
                                   last_round, fault_observer, false, false);
            auto off = allocate(item.block.get(), false);
            REQUIRE_FALSE(off->IsAesChainTied(item.first.Id()));
            REQUIRE_FALSE(off->IsAesChainTied(item.last.Id()));
            REQUIRE_FALSE(off->IsHostWriteCoalesced(item.publish->Id()));
            const auto off_size = emit_size(item.block.get(), *off, 0xab00, false);

            auto on = allocate(item.block.get(), true);
            REQUIRE(on->ValueFPR(item.first).id == kTarget);
            REQUIRE(on->ValueFPR(item.last).id == kTarget);
            REQUIRE(on->IsAesChainTied(item.first.Id()));
            REQUIRE(on->IsAesChainTied(item.last.Id()));
            REQUIRE(on->IsHostWriteCoalesced(item.publish->Id()));
            const auto on_size = emit_size(item.block.get(), *on, 0xac00, true);
            REQUIRE(on_size + 2 * vixl::aarch64::kInstructionSize == off_size);
        }
    }

    SECTION("an intermediate extra use rejects the whole component") {
        auto item = make_chain(0xad00, false, false, true, false);
        auto alloc = allocate(item.block.get(), true);
        REQUIRE_FALSE(alloc->IsAesChainTied(item.first.Id()));
        REQUIRE_FALSE(alloc->IsAesChainTied(item.last.Id()));
        REQUIRE_FALSE(alloc->IsHostWriteCoalesced(item.publish->Id()));
    }

    SECTION("two interleaved resident lanes retain independent ownership") {
        constexpr swift::u32 kOtherTarget = 19;
        FPRSMask two_homes{0};
        two_homes.Mark(kTarget);
        two_homes.Mark(kOtherTarget);
        IntrusivePtr<Block> block{new Block(0, Location{0xadc0})};
        auto home0 = block->GetHostFPR(HostRegIndex(kTarget), Imm{0u})
                             .SetType(ValueType::V128);
        auto home1 = block->GetHostFPR(HostRegIndex(kOtherTarget), Imm{0u})
                             .SetType(ValueType::V128);
        auto key0 = block->LoadUniform(Uniform{0, ValueType::V128});
        auto key1 = block->LoadUniform(Uniform{16, ValueType::V128});
        auto zero = block->LoadUniform(Uniform{32, ValueType::V128});
        auto first0 = block->VecAesEncFast(home0, key0, zero)
                              .SetType(ValueType::V128);
        auto first1 = block->VecAesEncFast(home1, key0, zero)
                              .SetType(ValueType::V128);
        auto last0 = block->VecAesEncLastFast(first0, key1, zero)
                             .SetType(ValueType::V128);
        auto last1 = block->VecAesEncLastFast(first1, key1, zero)
                             .SetType(ValueType::V128);
        auto* publish0 = block->AppendInst(
                OpCode::SetHostFPR, last0, HostRegIndex(kTarget), Imm{0u});
        auto* publish1 = block->AppendInst(
                OpCode::SetHostFPR, last1, HostRegIndex(kOtherTarget), Imm{0u});
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        RegAlloc alloc{block->MaxInstrId(), gprs, two_homes, FeatureSet{}};
        RegisterAllocTestSupport::RunForAesChainTieTest(block.get(), &alloc, true);
        REQUIRE(alloc.ValueFPR(first0).id == kTarget);
        REQUIRE(alloc.ValueFPR(last0).id == kTarget);
        REQUIRE(alloc.ValueFPR(first1).id == kOtherTarget);
        REQUIRE(alloc.ValueFPR(last1).id == kOtherTarget);
        REQUIRE(alloc.IsHostWriteCoalesced(publish0->Id()));
        REQUIRE(alloc.IsHostWriteCoalesced(publish1->Id()));
    }

    SECTION("a third-party interval mapped to the resident home rejects the chain") {
        auto item = make_chain(0xae00, false, false, false, true);
        RegAlloc alloc{item.block->MaxInstrId(), gprs, resident_fprs, FeatureSet{}};
        RegisterAllocTestSupport::RunForAesChainTieConflictTest(
                item.block.get(), &alloc, item.conflict.Id(), kTarget);
        REQUIRE(alloc.ValueFPR(item.conflict).id == kTarget);
        REQUIRE_FALSE(alloc.IsAesChainTied(item.first.Id()));
        REQUIRE_FALSE(alloc.IsAesChainTied(item.last.Id()));
        REQUIRE_FALSE(alloc.IsHostWriteCoalesced(item.publish->Id()));
    }

    SECTION("a single producer remains on the existing copy fallback") {
        IntrusivePtr<Block> block{new Block(0, Location{0xaf00})};
        auto home = block->GetHostFPR(HostRegIndex(kTarget), Imm{0u})
                            .SetType(ValueType::V128);
        auto key = block->LoadUniform(Uniform{0, ValueType::V128});
        auto zero = block->LoadUniform(Uniform{16, ValueType::V128});
        auto only = block->VecAesEncFast(home, key, zero).SetType(ValueType::V128);
        auto* publish = block->AppendInst(
                OpCode::SetHostFPR, only, HostRegIndex(kTarget), Imm{0u});
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        auto alloc = allocate(block.get(), true);
        REQUIRE_FALSE(alloc->IsAesChainTied(only.Id()));
        REQUIRE_FALSE(alloc->IsHostWriteCoalesced(publish->Id()));
    }
}

TEST_CASE("AES KEYGEN frontend preserves every rcon across register and memory sources") {
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

    struct Form {
        swift::u8 modrm;
        std::optional<swift::u32> source_uniform;
    };
    const std::array forms{
            // AESKEYGENASSIST xmm0,xmm0,imm8: destination/source alias.
            Form{0xc0, offsetof(ThreadContext64, xmm0)},
            // AESKEYGENASSIST xmm0,xmm1,imm8: distinct register source.
            Form{0xc1, offsetof(ThreadContext64, xmm1)},
            // AESKEYGENASSIST xmm0,[rax],imm8: memory source.
            Form{0x00, std::nullopt},
    };

    auto gf_mul = [](swift::u8 left, swift::u8 right) {
        swift::u8 value = 0;
        while (right) {
            if (right & 1) value ^= left;
            const bool high = (left & 0x80) != 0;
            left <<= 1;
            if (high) left ^= 0x1b;
            right >>= 1;
        }
        return value;
    };
    auto rotl8 = [](swift::u8 value, unsigned count) {
        return static_cast<swift::u8>((value << count) | (value >> (8 - count)));
    };
    auto sbox = [&](swift::u8 value) {
        swift::u8 inverse = 0;
        if (value != 0) {
            inverse = 1;
            swift::u8 base = value;
            for (unsigned exponent = 254; exponent; exponent >>= 1) {
                if (exponent & 1) inverse = gf_mul(inverse, base);
                base = gf_mul(base, base);
            }
        }
        return static_cast<swift::u8>(
                inverse ^ rotl8(inverse, 1) ^ rotl8(inverse, 2) ^
                rotl8(inverse, 3) ^ rotl8(inverse, 4) ^ 0x63);
    };
    auto golden = [&](const std::array<swift::u8, 16>& source, swift::u8 rcon) {
        std::array<swift::u8, 16> result{};
        for (unsigned pair = 0; pair < 2; ++pair) {
            const unsigned source_word = pair == 0 ? 1 : 3;
            for (unsigned byte = 0; byte < 4; ++byte) {
                result[pair * 8 + byte] = sbox(source[source_word * 4 + byte]);
                result[pair * 8 + 4 + byte] =
                        sbox(source[source_word * 4 + ((byte + 1) & 3)]);
            }
            result[pair * 8 + 4] ^= rcon;
        }
        return result;
    };

    Config interp_config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = false,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = sizeof(ThreadContext64),
    };
    backend::AddressSpace interp_space{interp_config};
    Runtime interp_runtime{&interp_space};
    auto* context = reinterpret_cast<ThreadContext64*>(
            interp_runtime.GetUniformBuffer().data());
    std::array<bool, 256> sbox_inputs_seen{};

    for (unsigned rcon = 0; rcon < 256; ++rcon) {
        for (unsigned form_index = 0; form_index < forms.size(); ++form_index) {
            const auto& form = forms[form_index];
            CAPTURE(rcon, form_index, form.modrm);
            std::array<swift::u8, 7> code{
                    0x66, 0x0f, 0x3a, 0xdf, form.modrm,
                    static_cast<swift::u8>(rcon), 0xf4,
            };
            const auto address = reinterpret_cast<VAddr>(code.data());
            Block block{0, Location{address}};
            Assembler assembler{&block};
            X64Decoder decoder{address, &memory, &assembler, true,
                               Arm64Features::None, false, false, FeatureSet{}};
            decoder.Decode();

            Inst* keygen{};
            for (auto& inst : block.GetInstList()) {
                if (inst.GetOp() != OpCode::VecAesKeygenAssist) continue;
                REQUIRE(keygen == nullptr);
                keygen = &inst;
            }
            if (keygen == nullptr && rcon == 0 && form.modrm == forms.front().modrm) {
                SUCCEED("AES KEYGEN frontend matrix requires host AES capability");
                return;
            }
            REQUIRE(keygen != nullptr);
            REQUIRE(keygen->GetArg<Imm>(1).Get() == rcon);
            const auto source = keygen->GetArg<Value>(0);
            if (form.source_uniform) {
                REQUIRE(source.Def()->GetOp() == OpCode::LoadUniform);
                REQUIRE(source.Def()->GetArg<Uniform>(0).GetOffset() ==
                        *form.source_uniform);
            } else {
                REQUIRE(source.Def()->GetOp() == OpCode::LoadMemory);
            }

            std::array<swift::u8, 16> input{};
            swift::u32 random = 0x9e3779b9u ^ (rcon * 0x45d9f3bu) ^
                                (form_index * 0x27d4eb2du);
            for (auto& byte : input) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                byte = static_cast<swift::u8>(random);
            }
            for (unsigned source_word : {1u, 3u}) {
                for (unsigned byte = 0; byte < 4; ++byte) {
                    sbox_inputs_seen[input[source_word * 4 + byte]] = true;
                }
            }
            *context = {};
            if (form_index == 0) {
                std::memcpy(&context->xmm0, input.data(), input.size());
            } else if (form_index == 1) {
                std::memcpy(&context->xmm1, input.data(), input.size());
            } else {
                context->rax.qword = reinterpret_cast<swift::u64>(input.data());
            }
            interp_runtime.GetState()->halt_reason = HaltReason::None;
            backend::interp::Interpreter interpreter{*interp_runtime.GetState(), &block};
            REQUIRE(interpreter.Run() == HaltReason::CallHost);
            const auto expected = golden(input, static_cast<swift::u8>(rcon));
            REQUIRE(std::memcmp(&context->xmm0, expected.data(), expected.size()) == 0);
        }
    }
    REQUIRE(std::all_of(sbox_inputs_seen.begin(), sbox_inputs_seen.end(),
                        [](bool seen) { return seen; }));
}

TEST_CASE("AES KEYGEN compact uses the immutable runtime prefix and exact lowering") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    constexpr std::array<swift::u64, 2> kSwizzle{
            0x040B0E010B0E0104ULL,
            0x0C0306090306090CULL,
    };
    constexpr std::array<swift::u64, 2> kMovmaskWeights{
            0x8040201008040201ULL,
            0x8040201008040201ULL,
    };
    auto read_prefix = [](const State* state) {
        std::array<swift::u64, 2> value{};
        std::memcpy(value.data(),
                    reinterpret_cast<const swift::u8*>(state) +
                            state_offset_aes_keygen_swizzle,
                    sizeof(value));
        return value;
    };
    auto read_movmask_weights = [](const State* state) {
        std::array<swift::u64, 2> value{};
        std::memcpy(value.data(),
                    reinterpret_cast<const swift::u8*>(state) +
                            state_offset_byte_movmask_weights,
                    sizeof(value));
        return value;
    };
    constexpr swift::u64 kLocation = 0xb1000;
    constexpr swift::u32 kInputOffset = 0;
    constexpr swift::u32 kOutputOffset = 16;
    constexpr swift::u32 kUniformSize = kOutputOffset + 256 * 16;

    FeatureSet hash_off{};
    FeatureSet hash_on = hash_off;
    hash_off.keygen_compact = false;
    hash_on.keygen_compact = true;
    REQUIRE(HashFeatureSet(hash_off) != HashFeatureSet(hash_on));

    auto make_config = [&] {
        return Config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .uniform_buffer_size = kUniformSize,
        };
    };
    auto make_block = [&](swift::u64 location, bool all_rcon) {
        IntrusivePtr<Block> block{new Block(0, Location{location})};
        auto source = block->LoadUniform(Uniform{kInputOffset, ValueType::V128})
                              .SetType(ValueType::V128);
        const unsigned count = all_rcon ? 256 : 1;
        Value first{};
        for (unsigned rcon = 0; rcon < count; ++rcon) {
            auto result = block->VecAesKeygenAssist(source, Imm{rcon})
                                  .SetType(ValueType::V128);
            if (!first.Defined()) first = result;
            block->StoreUniform(
                    Uniform{kOutputOffset + rcon * 16, ValueType::V128}, result);
        }
        block->SetTerminal(terminal::LinkBlock{Location{location + 0x100}});
        block->ReIdInstr();
        return std::pair{std::move(block), first};
    };

    SECTION("two runtimes own aligned equal prefixes without sharing State") {
        auto config = make_config();
        AddressSpace first_space{config};
        AddressSpace second_space{config};
        Runtime first{&first_space};
        Runtime second{&second_space};
        const auto* first_state = first.GetState();
        const auto* second_state = second.GetState();
        const auto first_constants = read_prefix(first_state);
        const auto second_constants = read_prefix(second_state);
        const auto* first_prefix = reinterpret_cast<const swift::u8*>(first_state) +
                                   state_offset_named_vector_constants;

        REQUIRE(first_state != second_state);
        REQUIRE(reinterpret_cast<std::uintptr_t>(first_state) % 16 == 0);
        REQUIRE(reinterpret_cast<std::uintptr_t>(first_prefix) % 16 == 0);
        REQUIRE(reinterpret_cast<const swift::u8*>(first_state) -
                        first_prefix ==
                -state_offset_named_vector_constants);
        REQUIRE(first_constants == kSwizzle);
        REQUIRE(second_constants == kSwizzle);
        REQUIRE(read_movmask_weights(first_state) == kMovmaskWeights);
        REQUIRE(read_movmask_weights(second_state) == kMovmaskWeights);

        const auto before = first_constants;
        const auto movmask_before = read_movmask_weights(first_state);
        first.SignalInterrupt();
        first.ClearInterrupt();
        REQUIRE(read_prefix(first_state) == before);
        REQUIRE(read_movmask_weights(first_state) == movmask_before);
    }

    struct Emitted {
        std::size_t bytes{};
        std::string text;
    };
    auto emit = [&](bool enabled, bool alias) {
        auto [block, first] = make_block(kLocation + enabled * 0x20 + alias * 0x40,
                                         false);
        auto features = FeatureSet{};
        // 两相测试显式钉死初始值，避免未来默认翻转污染 OFF 基线。
        features.keygen_compact = enabled;
        Config config = make_config();
        AddressSpace address_space{config};
        auto gprs = address_space.GetTrampolines().GetGPRRegs();
        auto fprs = address_space.GetTrampolines().GetFPRRegs();
        RegAlloc alloc{block->MaxInstrId(), gprs, fprs, features};
        RegisterAllocPass::Run(block.get(), &alloc, false, features);
        if (alias) {
            const auto source = first.Def()->GetArg<Value>(0);
            alloc.MapRegister(first.Id(), alloc.ValueFPR(source));
            REQUIRE(alloc.ValueFPR(first).id == alloc.ValueFPR(source).id);
        }

        ModuleConfig module_config{};
        module_config.feature_overrides.Set(FeatureId::keygen_compact, enabled);
        auto module = address_space.MapModule(
                LocationDescriptor{block->GetStartLocation().Value()},
                LocationDescriptor{block->GetStartLocation().Value() + 0x10},
                module_config);
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();

        Emitted emitted{.bytes = context.CurrentBufferSize()};
        auto& masm = context.GetMasm();
        auto* instruction =
                masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
        auto* end =
                masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        for (; instruction < end; instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction);
            emitted.text += disassembler.GetOutput();
            emitted.text += '\n';
        }
        return emitted;
    };

    const auto off = emit(false, false);
    const auto on = emit(true, false);
    INFO("OFF:\n" << off.text << "ON:\n" << on.text);
    REQUIRE(on.bytes + 11 * vixl::aarch64::kInstructionSize == off.bytes);
    REQUIRE(on.text.find("ldur q") != std::string::npos);
    REQUIRE(on.text.find("#-16") != std::string::npos);
    REQUIRE(on.text.find("dup") != std::string::npos);
    REQUIRE(off.text.find("ldur q") == std::string::npos);

    SECTION("result and source may alias") {
        const auto alias = emit(true, true);
        REQUIRE(alias.text.find("ldur q") != std::string::npos);
        REQUIRE(alias.text.find("dup") != std::string::npos);
    }

#if defined(__aarch64__)
    SECTION("all rcon values match the mathematical lane semantics and preserve the prefix") {
        const std::array<swift::u8, 16> source{
                0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        };
        auto config = make_config();
        std::array<swift::u8, 256 * 16> expected{};
        auto gf_mul = [](swift::u8 left, swift::u8 right) {
            swift::u8 value = 0;
            while (right) {
                if (right & 1) value ^= left;
                const bool high = (left & 0x80) != 0;
                left <<= 1;
                if (high) left ^= 0x1b;
                right >>= 1;
            }
            return value;
        };
        auto rotl8 = [](swift::u8 value, unsigned count) {
            return static_cast<swift::u8>((value << count) | (value >> (8 - count)));
        };
        auto sbox = [&](swift::u8 value) {
            swift::u8 inverse = 0;
            if (value != 0) {
                inverse = 1;
                swift::u8 base = value;
                for (unsigned exponent = 254; exponent; exponent >>= 1) {
                    if (exponent & 1) inverse = gf_mul(inverse, base);
                    base = gf_mul(base, base);
                }
            }
            return static_cast<swift::u8>(
                    inverse ^ rotl8(inverse, 1) ^ rotl8(inverse, 2) ^
                    rotl8(inverse, 3) ^ rotl8(inverse, 4) ^ 0x63);
        };
        for (unsigned rcon = 0; rcon < 256; ++rcon) {
            auto* result = expected.data() + rcon * 16;
            for (unsigned pair = 0; pair < 2; ++pair) {
                const unsigned source_word = pair == 0 ? 1 : 3;
                for (unsigned byte = 0; byte < 4; ++byte) {
                    result[pair * 8 + byte] = sbox(source[source_word * 4 + byte]);
                    result[pair * 8 + 4 + byte] =
                            sbox(source[source_word * 4 + ((byte + 1) & 3)]);
                }
                result[pair * 8 + 4] ^= static_cast<swift::u8>(rcon);
            }
        }

        std::array<swift::u8, 256 * 16> jit_off{};
        for (bool enabled : {false, true}) {
            CAPTURE(enabled);
            auto [block, first] = make_block(kLocation + 0x2000 + enabled * 0x100, true);
            (void)first;
            AddressSpace address_space{config};
            ModuleConfig module_config{};
            module_config.feature_overrides.Set(FeatureId::keygen_compact, enabled);
            auto module = address_space.MapModule(
                    LocationDescriptor{block->GetStartLocation().Value()},
                    LocationDescriptor{block->GetStartLocation().Value() + 0x10},
                    module_config);
            auto* code = TranslateIR(module, block);
            REQUIRE(code != nullptr);
            Runtime runtime{&address_space};
            const auto prefix_before = read_prefix(runtime.GetState());
            std::memcpy(runtime.GetUniformBuffer().data(), source.data(), source.size());
            runtime.SetLocation(block->GetStartLocation().Value());
            REQUIRE(address_space.GetTrampolines().GetRuntimeEntry()(
                            runtime.GetState(), code) == HaltReason::CodeMiss);
            const auto* actual = runtime.GetUniformBuffer().data() + kOutputOffset;
            const auto mismatch = std::mismatch(
                    actual, actual + expected.size(), expected.data());
            if (mismatch.first != actual + expected.size()) {
                UNSCOPED_INFO("first byte mismatch at " << (mismatch.first - actual)
                              << ": jit=" << unsigned(*mismatch.first)
                              << " golden=" << unsigned(*mismatch.second));
            }
            REQUIRE(std::memcmp(runtime.GetUniformBuffer().data() + kOutputOffset,
                                expected.data(), expected.size()) == 0);
            if (!enabled) {
                std::memcpy(jit_off.data(), actual, jit_off.size());
            }
            REQUIRE(read_prefix(runtime.GetState()) == prefix_before);
        }

        auto [interp_block, interp_first] = make_block(kLocation + 0x2400, true);
        (void)interp_first;
        AddressSpace interp_space{config};
        Runtime interp_runtime{&interp_space};
        std::memcpy(interp_runtime.GetUniformBuffer().data(), source.data(), source.size());
        backend::interp::Interpreter interpreter{*interp_runtime.GetState(),
                                                 interp_block.get()};
        REQUIRE(interpreter.Run() == HaltReason::None);
        const auto* interp_actual =
                interp_runtime.GetUniformBuffer().data() + kOutputOffset;
        REQUIRE(std::memcmp(interp_actual, expected.data(), expected.size()) == 0);
        REQUIRE(std::memcmp(interp_actual, jit_off.data(), jit_off.size()) == 0);
    }
#endif
}

TEST_CASE("PSHUFD direct lowering requires an exclusively shared canonical mask") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    const GPRSMask gprs{~((1u << 8) - 1u)};

    struct Case {
        IntrusivePtr<Block> block;
        Value source;
        Value indexes;
        Value first;
    };
    auto make_case = [&](swift::u64 location, swift::u8 control, bool mixed,
                         bool corrupt_mask, bool two_shuffles) {
        IntrusivePtr<Block> block{new Block(0, Location{location})};
        auto source = block->LoadUniform(Uniform{0, ValueType::V128})
                              .SetType(ValueType::V128);
        auto [low, high] = arm64::PshufdIndexMask(control);
        low ^= corrupt_mask;
        auto indexes = block->VecLoadConst(Imm{low}, Imm{high})
                               .SetType(ValueType::V128);
        auto first = block->VecShuffle32Indexed(source, indexes)
                             .SetType(ValueType::V128);
        block->StoreUniform(Uniform{16, ValueType::V128}, first);
        if (two_shuffles) {
            auto second = block->VecShuffle32Indexed(source, indexes)
                                  .SetType(ValueType::V128);
            block->StoreUniform(Uniform{32, ValueType::V128}, second);
        }
        if (mixed) {
            auto other = block->VecTableLookup8(source, indexes)
                                 .SetType(ValueType::V128);
            block->StoreUniform(Uniform{48, ValueType::V128}, other);
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        return Case{std::move(block), source, indexes, first};
    };

    auto allocate = [&](Block* block) {
        auto features = FeatureSet{};
        auto alloc = std::make_unique<RegAlloc>(
                block->MaxInstrId(), gprs, FPRSMask{0}, features);
        RegisterAllocPass::Run(block, alloc.get(), false, features);
        return alloc;
    };
    auto emit = [&](Block* block, RegAlloc& alloc, swift::u64 location) {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        auto module = address_space.MapModule(
                LocationDescriptor{location}, LocationDescriptor{location + 0x10},
                ModuleConfig{});
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block);
        context.Finish();
        std::string text;
        auto& masm = context.GetMasm();
        auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
        auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        for (auto* instruction = first; instruction < last;
             instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction);
            text += disassembler.GetOutput();
            text += '\n';
        }
        return text;
    };

    for (const auto [name, control, mnemonic] : {
                 std::tuple{"lane rotation", swift::u8{0x4E}, "ext"},
                 std::tuple{"lane splat", swift::u8{0x00}, "dup"}}) {
        DYNAMIC_SECTION(name << " uses one-instruction shuffles") {
            auto item = make_case(0xb0400 + control, control, false, false, true);
            auto alloc = allocate(item.block.get());
            REQUIRE(alloc->IsPshufdDirect(item.indexes.Id()));
            REQUIRE(alloc->IsPshufdDirect(item.first.Id()));
            auto code = emit(item.block.get(), *alloc, 0xb0500 + control);
            REQUIRE(code.find(mnemonic) != std::string::npos);
            REQUIRE(code.find("tbl") == std::string::npos);
        }
    }

    SECTION("result and source may alias") {
        auto item = make_case(0xb0700, 0x4E, false, false, false);
        auto alloc = allocate(item.block.get());
        alloc->MapRegister(item.first.Id(), alloc->ValueFPR(item.source));
        REQUIRE(alloc->ValueFPR(item.first).id == alloc->ValueFPR(item.source).id);
        auto code = emit(item.block.get(), *alloc, 0xb0800);
        REQUIRE(code.find("ext") != std::string::npos);
        REQUIRE(code.find("tbl") == std::string::npos);
    }

    for (const auto [name, mixed, corrupt_mask] : {
                 std::tuple{"mixed consumer", true, false},
                 std::tuple{"non-canonical mask", false, true}}) {
        DYNAMIC_SECTION(name << " keeps indexed lowering") {
            auto item = make_case(0xb0900 + mixed * 0x20 + corrupt_mask * 0x40,
                                  0x4E, mixed, corrupt_mask, true);
            auto alloc = allocate(item.block.get());
            REQUIRE_FALSE(alloc->IsPshufdDirect(item.indexes.Id()));
            auto code = emit(item.block.get(), *alloc, 0xb0a00);
            REQUIRE(code.find("tbl") != std::string::npos);
        }
    }

    SECTION("the canonical mask cannot also be the shuffled source") {
        auto item = make_case(0xb0b00, 0x4E, false, false, false);
        item.first.Def()->SetArg(0, item.indexes);
        item.block->ReIdInstr();
        auto alloc = allocate(item.block.get());
        REQUIRE_FALSE(alloc->IsPshufdDirect(item.indexes.Id()));
        auto code = emit(item.block.get(), *alloc, 0xb0c00);
        REQUIRE(code.find("tbl") != std::string::npos);
    }
}

TEST_CASE("scalar FPR fixed-home tie requires an exact safe publication window") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    constexpr swift::u32 target = 17;
    const GPRSMask gprs{~((1u << 8) - 1u)};
    auto resident_fprs = [] {
        FPRSMask mask{0};
        mask.Mark(target);
        return mask;
    };
    struct Case {
        IntrusivePtr<Block> block;
        Value left;
        Value produced;
        Value conflict;
        Inst* publish;
    };
    auto make_case = [&](OpCode op, swift::u64 location,
                         bool keep_left_live = false,
                         bool with_conflict = false) {
        IntrusivePtr<Block> block{new Block(0, Location{location})};
        auto left = block->GetHostFPR(HostRegIndex(target), Imm{0u})
                            .SetType(ValueType::V128);
        auto right = block->LoadUniform(Uniform{16, ValueType::V128})
                             .SetType(ValueType::V128);
        Value produced{};
        switch (op) {
            case OpCode::VecFAddScalar32: produced = block->VecFAddScalar32(left, right); break;
            case OpCode::VecFSubScalar32: produced = block->VecFSubScalar32(left, right); break;
            case OpCode::VecFMulScalar32: produced = block->VecFMulScalar32(left, right); break;
            case OpCode::VecFDivScalar32: produced = block->VecFDivScalar32(left, right); break;
            case OpCode::VecFAddScalar64: produced = block->VecFAddScalar64(left, right); break;
            case OpCode::VecFSubScalar64: produced = block->VecFSubScalar64(left, right); break;
            case OpCode::VecFMulScalar64: produced = block->VecFMulScalar64(left, right); break;
            case OpCode::VecFDivScalar64: produced = block->VecFDivScalar64(left, right); break;
            default: FAIL("missing scalar FPR tie producer test case");
        }
        produced = produced.SetType(ValueType::V128);
        if (keep_left_live) {
            block->StoreUniform(Uniform{32, ValueType::V128}, left);
        }
        Value conflict{};
        if (with_conflict) {
            conflict = block->VecOr(right, right).SetType(ValueType::V128);
        }
        auto* publish = block->AppendInst(
                OpCode::SetHostFPR, produced, HostRegIndex(target), Imm{0u});
        if (conflict.Defined()) {
            block->StoreUniform(Uniform{48, ValueType::V128}, conflict);
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        return Case{std::move(block), left, produced, conflict, publish};
    };
    auto allocate = [&](Block* block, bool enabled) {
        auto features = FeatureSet{};
        features.sse_scalar_tie = enabled;
        auto alloc = std::make_unique<RegAlloc>(
                block->MaxInstrId(), gprs, resident_fprs(), features);
        RegisterAllocTestSupport::RunForScalarFPRTieTest(block, alloc.get(), enabled);
        return alloc;
    };
    struct Emitted {
        std::size_t bytes{};
        std::string text;
    };
    auto emit = [&](Block* block, RegAlloc& alloc, swift::u64 location, bool enabled) {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .arm64_features = Arm64Features::AFP,
                .sse_scalar_insert = true,
                .sse_afp_nan = true,
        };
        AddressSpace address_space{config};
        ModuleConfig module_config{};
        module_config.feature_overrides.Set(FeatureId::sse_scalar_tie, enabled);
        auto module = address_space.MapModule(
                LocationDescriptor{location}, LocationDescriptor{location + 0x10},
                module_config);
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block);
        context.Finish();
        Emitted emitted{.bytes = context.CurrentBufferSize()};
        auto& masm = context.GetMasm();
        auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
        auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        for (auto* instruction = first; instruction < last;
             instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction);
            emitted.text += disassembler.GetOutput();
            emitted.text += '\n';
        }
        return emitted;
    };

    constexpr std::array producers{
            OpCode::VecFAddScalar32, OpCode::VecFSubScalar32,
            OpCode::VecFMulScalar32, OpCode::VecFDivScalar32,
            OpCode::VecFAddScalar64, OpCode::VecFSubScalar64,
            OpCode::VecFMulScalar64, OpCode::VecFDivScalar64,
    };
    std::size_t index = 0;
    for (auto op : producers) {
        CAPTURE(op);
        auto item = make_case(op, 0xa500 + index * 0x20);
        auto off = allocate(item.block.get(), false);
        REQUIRE(off->ValueFPR(item.produced).id != target);
        REQUIRE_FALSE(off->IsHostWriteCoalesced(item.publish->Id()));
        auto off_code = emit(item.block.get(), *off, 0xb500 + index * 0x20, false);

        auto on = allocate(item.block.get(), true);
        REQUIRE(on->ValueFPR(item.left).id == target);
        REQUIRE(on->ValueFPR(item.produced).id == target);
        REQUIRE(on->IsHostWriteCoalesced(item.publish->Id()));
        auto on_code = emit(item.block.get(), *on, 0xc500 + index * 0x20, true);
        INFO("OFF:\n" << off_code.text << "ON:\n" << on_code.text);
        REQUIRE(on_code.bytes + vixl::aarch64::kInstructionSize == off_code.bytes);
        REQUIRE(on_code.text.find("orr") == std::string::npos);

        auto untied = make_case(op, 0xd500 + index * 0x20, true);
        auto untied_off = allocate(untied.block.get(), false);
        auto untied_on = allocate(untied.block.get(), true);
        REQUIRE(untied_on->ValueFPR(untied.produced).id != target);
        REQUIRE_FALSE(untied_on->IsHostWriteCoalesced(untied.publish->Id()));
        auto untied_off_code = emit(
                untied.block.get(), *untied_off, 0xe500 + index * 0x20, false);
        auto untied_on_code = emit(
                untied.block.get(), *untied_on, 0xe500 + index * 0x20, true);
        REQUIRE(untied_on_code.bytes == untied_off_code.bytes);
        REQUIRE(untied_on_code.text == untied_off_code.text);
        ++index;
    }

    SECTION("a second read keeps the old resident value live") {
        for (bool read_before : {false, true}) {
            CAPTURE(read_before);
            IntrusivePtr<Block> block{new Block(0, Location{0x10300})};
            auto left = block->GetHostFPR(HostRegIndex(target), Imm{0u})
                                .SetType(ValueType::V128);
            Value retained;
            if (read_before) {
                retained = block->GetHostFPR(HostRegIndex(target), Imm{0u})
                                   .SetType(ValueType::V128);
            }
            auto right = block->LoadUniform(Uniform{16, ValueType::V128})
                                 .SetType(ValueType::V128);
            auto result = block->VecFAddScalar32(left, right).SetType(ValueType::V128);
            if (!read_before) {
                retained = block->GetHostFPR(HostRegIndex(target), Imm{0u})
                                   .SetType(ValueType::V128);
            }
            block->StoreUniform(Uniform{32, ValueType::V128}, retained);
            auto* publish = block->AppendInst(
                    OpCode::SetHostFPR, result, HostRegIndex(target), Imm{0u});
            block->SetTerminal(terminal::ReturnToDispatch{});
            block->ReIdInstr();
            auto alloc = allocate(block.get(), true);
            REQUIRE(alloc->ValueFPR(left).id == target);
            REQUIRE(alloc->ValueFPR(retained).id == target);
            REQUIRE(alloc->ValueFPR(result).id != target);
            REQUIRE_FALSE(alloc->IsHostWriteCoalesced(publish->Id()));
        }
    }

    SECTION("a scalar chain keeps the resident destination until publication") {
        IntrusivePtr<Block> block{new Block(0, Location{0x10400})};
        auto left = block->GetHostFPR(HostRegIndex(target), Imm{0u}).SetType(ValueType::V128);
        auto right = block->LoadUniform(Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto first = block->VecFMulScalar64(left, right).SetType(ValueType::V128);
        auto* first_publish = block->AppendInst(
                OpCode::SetHostFPR, first, HostRegIndex(target), Imm{0u});
        auto second = block->VecFAddScalar64(first, right).SetType(ValueType::V128);
        auto* second_publish = block->AppendInst(
                OpCode::SetHostFPR, second, HostRegIndex(target), Imm{0u});
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        auto off = allocate(block.get(), false);
        auto off_code = emit(block.get(), *off, 0x11400, false);
        auto on = allocate(block.get(), true);
        REQUIRE(on->ValueFPR(first).id == target);
        REQUIRE(on->ValueFPR(second).id == target);
        REQUIRE(on->IsHostWriteCoalesced(first_publish->Id()));
        REQUIRE(on->IsHostWriteCoalesced(second_publish->Id()));
        auto on_code = emit(block.get(), *on, 0x12400, true);
        INFO("OFF:\n" << off_code.text << "ON:\n" << on_code.text);
        REQUIRE(on_code.bytes + 2 * vixl::aarch64::kInstructionSize == off_code.bytes);
    }

    SECTION("a scalar chain crosses an in-place unary producer") {
        IntrusivePtr<Block> block{new Block(0, Location{0x10480})};
        auto left = block->GetHostFPR(HostRegIndex(target), Imm{0u})
                            .SetType(ValueType::V128);
        auto right = block->LoadUniform(Uniform{16, ValueType::V128})
                             .SetType(ValueType::V128);
        auto first = block->VecFMulScalar32(left, right).SetType(ValueType::V128);
        auto unary = block->VecFUnary(first, first, Imm{32u}, Imm{0u}, Imm{1u})
                             .SetType(ValueType::V128);
        auto second = block->VecFMulScalar32(unary, right).SetType(ValueType::V128);
        auto* publish = block->AppendInst(
                OpCode::SetHostFPR, second, HostRegIndex(target), Imm{0u});
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        auto on = allocate(block.get(), true);
        REQUIRE(on->ValueFPR(first).id == target);
        REQUIRE(on->ValueFPR(unary).id == target);
        REQUIRE(on->ValueFPR(second).id == target);
        REQUIRE(on->IsHostWriteCoalesced(publish->Id()));
        REQUIRE_NOTHROW(emit(block.get(), *on, 0x12480, true));
    }

    SECTION("a third-party fixed-home write rejects the scalar tie") {
        auto item = make_case(OpCode::VecFAddScalar64, 0x10500, false, true);
        auto features = FeatureSet{};
        features.sse_scalar_tie = true;
        RegAlloc alloc{item.block->MaxInstrId(), gprs, resident_fprs(), features};
        RegisterAllocTestSupport::RunForScalarFPRTieConflictTest(
                item.block.get(), &alloc, item.conflict.Id(), target);
        REQUIRE(alloc.ValueFPR(item.conflict).id == target);
        REQUIRE_FALSE(alloc.IsHostWriteCoalesced(item.publish->Id()));
    }
}

TEST_CASE("SHUFPS hot immediates tie only an exact last-use destination") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    const GPRSMask gprs{~((1u << 8) - 1u)};
    struct Case {
        IntrusivePtr<Block> block;
        Value left;
        Value result;
    };
    auto make_case = [](swift::u32 control, swift::u64 location,
                        bool alias, bool keep_left_live) {
        IntrusivePtr<Block> block{new Block(0, Location{location})};
        auto left = block->LoadUniform(Uniform{0, ValueType::V128})
                            .SetType(ValueType::V128);
        auto right = alias ? left : block->LoadUniform(Uniform{16, ValueType::V128})
                                          .SetType(ValueType::V128);
        auto result = block->VecShuffle32TwoSrc(
                                   left, right, Imm{swift::u64{control}})
                              .SetType(ValueType::V128);
        block->StoreUniform(Uniform{32, ValueType::V128}, result);
        if (keep_left_live) {
            block->StoreUniform(Uniform{48, ValueType::V128}, left);
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        return Case{std::move(block), left, result};
    };
    auto allocate = [&](Block* block, bool enabled) {
        auto features = FeatureSet{};
        features.sse_shufps_imm = enabled;
        auto alloc = std::make_unique<RegAlloc>(
                block->MaxInstrId(), gprs, FPRSMask{0}, features);
        RegisterAllocTestSupport::RunForShufpsImmTieTest(block, alloc.get(), enabled);
        return alloc;
    };
    struct Emitted {
        std::size_t bytes{};
        std::string text;
    };
    auto emit = [&](Block* block, RegAlloc& alloc,
                    swift::u64 location, bool enabled) {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        ModuleConfig module_config{};
        module_config.feature_overrides.Set(FeatureId::sse_shufps_imm, enabled);
        auto module = address_space.MapModule(
                LocationDescriptor{location}, LocationDescriptor{location + 0x10},
                module_config);
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block);
        context.Finish();
        Emitted emitted{.bytes = context.CurrentBufferSize()};
        auto& masm = context.GetMasm();
        auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
        auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        for (auto* instruction = first; instruction < last;
             instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction);
            emitted.text += disassembler.GetOutput();
            emitted.text += '\n';
        }
        return emitted;
    };

    const auto check = [&](swift::u32 control, bool alias,
                           std::size_t saved_instructions) {
        auto item = make_case(control, 0x11500 + control, alias, false);
        auto off = allocate(item.block.get(), false);
        auto on = allocate(item.block.get(), true);
        REQUIRE(off->ValueFPR(item.result).id != off->ValueFPR(item.left).id);
        REQUIRE(on->ValueFPR(item.result).id == on->ValueFPR(item.left).id);
        auto off_code = emit(item.block.get(), *off, 0x12500 + control, false);
        auto on_code = emit(item.block.get(), *on, 0x13500 + control, true);
        INFO("OFF:\n" << off_code.text << "ON:\n" << on_code.text);
        REQUIRE(on_code.bytes + saved_instructions * vixl::aarch64::kInstructionSize ==
                off_code.bytes);
        return on_code;
    };

    for (swift::u32 control : {0xe0u, 0xe5u}) {
        auto code = check(control, true, 1);
        REQUIRE(code.text.find("mov v") != std::string::npos);
        REQUIRE(code.text.find("tbl") == std::string::npos);
    }
    auto alias_direct = check(0xe4, true, 1);
    REQUIRE(alias_direct.text.find("tbl") == std::string::npos);
    auto distinct_pair = check(0xe4, false, 12);
    REQUIRE(distinct_pair.text.find("mov v") != std::string::npos);
    REQUIRE(distinct_pair.text.find("tbl") == std::string::npos);

    SECTION("a live source keeps the old bytes") {
        for (const auto [control, alias] :
             {std::pair{0xe0u, true}, std::pair{0xe5u, true},
              std::pair{0xe4u, false}}) {
            auto item = make_case(control, 0x14500 + control, alias, true);
            auto off = allocate(item.block.get(), false);
            auto on = allocate(item.block.get(), true);
            REQUIRE(on->ValueFPR(item.result).id != on->ValueFPR(item.left).id);
            auto off_code = emit(item.block.get(), *off, 0x15500 + control, false);
            auto on_code = emit(item.block.get(), *on, 0x15500 + control, true);
            REQUIRE(on_code.bytes == off_code.bytes);
            REQUIRE(on_code.text == off_code.text);
        }
    }

    SECTION("an unsupported two-source immediate keeps the old bytes") {
        auto item = make_case(0x55, 0x16555, false, false);
        auto off = allocate(item.block.get(), false);
        auto on = allocate(item.block.get(), true);
        REQUIRE(on->ValueFPR(item.result).id != on->ValueFPR(item.left).id);
        auto off_code = emit(item.block.get(), *off, 0x17555, false);
        auto on_code = emit(item.block.get(), *on, 0x17555, true);
        REQUIRE(on_code.bytes == off_code.bytes);
        REQUIRE(on_code.text == off_code.text);
    }

    SECTION("a resident source keeps its fixed home intact") {
        constexpr swift::u16 target = 19;
        IntrusivePtr<Block> block{new Block(0, Location{0x18500})};
        auto left = block->GetHostFPR(HostRegIndex(target), Imm{0u})
                            .SetType(ValueType::V128);
        auto result = block->VecShuffle32TwoSrc(
                                   left, left, Imm{swift::u64{0xe5}})
                              .SetType(ValueType::V128);
        block->StoreUniform(Uniform{32, ValueType::V128}, result);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        auto allocate_resident = [&](bool enabled) {
            auto features = FeatureSet{};
            features.sse_shufps_imm = enabled;
            FPRSMask resident{0};
            resident.Mark(target);
            auto alloc = std::make_unique<RegAlloc>(
                    block->MaxInstrId(), gprs, resident, features);
            RegisterAllocTestSupport::RunForShufpsImmTieTest(
                    block.get(), alloc.get(), enabled);
            return alloc;
        };
        auto off = allocate_resident(false);
        auto on = allocate_resident(true);
        REQUIRE(on->ValueFPR(left).id == target);
        REQUIRE(on->IsHostReadCoalesced(left.Id()));
        REQUIRE(on->ValueFPR(result).id != target);
        auto off_code = emit(block.get(), *off, 0x19500, false);
        auto on_code = emit(block.get(), *on, 0x19500, true);
        REQUIRE(on_code.bytes == off_code.bytes);
        REQUIRE(on_code.text == off_code.text);
    }
}
