#include "../support/allocation_inputs.h"
#include "../support/case_support.h"
#include "../support/fp_environment.h"

TEST_CASE("Scratch pool survives a register file saturated across a VecFAdd") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    // The regression this guards: JitContext::GetTmpX hands out registers the
    // allocation pass left free, and never releases them before the next
    // instruction. EmitVecFAdd needs eight at once. With a fixed reserve
    // (4 before this change) the scan was free to fill the register file down
    // to four, so a VecFAdd in a saturated block either panicked with
    // "No free temporary GPR" or -- worse, when the pool happened to have room
    // -- silently handed the NaN fixup a register holding a live value.
    //
    // Saturating the file is the whole point, so `live` is deliberately larger
    // than any ARM64 GPR pool.
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
#if defined(__linux__) && !defined(__ANDROID__)
    // Spill handling must not tax the whole unit by reserving x18.
    REQUIRE_FALSE(gprs.Get(18));
#endif

#if defined(__linux__) && !defined(__ANDROID__)
    bool saw_spill_unit = false;
#endif
    auto check_and_emit = [&](Block* raw, int expected_spill = -1) {
        swift::runtime::IntrusivePtr<Block> block{raw};
        RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocPass::Run(block.get(), &reg_alloc, false, FeatureSet{});
#if defined(__linux__) && !defined(__ANDROID__)
        const bool has_spill = reg_alloc.SpillCount() != 0;
        saw_spill_unit |= has_spill;
        if (expected_spill >= 0) {
            REQUIRE(has_spill == (expected_spill != 0));
        }
        REQUIRE_FALSE(reg_alloc.GetGprs().Get(18));
#endif

        // 1. The allocation must leave every instruction the scratch its
        //    emitter is declared to need, plus a reload register for each
        //    DISTINCT spilled value it names (JitContext reloads a value once
        //    per instruction however often the instruction names it). This is
        //    the contract GetTmpX relies on.
        for (auto& inst : block->GetInstList()) {
            auto need = ScratchBudget(inst, FeatureSet{});
            unsigned reloads_gpr = 0, reloads_fpr = 0;
            std::vector<std::uint32_t> counted;
            auto count = [&](const Value& value) {
                if (!value.Defined() || reg_alloc.ValueType(value) != RegAlloc::MEM) {
                    return;
                }
                if (std::find(counted.begin(), counted.end(), value.Id()) != counted.end()) {
                    return;
                }
                counted.push_back(value.Id());
                auto type = value.Type();
                (type >= ValueType::V8 && type <= ValueType::V256 ? reloads_fpr : reloads_gpr)++;
            };
            for (auto& value : inst.GetValues()) {
                count(value);
            }
            if (inst.HasValue()) {
                count(Value{&inst});
            }
            INFO("opcode " << static_cast<unsigned>(inst.GetOp()) << " at id " << inst.Id());
            REQUIRE(static_cast<unsigned>(reg_alloc.DirtyGPR(inst.Id()).GetClearCount()) >=
                    need.gpr + reloads_gpr);
            REQUIRE(static_cast<unsigned>(reg_alloc.DirtyFPR(inst.Id()).GetClearCount()) >=
                    need.fpr + reloads_fpr);
        }

        // 2. And the JIT must actually emit it. GetTmpX panics through
        //    AssertFailed, so reaching the end of Translate is the assertion:
        //    a scratch shortfall aborts here rather than returning.
        arm64::JitContext context{module, reg_alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
    };

#if defined(__linux__) && !defined(__ANDROID__)
    auto* no_spill = new Block(0, Location{0x1800});
    const auto immediate = no_spill->LoadImm(Imm{7u});
    no_spill->StoreUniform(Uniform{0, ValueType::U32}, immediate);
    no_spill->SetTerminal(terminal::ReturnToDispatch{});
    no_spill->ReIdInstr();
    check_and_emit(no_spill, 0);
#endif

    for (unsigned live : {8u, 16u, 24u, 40u}) {
        INFO("live scalar values across the VecFAdd: " << live);
        check_and_emit(BuildScratchPressureBlock(live));
    }
    // Reload pressure: values read twice across a saturated file, and one
    // instruction naming the same spilled value twice. Capped below the point
    // where the 64-slot State::spill_area runs out -- an unrelated limit
    // (backend::kMaxSpillSlots) with its own loud assert.
    for (unsigned live : {12u, 20u, 28u}) {
        INFO("values read twice across a saturated file: " << live);
        check_and_emit(BuildReloadPressureBlock(live));
    }

#if defined(__linux__) && !defined(__ANDROID__)
    REQUIRE(saw_spill_unit);
#endif
}

