#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include "../support/allocation_inputs.h"
#include "../support/case_support.h"
#include "../support/fp_environment.h"
#include "runtime/compile/allocate/transaction.h"

TEST_CASE("Allocation edits journal only touched values and restore aliases owners and masks") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    AllocationFixture allocation{4096, GPRSMask{UINT32_MAX}, FPRSMask{}, FeatureSet{}};
    allocation.MapRegister(10, HostGPR{4});
    allocation.MapReference(10, 11);
    allocation.MarkHostReadCoalesced(12);
    allocation.MarkHostWriteCoalesced(20);
    REQUIRE(allocation.FreezeWidthComponentOwner(10, 19, true));
    GPRSMask old_gprs{0x40};
    FPRSMask old_fprs{0x80};
    allocation.SetActiveRegs(43, old_gprs, old_fprs);
    const auto old10 = allocation.Mapping(10);
    const auto old11 = allocation.Mapping(11);
    const auto old12 = allocation.Mapping(12);
    const auto old43 = allocation.Mapping(43);
    const auto untouched = allocation.Mapping(3000);

    const auto change = [&](compile::AllocationEdit& edit) {
        allocation.MapRegister(10, HostGPR{4});
        allocation.MarkHostReadCoalesced(12);
        allocation.SetActiveRegs(43, old_gprs, old_fprs);
        REQUIRE(edit.TouchedValueCount() == 0);
        REQUIRE(edit.LocalIndexEntryCount() == 0);
        REQUIRE(edit.ExternalIndexEntryCount() == 0);
        allocation.MapRegister(10, HostGPR{7});
        allocation.MapRegister(10, HostGPR{8});
        allocation.MapReference(12, 11);
        allocation.MarkHostReadCoalesced(11);
        allocation.MarkHostWriteCoalesced(12);
        allocation.MarkWidthChainCoalesced(12, 10);
        REQUIRE(allocation.CommitWidthComponentOwner(10, 19));
        REQUIRE(allocation.FreezeWidthComponentOwner(11, 0, false));
        REQUIRE(allocation.CommitWidthComponentOwner(11, 0));
        GPRSMask next_gprs{0x400};
        FPRSMask next_fprs{0x800};
        allocation.SetActiveRegs(43, next_gprs, next_fprs);
        REQUIRE(edit.TouchedValueCount() == 4);
        REQUIRE(edit.LocalIndexEntryCount() == 34);
        REQUIRE(edit.ExternalIndexEntryCount() == 0);
        REQUIRE_FALSE(edit.OriginalHostWrite(12));
        REQUIRE(edit.OriginalHostWrite(20));
        REQUIRE(edit.OriginalWidthAnchor(12) == UINT32_MAX);
        REQUIRE(allocation.Mapping(3000) == untouched);
    };

    SECTION("rejection restores the original state") {
        compile::AllocationEdit edit{allocation, 10, 44};
        change(edit);
        edit.Rollback();
    }
    SECTION("scope unwinding restores the original state") {
        struct Rejected {};
        bool unwound = false;
        try {
            compile::AllocationEdit edit{allocation, 10, 44};
            change(edit);
            throw Rejected{};
        } catch (const Rejected&) { unwound = true; }
        REQUIRE(unwound);
    }
    SECTION("accepted edits persist after their scope") {
        {
            compile::AllocationEdit edit{allocation, 10, 44};
            change(edit);
            edit.Commit();
        }
        REQUIRE(allocation.ValueGPR(10).id == 8);
        REQUIRE(allocation.Mapping(11).type == AllocationFixture::REF);
        REQUIRE(allocation.Mapping(11).slot == 12);
        REQUIRE(allocation.IsHostReadCoalesced(11));
        REQUIRE(allocation.IsHostWriteCoalesced(12));
        REQUIRE(allocation.WidthChainAnchor(12) == 10);
        REQUIRE(allocation.WidthComponentOwnerCommitted(10));
        REQUIRE(allocation.WidthComponentOwnerCommitted(11));
        REQUIRE(allocation.DirtyGPR(43).Get(10));
        REQUIRE(allocation.DirtyFPR(43).Get(11));
        return;
    }
    SECTION("a cross-window value uses sparse storage and rolls back") {
        {
            compile::AllocationEdit edit{allocation, 10, 44};
            allocation.MapRegister(3000, HostGPR{7});
            allocation.MapRegister(3000, HostGPR{8});
            REQUIRE(edit.TouchedValueCount() == 1);
            REQUIRE(edit.LocalIndexEntryCount() == 0);
            REQUIRE(edit.ExternalIndexEntryCount() == 1);
        }
        REQUIRE(allocation.Mapping(3000) == untouched);
    }
    REQUIRE(allocation.Mapping(10) == old10);
    REQUIRE(allocation.Mapping(11) == old11);
    REQUIRE(allocation.Mapping(12) == old12);
    REQUIRE(allocation.Mapping(43) == old43);
    REQUIRE_FALSE(allocation.IsHostReadCoalesced(11));
    REQUIRE(allocation.IsHostReadCoalesced(12));
    REQUIRE_FALSE(allocation.IsHostWriteCoalesced(12));
    REQUIRE(allocation.IsHostWriteCoalesced(20));
    REQUIRE_FALSE(allocation.IsWidthChainCoalesced(12));
    REQUIRE(allocation.HasWidthComponentOwner(10));
    REQUIRE_FALSE(allocation.WidthComponentOwnerCommitted(10));
    REQUIRE_FALSE(allocation.HasWidthComponentOwner(11));
    REQUIRE(allocation.Mapping(3000) == untouched);
}

