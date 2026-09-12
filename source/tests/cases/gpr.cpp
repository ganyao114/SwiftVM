#include "../support/case_support.h"
#include "../support/fp_environment.h"

TEST_CASE("Register allocation gives every spilled value a private slot") {
    using namespace swift::runtime::ir;
    using swift::runtime::backend::RegAlloc;
    using swift::runtime::backend::GPRSMask;
    using swift::runtime::backend::FPRSMask;

    // Two live values sharing one State::spill_area slot is a silent
    // miscompile: both Str/Ldr target the same address, so each reload
    // observes the other's value. Squeeze the linear scan onto its spill path
    // and assert the slots are disjoint, counting a SIMD spill as the two u64
    // slots its 16-byte access actually covers.
    auto check_disjoint_slots = [](int n_scalar,
                                   int n_vector,
                                   std::uint32_t free_gprs,
                                   std::uint32_t free_fprs) {
        Block block{0, Location{0x1000}};
        std::vector<Value> scalars;
        for (int i = 0; i < n_scalar; i++) {
            scalars.push_back(block.LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
        }
        std::vector<Value> vectors;
        for (int i = 0; i < n_vector; i++) {
            vectors.push_back(block.LoadImm<TypedValue<ValueType::V128>>(
                    Imm{static_cast<std::uint64_t>(i + 0x100)}));
        }
        // Every value needs a use to own a live interval, and consuming them
        // in reverse definition order keeps them all live at once.
        for (int i = n_vector - 1; i >= 0; i--) {
            block.VecAdd<TypedValue<ValueType::V128>>(vectors[i], vectors[i], Imm{32u});
        }
        if (!scalars.empty()) {
            Value acc = scalars.back();
            for (int i = n_scalar - 2; i >= 0; i--) {
                acc = block.Add(acc, Operand{scalars[i]});
            }
        }
        block.ReIdInstr();  // AppendInst does not assign ids; the pass indexes by id

        // bit SET = unavailable; leave only the lowest `free_*` clear.
        GPRSMask gprs{~((1u << free_gprs) - 1u)};
        FPRSMask fprs{~((1u << free_fprs) - 1u)};
        RegAlloc reg_alloc{0x200, gprs, fprs, FeatureSet{}};
        RegisterAllocPass::Run(&block, &reg_alloc, false, FeatureSet{});

        std::map<std::uint32_t, int> occupancy;
        int spilled = 0;
        auto record = [&](const std::vector<Value>& values, int slot_width) {
            for (auto& value : values) {
                if (reg_alloc.ValueType(value) != RegAlloc::MEM) {
                    continue;
                }
                spilled++;
                const std::uint32_t slot = reg_alloc.ValueMem(value).offset;
                for (int k = 0; k < slot_width; k++) {
                    occupancy[slot + k]++;
                }
            }
        };
        record(scalars, 1);
        record(vectors, 2);

        REQUIRE(spilled >= 2);  // the case must actually reach the spill path
        for (auto& [slot, owners] : occupancy) {
            INFO("spill slot " << slot << " claimed by " << owners << " live values");
            REQUIRE(owners == 1);
        }
    };

    check_disjoint_slots(12, 0, 2, 8);  // scalar-only pressure
    check_disjoint_slots(0, 10, 8, 2);  // vector-only pressure
    check_disjoint_slots(10, 6, 2, 2);  // mixed
    check_disjoint_slots(5, 5, 1, 1);   // odd-sized stack before a SIMD pair
}

TEST_CASE("integer width ties require exact last-use and a proven W write") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    const GPRSMask gprs{~((1u << 19) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};

    auto check = [&](Block* raw,
                     Value source,
                     Value result,
                     bool expect_off_tie,
                     bool expect_on_tie) {
        swift::runtime::IntrusivePtr<Block> block{raw};
        raw->SetTerminal(terminal::ReturnToDispatch{});
        raw->ReIdInstr();

        RegAlloc off{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocTestSupport::RunForIntWidthTieTest(raw, &off, false);
        REQUIRE(off.ValueType(source) == RegAlloc::GPR);
        REQUIRE(off.ValueType(result) == RegAlloc::GPR);
        REQUIRE((off.ValueGPR(source).id == off.ValueGPR(result).id) ==
                expect_off_tie);

        RegAlloc on{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocTestSupport::RunForIntWidthTieTest(raw, &on, true);
        REQUIRE(on.ValueType(source) == RegAlloc::GPR);
        REQUIRE(on.ValueType(result) == RegAlloc::GPR);
        REQUIRE((on.ValueGPR(source).id == on.ValueGPR(result).id) ==
                expect_on_tie);
    };

    SECTION("U32 load producer") {
        auto* block = new Block(0, Location{0x8600});
        auto source = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, false, true);
    }

    SECTION("U32 ALU producer") {
        auto* block = new Block(0, Location{0x8610});
        auto left = block->LoadImm(Imm{0xffffffffu}).SetType(ValueType::U32);
        auto right = block->LoadImm(Imm{0x12345678u}).SetType(ValueType::U32);
        auto source = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, false, true);
    }

    SECTION("U32 shift producer remains compatible with the existing tie") {
        auto* block = new Block(0, Location{0x8620});
        auto input = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto source = block->LsrImm(input, Imm{3u}).SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, true, true);
    }

    SECTION("two consecutive direct ties keep the original W producer") {
        auto* block = new Block(0, Location{0x8630});
        auto left = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto right = block->LoadImm(Imm{7u}).SetType(ValueType::U32);
        auto producer = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto extract = block->BitExtract(producer, Imm{0u}, Imm{32u})
                               .SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(extract);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);

        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        RegAlloc off{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocTestSupport::RunForIntWidthTieTest(block, &off, false);
        REQUIRE(off.ValueGPR(producer).id != off.ValueGPR(extract).id);
        REQUIRE(off.ValueGPR(extract).id != off.ValueGPR(result).id);

        RegAlloc on{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocTestSupport::RunForIntWidthTieTest(block, &on, true);
        REQUIRE(on.ValueGPR(producer).id == on.ValueGPR(extract).id);
        REQUIRE(on.ValueGPR(extract).id == on.ValueGPR(result).id);
    }

    SECTION("GetHostGPR is a W view, not a W write") {
        auto* block = new Block(0, Location{0x8640});
        auto source = block->GetHostGPR(HostRegIndex(0), Imm{0u})
                              .SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, false, false);
    }

    SECTION("direct BitExtract cannot launder a GetHostGPR high half") {
        auto* block = new Block(0, Location{0x8650});
        auto host = block->GetHostGPR(HostRegIndex(0), Imm{0u})
                            .SetType(ValueType::U32);
        auto source = block->BitExtract(host, Imm{0u}, Imm{32u})
                              .SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, false, false);
    }

    SECTION("pure BitCast is not a physical W write") {
        auto* block = new Block(0, Location{0x8660});
        auto producer = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto source = block->BitCast(producer).SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, false, false);
    }

    SECTION("a later source use defeats the authoritative last-use proof") {
        auto* block = new Block(0, Location{0x8670});
        auto left = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto right = block->LoadImm(Imm{1u}).SetType(ValueType::U32);
        auto source = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        auto late = block->Add(source, Operand{right}).SetType(ValueType::U32);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        block->StoreUniform(Uniform{16, ValueType::U32}, late);
        check(block, source, result, false, false);
    }
}