TEST_CASE("SSE4.2 string lowering stays within its declared scratch contract") {
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

    // Exercise every control-byte shape. In particular, the equal-ordered
    // validity vectors and both result-collapse widths used to call VIXL's
    // two-lane Movi macro, which silently acquired a fifth GPR.
    for (swift::u32 imm = 0; imm < 256; ++imm) {
        INFO("SSE4.2 string control byte " << imm);
        Block block{0, Location{0x1c00 + imm}};
        auto left = block.LoadUniform(Uniform{0, ValueType::V128});
        auto right = block.LoadUniform(Uniform{16, ValueType::V128});
        auto result = block.Sse42Str(left, right, Imm{imm}).SetType(ValueType::U64);
        block.StoreUniform(Uniform{32, ValueType::U64}, result);
        block.SetTerminal(terminal::ReturnToDispatch{});
        block.ReIdInstr();

        RegAlloc alloc{block.MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocPass::Run(&block, &alloc, false, FeatureSet{});
#if defined(__aarch64__)
        const swift::u32 expected_gprs =
                swift::x86::Sse42StrVectorHelperAddress(swift::u8(imm))
                        ? 0u
                        : 4u;
#else
        const swift::u32 expected_gprs = 4u;
#endif
        REQUIRE(ScratchBudget(*result.Def(), FeatureSet{}).gpr == expected_gprs);

        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        context.SetCurrent(&block);
        context.TickIR(result.Def());
        translator.EmitSse42Str(result.Def());
        context.EndInstructionScratch();
        REQUIRE(context.LastInstructionScratchGPR() == expected_gprs);
        context.Finish();
    }
}

TEST_CASE("Add Sub precise scratch prices cover emitted peaks") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    enum class RightShape { Reg, LargeImm, Shift, Composite };
    auto make_price = [](ValueType type, Flags flags, bool branch_only,
                         RightShape shape) {
        Block block{0, Location{0x1d00}};
        auto left = block.LoadImm(Imm{7u}).SetType(type);
        auto other = block.LoadImm(Imm{3u}).SetType(type);
        Operand right{other};
        switch (shape) {
            case RightShape::Reg:
                break;
            case RightShape::LargeImm:
                right = Operand{Imm{swift::u64{0x123456789}}};
                break;
            case RightShape::Shift:
                right = Operand{other, Imm{1u}, OperandLsl};
                break;
            case RightShape::Composite:
                right = Operand{left, other, OperandPlus};
                break;
        }
        auto result = block.Add(left, right).SetType(type);
        if (branch_only) {
            block.AppendInst(OpCode::BranchOnlyFlags, result, flags);
        } else if (flags != Flags::None) {
            block.SaveFlags(result, flags);
        }
        return PreciseAddSubScratchBudget(*result.Def()).gpr;
    };

    REQUIRE(make_price(ValueType::U64, Flags::None, false, RightShape::Reg) == 0);
    REQUIRE(make_price(ValueType::U64, Flags::Parity, false, RightShape::Reg) == 0);
    REQUIRE(make_price(ValueType::U64, Flags::AuxiliaryCarry, false, RightShape::Reg) == 1);
    REQUIRE(make_price(ValueType::U64, Flags::NZCV, false, RightShape::Reg) == 1);
    REQUIRE(make_price(ValueType::U64, Flags::All, false, RightShape::Reg) == 2);
    REQUIRE(make_price(ValueType::U64, Flags::All, false, RightShape::LargeImm) == 3);
    REQUIRE(make_price(ValueType::U64, Flags::All, false, RightShape::Shift) == 2);
    REQUIRE(make_price(ValueType::U64, Flags::All, false, RightShape::Composite) == 3);
    REQUIRE(make_price(ValueType::U64, Flags::All, true, RightShape::Composite) == 1);
    REQUIRE(make_price(ValueType::U8, Flags::All, false, RightShape::Reg) == 3);
    REQUIRE(make_price(ValueType::U16, Flags::All, false, RightShape::Composite) == 4);
    REQUIRE(make_price(ValueType::U8, Flags::All, true, RightShape::Composite) == 2);
    REQUIRE(make_price(ValueType::U16, Flags::AuxiliaryCarry, false,
                       RightShape::Composite) == 2);

    auto make_host_price = [](ValueType type, Flags flags) {
        Block block{0, Location{0x1d80}};
        auto left = block.GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(type);
        auto result = block.Add(left, Operand{left}).SetType(type);
        if (flags != Flags::None) block.SaveFlags(result, flags);
        return PreciseAddSubScratchBudget(*result.Def()).gpr;
    };
    REQUIRE(make_host_price(ValueType::U16, Flags::None) == 2);
    REQUIRE(make_host_price(ValueType::U16, Flags::All) == 5);
    REQUIRE(make_host_price(ValueType::U32, Flags::All) == 2);

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

    Block allocation_contract{0, Location{0x1dc0}};
    auto contract_left =
            allocation_contract.LoadImm(Imm{7u}).SetType(ValueType::U16);
    auto contract_right =
            allocation_contract.LoadImm(Imm{3u}).SetType(ValueType::U16);
    auto contract_result = allocation_contract
                                   .Add(contract_left, Operand{contract_right})
                                   .SetType(ValueType::U16);
    allocation_contract.SaveFlags(contract_result, Flags::All);
    allocation_contract.ReIdInstr();
    RegAlloc contract_alloc{allocation_contract.MaxInstrId(), gprs, fprs,
                            FeatureSet{}};
    RegisterAllocPass::Run(&allocation_contract, &contract_alloc, false, FeatureSet{});
    REQUIRE(contract_alloc.ValueType(contract_left) == RegAlloc::GPR);
    REQUIRE(contract_alloc.ValueType(contract_right) == RegAlloc::GPR);
    REQUIRE(contract_alloc.ValueType(contract_result) == RegAlloc::GPR);
    REQUIRE(contract_alloc.ValueGPR(contract_result).id !=
            contract_alloc.ValueGPR(contract_left).id);
    REQUIRE(contract_alloc.ValueGPR(contract_result).id !=
            contract_alloc.ValueGPR(contract_right).id);

    auto measure = [&](ValueType type, Flags flags, bool branch_only,
                       RightShape shape, bool host_read = false) {
        Block block{0, Location{0x1e00}};
        Value left;
        Value other;
        if (host_read) {
            left = block.GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(type);
            other = block.GetHostGPR(HostRegIndex(23), Imm{0u}).SetType(type);
        } else {
            left = block.LoadImm(Imm{7u}).SetType(type);
            other = block.LoadImm(Imm{3u}).SetType(type);
        }
        auto pending = block.Add(left, Operand{other}).SetType(ValueType::U64);
        block.SaveFlags(pending, Flags::All);
        Operand right{left};
        switch (shape) {
            case RightShape::Reg:
                break;
            case RightShape::LargeImm:
                right = Operand{Imm{swift::u64{0x123456789}}};
                break;
            case RightShape::Shift:
                right = Operand{left, Imm{1u}, OperandLsl};
                break;
            case RightShape::Composite:
                right = Operand{left, other, OperandPlus};
                break;
        }
        auto target = block.Add(left, right).SetType(type);
        if (branch_only) {
            block.AppendInst(OpCode::BranchOnlyFlags, target, flags);
        } else if (flags != Flags::None) {
            block.SaveFlags(target, flags);
        }
        block.ReIdInstr();

        RegAlloc alloc{block.MaxInstrId(), gprs, fprs, FeatureSet{}};
        alloc.MapRegister(left.Id(), HostGPR{22});
        alloc.MapRegister(other.Id(), HostGPR{23});
        alloc.MapRegister(pending.Id(), HostGPR{24});
        // Current RA does not tie narrow Add/Sub destinations to either input.
        // Keep the emitter measurement on that allocation contract.
        alloc.MapRegister(target.Id(), HostGPR{10});
        auto active_gprs = gprs;
        auto active_fprs = fprs;
        alloc.SetActiveRegs(pending.Id(), active_gprs, active_fprs);
        alloc.SetActiveRegs(target.Id(), active_gprs, active_fprs);

        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        context.SetCurrent(&block);
        context.TickIR(pending.Def());
        translator.EmitAdd(pending.Def());
        context.EndInstructionScratch();
        context.TickIR(target.Def());
        // Measure independently of either OFF's legacy cap or ON's proposed
        // price. Terminal scratch supplies a seven-register observation
        // envelope but otherwise uses the same per-instruction masks.
        context.BeginTerminalScratch();
        translator.EmitAdd(target.Def());
        context.EndTerminalScratch();
        const swift::u32 peak = context.LastInstructionScratchGPR();
        const swift::u32 price = PreciseAddSubScratchBudget(*target.Def()).gpr;
        INFO("type " << static_cast<unsigned>(type) << " flags "
                     << static_cast<unsigned long long>(flags) << " branch "
                     << branch_only << " shape " << static_cast<unsigned>(shape)
                     << " peak " << peak << " price " << price);
        REQUIRE(peak <= price);
        context.Finish();
        return peak;
    };

    REQUIRE(measure(ValueType::U64, Flags::None, false, RightShape::Reg) == 0);
    REQUIRE(measure(ValueType::U64, Flags::All, false, RightShape::LargeImm) == 3);
    REQUIRE(measure(ValueType::U64, Flags::All, false, RightShape::Composite) == 3);
    REQUIRE(measure(ValueType::U64, Flags::All, true, RightShape::Composite) == 1);
    REQUIRE(measure(ValueType::U8, Flags::All, false, RightShape::Reg) == 3);
    REQUIRE(measure(ValueType::U16, Flags::All, false, RightShape::Composite) == 4);
    REQUIRE(measure(ValueType::U8, Flags::All, true, RightShape::Composite) == 2);
    REQUIRE(measure(ValueType::U16, Flags::None, false, RightShape::Reg,
                    true) == 2);
    REQUIRE(measure(ValueType::U16, Flags::All, false, RightShape::Reg,
                    true) == 5);
}

