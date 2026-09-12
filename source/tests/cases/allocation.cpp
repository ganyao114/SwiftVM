#include "../support/allocation_inputs.h"
#include "../support/case_support.h"
#include "../support/fp_environment.h"

TEST_CASE("Unreachable blocks keep unique IDs after function finalization") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;
    FeatureSet features{};
    features.const_addr_cache = true;
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0x1000}, Location{0x1200});
    auto live = function->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
    function->StoreUniform(Uniform{0, ValueType::U64}, live);
    function->EndBlock(terminal::ReturnToHost{});

    auto* dead = function->AppendBlock(Location{0x1100});
    function->SetCurBlock(dead);
    for (swift::u64 offset : {0u, 8u}) {
        auto address = function->GetOperand(Operand{Imm{swift::u64{0x400000} + offset}})
                               .SetType(ValueType::U64);
        auto value = function->LoadMemory(address).SetType(ValueType::U64);
        function->StoreUniform(Uniform{0, ValueType::U64}, value);
    }
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    REQUIRE(function->GetHIRBlockList().empty());
    function->ComputeRPO();
    REQUIRE(function->GetHIRBlocksRPO().size() == 1);

    // Runtime renumbers both before and after optimization. Each pass must
    // keep dead instruction IDs out of the live range and inside the table.
    for (unsigned pass = 0; pass < 2; ++pass) {
        function->IdByRPO();
        std::vector<bool> seen(function->MaxInstrCount());
        const auto live_count = function->GetHIRValues().size();
        for (auto* current : function->GetHIRBlocks()) {
            for (auto& inst : current->GetInstList()) {
                REQUIRE(inst.Id() < seen.size());
                REQUIRE_FALSE(seen[inst.Id()]);
                seen[inst.Id()] = true;
                if (current == dead) {
                    REQUIRE(inst.Id() >= live_count);
                    REQUIRE(function->GetHIRValue(Value{&inst}) == nullptr);
                }
            }
        }
        REQUIRE(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
        RegAlloc allocation{function->MaxInstrCount(), GPRSMask{}, FPRSMask{}, features};
        RegisterAllocPass::Run(function, &allocation, features);
        REQUIRE(allocation.ValueType(live) == RegAlloc::GPR);
    }
}

TEST_CASE("Single-block register allocation is map-identical to the general path") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    // One real block containing all high-risk shapes: fixed host-register
    // aliases (both crossing and non-crossing writes), a host call, a terminal-
    // only value, a BitCast alias, and enough simultaneous scalar liveness to
    // force spills in the deliberately small register pool below.
    HIRBuilder builder{1, true, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{0x1000}, Location{0x1100});

    auto pinned =
            function->GetHostGPR(HostRegIndex(0), Imm{0u}).SetType(ValueType::U64);
    auto capture =
            function->GetHostGPR(HostRegIndex(1), Imm{0u}).SetType(ValueType::U64);
    auto fixed_fpr =
            function->GetHostFPR(HostRegIndex(2), Imm{0u}).SetType(ValueType::V128);

    std::vector<Value> live;
    for (std::uint64_t i = 0; i < 14; ++i) {
        live.push_back(function->LoadImm(Imm{0x100u + i}).SetType(ValueType::U64));
    }
    auto alias = function->BitCast(live[3]).SetType(ValueType::U64);

    // The SetHostGPR crosses capture's lifetime, so capture must receive a
    // normal allocation instead of aliasing host register 1.
    function->SetHostGPR(live[0], HostRegIndex(1), Imm{0u});
    auto pinned_use = function->Add(pinned, Operand{live[1]}).SetType(ValueType::U64);
    auto capture_use =
            function->Add(capture, Operand{alias}).SetType(ValueType::U64);

    Params params{};
    params.Push(live[2]);
    params.Push(capture_use);
    params.Push(pinned_use);
    function->CallDynamic(Lambda{Imm{std::uint64_t(1ull)}}, params);

    // Consume the pressure values after the call so all of them cross it.
    Value sum = live.back();
    for (int i = static_cast<int>(live.size()) - 2; i >= 0; --i) {
        sum = function->Add(sum, Operand{live[i]}).SetType(ValueType::U64);
    }
    sum = function->Add(sum, Operand{capture_use}).SetType(ValueType::U64);
    function->StoreUniform(Uniform{0, ValueType::U64}, sum);
    function->StoreUniform(Uniform{16, ValueType::V128}, fixed_fpr);

    auto terminal_cond = function->TestNotZero(live[4]);
    function->EndBlock(terminal::If{
            terminal_cond, terminal::ReturnToDispatch{}, terminal::ReturnToHost{}});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    REQUIRE(function->GetHIRBlocksRPO().size() == 1);

    // Bits set are unavailable. Leave six allocatable GPRs/FPRs, less the
    // default scratch reserve, so the block must take the spill path.
    constexpr std::uint32_t available_gprs = 0x000003f0u;  // x4..x9
    constexpr std::uint32_t available_fprs = 0x00000f78u;  // v3..v6, v8..v11
    GPRSMask gprs{~available_gprs};
    FPRSMask fprs{~available_fprs};
    RegAlloc general{function->MaxInstrCount(), gprs, fprs, FeatureSet{}};
    RegAlloc fast{function->MaxInstrCount(), gprs, fprs, FeatureSet{}};
    RegAlloc selected{function->MaxInstrCount(), gprs, fprs, FeatureSet{}};

    RegisterAllocPass::Run(function, &general, false, FeatureSet{});
    RegisterAllocPass::Run(function, &fast, true, FeatureSet{});
    RegisterAllocPass::Run(function, &selected, FeatureSet{});

    REQUIRE(general.MapCount() == fast.MapCount());
    bool saw_spill = false;
    for (std::uint32_t id = 0; id < general.MapCount(); ++id) {
        INFO("allocation map id " << id);
        REQUIRE(general.Mapping(id) == fast.Mapping(id));
        saw_spill |= general.Mapping(id).type == RegAlloc::MEM;
    }
    REQUIRE(saw_spill);
    REQUIRE(general.ValueType(pinned) == RegAlloc::GPR);
    REQUIRE(general.ValueGPR(pinned).id == 0);
    REQUIRE(general.ValueType(capture) != RegAlloc::REF);
    REQUIRE(general.ValueType(fixed_fpr) == RegAlloc::FPR);
    REQUIRE(general.ValueFPR(fixed_fpr).id == 2);
    const auto& expected = swift::runtime::GetSvmConfig().ra_1blk ? fast : general;
    for (std::uint32_t id = 0; id < selected.MapCount(); ++id) {
        INFO("production selector map id " << id);
        REQUIRE(selected.Mapping(id) == expected.Mapping(id));
    }
}