TEST_CASE("Fixed read inputs preserve captures across guest home writes",
          "[fixed-register-inputs]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        CAPTURE(scenario);
        const bool floating = scenario >= 3;
        const bool overwrite = scenario == 1 || scenario == 4;
        const bool narrow = scenario == 2;
        const auto type = floating ? ValueType::V128 : narrow ? ValueType::U32 : ValueType::U64;
        const swift::u32 home = floating ? 17 : 20;
        IntrusivePtr<Block> block{new Block(0, Location{0x58c0})};
        const auto read = floating ? block->GetHostFPR(type, Imm{home}, Imm{swift::u32{0}})
                                   : block->GetHostGPR(type, Imm{home}, Imm{swift::u32{0}});
        if (overwrite) {
            const auto next = floating ? block->LoadUniform(type, Uniform{16, type})
                                       : block->LoadImm(type, Imm{swift::u64{17}});
            if (floating)
                block->SetHostFPR(next, Imm{home}, Imm{swift::u32{0}});
            else
                block->SetHostGPR(next, Imm{home}, Imm{swift::u32{0}});
        }
        block->StoreUniform(Uniform{0, type}, read);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        GPRSMask gprs{0};
        for (swift::u32 code = 0; code < 32; ++code)
            if ((kX86FixedGPRHomes & (swift::u32{1} << code)) || code == 10 || code == 18 ||
                (code >= 24 && code <= 28) || code >= 30)
                gprs.Mark(code);
        auto features = FeatureSet{};
        features.ra_fixed_class = true;
        features.ra_coalesce = false;
        features.ra_width_chain = false;
        features.ra_width_chain_long = false;
        AllocationFixture allocation{block->MaxInstrId(), gprs, FPRSMask{~0xffu}, features};
        auto input = arm64::PrepareAllocationInputs(block.get(), allocation, features);
        REQUIRE(input.fixed_reads.size() == (narrow ? 0 : 1));
        // Exercise interval-time read admission independently of the initial
        // fixed/preservation declarations; surviving writes must force a copy.
        input.fixed.clear();
        compile::AllocateBlock(block.get(), &allocation, std::move(input));
        const bool alias = !overwrite && !narrow;
        if (floating) {
            REQUIRE(allocation.ValueType(read) == AllocationFixture::FPR);
            REQUIRE((allocation.ValueFPR(read).id == home) == alias);
            REQUIRE(allocation.IsHostReadCoalesced(read.Id()) == alias);
        } else {
            REQUIRE(allocation.ValueType(read) == AllocationFixture::GPR);
            REQUIRE((allocation.ValueGPR(read).id == home) == alias);
            REQUIRE(allocation.IsFixedGPR(read.Id()) == (alias && GetSvmConfig().x86_pin_ext >= 3));
        }
        allocation.FreezeGPRLocations();
    }
}