// --- spill-slot recycling ----------------------------------------------------
//
// The blocks below pin the register file full with long-lived "anchor" values
// and then push a long stream of SHORT-lived values past it. Every short value
// spills (the file is full) but at most two are live at any instant, so the
// number of stack slots the unit needs is a small constant while the number of
// spill *events* is proportional to the churn. That difference is the whole
// point: a linear scan that never returns a slot consumes one per event and
// dies on backend::kMaxSpillSlots; one that recycles stays flat.

static swift::runtime::ir::Block* BuildSpillChurnBlock(unsigned anchors, unsigned churn) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x3000});
    std::vector<Value> held;
    held.reserve(anchors);
    for (unsigned i = 0; i < anchors; i++) {
        held.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    // The churn. `acc` is a chain: acc(i) dies at exactly the instruction that
    // defines acc(i+1), which is the boundary case an off-by-one in the expiry
    // test (`end < start` vs `end <= start`) gets wrong -- both are live at
    // that instruction, so they must not share a slot.
    Value acc = block->LoadImm(Imm{0u});
    for (unsigned i = 0; i < churn; i++) {
        auto tmp = block->LoadImm(Imm{static_cast<std::uint32_t>(0x1000 + i)});
        acc = block->Add(acc, Operand{tmp});
    }
    // Anchors are consumed only here, so they are live across the whole churn.
    Value sum = held.back();
    for (int i = static_cast<int>(anchors) - 2; i >= 0; i--) {
        sum = block->Add(sum, Operand{held[i]});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, block->Add(sum, Operand{acc}));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Same shape in the vector file. A spilled V128 occupies TWO consecutive u64
// slots and is accessed with a 16-byte Ldr/Str, so recycling it has to return
// both halves and keep the surviving pairs even-aligned (the scaled offset form
// cannot encode an odd one).
static swift::runtime::ir::Block* BuildVecSpillChurnBlock(unsigned anchors, unsigned churn) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x4000});
    std::vector<Value> held;
    held.reserve(anchors);
    for (unsigned i = 0; i < anchors; i++) {
        held.push_back(block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i + 1)), ValueType::V128}));
    }
    Value acc = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{0, ValueType::V128});
    for (unsigned i = 0; i < churn; i++) {
        auto tmp = block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i % 8)), ValueType::V128});
        acc = block->VecAdd<TypedValue<ValueType::V128>>(acc, tmp, Imm{32u});
    }
    Value sum = held.back();
    for (int i = static_cast<int>(anchors) - 2; i >= 0; i--) {
        sum = block->VecAdd<TypedValue<ValueType::V128>>(sum, held[i], Imm{32u});
    }
    block->StoreUniform(Uniform{0, ValueType::V128},
                        block->VecAdd<TypedValue<ValueType::V128>>(sum, acc, Imm{32u}));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Both files churning at once, so scalar and SIMD spills interleave over the