// Builds a block that keeps `live` scalar values simultaneously live across a
// VecFAdd -- the emitter with the largest scratch appetite in the backend
// (JitTranslator::EmitVecFloatNaNFixup holds eight GPRs at once). Consuming
// the scalars only after the VecFAdd is what makes them live *across* it, so
// the linear scan is asked to fill the register file at exactly the
// instruction that needs the most scratch.

struct SpillEvictChoiceBlock {
    swift::runtime::ir::Block* block{};
    swift::runtime::ir::Value longest{};
    swift::runtime::ir::Value arriving{};
};

static SpillEvictChoiceBlock BuildSpillEvictChoiceBlock() {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x2400});
    auto longest = block->LoadImm(Imm{swift::u64{1}});
    auto middle = block->LoadImm(Imm{swift::u64{2}});
    auto shortest = block->LoadImm(Imm{swift::u64{3}});
    auto arriving = block->LoadImm(Imm{swift::u64{4}});
    auto first = block->Add(arriving, Operand{shortest});
    auto second = block->Add(first, Operand{middle});
    auto total = block->Add(second, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {block, longest, arriving};
}

static swift::runtime::ir::Block* BuildSpillEvictFallbackBlock() {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x2410});
    std::vector<Value> anchors;
    for (swift::u64 value = 1; value <= 7; ++value) {
        anchors.push_back(block->LoadImm(Imm{value}));
    }
    auto arriving = block->LoadImm(Imm{swift::u64{8}});
    auto vector = block->LoadUniform<TypedValue<ValueType::V128>>(
            Uniform{32, ValueType::V128});
    auto converted = block->VecFCvtFloatToInt(
            vector, Imm{32u}, Imm{64u}, Imm{0u}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{40, ValueType::U64}, converted);
    Value total = arriving;
    for (auto it = anchors.rbegin(); it != anchors.rend(); ++it) {
        total = block->Add(total, Operand{*it});
    }
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

TEST_CASE("Spill eviction chooses farthest end and preserves verified fallback") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    // Six free registers and the ordinary reserve of three leave exactly
    // three value locations. The fourth definition is shorter-lived than all
    // three active values, while `longest` has the unique farthest end.
    const GPRSMask gprs{~((1u << 6) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    auto off_case = BuildSpillEvictChoiceBlock();
    swift::runtime::IntrusivePtr<Block> off_block{off_case.block};
    RegAlloc off{off_case.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    const auto off_result = RegisterAllocTestSupport::RunForSpillEvictTest(
            off_case.block, &off, false);
    REQUIRE(off.ValueType(off_case.arriving) == RegAlloc::MEM);
    REQUIRE(off_result.eviction_restarts == 0);

    auto on_case = BuildSpillEvictChoiceBlock();
    swift::runtime::IntrusivePtr<Block> on_block{on_case.block};
    RegAlloc on{on_case.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    const auto on_result = RegisterAllocTestSupport::RunForSpillEvictTest(
            on_case.block, &on, true);
    REQUIRE(on_result.eviction_restarts >= 1);
    REQUIRE_FALSE(on_result.fell_back_to_ladder);
    REQUIRE(on.ValueType(on_case.longest) == RegAlloc::MEM);
    REQUIRE(on.ValueType(on_case.arriving) == RegAlloc::GPR);

    // This block deliberately reads many long-lived spilled values while the
    // opcode scratch budget is also live. If the default-reserve eviction
    // attempt cannot satisfy Verify(), it must be discarded and the existing
    // reserve ladder must produce the final allocation.
    swift::runtime::IntrusivePtr<Block> fallback_block{BuildSpillEvictFallbackBlock()};
    const GPRSMask fallback_gprs{~((1u << 10) - 1u)};
    RegAlloc fallback{fallback_block->MaxInstrId(), fallback_gprs, fprs,
                      FeatureSet{}};
    const auto fallback_result = RegisterAllocTestSupport::RunForSpillEvictTest(
            fallback_block.get(), &fallback, true);
    REQUIRE(fallback_result.eviction_restarts >= 1);
    REQUIRE(fallback_result.fell_back_to_ladder);
    REQUIRE(fallback_result.final_gpr_reserve > 3);
}