TEST_CASE("Fixed home conflicts respect transferred windows and ignored alias owners") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    AllocationFixture allocation{64, GPRSMask{UINT32_MAX}, FPRSMask{}, FeatureSet{}};
    allocation.PlanFixedGPRDefinition(2, {.home = 0, .end = 30});
    allocation.PlanFixedGPRDefinition(4, {.home = 0, .end = 6});
    allocation.PlanFixedGPRDefinition(
            8, {.home = 0, .transferred_home = 19, .end = 12, .transfer = 12, .last_use = 20});
    // The earlier long span remains relevant after a later short span ends.
    REQUIRE(allocation.PrepareFixedGPRHomes().Conflicts(0, 25, 28));
    REQUIRE_FALSE(allocation.PrepareFixedGPRHomes().Conflicts(0, 25, 28, 2));
    REQUIRE(allocation.PrepareFixedGPRHomes().Conflicts(0, 5, 5, 2));
    REQUIRE_FALSE(allocation.PrepareFixedGPRHomes().Conflicts(0, 5, 5, 2, 4));
    REQUIRE_FALSE(allocation.PrepareFixedGPRHomes().Conflicts(0, 30, 31));
    REQUIRE_FALSE(allocation.PrepareFixedGPRHomes().Conflicts(19, 10, 11));
    REQUIRE(allocation.PrepareFixedGPRHomes().Conflicts(19, 11, 12));
    REQUIRE(allocation.PrepareFixedGPRHomes().Conflicts(19, 12, 19));
    REQUIRE_FALSE(allocation.PrepareFixedGPRHomes().Conflicts(19, 12, 19, 8));
    REQUIRE_FALSE(allocation.PrepareFixedGPRHomes().Conflicts(19, 20, 21));
    REQUIRE_FALSE(allocation.PrepareFixedGPRHomes().Conflicts(20, 2, 30));
    allocation.PlanFixedGPRDefinition(8, {});
    REQUIRE_FALSE(allocation.PrepareFixedGPRHomes().Conflicts(19, 12, 19));
    REQUIRE(allocation.PrepareFixedGPRHomes().Conflicts(0, 25, 28));
}

TEST_CASE("Rejected allocation edits restore fixed homes publications and consumer locations") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    AllocationFixture allocation{64, GPRSMask{UINT32_MAX}, FPRSMask{}, FeatureSet{}};
    const compile::FixedGPRDefinition original{.home = 0,
                                               .end = 20,
                                               .last_use = 20,
                                               .publication = 20,
                                               .owner = 5,
                                               .width = 4,
                                               .version = true};
    allocation.PlanFixedGPRDefinition(5, original);
    allocation.AssignGPRLowView(5, 0);
    allocation.RegisterGPRUseHome(5, 10, {.home = 0, .width = 4}, false);
    REQUIRE(allocation.PrepareFixedGPRHomes().Conflicts(0, 6, 10));
    REQUIRE_FALSE(allocation.PrepareFixedGPRHomes().Conflicts(19, 6, 10));
    {
        compile::AllocationEdit edit{allocation};
        auto changed = original;
        changed.home = 19;
        changed.publication = 21;
        allocation.PlanFixedGPRDefinition(5, changed);
        changed.owner = 6;
        changed.publication = 23;
        allocation.PlanFixedGPRDefinition(6, changed);
        allocation.AssignGPRLowView(6, 19);
        allocation.RegisterGPRUseHome(5, 10, {.home = 19, .width = 4}, true);
        allocation.RegisterGPRUseHome(5, 11, {.home = 19, .width = 4}, true);
        REQUIRE(allocation.GPRUseHome(5, 10, true)->home == 19);
        REQUIRE(allocation.AdditionalGPRResidentUses(5) == 1);
        REQUIRE(allocation.NeedsFixedGPRPublication(21));
        REQUIRE(allocation.NeedsFixedGPRPublication(23));
        REQUIRE_FALSE(allocation.PrepareFixedGPRHomes().Conflicts(0, 6, 10));
        REQUIRE(allocation.PrepareFixedGPRHomes().Conflicts(19, 6, 10));
    }
    const auto* restored = allocation.FixedGPRDefinitionAt(5);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->home == original.home);
    REQUIRE(restored->publication == original.publication);
    REQUIRE(restored->last_use == original.last_use);
    REQUIRE_FALSE(allocation.HasFixedGPRDefinition(6));
    REQUIRE(allocation.NeedsFixedGPRPublication(20));
    REQUIRE_FALSE(allocation.NeedsFixedGPRPublication(21));
    REQUIRE_FALSE(allocation.NeedsFixedGPRPublication(23));
    REQUIRE(allocation.GPRLowView(5) == 0);
    REQUIRE_FALSE(allocation.GPRLowView(6));
    REQUIRE(allocation.GPRUseHome(5, 10)->home == 0);
    REQUIRE_FALSE(allocation.GPRUseHome(5, 10, true));
    REQUIRE_FALSE(allocation.GPRUseHome(5, 11));
    REQUIRE(allocation.AdditionalGPRResidentUses(5) == 0);
    REQUIRE(allocation.PrepareFixedGPRHomes().Conflicts(0, 6, 10));
    REQUIRE_FALSE(allocation.PrepareFixedGPRHomes().Conflicts(19, 6, 10));
}