// same recycled stack: this is the case where returning only one half of a
// SIMD pair, or losing the even alignment, shows up.
static swift::runtime::ir::Block* BuildMixedSpillChurnBlock(unsigned anchors, unsigned churn) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x5000});
    std::vector<Value> gpr_held;
    std::vector<Value> vec_held;
    for (unsigned i = 0; i < anchors; i++) {
        gpr_held.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
        vec_held.push_back(block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i + 1)), ValueType::V128}));
    }
    Value gpr_acc = block->LoadImm(Imm{0u});
    Value vec_acc = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{0, ValueType::V128});
    for (unsigned i = 0; i < churn; i++) {
        auto scalar = block->LoadImm(Imm{static_cast<std::uint32_t>(0x2000 + i)});
        gpr_acc = block->Add(gpr_acc, Operand{scalar});
        auto vec = block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i % 8)), ValueType::V128});
        vec_acc = block->VecAdd<TypedValue<ValueType::V128>>(vec_acc, vec, Imm{32u});
    }
    Value gpr_sum = gpr_held.back();
    Value vec_sum = vec_held.back();
    for (int i = static_cast<int>(anchors) - 2; i >= 0; i--) {
        gpr_sum = block->Add(gpr_sum, Operand{gpr_held[i]});
        vec_sum = block->VecAdd<TypedValue<ValueType::V128>>(vec_sum, vec_held[i], Imm{32u});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, block->Add(gpr_sum, Operand{gpr_acc}));
    block->StoreUniform(Uniform{16, ValueType::V128},
                        block->VecAdd<TypedValue<ValueType::V128>>(vec_sum, vec_acc, Imm{32u}));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Manufactures the one stack layout that tells an even-aligned pair search from
// a naive one: a scalar sitting on the low half of a freed SIMD pair, with the
// high half and the pair above it free. The free slots are then {odd, even,
// even+1} and the first *adjacent free pair* starts on an ODD index -- which a
// 16-byte Ldr/Str cannot address with the scaled offset form. Only a search
// that steps by two rejects it.
static swift::runtime::ir::Block* BuildPairFragmentBlock(unsigned gpr_anchors,
                                                         unsigned vec_anchors,
                                                         unsigned rounds) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x6000});
    std::vector<Value> gpr_held;
    std::vector<Value> vec_held;
    for (unsigned i = 0; i < gpr_anchors; i++) {
        gpr_held.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    for (unsigned i = 0; i < vec_anchors; i++) {
        vec_held.push_back(block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i + 1)), ValueType::V128}));
    }
    for (unsigned r = 0; r < rounds; r++) {
        auto va = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{16, ValueType::V128});
        auto vb = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{32, ValueType::V128});
        block->StoreUniform(Uniform{64, ValueType::V128}, va);  // va dies
        auto sx = block->LoadImm(Imm{static_cast<std::uint32_t>(0x30 + r)});
        block->StoreUniform(Uniform{80, ValueType::V128}, vb);  // vb dies; sx now holds va's low slot
        auto vc = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{48, ValueType::V128});
        block->StoreUniform(Uniform{96, ValueType::V128}, vc);
        block->StoreUniform(Uniform{0, ValueType::U32}, sx);
    }
    Value gpr_sum = gpr_held.back();
    for (int i = static_cast<int>(gpr_anchors) - 2; i >= 0; i--) {
        gpr_sum = block->Add(gpr_sum, Operand{gpr_held[i]});
    }
    Value vec_sum = vec_held.back();
    for (int i = static_cast<int>(vec_anchors) - 2; i >= 0; i--) {
        vec_sum = block->VecAdd<TypedValue<ValueType::V128>>(vec_sum, vec_held[i], Imm{32u});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, gpr_sum);
    block->StoreUniform(Uniform{112, ValueType::V128}, vec_sum);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