TEST_CASE("pinned W-view inputs hand off only to a published destructive result") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    auto make_gprs = [] {
        GPRSMask result{swift::u32{0}};
        for (swift::u32 code :
             {0u, 1u, 2u, 3u, 4u, 5u, 19u, 20u, 21u, 22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u}) {
            result.Mark(code);
        }
        return result;
    };
    const FPRSMask fprs{~((1u << 8) - 1u)};
    auto allocate = [&](bool keep_source_live) {
        IntrusivePtr<Block> block{new Block(0, Location{0x8678})};
        auto seed = block->LoadImm(Imm{swift::u32{7}}).SetType(ValueType::U32);
        auto source = block->ZeroExtend32To64(seed).SetType(ValueType::U64);
        block->SetHostGPR(source, HostRegIndex(22), Imm{0u});
        block->AdvancePC(Imm{1u});
        auto bridge = block->BitExtract(source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
        auto right = block->LoadImm(Imm{swift::u32{3}}).SetType(ValueType::U32);
        auto product = block->Mul(bridge, Operand{right}).SetType(ValueType::U32);
        auto published = block->ZeroExtend32To64(product).SetType(ValueType::U64);
        block->SetHostGPR(published, HostRegIndex(22), Imm{0u});
        if (keep_source_live) {
            block->StoreUniform(Uniform{8, ValueType::U64}, source);
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        auto features = FeatureSet{};
        REQUIRE_FALSE(features.ra_width_chain);
        auto alloc = std::make_unique<RegAlloc>(block->MaxInstrId(), make_gprs(), fprs, features);
        RegisterAllocPass::Run(block.get(), alloc.get(), false, features);
        return std::tuple{std::move(block), std::move(alloc), bridge, product};
    };

    auto [accepted_block, accepted, accepted_bridge, accepted_product] = allocate(false);
    REQUIRE(accepted->IsWidthChainCoalesced(accepted_bridge.Id()));
    REQUIRE(accepted->ValueGPR(accepted_bridge).id == 22);
    REQUIRE(accepted->ValueGPR(accepted_product).id == 22);

    auto [rejected_block, rejected, rejected_bridge, rejected_product] = allocate(true);
    REQUIRE_FALSE(rejected->IsWidthChainCoalesced(rejected_bridge.Id()));
}

TEST_CASE("unit-local width chains share only proven W-clean direct components") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    auto make_gprs = [] {
        GPRSMask result{swift::u32{0}};
        for (swift::u32 code : {0u, 1u, 2u, 3u, 4u, 5u, 19u, 20u, 21u,
                                22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u}) {
            result.Mark(code);
        }
        return result;
    };
    const FPRSMask fprs{~((1u << 8) - 1u)};

    struct Chain {
        IntrusivePtr<Block> block;
        Value producer;
        Value extract;
        Value extend;
    };
    auto make_chain = [](swift::u64 location) {
        IntrusivePtr<Block> block{new Block(0, Location{location})};
        auto left = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto right = block->LoadImm(Imm{swift::u32{7}}).SetType(ValueType::U32);
        auto producer = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto extract = block->BitExtract(producer, Imm{0u}, Imm{32u})
                               .SetType(ValueType::U32);
        auto extend = block->ZeroExtend32To64(extract).SetType(ValueType::U64);
        // Both predecessors remain live beyond the next bridge. The old
        // exact-last-use tie therefore cannot remove either instruction.
        block->StoreUniform(Uniform{8, ValueType::U64}, extend);
        block->StoreUniform(Uniform{16, ValueType::U32}, extract);
        block->StoreUniform(Uniform{20, ValueType::U32}, producer);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        return Chain{std::move(block), producer, extract, extend};
    };
    auto allocate = [&](Chain& chain, bool enabled) {
        auto features = FeatureSet{};
        features.ra_width_chain = enabled;
        features.ra_width_chain_long = false;
        features.ra_coalesce = false;
        auto alloc = std::make_unique<RegAlloc>(
                chain.block->MaxInstrId(), make_gprs(), fprs, features);
        RegisterAllocTestSupport::RunForWidthChainTest(
                chain.block.get(), alloc.get(), enabled);
        return alloc;
    };

    struct LongChain {
        IntrusivePtr<Block> block;
        Vector<Value> bridges;
        Value conflict;
    };
    auto make_long_chain = [](swift::u64 location, swift::u32 steps,
                              bool insert_fault_barrier, bool add_conflict = false) {
        IntrusivePtr<Block> block{new Block(0, Location{location})};
        Value conflict{};
        if (add_conflict) {
            conflict = block->LoadImm(Imm{swift::u64{0xfeedfacecafebeef}})
                               .SetType(ValueType::U64);
        }
        auto invariant = block->GetHostGPR(HostRegIndex(29), Imm{0u})
                                 .SetType(ValueType::U32);
        Value current = block->GetHostGPR(HostRegIndex(22), Imm{0u})
                                .SetType(ValueType::U32);
        Vector<Value> bridges{};
        for (swift::u32 index = 0; index < steps; ++index) {
            Value left = current;
            Value right = invariant;
            if (index != 0) {
                left = block->BitExtract(current, Imm{0u}, Imm{32u})
                               .SetType(ValueType::U32);
                right = block->BitExtract(invariant, Imm{0u}, Imm{32u})
                                .SetType(ValueType::U32);
                bridges.push_back(left);
                bridges.push_back(right);
            }
            auto producer = (index & 1u)
                    ? block->Xor(left, Operand{right}).SetType(ValueType::U32)
                    : block->Add(left, Operand{right}).SetType(ValueType::U32);
            auto wrapper = block->ZeroExtend32To64(producer).SetType(ValueType::U64);
            block->SetHostGPR(wrapper, HostRegIndex(22), Imm{0u});
            current = wrapper;
            if (insert_fault_barrier && index + 1 == steps / 2) {
                auto address = block->LoadImm(Imm{swift::u64{0x1000}})
                                       .SetType(ValueType::U64);
                (void)block->LoadMemory(Operand{address}).SetType(ValueType::U64);
            }
        }
        if (conflict.Defined()) {
            block->StoreUniform(Uniform{24, ValueType::U64}, conflict);
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        return LongChain{std::move(block), std::move(bridges), conflict};
    };
    auto allocate_long = [&](LongChain& chain, bool enabled) {
        auto features = FeatureSet{};
        features.ra_width_chain = false;
        features.ra_width_chain_long = enabled;
        auto alloc = std::make_unique<RegAlloc>(
                chain.block->MaxInstrId(), make_gprs(), fprs, features);
        RegisterAllocPass::Run(chain.block.get(), alloc.get(), false, features);
        return alloc;
    };

    SECTION("a multi-node direct chain forms one ownership component") {
        auto chain = make_chain(0x8680);
        auto off = allocate(chain, false);
        REQUIRE_FALSE(off->IsWidthChainCoalesced(chain.extract.Id()));
        REQUIRE_FALSE(off->IsWidthChainCoalesced(chain.extend.Id()));
        REQUIRE(off->ValueGPR(chain.producer).id != off->ValueGPR(chain.extract).id);

        auto on = allocate(chain, true);
        REQUIRE(on->IsWidthChainCoalesced(chain.extract.Id()));
        REQUIRE(on->IsWidthChainCoalesced(chain.extend.Id()));
        REQUIRE(on->WidthChainAnchor(chain.extract.Id()) == chain.producer.Id());
        REQUIRE(on->WidthChainAnchor(chain.extend.Id()) == chain.producer.Id());
        REQUIRE(on->ValueGPR(chain.producer).id == on->ValueGPR(chain.extract).id);
        REQUIRE(on->ValueGPR(chain.extract).id == on->ValueGPR(chain.extend).id);
    }

    SECTION("emitter reproof removes exactly two bridge instructions") {
        auto emit = [&](bool enabled) {
            Config config{
                    .loc_start = 0,
                    .loc_end = 1ull << 48,
                    .enable_jit = true,
                    .has_local_operation = false,
                    .backend_isa = kArm64,
            };
            AddressSpace address_space{config};
            ModuleConfig module_config{};
            module_config.feature_overrides.Set(FeatureId::ra_width_chain, enabled);
            module_config.feature_overrides.Set(FeatureId::ra_width_chain_long, false);
            module_config.feature_overrides.Set(FeatureId::ra_coalesce, false);
            auto module = address_space.MapModule(
                    LocationDescriptor{0x8690}, LocationDescriptor{0x86b0}, module_config);
            auto chain = make_chain(0x8690);
            auto features = ResolveFeatureSet(module_config);
            RegAlloc alloc{chain.block->MaxInstrId(), make_gprs(), fprs, features};
            RegisterAllocPass::Run(chain.block.get(), &alloc, false, features);
            arm64::JitContext context{module, alloc};
            arm64::JitTranslator translator{context};
            translator.Translate(chain.block.get());
            context.Finish();
            return context.CurrentBufferSize();
        };
        REQUIRE(emit(true) + 2 * vixl::aarch64::kInstructionSize == emit(false));
    }

    SECTION("GetHostGPR cannot launder an unknown high half") {
        IntrusivePtr<Block> block{new Block(0, Location{0x86c0})};
        auto host = block->GetHostGPR(HostRegIndex(22), Imm{0u})
                            .SetType(ValueType::U32);
        auto extract = block->BitExtract(host, Imm{0u}, Imm{32u})
                              .SetType(ValueType::U32);
        auto extend = block->ZeroExtend32To64(extract).SetType(ValueType::U64);
        block->StoreUniform(Uniform{8, ValueType::U64}, extend);
        block->StoreUniform(Uniform{16, ValueType::U32}, extract);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        auto features = FeatureSet{};
        features.ra_width_chain = true;
        RegAlloc alloc{block->MaxInstrId(), make_gprs(), fprs, features};
        RegisterAllocTestSupport::RunForWidthChainTest(block.get(), &alloc, true);
        REQUIRE_FALSE(alloc.IsWidthChainCoalesced(extract.Id()));
        REQUIRE_FALSE(alloc.IsWidthChainCoalesced(extend.Id()));
    }

    SECTION("8 and 16 bit partial definitions never enter a 32-bit component") {
        IntrusivePtr<Block> block{new Block(0, Location{0x86d0})};
        auto narrow = block->LoadUniform<TypedValue<ValueType::U16>>(
                Uniform{0, ValueType::U16});
        auto fake = block->BitExtract(narrow, Imm{0u}, Imm{32u})
                           .SetType(ValueType::U32);
        block->StoreUniform(Uniform{8, ValueType::U32}, fake);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        auto features = FeatureSet{};
        features.ra_width_chain = true;
        RegAlloc alloc{block->MaxInstrId(), make_gprs(), fprs, features};
        RegisterAllocTestSupport::RunForWidthChainTest(block.get(), &alloc, true);
        REQUIRE_FALSE(alloc.IsWidthChainCoalesced(fake.Id()));
    }

    SECTION("a third-party tied interval crossing the window rejects the bridge") {
        IntrusivePtr<Block> block{new Block(0, Location{0x86e0})};
        auto left = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto right = block->LoadImm(Imm{swift::u32{7}}).SetType(ValueType::U32);
        auto producer = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto extract = block->BitExtract(producer, Imm{0u}, Imm{32u})
                               .SetType(ValueType::U32);
        auto tied = block->LoadImm(Imm{swift::u64{0xfeedfacecafebeef}})
                            .SetType(ValueType::U64);
        block->StoreUniform(Uniform{8, ValueType::U32}, extract);
        block->StoreUniform(Uniform{16, ValueType::U32}, producer);
        block->StoreUniform(Uniform{24, ValueType::U64}, tied);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        auto features = FeatureSet{};
        features.ra_width_chain = false;
        RegAlloc control{block->MaxInstrId(), make_gprs(), fprs, features};
        RegisterAllocTestSupport::RunForWidthChainTest(block.get(), &control, false);
        const auto target = control.ValueGPR(producer).id;

        RegAlloc conflict{block->MaxInstrId(), make_gprs(), fprs, features};
        RegisterAllocTestSupport::RunForWidthChainConflictTest(
                block.get(), &conflict, tied.Id(), target);
        REQUIRE_FALSE(conflict.IsWidthChainCoalesced(extract.Id()));
    }

    SECTION("a fixed-home write inside the alias window rejects the bridge") {
        IntrusivePtr<Block> block{new Block(0, Location{0x86f0})};
        auto left = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto right = block->LoadImm(Imm{swift::u32{3}}).SetType(ValueType::U32);
        auto producer = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto extract = block->BitExtract(producer, Imm{0u}, Imm{32u})
                               .SetType(ValueType::U32);
        auto replacement = block->LoadImm(Imm{swift::u64{99}}).SetType(ValueType::U64);
        block->SetHostGPR(replacement, HostRegIndex(22), Imm{0u});
        block->StoreUniform(Uniform{8, ValueType::U32}, extract);
        block->StoreUniform(Uniform{16, ValueType::U32}, producer);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        auto features = FeatureSet{};
        RegAlloc alloc{block->MaxInstrId(), make_gprs(), fprs, features};
        RegisterAllocTestSupport::RunForWidthChainConflictTest(
                block.get(), &alloc, producer.Id(), 22);
        REQUIRE_FALSE(alloc.IsWidthChainCoalesced(extract.Id()));
    }

    SECTION("an emitter fixed clobber inside the alias window rejects the bridge") {
        IntrusivePtr<Block> block{new Block(0, Location{0x8700})};
        auto left = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto right = block->LoadImm(Imm{swift::u32{3}}).SetType(ValueType::U32);
        auto producer = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto extract = block->BitExtract(producer, Imm{0u}, Imm{32u})
                               .SetType(ValueType::U32);
        auto arg = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
        (void)block->CallLambda(Lambda{Imm{swift::u64{0}}}, arg, arg, arg)
                .SetType(ValueType::U64);
        block->StoreUniform(Uniform{8, ValueType::U32}, extract);
        block->StoreUniform(Uniform{16, ValueType::U32}, producer);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        auto features = FeatureSet{};
        RegAlloc alloc{block->MaxInstrId(), make_gprs(), fprs, features};
        RegisterAllocTestSupport::RunForWidthChainConflictTest(
                block.get(), &alloc, producer.Id(), 11);
        REQUIRE_FALSE(alloc.IsWidthChainCoalesced(extract.Id()));
    }

    SECTION("a faulting observer does not publish or mutate the read-only component") {
        IntrusivePtr<Block> block{new Block(0, Location{0x8710})};
        auto left = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto right = block->LoadImm(Imm{swift::u32{3}}).SetType(ValueType::U32);
        auto producer = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto extract = block->BitExtract(producer, Imm{0u}, Imm{32u})
                               .SetType(ValueType::U32);
        auto address = block->LoadImm(Imm{swift::u64{0x1000}}).SetType(ValueType::U64);
        (void)block->LoadMemory(Operand{address}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{8, ValueType::U32}, extract);
        block->StoreUniform(Uniform{16, ValueType::U32}, producer);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        auto features = FeatureSet{};
        features.ra_width_chain = true;
        RegAlloc alloc{block->MaxInstrId(), make_gprs(), fprs, features};
        RegisterAllocTestSupport::RunForWidthChainTest(block.get(), &alloc, true);
        REQUIRE(alloc.IsWidthChainCoalesced(extract.Id()));
    }

    SECTION("long Add/Xor publication components coalesce both low-W captures") {
        auto chain = make_long_chain(0x8720, 32, false);
        auto off = allocate_long(chain, false);
        REQUIRE(std::none_of(chain.bridges.begin(), chain.bridges.end(),
                             [&](Value bridge) {
                                 return off->IsWidthChainCoalesced(bridge.Id());
                             }));

        auto on = allocate_long(chain, true);
        REQUIRE(std::all_of(chain.bridges.begin(), chain.bridges.end(),
                            [&](Value bridge) {
                                return on->IsWidthChainCoalesced(bridge.Id());
                            }));
    }

    SECTION("a 31-step chain stays below the long-component threshold") {
        auto chain = make_long_chain(0x8730, 31, false);
        auto on = allocate_long(chain, true);
        REQUIRE(std::none_of(chain.bridges.begin(), chain.bridges.end(),
                             [&](Value bridge) {
                                 return on->IsWidthChainCoalesced(bridge.Id());
                             }));
    }

    SECTION("a faulting memory operation splits the component fail-closed") {
        auto chain = make_long_chain(0x8740, 32, true);
        auto on = allocate_long(chain, true);
        REQUIRE(std::none_of(chain.bridges.begin(), chain.bridges.end(),
                             [&](Value bridge) {
                                 return on->IsWidthChainCoalesced(bridge.Id());
                             }));
    }

    SECTION("a third-party interval crossing a long capture still rejects it") {
        auto chain = make_long_chain(0x8748, 32, false, true);
        auto features = FeatureSet{};
        features.ra_width_chain = false;
        features.ra_width_chain_long = false;
        RegAlloc control{chain.block->MaxInstrId(), make_gprs(), fprs, features};
        RegisterAllocPass::Run(chain.block.get(), &control, false, features);
        auto invariant_bridge = chain.bridges.at(1);
        auto invariant_source = invariant_bridge.Def()->GetArg<Value>(0);
        const swift::u16 target = control.ValueGPR(invariant_source).id;

        RegAlloc conflict{chain.block->MaxInstrId(), make_gprs(), fprs, features};
        RegisterAllocTestSupport::RunForWidthChainLongConflictTest(
                chain.block.get(), &conflict, chain.conflict.Id(), target);
        REQUIRE_FALSE(conflict.IsWidthChainCoalesced(invariant_bridge.Id()));
    }

    SECTION("long-chain emission removes the remaining bridge instructions") {
        auto emit = [&](bool enabled) {
            Config config{
                    .loc_start = 0,
                    .loc_end = 1ull << 48,
                    .enable_jit = true,
                    .has_local_operation = false,
                    .backend_isa = kArm64,
            };
            AddressSpace address_space{config};
            ModuleConfig module_config{};
            module_config.feature_overrides.Set(FeatureId::ra_width_chain, false);
            module_config.feature_overrides.Set(FeatureId::ra_width_chain_long, enabled);
            auto module = address_space.MapModule(
                    LocationDescriptor{0x8750}, LocationDescriptor{0x8760}, module_config);
            auto chain = make_long_chain(0x8750, 32, false);
            auto features = ResolveFeatureSet(module_config);
            RegAlloc alloc{chain.block->MaxInstrId(), make_gprs(), fprs, features};
            RegisterAllocPass::Run(chain.block.get(), &alloc, false, features);
            arm64::JitContext context{module, alloc};
            arm64::JitTranslator translator{context};
            translator.Translate(chain.block.get());
            context.Finish();
            const auto remaining = std::ranges::count_if(chain.bridges, [&](Value bridge) {
                return !alloc.IsLow32CopyCoalesced(bridge.Id()) &&
                       !alloc.IsWidthChainCoalesced(bridge.Id());
            });
            return std::pair{context.CurrentBufferSize(), remaining};
        };
        const auto off = emit(false);
        const auto on = emit(true);
        REQUIRE(off.second > 0);
        REQUIRE(on.second == 0);
        REQUIRE(on.first + off.second * vixl::aarch64::kInstructionSize == off.first);
    }
}

TEST_CASE("64-bit induction ties require exact last-use and matching pinned publish") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    const GPRSMask gprs{~((1u << 19) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};

    auto check = [&](bool use_source_after_add,
                     swift::u16 publish_target,
                     bool active_flags,
                     bool expect_tie) {
        auto* raw = new Block(0, Location{0x86a0});
        swift::runtime::IntrusivePtr<Block> block{raw};
        auto source = raw->GetHostGPR(HostRegIndex(22), Imm{0u})
                              .SetType(ValueType::U64);
        auto immediate = raw->LoadImm(Imm{swift::u64{16}}).SetType(ValueType::U64);
        auto alias = raw->BitCast(source).SetType(ValueType::U64);
        auto result = raw->Add(alias, Operand{immediate}).SetType(ValueType::U64);
        if (active_flags) {
            raw->SaveFlags(result, Flags::All);
        }
        if (use_source_after_add) {
            raw->StoreUniform(Uniform{24, ValueType::U64}, source);
        }
        raw->SetHostGPR(result, HostRegIndex(publish_target), Imm{0u});
        raw->StoreUniform(Uniform{32, ValueType::U64}, result);
        raw->SetTerminal(terminal::ReturnToDispatch{});
        raw->ReIdInstr();

        RegAlloc off{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocTestSupport::RunForInductTieTest(raw, &off, false);
        REQUIRE(off.ValueGPR(source).id == 22);
        REQUIRE(off.ValueGPR(result).id != 22);

        RegAlloc on{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocTestSupport::RunForInductTieTest(raw, &on, true);
        REQUIRE(on.ValueGPR(source).id == 22);
        REQUIRE((on.ValueGPR(result).id == 22) == expect_tie);
    };

    SECTION("exact last use and matching target tie") {
        check(false, 22, false, true);
    }
    SECTION("source live after result start falls back") {
        check(true, 22, false, false);
    }
    SECTION("different architectural target falls back") {
        check(false, 23, false, false);
    }
    SECTION("live full-width flags reject destructive source") {
        check(false, 22, true, false);
    }

    SECTION("later pinned overwrite while result capture is live falls back") {
        auto* raw = new Block(0, Location{0x86b0});
        swift::runtime::IntrusivePtr<Block> block{raw};
        auto source = raw->GetHostGPR(HostRegIndex(22), Imm{0u})
                              .SetType(ValueType::U64);
        auto immediate = raw->LoadImm(Imm{swift::u64{32}}).SetType(ValueType::U64);
        auto result = raw->Add(source, Operand{immediate}).SetType(ValueType::U64);
        raw->SetHostGPR(result, HostRegIndex(22), Imm{0u});
        auto next = raw->Add(result, Operand{immediate}).SetType(ValueType::U64);
        raw->SetHostGPR(next, HostRegIndex(22), Imm{0u});
        raw->StoreUniform(Uniform{32, ValueType::U64}, result);
        raw->SetTerminal(terminal::ReturnToDispatch{});
        raw->ReIdInstr();

        RegAlloc on{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocTestSupport::RunForInductTieTest(raw, &on, true);
        REQUIRE(on.ValueGPR(source).id == 22);
        REQUIRE(on.ValueGPR(result).id != 22);
    }
}

TEST_CASE("fixed GPR class is level3-only and keeps fixed clobbers disjoint") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    GPRSMask gprs{0};
    for (swift::u32 code = 0; code < 32; ++code) {
        if ((kX86FixedGPRHomes & (1u << code)) || code == 10 || code == 18 ||
            (code >= 24 && code <= 28) || code >= 30) {
            gprs.Mark(code);
        }
    }
    const FPRSMask fprs{~((1u << 8) - 1u)};
    auto features = FeatureSet{};
    features.ra_fixed_class = true;

    REQUIRE(std::popcount(kX86FixedGPRHomes) == 16);
    REQUIRE(FixedGPRClassEnabled(gprs, features) ==
            (GetSvmConfig().x86_pin_ext >= 3));

    IntrusivePtr<Block> block{new Block(0, Location{0x86b8})};
    auto value = block->LoadImm(Imm{swift::u64{0x123456789abcdef0}})
                         .SetType(ValueType::U64);
    auto* publish = block->AppendInst(
            OpCode::SetHostGPR, value, HostRegIndex(22), Imm{0u});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    auto contract = ClassifyGPRContract(gprs, *value.Def(), features);
    REQUIRE(contract.value_pool == gprs.GetClearCount());
    REQUIRE((contract.fixed_clobber_mask & kX86FixedGPRHomes) == 0);
    REQUIRE(contract.hot_instruction_scratch ==
            ScratchBudget(*value.Def(), features).gpr);

    RegAlloc alloc{block->MaxInstrId(), gprs, fprs, features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    if (GetSvmConfig().x86_pin_ext >= 3) {
        REQUIRE(alloc.ValueGPR(value).id == 22);
        REQUIRE(alloc.IsFixedGPR(value.Id()));
        REQUIRE(alloc.IsHostWriteCoalesced(publish->Id()));
    } else {
        REQUIRE_FALSE(alloc.IsFixedGPR(value.Id()));
    }

    for (const auto op : {OpCode::CompareAndSwap,
                          OpCode::AtomicExchange,
                          OpCode::AtomicFetchAdd,
                          OpCode::AtomicRMW,
                          OpCode::CompareAndSwap128}) {
        const swift::u32 clobbers = FixedGPRClobbers(op, features);
        REQUIRE((clobbers & ((1u << 12) | (1u << 13))) ==
                ((1u << 12) | (1u << 13)));
        REQUIRE((clobbers & kX86FixedGPRHomes) == 0);
    }
}

TEST_CASE("guest GPR coalescing keeps publication and capture proofs local") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    auto pinned_gprs = [] {
        GPRSMask mask{0};
        for (swift::u32 code : {0u, 1u, 2u, 3u, 4u, 5u, 19u, 20u, 21u,
                                22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u}) {
            mask.Mark(code);
        }
        return mask;
    };
    const FPRSMask fprs{~((1u << 8) - 1u)};

    auto allocate = [&](Block* block, bool enabled) {
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        auto features = FeatureSet{};
        features.ra_coalesce = enabled;
        auto alloc = std::make_unique<RegAlloc>(
                block->MaxInstrId(), pinned_gprs(), fprs, features);
        RegisterAllocTestSupport::RunForCoalesceTest(block, alloc.get(), enabled);
        return alloc;
    };
    struct ExpandedProducer {
        OpCode op;
        const char* name;
    };
    constexpr std::array expanded_producers{
            ExpandedProducer{OpCode::LoadMemory, "LoadMemory"},
            ExpandedProducer{OpCode::GetHostFPR, "GetHostFPR"},
            ExpandedProducer{OpCode::Adc, "Adc"},
            ExpandedProducer{OpCode::Sbb, "Sbb"},
            ExpandedProducer{OpCode::Mul, "Mul"},
            ExpandedProducer{OpCode::Div, "Div"},
            ExpandedProducer{OpCode::AndNot, "AndNot"},
            ExpandedProducer{OpCode::Not, "Not"},
            ExpandedProducer{OpCode::Neg, "Neg"},
            ExpandedProducer{OpCode::GetOperand, "GetOperand"},
            ExpandedProducer{OpCode::SignExtend, "SignExtend"},
            ExpandedProducer{OpCode::LslImm, "LslImm"},
            ExpandedProducer{OpCode::LslValue, "LslValue"},
            ExpandedProducer{OpCode::LsrImm, "LsrImm"},
            ExpandedProducer{OpCode::LsrValue, "LsrValue"},
            ExpandedProducer{OpCode::AsrImm, "AsrImm"},
            ExpandedProducer{OpCode::AsrValue, "AsrValue"},
            ExpandedProducer{OpCode::RorImm, "RorImm"},
            ExpandedProducer{OpCode::RorValue, "RorValue"},
            ExpandedProducer{OpCode::ByteSwap, "ByteSwap"},
            ExpandedProducer{OpCode::BitExtract, "BitExtract"},
            ExpandedProducer{OpCode::BitClear, "BitClear"},
            ExpandedProducer{OpCode::Select, "Select"},
            ExpandedProducer{OpCode::SelectZero, "SelectZero"},
            ExpandedProducer{OpCode::CondSelect, "CondSelect"},
            ExpandedProducer{OpCode::MulHigh, "MulHigh"},
    };

    auto append_expanded_producer = [](Block* block, OpCode op,
                                       ValueType type) -> Value {
        auto load_scalar = [&](swift::u64 value) {
            return block->LoadImm(Imm{value}).SetType(type);
        };
        auto binary = [&](auto emit) {
            auto left = load_scalar(0x123456789abcdef0ull);
            auto right = load_scalar(0x0102030405060708ull);
            return emit(left, right).SetType(type);
        };
        switch (op) {
            case OpCode::LoadMemory:
                return block->LoadMemory(Operand{load_scalar(0x1000)})
                        .SetType(type);
            case OpCode::GetHostFPR:
                return block->GetHostFPR(HostRegIndex(24), Imm{0u})
                        .SetType(type);
            case OpCode::Adc:
                return binary([&](Value left, Value right) {
                    return block->Adc(left, Operand{right});
                });
            case OpCode::Sbb:
                return binary([&](Value left, Value right) {
                    return block->Sbb(left, Operand{right});
                });
            case OpCode::Mul:
                return binary([&](Value left, Value right) {
                    return block->Mul(left, Operand{right});
                });
            case OpCode::Div:
                return binary([&](Value left, Value right) {
                    return block->Div(left, Operand{right});
                });
            case OpCode::AndNot:
                return binary([&](Value left, Value right) {
                    return block->AndNot(left, Operand{right});
                });
            case OpCode::Not:
                return binary([&](Value left, Value right) {
                    return block->Not(left, Operand{right});
                });
            case OpCode::Neg:
                return block->Neg(load_scalar(0x123456789abcdef0ull)).SetType(type);
            case OpCode::GetOperand:
                return block->GetOperand(Operand{load_scalar(0x1000)}).SetType(type);
            case OpCode::SignExtend: {
                const auto input_type = type == ValueType::U64
                        ? ValueType::U32
                        : ValueType::U16;
                auto input = block->LoadImm(Imm{swift::u32{0x8001}})
                                     .SetType(input_type);
                return block->SignExtend(input).SetType(type);
            }
            case OpCode::LslImm:
                return block->LslImm(load_scalar(0x123456789abcdef0ull), Imm{5u})
                        .SetType(type);
            case OpCode::LsrImm:
                return block->LsrImm(load_scalar(0x123456789abcdef0ull), Imm{5u})
                        .SetType(type);
            case OpCode::AsrImm:
                return block->AsrImm(load_scalar(0x923456789abcdef0ull), Imm{5u})
                        .SetType(type);
            case OpCode::RorImm:
                return block->RorImm(load_scalar(0x123456789abcdef0ull), Imm{5u})
                        .SetType(type);
            case OpCode::LslValue:
                return block->LslValue(load_scalar(0x123456789abcdef0ull),
                                       load_scalar(5)).SetType(type);
            case OpCode::LsrValue:
                return block->LsrValue(load_scalar(0x123456789abcdef0ull),
                                       load_scalar(5)).SetType(type);
            case OpCode::AsrValue:
                return block->AsrValue(load_scalar(0x923456789abcdef0ull),
                                       load_scalar(5)).SetType(type);
            case OpCode::RorValue:
                return block->RorValue(load_scalar(0x123456789abcdef0ull),
                                       load_scalar(5)).SetType(type);
            case OpCode::ByteSwap:
                return block->ByteSwap(load_scalar(0x123456789abcdef0ull),
                                       Imm{type == ValueType::U64 ? 64u : 32u})
                        .SetType(type);
            case OpCode::BitExtract:
                return block->BitExtract(load_scalar(0x123456789abcdef0ull),
                                         Imm{4u},
                                         Imm{type == ValueType::U64 ? 60u : 28u})
                        .SetType(type);
            case OpCode::BitClear:
                return block->BitClear(load_scalar(0x123456789abcdef0ull),
                                       Imm{8u}, Imm{8u})
                        .SetType(type);
            case OpCode::Select: {
                auto cond = block->LoadImm(Imm{swift::u32{1}}).SetType(ValueType::U32);
                auto on_true = load_scalar(0x123456789abcdef0ull);
                auto on_false = load_scalar(0x0102030405060708ull);
                return block->Select(cond, on_true, on_false).SetType(type);
            }
            case OpCode::SelectZero: {
                auto test = load_scalar(1);
                auto zero_value = load_scalar(0x123456789abcdef0ull);
                auto nonzero_value = load_scalar(0x0102030405060708ull);
                return block->SelectZero(test, zero_value, nonzero_value).SetType(type);
            }
            case OpCode::CondSelect: {
                auto on_true = load_scalar(0x123456789abcdef0ull);
                auto on_false = load_scalar(0x0102030405060708ull);
                return block->CondSelect(Cond::EQ, on_true, on_false)
                        .SetType(type);
            }
            case OpCode::MulHigh:
                return binary([&](Value left, Value right) {
                    return block->MulHigh(left, right, Imm{0u});
                });
            default:
                return {};
        }
    };

    struct ExpandedCase {
        IntrusivePtr<Block> block;
        Value produced;
        Value conflict;
        Inst* publish;
    };
    auto make_expanded_case = [&](OpCode op, ValueType type, swift::u64 location) {
        IntrusivePtr<Block> block{new Block(0, Location{location})};
        auto produced = append_expanded_producer(block.get(), op, type);
        auto conflict = block->LoadImm(Imm{swift::u64{0xfeedfacecafebeef}})
                                .SetType(ValueType::U64);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, produced, HostRegIndex(22), Imm{0u});
        block->StoreUniform(Uniform{24, ValueType::U64}, conflict);
        return ExpandedCase{std::move(block), produced, conflict, publish};
    };

    SECTION("callee-saved RSP/RBX/RBP homes enter the same publication proof") {
        for (swift::u16 home : {19u, 20u, 21u}) {
            IntrusivePtr<Block> block{new Block(0, Location{0x86c8})};
            auto value = block->LoadImm(Imm{swift::u64{0x123456789abcdef0}})
                                 .SetType(ValueType::U64);
            auto* publish = block->AppendInst(
                    OpCode::SetHostGPR, value, HostRegIndex(home), Imm{0u});
            auto on = allocate(block.get(), true);
            REQUIRE(on->ValueGPR(value).id == home);
            REQUIRE(on->IsHostWriteCoalesced(publish->Id()));
        }
    }

    SECTION("last-use full-width publication enters the fixed home") {
        IntrusivePtr<Block> block{new Block(0, Location{0x86c0})};
        auto value = block->LoadImm(Imm{swift::u64{0x123456789abcdef0}})
                             .SetType(ValueType::U64);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, value, HostRegIndex(22), Imm{0u});
        auto off = allocate(block.get(), false);
        REQUIRE(off->ValueGPR(value).id != 22);
        REQUIRE_FALSE(off->IsHostWriteCoalesced(publish->Id()));
        auto on = allocate(block.get(), true);
        REQUIRE(on->ValueGPR(value).id == 22);
        REQUIRE(on->IsHostWriteCoalesced(publish->Id()));
    }

    SECTION("a later fixed-home write while the value is live rejects publication") {
        IntrusivePtr<Block> block{new Block(0, Location{0x86d0})};
        auto value = block->LoadImm(Imm{swift::u64{11}}).SetType(ValueType::U64);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, value, HostRegIndex(22), Imm{0u});
        auto next = block->LoadImm(Imm{swift::u64{22}}).SetType(ValueType::U64);
        block->SetHostGPR(next, HostRegIndex(22), Imm{0u});
        block->StoreUniform(Uniform{32, ValueType::U64}, value);
        auto on = allocate(block.get(), true);
        REQUIRE(on->ValueGPR(value).id != 22);
        REQUIRE_FALSE(on->IsHostWriteCoalesced(publish->Id()));
    }

    SECTION("live SSA after publish enters the home only with coalesce live") {
        auto make = [&] {
            IntrusivePtr<Block> block{new Block(0, Location{0x86d4})};
            auto value = block->LoadImm(Imm{swift::u64{11}}).SetType(ValueType::U64);
            auto* publish = block->AppendInst(
                    OpCode::SetHostGPR, value, HostRegIndex(22), Imm{0u});
            block->StoreUniform(Uniform{32, ValueType::U64}, value);
            return std::pair{std::move(block), publish};
        };
        auto [off_block, off_publish] = make();
        auto off = allocate(off_block.get(), true);
        REQUIRE(off->ValueGPR(Value{&off_block->GetInstList().front()}).id != 22);
        REQUIRE_FALSE(off->IsHostWriteCoalesced(off_publish->Id()));

        auto [on_block, on_publish] = make();
        on_block->SetTerminal(terminal::ReturnToDispatch{});
        on_block->ReIdInstr();
        auto on_alloc = std::make_unique<RegAlloc>(
                on_block->MaxInstrId(), pinned_gprs(), fprs, FeatureSet{});
        RegisterAllocTestSupport::RunForCoalesceLiveTest(on_block.get(), on_alloc.get(), true);
        auto value_on = Value{&on_block->GetInstList().front()};
        REQUIRE(on_alloc->ValueGPR(value_on).id == 22);
        REQUIRE(on_alloc->IsHostWriteCoalesced(on_publish->Id()));

        IntrusivePtr<Block> rewrite{new Block(0, Location{0x86d5})};
        auto live = rewrite->LoadImm(Imm{swift::u64{11}}).SetType(ValueType::U64);
        auto* first = rewrite->AppendInst(
                OpCode::SetHostGPR, live, HostRegIndex(22), Imm{0u});
        auto next = rewrite->LoadImm(Imm{swift::u64{22}}).SetType(ValueType::U64);
        rewrite->SetHostGPR(next, HostRegIndex(22), Imm{0u});
        rewrite->StoreUniform(Uniform{32, ValueType::U64}, live);
        rewrite->SetTerminal(terminal::ReturnToDispatch{});
        rewrite->ReIdInstr();
        auto rewrite_alloc = std::make_unique<RegAlloc>(
                rewrite->MaxInstrId(), pinned_gprs(), fprs, FeatureSet{});
        RegisterAllocTestSupport::RunForCoalesceLiveTest(
                rewrite.get(), rewrite_alloc.get(), true);
        REQUIRE(rewrite_alloc->ValueGPR(live).id != 22);
        REQUIRE_FALSE(rewrite_alloc->IsHostWriteCoalesced(first->Id()));

        IntrusivePtr<Block> flagged{new Block(0, Location{0x86d6})};
        auto added = flagged->LoadImm(Imm{swift::u64{11}}).SetType(ValueType::U64);
        flagged->SaveFlags(added, Flags::All);
        auto* flagged_pub = flagged->AppendInst(
                OpCode::SetHostGPR, added, HostRegIndex(22), Imm{0u});
        flagged->StoreUniform(Uniform{32, ValueType::U64}, added);
        flagged->SetTerminal(terminal::ReturnToDispatch{});
        flagged->ReIdInstr();
        auto flagged_alloc = std::make_unique<RegAlloc>(
                flagged->MaxInstrId(), pinned_gprs(), fprs, FeatureSet{});
        RegisterAllocTestSupport::RunForCoalesceLiveTest(
                flagged.get(), flagged_alloc.get(), true);
        REQUIRE(flagged_alloc->ValueGPR(added).id == 22);
        REQUIRE(flagged_alloc->IsHostWriteCoalesced(flagged_pub->Id()));
    }

    SECTION("a pre-tied value born inside the publication window rejects coalescing") {
        IntrusivePtr<Block> block{new Block(0, Location{0x86d8})};
        auto candidate = block->LoadImm(Imm{swift::u64{11}}).SetType(ValueType::U64);
        // This definition is between candidate and its logical publication.
        // The conflict selector models an earlier tie assigning it to x22;
        // its use after publish makes that fixed-home interval cross the store.
        auto tied = block->LoadImm(Imm{swift::u64{22}}).SetType(ValueType::U64);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, candidate, HostRegIndex(22), Imm{0u});
        block->StoreUniform(Uniform{24, ValueType::U64}, tied);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        auto features = FeatureSet{};
        features.ra_coalesce = true;
        RegAlloc control{block->MaxInstrId(), pinned_gprs(), fprs, features};
        RegisterAllocTestSupport::RunForCoalesceTest(block.get(), &control, true);
        REQUIRE(control.IsHostWriteCoalesced(publish->Id()));

        RegAlloc conflict{block->MaxInstrId(), pinned_gprs(), fprs, features};
        RegisterAllocTestSupport::RunForCoalesceConflictTest(
                block.get(), &conflict, tied.Id(), 22);
        REQUIRE(conflict.ValueGPR(tied).id == 22);
        REQUIRE_FALSE(conflict.IsHostWriteCoalesced(publish->Id()));
    }

    SECTION("flags and fault observers reject early publication") {
        IntrusivePtr<Block> flag_block{new Block(0, Location{0x86e0})};
        auto flagged = flag_block->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
        flag_block->SaveFlags(flagged, Flags::All);
        auto* flags_publish = flag_block->AppendInst(
                OpCode::SetHostGPR, flagged, HostRegIndex(22), Imm{0u});
        auto flags_on = allocate(flag_block.get(), true);
        REQUIRE(flags_on->IsHostWriteCoalesced(flags_publish->Id()));

        IntrusivePtr<Block> fault{new Block(0, Location{0x86f0})};
        auto value = fault->LoadImm(Imm{swift::u64{9}}).SetType(ValueType::U64);
        auto address = fault->LoadImm(Imm{swift::u64{0x1000}}).SetType(ValueType::U64);
        (void)fault->LoadMemory(Operand{address}).SetType(ValueType::U64);
        auto* fault_publish = fault->AppendInst(
                OpCode::SetHostGPR, value, HostRegIndex(22), Imm{0u});
        auto fault_on = allocate(fault.get(), true);
        REQUIRE_FALSE(fault_on->IsHostWriteCoalesced(fault_publish->Id()));
    }

    SECTION("W-clean publication removes the bridge and feeds a zero-cost U32 read") {
        IntrusivePtr<Block> block{new Block(0, Location{0x8700})};
        auto left = block->LoadImm(Imm{swift::u32{0x1234}}).SetType(ValueType::U32);
        auto right = block->LoadImm(Imm{swift::u32{0x20}}).SetType(ValueType::U32);
        auto value = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto zext = block->ZeroExtend32To64(value);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, zext, HostRegIndex(22), Imm{0u});
        auto read = block->GetHostGPR(HostRegIndex(22), Imm{0u})
                            .SetType(ValueType::U32);
        block->StoreUniform(Uniform{40, ValueType::U32}, read);
        // An unrelated full-width write makes the old block-wide pin-chain
        // fuse decline this block, so this section exercises only the new gate.
        auto unrelated = block->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
        block->SetHostGPR(unrelated, HostRegIndex(23), Imm{0u});

        auto off = allocate(block.get(), false);
        REQUIRE(off->ValueGPR(value).id != 22);
        REQUIRE(off->ValueGPR(read).id != 22);
        auto on = allocate(block.get(), true);
        REQUIRE(on->ValueGPR(value).id == 22);
        REQUIRE(on->ValueGPR(zext).id == 22);
        REQUIRE(on->ValueGPR(read).id == 22);
        REQUIRE(on->IsHostWriteCoalesced(publish->Id()));
        REQUIRE(on->IsHostReadCoalesced(read.Id()));
    }

    SECTION("unknown high half and a crossing write keep the read bridge") {
        IntrusivePtr<Block> unknown{new Block(0, Location{0x8710})};
        auto full = unknown->LoadImm(Imm{swift::u64{0x100000001}})
                            .SetType(ValueType::U64);
        unknown->SetHostGPR(full, HostRegIndex(22), Imm{0u});
        auto read = unknown->GetHostGPR(HostRegIndex(22), Imm{0u})
                           .SetType(ValueType::U32);
        unknown->StoreUniform(Uniform{48, ValueType::U32}, read);
        auto unknown_on = allocate(unknown.get(), true);
        REQUIRE(unknown_on->ValueGPR(read).id != 22);
        REQUIRE_FALSE(unknown_on->IsHostReadCoalesced(read.Id()));

        IntrusivePtr<Block> crossing{new Block(0, Location{0x8720})};
        auto w = crossing->LoadImm(Imm{swift::u32{1}}).SetType(ValueType::U32);
        auto zext = crossing->ZeroExtend32To64(w);
        crossing->SetHostGPR(zext, HostRegIndex(22), Imm{0u});
        auto capture = crossing->GetHostGPR(HostRegIndex(22), Imm{0u})
                                .SetType(ValueType::U32);
        auto replacement = crossing->LoadImm(Imm{swift::u64{2}})
                                   .SetType(ValueType::U64);
        crossing->SetHostGPR(replacement, HostRegIndex(22), Imm{0u});
        crossing->StoreUniform(Uniform{56, ValueType::U32}, capture);
        auto crossing_on = allocate(crossing.get(), true);
        REQUIRE(crossing_on->ValueGPR(capture).id != 22);
        REQUIRE_FALSE(crossing_on->IsHostReadCoalesced(capture.Id()));
    }

    SECTION("a U32 GetHostGPR is not accepted as a W-clean publication proof") {
        IntrusivePtr<Block> block{new Block(0, Location{0x8730})};
        auto source = block->GetHostGPR(HostRegIndex(23), Imm{0u})
                              .SetType(ValueType::U32);
        auto zext = block->ZeroExtend32To64(source);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, zext, HostRegIndex(22), Imm{0u});
        auto on = allocate(block.get(), true);
        // The older pinned-write chain may choose x22 as the UBFX destination,
        // but that UBFX remains the operation which proves/creates the clean W
        // value. The new pass must not label the whole chain zero-instruction.
        REQUIRE_FALSE(on->IsHostWriteCoalesced(publish->Id()));
    }

    SECTION("emitter removes the proven publication instruction") {
        auto emit_size = [&](bool enabled) {
            Config config{
                    .loc_start = 0,
                    .loc_end = 1ull << 48,
                    .enable_jit = true,
                    .has_local_operation = false,
                    .backend_isa = kArm64,
            };
            AddressSpace address_space{config};
            ModuleConfig module_config{};
            module_config.feature_overrides.Set(FeatureId::ra_coalesce, enabled);
            auto module = address_space.MapModule(
                    LocationDescriptor{0x8740}, LocationDescriptor{0x8750}, module_config);

            IntrusivePtr<Block> block{new Block(0, Location{0x8740})};
            auto value = block->LoadImm(Imm{swift::u64{0x123456789abcdef0}})
                                 .SetType(ValueType::U64);
            block->SetHostGPR(value, HostRegIndex(22), Imm{0u});
            block->SetTerminal(terminal::ReturnToDispatch{});
            block->ReIdInstr();

            auto features = FeatureSet{};
            features.ra_coalesce = enabled;
            RegAlloc alloc{block->MaxInstrId(), pinned_gprs(), fprs, features};
            RegisterAllocTestSupport::RunForCoalesceTest(block.get(), &alloc, enabled);
            arm64::JitContext context{module, alloc};
            arm64::JitTranslator translator{context};
            translator.Translate(block.get());
            context.Finish();
            return context.CurrentBufferSize();
        };

        const auto off = emit_size(false);
        const auto on = emit_size(true);
        REQUIRE(on + vixl::aarch64::kInstructionSize == off);
    }

    SECTION("expanded scalar producers preserve alias emission and conflict proofs") {
        auto emit_size = [&](OpCode op, ValueType type, swift::u64 location,
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
            module_config.feature_overrides.Set(FeatureId::ra_coalesce, enabled);
            auto module = address_space.MapModule(
                    LocationDescriptor{location}, LocationDescriptor{location + 0x10},
                    module_config);
            auto item = make_expanded_case(op, type, location);
            auto alloc = allocate(item.block.get(), enabled);
            arm64::JitContext context{module, *alloc};
            arm64::JitTranslator translator{context};
            translator.Translate(item.block.get());
            context.Finish();
            return context.CurrentBufferSize();
        };

        std::size_t case_index = 0;
        for (std::size_t index = 0; index < expanded_producers.size(); ++index) {
            const auto [op, name] = expanded_producers[index];
            const std::array types = {ValueType::U32, ValueType::U64};
            for (auto type : types) {
                if (op == OpCode::MulHigh && type == ValueType::U32) {
                    continue;
                }
                CAPTURE(name, type);
                auto item = make_expanded_case(op, type, 0x8800 + case_index * 0x20);
                auto off = allocate(item.block.get(), false);
                REQUIRE(off->ValueGPR(item.produced).id != 22);
                REQUIRE_FALSE(off->IsHostWriteCoalesced(item.publish->Id()));

                auto on = allocate(item.block.get(), true);
                CAPTURE(item.produced.Id(), item.produced.Def()->GetUses(),
                        item.publish->Id(), on->ValueGPR(item.produced).id);
                REQUIRE(on->ValueGPR(item.produced).id == 22);
                REQUIRE(on->IsHostWriteCoalesced(item.publish->Id()));

                auto features = FeatureSet{};
                features.ra_coalesce = true;
                RegAlloc conflict{item.block->MaxInstrId(), pinned_gprs(), fprs, features};
                RegisterAllocTestSupport::RunForCoalesceConflictTest(
                        item.block.get(), &conflict, item.conflict.Id(), 22);
                REQUIRE(conflict.ValueGPR(item.conflict).id == 22);
                REQUIRE_FALSE(conflict.IsHostWriteCoalesced(item.publish->Id()));

                const auto location = 0x9800 + case_index * 0x20;
                const auto off_size = emit_size(op, type, location, false);
                const auto on_size = emit_size(op, type, location, true);
                REQUIRE(on_size + vixl::aarch64::kInstructionSize <= off_size);
                ++case_index;
            }
        }
    }
}