TEST_CASE("Publication recoloring preserves other live and reload owners", "[allocation][fixed-gpr]") {
    using swift::u32;
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    AllocationFixture allocation{
            64, GPRSMask{(1u << 19) | (1u << 20)}, FPRSMask{}, FeatureSet{}};
    allocation.MapMemSpill(9, SpillSlot{16});
    allocation.MapMemSpill(10, SpillSlot{24});
    allocation.MapReference(10, 11);
    allocation.MapRegister(12, HostGPR{5});
    allocation.MapRegister(14, HostGPR{5});
    allocation.SetLiveEnd(9, 16);
    allocation.SetLiveEnd(10, 16);
    allocation.SetLiveEnd(12, 17);
    allocation.SetLiveEnd(14, 20);
    allocation.MarkWidthChainCoalesced(12, 10);
    allocation.MarkLow32CopyCoalesced(12, 10);
    allocation.AssignGPRLowView(12, 5);
    for (u32 id = 10; id <= 17; ++id) {
        GPRSMask gprs{(1u << 19) | (1u << 20) | (1u << 7) | (1u << 5)};
        FPRSMask fprs{0x80};
        allocation.SetActiveRegs(id, gprs, fprs);
    }
    allocation.MapSpillReload(10, 11, 15, HostGPR{4});
    allocation.MapSpillReload(9, 14, 16, HostGPR{4});
    const auto old10 = allocation.Mapping(10);
    const auto old11 = allocation.Mapping(11);
    const auto old12 = allocation.Mapping(12);
    const auto version = allocation.AllocationAttempt();
    const std::array<u32, 5> component{12, 11, 10, 12, 11};
    Vector<compile::GPRRecolorSite> sites;
    for (u32 id = 10; id <= 17; ++id) sites.push_back({id, id == 13 ? 1u << 4 : 0});

    const auto recolor = [&](compile::AllocationEdit& edit) {
        REQUIRE(edit.RecolorGPR(component, 19, true, sites) == compile::RecolorFailure::None);
        REQUIRE(allocation.ValueGPR(10).id == 19);
        REQUIRE(allocation.ValueGPR(11).id == 19);
        REQUIRE(allocation.ValueGPR(12).id == 19);
        REQUIRE(allocation.GPRLowView(12) == 19);
        REQUIRE(allocation.IsFixedGPR(10));
        REQUIRE_FALSE(allocation.HasSpillReload(10, 11));
        REQUIRE_FALSE(allocation.HasSpillReload(10, 15));
        REQUIRE(allocation.HasSpillReload(9, 14));
        REQUIRE_FALSE(allocation.DirtyGPR(11).Get(4));
        REQUIRE(allocation.DirtyGPR(13).Get(4));
        REQUIRE(allocation.DirtyGPR(14).Get(4));
        REQUIRE_FALSE(allocation.DirtyGPR(17).Get(4));
        REQUIRE_FALSE(allocation.DirtyGPR(13).Get(5));
        REQUIRE(allocation.DirtyGPR(14).Get(5));
        REQUIRE(allocation.DirtyGPR(17).Get(5));
        REQUIRE(allocation.DirtyGPR(11).Get(7));
        REQUIRE(allocation.DirtyGPR(11).Get(19));
        REQUIRE(allocation.DirtyFPR(11).Get(7));
    };
    SECTION("a rejected edit restores reloads views masks and the physical version") {
        {
            compile::AllocationEdit edit{allocation, 10, 18};
            recolor(edit);
        }
        REQUIRE(allocation.Mapping(10) == old10);
        REQUIRE(allocation.Mapping(11) == old11);
        REQUIRE(allocation.Mapping(12) == old12);
        REQUIRE(allocation.GPRLowView(12) == 5);
        REQUIRE(allocation.HasSpillReload(10, 11));
        REQUIRE(allocation.HasSpillReload(10, 15));
        REQUIRE(allocation.HasSpillReload(9, 14));
        REQUIRE(allocation.AllocationAttempt() == version);
    }
    SECTION("a committed edit advances the physical version") {
        {
            compile::AllocationEdit edit{allocation, 10, 18};
            recolor(edit);
            edit.Commit();
        }
        REQUIRE(allocation.AllocationAttempt() == version + 1);
        REQUIRE(allocation.ValueGPR(11).id == 19);
        REQUIRE_FALSE(allocation.HasSpillReload(10, 15));
        REQUIRE(allocation.HasSpillReload(9, 14));
    }
    SECTION("successive recolors use current homes and roll back the whole edit") {
        {
            compile::AllocationEdit edit{allocation, 10, 18};
            recolor(edit);
            REQUIRE(edit.RecolorGPR(component, 20, true, sites) == compile::RecolorFailure::None);
            REQUIRE(allocation.ValueGPR(10).id == 20);
            REQUIRE(allocation.ValueGPR(11).id == 20);
            REQUIRE(allocation.ValueGPR(12).id == 20);
            REQUIRE(allocation.GPRLowView(12) == 20);
            REQUIRE(allocation.AllocationAttempt() == version);
        }
        REQUIRE(allocation.Mapping(10) == old10);
        REQUIRE(allocation.Mapping(11) == old11);
        REQUIRE(allocation.Mapping(12) == old12);
        REQUIRE(allocation.GPRLowView(12) == 5);
        REQUIRE(allocation.HasSpillReload(10, 11));
        REQUIRE(allocation.HasSpillReload(10, 15));
        REQUIRE(allocation.HasSpillReload(9, 14));
        REQUIRE(allocation.AllocationAttempt() == version);
    }
    SECTION("MEM-only recoloring preserves masks across fixed class changes") {
        for (const bool accept : {false, true}) {
            AllocationFixture spilled{
                    16, GPRSMask{1u << 19}, FPRSMask{}, FeatureSet{}};
            spilled.MapMemSpill(4, SpillSlot{16});
            spilled.MapReference(4, 5);
            spilled.SetLiveEnd(4, 8);
            const GPRSMask gprs{(1u << 19) | (1u << 9) | (1u << 7)};
            const FPRSMask fprs{0x80};
            const std::array<compile::GPRRecolorSite, 3> sparse_sites{
                    {{4, 0}, {6, 1u << 9}, {8, 0}}};
            for (const auto& site : sparse_sites)
                spilled.SetActiveRegs(site.instruction, gprs, fprs);
            const auto original = spilled.Mapping(4);
            const auto reference = spilled.Mapping(5);
            const auto attempt = spilled.AllocationAttempt();
            const std::array<u32, 3> aliases{5, 4, 5};
            {
                compile::AllocationEdit edit{spilled, 4, 9};
                REQUIRE(edit.RecolorGPR(aliases, 19, true, sparse_sites) ==
                        compile::RecolorFailure::None);
                REQUIRE(spilled.IsFixedGPR(4));
                REQUIRE(spilled.ValueGPR(5).id == 19);
                REQUIRE(edit.RecolorGPR(aliases, 19, false, sparse_sites) ==
                        compile::RecolorFailure::None);
                REQUIRE_FALSE(spilled.IsFixedGPR(4));
                REQUIRE(edit.TouchedValueCount() == 1);
                for (const auto& site : sparse_sites) {
                    REQUIRE(spilled.DirtyGPR(site.instruction) == gprs);
                    REQUIRE(spilled.DirtyFPR(site.instruction) == fprs);
                }
                if (accept) edit.Commit();
            }
            REQUIRE(spilled.Mapping(5) == reference);
            REQUIRE(spilled.AllocationAttempt() == attempt + (accept ? 1 : 0));
            if (accept) {
                REQUIRE(spilled.ValueGPR(4).id == 19);
                REQUIRE_FALSE(spilled.IsFixedGPR(4));
            } else
                REQUIRE(spilled.Mapping(4) == original);
            for (const auto& site : sparse_sites) {
                REQUIRE(spilled.DirtyGPR(site.instruction) == gprs);
                REQUIRE(spilled.DirtyFPR(site.instruction) == fprs);
            }
        }
    }
    SECTION("partial components are refused before any write") {
        const std::array<u32, 1> partial{10};
        compile::AllocationEdit edit{allocation, 10, 18};
        const auto failure = edit.RecolorGPR(partial, 19, true, sites);
        REQUIRE(failure == compile::RecolorFailure::PartialAliasGroup);
        REQUIRE(edit.TouchedValueCount() == 0);
        REQUIRE(allocation.Mapping(10) == old10);
        REQUIRE(allocation.AllocationAttempt() == version);
        return;
    }

}