TEST_CASE("Spill slots are recycled, not merely handed out") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    // The regression this guards: SpillAtInterval never recorded its interval
    // in the allocator's active set, so the MEM arm of ExpireOldIntervals --
    // and FreeSpill with it -- was unreachable. Slots were handed out and never
    // returned, making the 64-slot State::spill_area a budget for the TOTAL
    // number of spills in a compilation unit instead of the number live at
    // once. Any long enough block of short-lived spills aborted the guest with
    // "spill area exhausted".
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
    // Filling the file exactly means the anchors take every allocatable
    // register and only the scratch reserve's worth spills permanently, so the
    // slot high-water mark is dominated by the churn -- which is what is under
    // test.
    const auto gpr_pool = static_cast<unsigned>(GPRSMask{gprs}.GetClearCount());
    const auto fpr_pool = static_cast<unsigned>(FPRSMask{fprs}.GetClearCount());

    // Live range of a value as the linear scan models it in block mode: from
    // its defining instruction to the last instruction that names it, extended
    // to the end of the block when the terminal reads it.
    auto live_ranges = [](Block* block) {
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> range;
        for (auto& inst : block->GetInstList()) {
            if (inst.HasValue()) {
                range[inst.Id()] = {inst.Id(), inst.Id()};
            }
        }
        for (auto& inst : block->GetInstList()) {
            for (auto& value : inst.GetValues()) {
                if (!value.Defined()) continue;
                auto it = range.find(value.Id());
                if (it != range.end()) {
                    it->second.second = std::max<std::uint32_t>(it->second.second, inst.Id());
                }
            }
        }
        return range;
    };

    auto check_recycling = [&](Block* raw, unsigned expect_min_spills) {
        swift::runtime::IntrusivePtr<Block> block{raw};
        RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
        // Pre-fix this call itself threw ("spill area exhausted: slot 64 (+1)
        // >= 64 reserved slots") long before any assertion below could run.
        // Select the historical spill-current policy explicitly: this test's
        // pressure generator relies on hundreds of churn spills, while the
        // farthest-end experiment deliberately removes most of them.
        (void)RegisterAllocTestSupport::RunForSpillEvictTest(
                block.get(), &reg_alloc, false);

        auto ranges = live_ranges(block.get());
        struct Occupancy {
            std::uint32_t id;
            std::uint32_t slot;
            std::uint32_t width;
            std::uint32_t start;
            std::uint32_t end;
        };
        std::vector<Occupancy> spilled;
        for (auto& inst : block->GetInstList()) {
            // A bitcast is an alias of its source, not an allocation of its
            // own; counting it would report one slot with two owners.
            if (!inst.HasValue() || inst.IsBitCastOperation()) {
                continue;
            }
            Value value{&inst};
            if (reg_alloc.ValueType(value) != RegAlloc::MEM) {
                continue;
            }
            const auto type = inst.ReturnType();
            const bool is_vector = type >= ValueType::V8 && type <= ValueType::V256;
            const std::uint32_t slot = reg_alloc.ValueMem(value).offset;
            auto& r = ranges[inst.Id()];
            spilled.push_back({inst.Id(), slot, is_vector ? 2u : 1u, r.first, r.second});
        }

        // 1. The unit must actually have gone down the spill path, hard.
        INFO("spilled values: " << spilled.size());
        REQUIRE(spilled.size() >= expect_min_spills);

        std::uint32_t high_water = 0;
        for (auto& s : spilled) {
            high_water = std::max(high_water, s.slot + s.width);
            // 2. A 16-byte Ldr/Str encodes only a multiple-of-16 offset, so a
            //    SIMD pair must stay even-aligned however the stack is reused.
            if (s.width == 2) {
                INFO("SIMD value " << s.id << " at slot " << s.slot);
                REQUIRE(s.slot % 2 == 0);
            }
            // 3. Never past the reservation: beyond it the Str walks into the
            //    uniform buffer that follows State::spill_area.
            REQUIRE(s.slot + s.width <= kMaxSpillSlots);
        }

        // 4. Two values that are live at the same time must not share a slot.
        //    This is what makes recycling safe rather than merely cheap: a slot
        //    returned one instruction too early is handed to an overlapping
        //    value and both Ldr/Str target the same address.
        for (std::size_t i = 0; i < spilled.size(); i++) {
            for (std::size_t j = i + 1; j < spilled.size(); j++) {
                const auto& a = spilled[i];
                const auto& b = spilled[j];
                const bool ranges_overlap = a.start <= b.end && b.start <= a.end;
                if (!ranges_overlap) {
                    continue;
                }
                const bool slots_overlap =
                        a.slot < b.slot + b.width && b.slot < a.slot + a.width;
                INFO("values " << a.id << " [" << a.start << "," << a.end << "] slot " << a.slot
                               << "+" << a.width << " and " << b.id << " [" << b.start << ","
                               << b.end << "] slot " << b.slot << "+" << b.width);
                REQUIRE_FALSE(slots_overlap);
            }
        }

        // 5. And the reuse must be real: far more spills than slots. Without
        //    recycling these are equal by construction.
        INFO("high water " << high_water << " slots for " << spilled.size() << " spills");
        REQUIRE(high_water * 2 < spilled.size());

        // 6. Finally the JIT has to emit it. GetTmpX/GetTmpV and
        //    SpillGPR/SpillFPR assert through AssertFailed, so reaching the end
        //    of Translate is itself the assertion.
        arm64::JitContext context{module, reg_alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
        return high_water;
    };

    SECTION("scalar churn") {
        // 200 rounds of (LoadImm, Add) past a full file: >=400 spill events
        // against a 64-slot area. Pre-fix this aborts at the 64th.
        check_recycling(BuildSpillChurnBlock(gpr_pool, 200), 300);
    }
    SECTION("vector churn") {
        check_recycling(BuildVecSpillChurnBlock(fpr_pool, 200), 300);
    }
    SECTION("mixed churn") {
        // Scalar and SIMD spills interleaved over one recycled stack.
        check_recycling(BuildMixedSpillChurnBlock(std::min(gpr_pool, fpr_pool), 200), 500);
    }
    SECTION("fragmented pairs stay 16-byte aligned") {
        // Deliberately leaves the first adjacent free pair on an ODD index.
        check_recycling(BuildPairFragmentBlock(gpr_pool, fpr_pool, 120), 400);
    }
}

// --- Peeled GetOperand must retain its address register ----------------------
//
// SVM_MEM_NARROW_FUSE originally suppressed a single-use direct GetOperand
// in the emitter and made the following memory operation read the wrapper's
// source directly.  Register allocation still considered that source dead at
// GetOperand, however, and could reuse its register for a StoreMemory value:
//
//   LoadUniform address -> x0
//   GetOperand(address)  -> x1   (suppressed)
//   LoadUniform value    -> x0
//   StoreMemory          -> str x0, [x0]  // value used as its own address
//
// The optimized form is valid only when RA transfers the source register to
// the GetOperand result, whose interval remains live through StoreMemory.
TEST_CASE("peeled GetOperand keeps its address live through the memory use") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    auto* raw_block = new Block(0, Location{0x6f00});
    IntrusivePtr<Block> block{raw_block};
    auto address =
            raw_block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto memory_address =
            raw_block->GetOperand(Operand{address}).SetType(ValueType::U64);
    auto value =
            raw_block->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    raw_block->StoreMemory(Operand{memory_address}, value);
    raw_block->SetTerminal(terminal::ReturnToDispatch{});
    raw_block->ReIdInstr();

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    RegAlloc reg_alloc{raw_block->MaxInstrId(),
                       address_space.GetTrampolines().GetGPRRegs(),
                       address_space.GetTrampolines().GetFPRRegs(),
                       FeatureSet{}};
    RegisterAllocPass::Run(raw_block, &reg_alloc, false, FeatureSet{});

    const bool enabled = swift::runtime::GetSvmConfig().mem_narrow_fuse;
    if (enabled) {
        REQUIRE(reg_alloc.ValueType(address) == RegAlloc::GPR);
        REQUIRE(reg_alloc.ValueType(memory_address) == RegAlloc::GPR);
        REQUIRE(reg_alloc.ValueType(value) == RegAlloc::GPR);
        REQUIRE(reg_alloc.ValueGPR(address).id ==
                reg_alloc.ValueGPR(memory_address).id);
        REQUIRE(reg_alloc.ValueGPR(memory_address).id !=
                reg_alloc.ValueGPR(value).id);
    }

    arm64::JitContext context{module, reg_alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(raw_block);
    context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);
}

TEST_CASE("address EA tie transfers a terminal fixed alias") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    auto* raw = new Block(0, Location{0x6f20});
    IntrusivePtr<Block> block{raw};
    auto source = raw->GetHostGPR(HostRegIndex(6), Imm{0u}).SetType(ValueType::U64);
    auto address = raw->GetOperand(Operand{source}).SetType(ValueType::U64);
    auto value = raw->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    raw->StoreMemory(Operand{address}, value);
    raw->SetTerminal(terminal::ReturnToDispatch{});
    raw->ReIdInstr();

    // x6 模拟静态映射的固定家；普通线性扫描不能把其它值分配到它。
    const GPRSMask gprs{~((1u << 19) - 1u) | (1u << 6)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    RegAlloc alloc{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocPass::Run(raw, &alloc, false, FeatureSet{});

    const bool enabled = swift::runtime::GetSvmConfig().addr_ea_tie;
    REQUIRE(alloc.ValueGPR(source).id == 6);
    REQUIRE((alloc.ValueGPR(address).id == alloc.ValueGPR(source).id) == enabled);
    REQUIRE(alloc.ValueGPR(address).id != alloc.ValueGPR(value).id);
}

TEST_CASE("composite memory EA survives only in direct mode") {
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

    auto decode_address = [&](std::array<swift::u8, 5> bytes, bool direct) {
        const auto pc = reinterpret_cast<VAddr>(bytes.data());
        auto* raw = new Block(0, Location{pc});
        IntrusivePtr<Block> block{raw};
        Assembler assembler{raw};
        X64Decoder decoder{pc,
                           &memory,
                           &assembler,
                           true,
                           Arm64Features::None,
                           false,
                           direct,
                           FeatureSet{}};
        decoder.Decode();
        for (auto& inst : raw->GetInstList()) {
            if (inst.GetOp() == OpCode::LoadMemory) {
                return std::pair{block, inst.GetArg<Operand>(0)};
            }
        }
        FAIL("memory load was not decoded");
        return std::pair{block, Operand{}};
    };

    const bool enabled = swift::runtime::GetSvmConfig().addr_ea_tie;
    // mov eax,[rbx+8]; hlt。末尾补零只用于固定数组长度。
    auto [direct_imm_block, direct_imm] =
            decode_address({0x8b, 0x43, 0x08, 0xf4, 0x00}, true);
    auto [bias_imm_block, bias_imm] =
            decode_address({0x8b, 0x43, 0x08, 0xf4, 0x00}, false);
    // mov eax,[rbx+rcx]; hlt。
    auto [direct_reg_block, direct_reg] =
            decode_address({0x8b, 0x04, 0x0b, 0xf4, 0x00}, true);
    // mov eax,[rbx+rcx*4]; hlt。
    auto [direct_ext_block, direct_ext] =
            decode_address({0x8b, 0x04, 0x8b, 0xf4, 0x00}, true);

    REQUIRE((!direct_imm.GetRight().Null()) == enabled);
    if (enabled) {
        REQUIRE(direct_imm.GetOp() == OperandOp::Plus);
        REQUIRE(direct_imm.GetRight().IsImm());
        REQUIRE(direct_reg.GetOp() == OperandOp::Plus);
        REQUIRE(direct_reg.GetRight().IsValue());
        REQUIRE(direct_ext.GetOp() == OperandOp::PlusExt);
        REQUIRE(direct_ext.GetRight().IsValue());
    } else {
        REQUIRE(direct_reg.GetRight().Null());
        REQUIRE(direct_ext.GetRight().Null());
    }
    // bias 不论开关状态都保留旧的 GetOperand 单值边界。
    REQUIRE(bias_imm.GetRight().Null());
    REQUIRE(bias_imm.GetLeft().IsValue());
    REQUIRE(bias_imm.GetLeft().value.Def()->GetOp() == OpCode::GetOperand);
}

TEST_CASE("absolute GetOperand materializes directly into its result") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x7800})};
    auto address =
            block->GetOperand(Operand{Imm{swift::u64{0x004c7e10}}})
                    .SetType(ValueType::U64);
    block->ReIdInstr();

    const GPRSMask gprs{~((1u << 8) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    RegAlloc alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    alloc.MapRegister(address.Id(), HostGPR{6});
    auto active_gprs = gprs;
    auto active_fprs = fprs;
    active_gprs.Mark(6);
    alloc.SetActiveRegs(address.Id(), active_gprs, active_fprs);

    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
    arm64::JitTranslator translator{context};
    context.TickIR(address.Def());
    const auto begin = context.CurrentBufferSize();
    translator.EmitGetOperand(address.Def());
    context.EndInstructionScratch();
    const auto bytes = context.CurrentBufferSize() - begin;

    const bool enabled = swift::runtime::GetSvmConfig().abs_const_mat;
    REQUIRE(bytes == (enabled ? 8 : 12));
}

TEST_CASE("unit-local absolute addresses reuse only verified idle GPR windows") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    struct Result {
        swift::u32 bytes{};
        bool anchor{};
        bool reuse{};
        bool host_write{};
    };
    auto run = [](bool cache, bool tight_pool, bool coalesce,
                  swift::u64 second_address = 0x004958d8,
                  bool biased_memory = false) {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .page_table = biased_memory ? reinterpret_cast<void*>(0x1000) : nullptr,
        };
        AddressSpace address_space{config};
        ModuleConfig module_config{};
        module_config.feature_overrides.Set(FeatureId::abs_const_mat, false);
        module_config.feature_overrides.Set(FeatureId::const_addr_cache, cache);
        module_config.feature_overrides.Set(FeatureId::ra_coalesce, coalesce);
        auto module = address_space.MapModule(
                LocationDescriptor{0x78a0}, LocationDescriptor{0x78c0}, module_config);

        IntrusivePtr<Block> block{new Block(0, Location{0x78a0})};
        auto first = block->GetOperand(Operand{Imm{swift::u64{0x004958d8}}})
                             .SetType(ValueType::U64);
        (void)block->LoadMemory(Operand{first}).SetType(ValueType::U64);
        auto second = block->GetOperand(Operand{Imm{second_address}})
                              .SetType(ValueType::U64);
        (void)block->LoadMemory(Operand{second}).SetType(ValueType::U64);
        Inst* publish = nullptr;
        if (coalesce) {
            auto value = block->LoadImm(Imm{swift::u64{0x123456789abcdef0}})
                                 .SetType(ValueType::U64);
            publish = block->AppendInst(
                    OpCode::SetHostGPR, value, HostRegIndex(22), Imm{0u});
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        GPRSMask gprs{~swift::u32{0}};
        if (tight_pool) {
            for (swift::u32 code : {6u, 7u, 8u, 9u}) {
                gprs.Clear(code);
            }
        } else {
            for (swift::u32 code : {6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u,
                             15u, 16u, 17u, 18u, 24u}) {
                gprs.Clear(code);
            }
            for (swift::u32 code : {0u, 1u, 2u, 3u, 4u, 5u, 19u, 20u, 21u,
                             22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u}) {
                gprs.Mark(code);
            }
        }
        const FPRSMask fprs{~((1u << 8) - 1u)};
        auto features = FeatureSet{};
        features.const_addr_cache = cache;
        features.ra_coalesce = coalesce;
        RegAlloc alloc{block->MaxInstrId(), gprs, fprs, features};
        RegisterAllocPass::Run(block.get(), &alloc, false, features);

        const bool anchor = alloc.IsConstAddressCached(first.Id());
        const bool reuse = alloc.IsConstAddressCached(second.Id());
        const bool host_write = publish && alloc.IsHostWriteCoalesced(publish->Id());
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        return Result{context.CurrentBufferSize(), anchor, reuse, host_write};
    };

    SECTION("a repeated RIP-style absolute address shares one page base") {
        const auto off = run(false, false, false);
        const auto on = run(true, false, false);
        REQUIRE_FALSE(off.anchor);
        REQUIRE_FALSE(off.reuse);
        REQUIRE(on.anchor);
        REQUIRE(on.reuse);
        REQUIRE(on.bytes + 4 * vixl::aarch64::kInstructionSize == off.bytes);
    }

    SECTION("nearby absolute addresses share one page base") {
        const auto off = run(false, false, false, 0x004958e0);
        const auto on = run(true, false, false, 0x004958e0);
        REQUIRE(on.anchor);
        REQUIRE(on.reuse);
        REQUIRE(on.bytes + 4 * vixl::aarch64::kInstructionSize == off.bytes);
    }

    SECTION("biased memory rematerializes each exact guest address") {
        const auto off = run(false, false, false, 0x004958e0, true);
        const auto on = run(true, false, false, 0x004958e0, true);
        REQUIRE(on.anchor);
        REQUIRE(on.reuse);
        REQUIRE(on.bytes + 2 * vixl::aarch64::kInstructionSize == off.bytes);
    }

    SECTION("biased memory reuses an unchanged exact guest address") {
        const auto off = run(false, false, false, 0x004958d8, true);
        const auto on = run(true, false, false, 0x004958d8, true);
        REQUIRE(on.anchor);
        REQUIRE(on.reuse);
        REQUIRE(on.bytes + 4 * vixl::aarch64::kInstructionSize == off.bytes);
    }

    SECTION("scratch headroom shortage falls back without a cache owner") {
        const auto tight = run(true, true, false);
        REQUIRE_FALSE(tight.anchor);
        REQUIRE_FALSE(tight.reuse);
    }

    SECTION("guest publication coalescing and address caching remain independent") {
        const auto both = run(true, false, true);
        REQUIRE(both.anchor);
        REQUIRE(both.reuse);
        REQUIRE(both.host_write);
    }
}