TEST_CASE("Unreachable blocks keep unique IDs after function finalization") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;
    FeatureSet features{};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0x1000}, Location{0x1200});
    auto live = function->LoadImm(ValueType::U64, Imm{swift::u64{42}});
    function->StoreUniform(Uniform{0, ValueType::U64}, live);
    function->EndBlock(terminal::ReturnToHost{});

    auto* dead = function->AppendBlock(Location{0x1100});
    function->SetCurBlock(dead);
    for (swift::u64 offset : {0u, 8u}) {
        auto address =
                function->GetOperand(ValueType::U64, Operand{Imm{swift::u64{0x400000} + offset}});
        auto value = function->LoadMemory(ValueType::U64, Operand{address});
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
        AllocationFixture allocation{function->MaxInstrCount(), GPRSMask{}, FPRSMask{}, features};
        ::swift::runtime::compile::AllocateFunction(
                function,
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        function, allocation, features));
        REQUIRE(allocation.ValueType(live) == AllocationFixture::GPR);
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

    auto pinned = function->GetHostGPR(ValueType::U64, HostRegIndex(0), Imm{0u});
    auto capture = function->GetHostGPR(ValueType::U64, HostRegIndex(1), Imm{0u});
    auto fixed_fpr = function->GetHostFPR(ValueType::V128, HostRegIndex(2), Imm{0u});

    std::vector<Value> live;
    for (std::uint64_t i = 0; i < 14; ++i) {
        live.push_back(function->LoadImm(ValueType::U64, Imm{0x100u + i}));
    }
    auto alias = function->BitCast(ValueType::U64, live[3]);

    // The SetHostGPR crosses capture's lifetime, so capture must receive a
    // normal allocation instead of aliasing host register 1.
    function->SetHostGPR(live[0], HostRegIndex(1), Imm{0u});
    auto pinned_use = function->Add(ValueType::U64, pinned, Operand{live[1]});
    auto capture_use = function->Add(ValueType::U64, capture, Operand{alias});

    Params params{};
    params.Push(live[2]);
    params.Push(capture_use);
    params.Push(pinned_use);
    function->CallDynamic(ValueType::VOID, Lambda{Imm{std::uint64_t(1ull)}}, std::move(params));

    // Consume the pressure values after the call so all of them cross it.
    Value sum = live.back();
    for (int i = static_cast<int>(live.size()) - 2; i >= 0; --i) {
        sum = function->Add(ValueType::U64, sum, Operand{live[i]});
    }
    sum = function->Add(ValueType::U64, sum, Operand{capture_use});
    function->StoreUniform(Uniform{0, ValueType::U64}, sum);
    function->StoreUniform(Uniform{16, ValueType::V128}, fixed_fpr);

    auto terminal_cond = function->TestNotZero(live[4]);
    function->EndBlock(terminal::If{
            terminal_cond, terminal::ReturnToDispatch{}, terminal::ReturnToHost{}});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    REQUIRE(function->GetHIRBlocksRPO().size() == 1);

    // Bits set are unavailable. Leave seven allocatable GPRs and eight FPRs,
    // less the emitter reserve, so the block must take the spill path while
    // still covering the four-reload call shape.
    constexpr std::uint32_t available_gprs = 0x000007f0u;  // x4..x10
    constexpr std::uint32_t available_fprs = 0x00000f78u;  // v3..v6, v8..v11
    REQUIRE(swift::runtime::compile::SemanticFacts{function}.CanUseBlockIntervals());
    GPRSMask gprs{~available_gprs};
    FPRSMask fprs{~available_fprs};
    AllocationFixture general{function->MaxInstrCount(), gprs, fprs, FeatureSet{}};
    AllocationFixture fast{function->MaxInstrCount(), gprs, fprs, FeatureSet{}};
    AllocationFixture selected{function->MaxInstrCount(), gprs, fprs, FeatureSet{}};

    ::swift::runtime::compile::AllocateFunction(
            function,
            &general,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, general, FeatureSet{}),
            {.block_intervals = false});
    ::swift::runtime::compile::AllocateFunction(
            function,
            &fast,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(function, fast, FeatureSet{}),
            {.block_intervals = true});
    ::swift::runtime::compile::AllocateFunction(
            function,
            &selected,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, selected, FeatureSet{}));

    REQUIRE(general.MapCount() == fast.MapCount());
    bool saw_spill = false;
    for (std::uint32_t id = 0; id < general.MapCount(); ++id) {
        INFO("allocation map id " << id);
        REQUIRE(general.Mapping(id) == fast.Mapping(id));
        saw_spill |= general.Mapping(id).type == AllocationFixture::MEM;
    }
    REQUIRE(saw_spill);
    REQUIRE(general.ValueType(pinned) == AllocationFixture::GPR);
    REQUIRE(general.ValueGPR(pinned).id == 0);
    REQUIRE(general.ValueType(capture) != AllocationFixture::REF);
    REQUIRE(general.ValueType(fixed_fpr) == AllocationFixture::FPR);
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
    auto vector = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{32, ValueType::V128});
    auto converted = block->VecFCvtFloatToInt(ValueType::U64, vector, Imm{32u}, Imm{64u}, Imm{0u});
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