TEST_CASE("indirect L1 and lean shadow stack select compatible return paths") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    struct Result {
        swift::u32 bytes{};
        std::map<std::string, swift::u32> mnemonics{};
        bool host_write_coalesced{};
    };
    enum class Shape { Indirect, Call, Return };
    auto run = [](bool indirect_l1, bool shadow_lean, Shape shape,
                  bool trailing_instruction, bool coalesce) {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .global_opts = Optimizations::All,
        };
        AddressSpace address_space{config};
        ModuleConfig module_config{};
        module_config.feature_overrides.Set(FeatureId::indirect_l1, indirect_l1);
        module_config.feature_overrides.Set(FeatureId::shadow_lean, shadow_lean);
        module_config.feature_overrides.Set(FeatureId::ra_coalesce, coalesce);
        auto module = address_space.MapModule(
                LocationDescriptor{0x79a0}, LocationDescriptor{0x79e0}, module_config);

        IntrusivePtr<Block> block{new Block(0, Location{0x79a0})};
        Inst* publish = nullptr;
        if (coalesce) {
            auto value = block->LoadImm(Imm{swift::u64{0x123456789abcdef0}})
                                 .SetType(ValueType::U64);
            publish = block->AppendInst(
                    OpCode::SetHostGPR, value, HostRegIndex(22), Imm{0u});
        }
        auto target = block->LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{0, ValueType::U64});
        if (shape == Shape::Call) {
            block->AppendInst(OpCode::PushRSB,
                              Lambda{Imm{swift::u64{0x42f2b1}}});
        }
        block->AppendInst(OpCode::SetLocation, Lambda{target});
        if (trailing_instruction) {
            block->AppendInst(OpCode::Nop);
        }
        if (shape == Shape::Return) {
            block->SetTerminal(terminal::PopRSBHint{});
        } else {
            block->SetTerminal(terminal::ReturnToDispatch{});
        }
        block->ReIdInstr();

        GPRSMask gprs{0};
        for (swift::u32 code : {0u, 1u, 2u, 3u, 4u, 5u, 19u, 20u, 21u,
                                22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u}) {
            gprs.Mark(code);
        }
        const FPRSMask fprs{~((1u << 8) - 1u)};
        auto features = ResolveFeatureSet(module_config);
        RegAlloc alloc{block->MaxInstrId(), gprs, fprs, features};
        RegisterAllocPass::Run(block.get(), &alloc, false, features);
        const bool host_write = publish && alloc.IsHostWriteCoalesced(publish->Id());

        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();

        std::map<std::string, swift::u32> mnemonics;
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        auto& masm = context.GetMasm();
        auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
        auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
        for (auto* instruction = first; instruction < last; ++instruction) {
            decoder.Decode(instruction);
            std::string_view text{disassembler.GetOutput()};
            const auto begin = text.find_first_not_of(" \t");
            if (begin == std::string_view::npos) continue;
            text.remove_prefix(begin);
            const auto end = text.find_first_of(" \t");
            ++mnemonics[std::string{text.substr(0, end)}];
        }
        return Result{context.CurrentBufferSize(), std::move(mnemonics), host_write};
    };

    const auto off = run(false, false, Shape::Indirect, false, true);
    const auto l1 = run(true, false, Shape::Indirect, false, true);
    const auto shadow = run(false, true, Shape::Indirect, false, true);
    auto count = [](const Result& result, std::string_view mnemonic) {
        auto it = result.mnemonics.find(std::string{mnemonic});
        return it == result.mnemonics.end() ? 0u : it->second;
    };
    REQUIRE(l1.bytes == off.bytes + 6 * vixl::aarch64::kInstructionSize);
    REQUIRE(shadow.bytes == off.bytes);
    REQUIRE(count(off, "br") == 0);
    REQUIRE(count(l1, "br") == 1);
    REQUIRE(count(l1, "ret") == 0);
    REQUIRE(count(l1, "cmp") == count(off, "cmp") + 1);
    REQUIRE(count(l1, "ccmp") == count(off, "ccmp"));
    REQUIRE(count(l1, "csel") == 1);
    REQUIRE(count(l1, "bfi") == count(off, "bfi") + 1);
    REQUIRE(count(l1, "ldp") == count(off, "ldp") + 1);
    REQUIRE(count(l1, "tst") == count(off, "tst"));
    REQUIRE(l1.host_write_coalesced);

    const auto call_off = run(false, false, Shape::Call, false, false);
    const auto call_l1 = run(true, false, Shape::Call, false, false);
    const auto call_shadow = run(false, true, Shape::Call, false, false);
    const auto call_both = run(true, true, Shape::Call, false, false);
    REQUIRE(call_l1.bytes ==
            call_off.bytes + vixl::aarch64::kInstructionSize);
    REQUIRE(call_shadow.bytes ==
            call_off.bytes - 2 * vixl::aarch64::kInstructionSize);
    REQUIRE(call_both.bytes == call_l1.bytes);
    REQUIRE(count(call_l1, "stp") + 1 == count(call_off, "stp"));

    const auto ret_off = run(false, false, Shape::Return, false, false);
    const auto ret_l1 = run(true, false, Shape::Return, false, false);
    const auto ret_shadow = run(false, true, Shape::Return, false, false);
    const auto ret_both = run(true, true, Shape::Return, false, false);
    REQUIRE(ret_l1.bytes ==
            ret_off.bytes - 4 * vixl::aarch64::kInstructionSize);
    REQUIRE(ret_shadow.bytes ==
            ret_off.bytes - vixl::aarch64::kInstructionSize);
    REQUIRE(ret_both.bytes == ret_l1.bytes);
    REQUIRE(count(ret_l1, "ldp") == count(ret_off, "ldp"));
    REQUIRE(count(ret_l1, "cmp") == count(ret_off, "cmp"));

    const auto ret_trailing = run(false, true, Shape::Return, true, false);
    REQUIRE(ret_trailing.bytes ==
            ret_shadow.bytes + vixl::aarch64::kInstructionSize);
    REQUIRE(count(ret_trailing, "ldr") == count(ret_shadow, "ldr") + 1);
    const auto ret_l1_trailing = run(true, true, Shape::Return, true, false);
    REQUIRE(ret_l1_trailing.bytes < ret_trailing.bytes);
    REQUIRE(count(ret_l1_trailing, "br") == 0);
    REQUIRE(count(ret_l1_trailing, "ret") == 1);

    // Any instruction after SetLocation clears the retained SSA register;
    // the feature must then emit the byte-identical dispatcher fallback.
    const auto trailing_off =
            run(false, false, Shape::Indirect, true, false);
    const auto trailing_on =
            run(true, false, Shape::Indirect, true, false);
    REQUIRE(trailing_on.bytes == trailing_off.bytes);
    REQUIRE(count(trailing_on, "br") == 0);
}

// --- x86 mul CF/OF must reach the flags register -----------------------------
//
// JitTranslator::SaveCV/SaveOF used to write their C/V bits into the HOST NZCV
// register (Msr) and then clear nzcv_dirty, which is exactly the state that
// makes MergeNZCV() do nothing: the bits were produced and then dropped. The
// x86 frontend cannot reach either function today -- MulWithFlags materialises
// CF/OF through a separate `SaveFlags(t + t, C|V)` producer rather than hanging
// the pseudo on the Mul, and the only frontend ir::Div is the flagless RCL/RCR
// modulus -- so no guest program and no exit code can catch this. The IR the
// backend accepts is wider than the IR the frontend currently emits, and this
// builds the missing shape directly.
//
// The check is on emitted code rather than on a running guest because
// executing a bare Block needs the trampoline/State machinery a unit test does
// not have. That is still a real check: whether the two C/V bits are OR-ed into
// the flags register (x26) or parked in host NZCV is precisely the difference
// between the broken and the fixed lowering.