TEST_CASE("Spill eviction chooses distant equal-cost uses and preserves verified fallback") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    // Six free registers and the ordinary reserve of three leave exactly
    // three value locations. The fourth definition is shorter-lived than all
    // three active values, while `longest` has the unique farthest end.
    const GPRSMask gprs{~((1u << 6) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    auto off_case = BuildSpillEvictChoiceBlock();
    swift::runtime::IntrusivePtr<Block> off_block{off_case.block};
    AllocationFixture off{off_case.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    const auto off_result = RegisterAllocTestSupport::RunForSpillEvictTest(
            off_case.block, &off, false);
    REQUIRE(off.ValueType(off_case.arriving) == AllocationFixture::MEM);
    REQUIRE(off_result.eviction_restarts == 0);
    REQUIRE(off_result.spill_use_builds == 0);

    auto on_case = BuildSpillEvictChoiceBlock();
    swift::runtime::IntrusivePtr<Block> on_block{on_case.block};
    AllocationFixture on{on_case.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    const auto on_result = RegisterAllocTestSupport::RunForSpillEvictTest(
            on_case.block, &on, true);
    REQUIRE(on_result.eviction_restarts >= 1);
    REQUIRE(on_result.spill_use_builds == 1);
    REQUIRE_FALSE(on_result.fell_back_to_ladder);
    REQUIRE(on.ValueType(on_case.longest) == AllocationFixture::MEM);
    REQUIRE(on.ValueType(on_case.arriving) == AllocationFixture::GPR);

    // This block deliberately reads many long-lived spilled values while the
    // opcode scratch budget is also live. If the default-reserve eviction
    // attempt cannot satisfy Verify(), it must be discarded and the existing
    // reserve ladder must produce the final allocation.
    swift::runtime::IntrusivePtr<Block> fallback_block{BuildSpillEvictFallbackBlock()};
    const GPRSMask fallback_gprs{~((1u << 10) - 1u)};
    AllocationFixture fallback{fallback_block->MaxInstrId(), fallback_gprs, fprs, FeatureSet{}};
    const auto fallback_result = RegisterAllocTestSupport::RunForSpillEvictTest(
            fallback_block.get(), &fallback, true);
    REQUIRE(fallback_result.eviction_restarts >= 1);
    REQUIRE(fallback_result.fell_back_to_ladder);
    REQUIRE(fallback_result.final_gpr_reserve > 3);
}

TEST_CASE("Spill eviction chooses a verified all-spill terminal reserve") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    swift::runtime::IntrusivePtr<Block> block{BuildSpillEvictFallbackBlock()};
    const GPRSMask gprs{~((1u << 6) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    AllocationFixture alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    const auto result = RegisterAllocTestSupport::RunForSpillEvictTest(
            block.get(), &alloc, true);
    REQUIRE(result.fell_back_to_ladder);
    REQUIRE(result.final_gpr_reserve == 6);
}

TEST_CASE("Spill eviction rejects an impossible scratch contract") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    swift::runtime::IntrusivePtr<Block> block{BuildSpillEvictFallbackBlock()};
    const GPRSMask gprs{~((1u << 2) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    AllocationFixture alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    REQUIRE_THROWS(RegisterAllocTestSupport::RunForSpillEvictTest(
            block.get(), &alloc, true));
}

TEST_CASE("Spill eviction chooses whole-interval cost before next-use distance") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    const GPRSMask gprs{~((1u << 6) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    for (const bool frequent : {false, true}) {
        CAPTURE(frequent);
        swift::runtime::IntrusivePtr<Block> block{new Block(0, Location{0x2420})};
        auto longest = block->LoadImm(Imm{swift::u64{1}});
        if (frequent) {
            // These reads precede the pressure point, but a whole-interval
            // retry would spill them too. Keep their cost in the decision.
            block->StoreUniform(Uniform{0, ValueType::U64}, longest);
            block->StoreUniform(Uniform{0, ValueType::U64}, longest);
        }
        auto distant = block->LoadImm(Imm{swift::u64{2}});
        auto nearby = block->LoadImm(Imm{swift::u64{3}});
        auto arriving = block->LoadImm(Imm{swift::u64{4}});
        auto read = [&](Value value) {
            block->StoreUniform(Uniform{0, ValueType::U64}, value);
        };
        if (!frequent) read(longest);
        read(arriving);
        read(nearby);
        if (!frequent) read(nearby);
        read(distant);
        if (!frequent) {
            read(distant);
            read(arriving);
        }
        read(longest);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        AllocationFixture alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
        const auto result = RegisterAllocTestSupport::RunForSpillEvictTest(
                block.get(), &alloc, true);
        REQUIRE_FALSE(result.fell_back_to_ladder);
        REQUIRE(result.eviction_restarts == 1);
        REQUIRE(alloc.ValueType(distant) == AllocationFixture::MEM);
        REQUIRE(alloc.ValueType(longest) == AllocationFixture::GPR);
        REQUIRE(alloc.ValueType(arriving) == AllocationFixture::GPR);
    }
}

TEST_CASE("Spill eviction chooses a bounded retry path for repeated pressure") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    swift::runtime::IntrusivePtr<Block> block{new Block(0, Location{0x2430})};
    // Nine disjoint groups keep peak pressure at four values. This exercises
    // compile work limits without a large live set or a guest stress loop.
    for (swift::u64 group = 0; group < 9; ++group) {
        std::array<Value, 4> values;
        for (swift::u64 i = 0; i < values.size(); ++i) {
            values[i] = block->LoadImm(Imm{group * 4 + i});
        }
        for (auto it = values.rbegin(); it != values.rend(); ++it) {
            block->StoreUniform(Uniform{0, ValueType::U64}, *it);
        }
    }
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    const GPRSMask gprs{~((1u << 6) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    AllocationFixture alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    const auto result = RegisterAllocTestSupport::RunForSpillEvictTest(
            block.get(), &alloc, true);
    REQUIRE(result.eviction_restarts == 8);
    REQUIRE(result.fell_back_to_ladder);
}
