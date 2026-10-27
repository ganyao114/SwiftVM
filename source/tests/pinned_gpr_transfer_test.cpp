#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <stdexcept>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/residence_preparation.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/register_state.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/ir/hir_builder.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

std::vector<std::string> Disassemble(arm64::JitContext& context, swift::u32 offset = 0) {
    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
    first = reinterpret_cast<const vixl::aarch64::Instruction*>(
            reinterpret_cast<const swift::u8*>(first) + offset);
    auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::vector<std::string> lines;
    for (auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        lines.emplace_back(disassembler.GetOutput());
    }
    return lines;
}

std::vector<std::string> EmitTransfer(bool overwrite_target,
                                      bool alu_consumer = false,
                                      unsigned padding = 0,
                                      unsigned alias_depth = 0) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x9720})};
    for (unsigned i = 0; i < padding; ++i) block->Nop();

    auto old_source = block->GetHostGPR(ValueType::U64, HostRegIndex(1), Imm{0u});
    if (padding) block->Nop();  // Keep both survival intervals nonempty.
    block->SetHostGPR(old_source, HostRegIndex(23), Imm{0u});
    auto new_source = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
    block->SetHostGPR(new_source, HostRegIndex(1), Imm{0u});
    if (alu_consumer) {
        auto result = block->Add(ValueType::U64, old_source, Operand{Imm{swift::u64{7}}});
        block->StoreUniform(Uniform{64, ValueType::U64}, result);
    } else {
        auto address_source = old_source;
        for (unsigned i = 0; i < alias_depth; ++i)
            address_source = block->BitCast(ValueType::U64, address_source);
        auto first_address = block->BitCast(ValueType::U64, address_source);
        auto first = block->LoadMemory(ValueType::U64, Operand{first_address, Imm{8u}});
        block->StoreUniform(Uniform{64, ValueType::U64}, first);
        if (overwrite_target) {
            auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x3000}});
            block->SetHostGPR(replacement, HostRegIndex(23), Imm{0u});
        }
        auto second_address = block->BitCast(ValueType::U64, address_source);
        auto second = block->LoadMemory(ValueType::U64, Operand{second_address, Imm{16u}});
        block->StoreUniform(Uniform{72, ValueType::U64}, second);
    }
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    AllocationFixture alloc{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(block.get(), alloc, features),
            false);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    return Disassemble(context);
}

std::vector<std::string> EmitHelperTransfer(HostRegisterEffect effect, swift::u32 target) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x9760})};

    auto old_source = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
    block->SetHostGPR(old_source, HostRegIndex(target), Imm{0u});
    auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
    block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    (void)block->CallLambda(ValueType::VOID, Lambda{DataClass{Imm{1}}, HelperCallTraits{.host_registers = effect}});
    auto address = block->BitCast(ValueType::U64, old_source);
    auto loaded = block->LoadMemory(ValueType::U64, Operand{address, Imm{8u}});
    block->StoreUniform(Uniform{64, ValueType::U64}, loaded);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    AllocationFixture alloc{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(block.get(), alloc, features),
            false);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    return Disassemble(context);
}

std::vector<std::string> EmitSse42Transfer(swift::u8 imm) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x9770})};

    auto source = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
    block->SetHostGPR(source, HostRegIndex(7), Imm{0u});
    auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
    block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    auto left = block->LoadUniform(Uniform{0, ValueType::V128});
    auto right = block->LoadUniform(Uniform{16, ValueType::V128});
    auto result = block->Sse42Str(ValueType::U64, left, right, Imm{imm});
    block->StoreUniform(Uniform{32, ValueType::U64}, result);
    auto address = block->BitCast(ValueType::U64, source);
    auto loaded = block->LoadMemory(ValueType::U64, Operand{address, Imm{8u}});
    block->StoreUniform(Uniform{40, ValueType::U64}, loaded);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    AllocationFixture alloc{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(block.get(), alloc, features),
            false);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    return Disassemble(context);
}

struct CFGWidthEmission {
    std::vector<std::string> code;
    arm64::RegisterStatePreparation::ExtensionFacts entry;
};

CFGWidthEmission EmitCrossBlockWidth(bool external_entry) {
    constexpr swift::VAddr first_guest = 0x9780;
    constexpr swift::VAddr second_guest = 0x9790;
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    FeatureSet features{};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(
            Location{first_guest}, Location{second_guest + 1});

    auto value = function->LoadImm(ValueType::U32, Imm{swift::u32{7}});
    function->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto* second = builder.LinkBlock(
            terminal::LinkBlock{Location{second_guest}});
    if (external_entry) {
        function->RegisterExternalEntryRoot(second);
    }
    builder.SetCurBlock(second);
    auto resident = function->GetHostGPR(ValueType::U32, HostRegIndex(23), Imm{0u});
    function->SetHostGPR(resident, HostRegIndex(23), Imm{0u});
    auto result = function->Add(ValueType::U32, resident, Operand{Imm{swift::u32{1}}});
    function->StoreUniform(Uniform{64, ValueType::U32}, result);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();

    arm64::RegisterStatePreparation state;
    compile::SemanticFacts shared_facts{function};
    REQUIRE(shared_facts.FindFunctionCFG() == nullptr);
    state.BeginFunction(*function, features, shared_facts);
    REQUIRE(shared_facts.FindFunctionCFG() == nullptr);
    const auto entry = state.PrepareEntryExtensionFacts(second->GetBlock(), 23);
    REQUIRE(state.HasEntryWidthFacts(second->GetBlock()) == !external_entry);
    REQUIRE_FALSE(state.HasEntryWidthFacts(function->GetHIRBlocksRPO().front().GetBlock()));
    const auto* cfg = shared_facts.FindFunctionCFG();
    REQUIRE(cfg);
    arm64::RegisterStatePreparation independent_state;
    independent_state.BeginFunction(*function, features, shared_facts);
    REQUIRE(independent_state.PrepareEntryExtensionFacts(second->GetBlock(), 23) == entry);
    REQUIRE(shared_facts.FindFunctionCFG() == cfg);
    (void)shared_facts.PrepareCFGUseEnds();
    REQUIRE(shared_facts.FindFunctionCFG() == cfg);
    AllocationFixture alloc{function->MaxInstrCount(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateFunction(
            function,
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(function, alloc, features));
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    const auto* entry_inputs = alloc.Actions().Edges().ForBlock(second->GetBlock());
    REQUIRE(entry_inputs);
    REQUIRE(bool(entry_inputs->requires_unit_state) == !external_entry);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    REQUIRE(translator.Translate(function));
    context.Finish();
    const auto offset = context.GetCodeOffset(second_guest);
    REQUIRE(offset >= 0);
    return {Disassemble(context, static_cast<swift::u32>(offset)), entry};
}

struct CrossBlockCoalesceState {
    bool write_coalesced{};
    bool read_coalesced{};
    bool width_coalesced{};
    swift::u16 write_home{};
    swift::u16 read_home{};
};

CrossBlockCoalesceState AllocateCrossBlockPublication() {
    constexpr Location entry{0x97a0};
    constexpr Location external_entry{0x97b0};
    constexpr Location successor{0x97c0};
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    FeatureSet features{};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(entry, Location{0x97d0});

    auto* external = builder.LinkBlock(terminal::LinkBlock{external_entry});
    function->RegisterExternalEntryRoot(external);
    builder.SetCurBlock(external);
    auto published = function->LoadImm(ValueType::U32, Imm{swift::u32{7}});
    auto* publication =
            function->AppendInst(OpCode::SetHostGPR, published, HostRegIndex(23), Imm{0u});
    auto read = function->GetHostGPR(ValueType::U32, HostRegIndex(23), Imm{0u});
    auto invariant = function->GetHostGPR(ValueType::U32, HostRegIndex(29), Imm{0u});
    Value current = function->GetHostGPR(ValueType::U32, HostRegIndex(22), Imm{0u});
    Value cross_block_bridge{};
    for (swift::u32 index = 0; index < 32; ++index) {
        Value left = current;
        Value right = invariant;
        if (index != 0) {
            left = function->BitExtract(ValueType::U32, current, Imm{0u}, Imm{32u});
            right = function->BitExtract(ValueType::U32, invariant, Imm{0u}, Imm{32u});
            if (!cross_block_bridge.Defined()) {
                cross_block_bridge = left;
            }
        }
        auto producer = (index & 1u) ? function->Xor(ValueType::U32, left, Operand{right})
                                     : function->Add(ValueType::U32, left, Operand{right});
        current = function->ZeroExtend32To64(ValueType::U64, producer);
        function->SetHostGPR(current, HostRegIndex(22), Imm{0u});
    }
    auto* next = builder.LinkBlock(terminal::LinkBlock{successor});
    builder.SetCurBlock(next);
    auto replacement = function->LoadImm(ValueType::U32, Imm{swift::u32{9}});
    function->SetHostGPR(replacement, HostRegIndex(23), Imm{0u});
    auto first = function->Add(ValueType::U32, published, Operand{Imm{swift::u32{1}}});
    auto second = function->Add(ValueType::U32, read, Operand{Imm{swift::u32{1}}});
    auto third = function->Add(ValueType::U32, cross_block_bridge, Operand{Imm{swift::u32{1}}});
    function->StoreUniform(Uniform{64, ValueType::U32}, first);
    function->StoreUniform(Uniform{68, ValueType::U32}, second);
    function->StoreUniform(Uniform{72, ValueType::U32}, third);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();

    AllocationFixture alloc{function->MaxInstrCount(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateFunction(
            function,
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(function, alloc, features));
    return {
            alloc.IsHostWriteCoalesced(publication->Id()),
            alloc.IsHostReadCoalesced(read.Id()),
            alloc.IsWidthChainCoalesced(cross_block_bridge.Id()),
            alloc.ValueGPR(published).id,
            alloc.ValueGPR(read).id,
    };
}

arm64::RegisterStatePreparation::ExtensionFacts DiamondEntryFacts(bool signed_value,
                                                       bool clobber_predecessor,
                                                       bool unreachable_predecessor = false) {
    constexpr Location entry{0x9800};
    constexpr Location left_location{0x9810};
    constexpr Location right_location{0x9820};
    constexpr Location join_location{0x9830};
    FeatureSet features{};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(entry, Location{0x9840});
    Value value;
    if (signed_value) {
        auto narrow = function->LoadImm(ValueType::S8, Imm{swift::u8{0x80}});
        value = function->SignExtend(ValueType::U64, narrow);
    } else {
        value = function->LoadImm(ValueType::U32, Imm{swift::u32{1}});
    }
    function->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto condition = function->LoadImm(ValueType::U8, Imm{swift::u8{1}});
    auto [left, right] = builder.If(terminal::If{
            condition,
            terminal::LinkBlock{left_location},
            terminal::LinkBlock{right_location},
    });

    builder.SetCurBlock(left);
    auto* join = builder.LinkBlock(terminal::LinkBlock{join_location});
    builder.SetCurBlock(right);
    if (clobber_predecessor) {
        auto wide = function->LoadImm(ValueType::U64, Imm{UINT64_C(0x10000000000)});
        function->SetHostGPR(wide, HostRegIndex(23), Imm{0u});
    }
    builder.LinkBlock(terminal::LinkBlock{join_location});
    builder.SetCurBlock(join);
    function->EndBlock(terminal::ReturnToHost{});
    const HIRBlock* extra{};
    if (unreachable_predecessor) {
        extra = function->AppendBlock(Location{0x9838});
        builder.SetCurBlock(extra);
        builder.LinkBlock(terminal::LinkBlock{join_location});
    }
    function->EndFunction();
    function->ComputeRPO();

    arm64::RegisterStatePreparation state_map;
    compile::SemanticFacts shared_facts{function};
    state_map.BeginFunction(*function, features, shared_facts);
    if (extra)
        REQUIRE(std::none_of(function->GetHIRBlocksRPO().begin(),
                             function->GetHIRBlocksRPO().end(),
                             [&](const HIRBlock& block) { return &block == extra; }));
    return state_map.PrepareEntryExtensionFacts(join->GetBlock(), 23);
}

bool LoopEntryKnownZero(bool clobber_backedge, bool duplicate_edge = false) {
    constexpr Location entry{0x9850};
    constexpr Location loop_location{0x9860};
    FeatureSet features{};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(entry, Location{0x9870});
    auto value = function->LoadImm(ValueType::U32, Imm{swift::u32{1}});
    function->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto* loop = builder.LinkBlock(terminal::LinkBlock{loop_location});
    builder.SetCurBlock(loop);
    if (clobber_backedge) {
        auto wide = function->LoadImm(ValueType::U64, Imm{UINT64_C(0x10000000000)});
        function->SetHostGPR(wide, HostRegIndex(23), Imm{0u});
    }
    if (duplicate_edge) {
        const auto condition = function->LoadImm(ValueType::U8, Imm{swift::u8{1}});
        builder.If(terminal::If{
                condition, terminal::LinkBlock{loop_location}, terminal::LinkBlock{loop_location}});
    } else {
        builder.LinkBlock(terminal::LinkBlock{loop_location});
    }
    function->EndFunction();
    function->ComputeRPO();

    arm64::RegisterStatePreparation state_map;
    compile::SemanticFacts shared_facts{function};
    state_map.BeginFunction(*function, features, shared_facts);
    const auto known_zero = state_map.PrepareEntryExtensionFacts(loop->GetBlock(), 23).KnownZeroAbove(32);
    if (duplicate_edge) {
        const auto* cfg = shared_facts.FindFunctionCFG();
        REQUIRE(cfg);
        const auto id = cfg->FindBlock(loop->GetBlock());
        REQUIRE(id);
        const auto successors = cfg->Successors(cfg->Blocks()[*id]);
        REQUIRE(successors.size() == 2);
        REQUIRE(successors.front() == *id);
        REQUIRE(successors.back() == *id);
    }
    return known_zero;
}

std::size_t Count(const std::vector<std::string>& lines,
                  std::string_view first,
                  std::string_view second) {
    return std::ranges::count_if(lines, [&](const auto& line) {
        return line.find(first) != std::string::npos && line.find(second) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("a published full-width GPR version feeds later faulting addresses") {
    const auto lines = EmitTransfer(false);
    REQUIRE(Count(lines, "mov x23, x1", "") == 1);
    REQUIRE(Count(lines, "ldr x", "[x23, #8]") == 1);
    REQUIRE(Count(lines, "ldr x", "[x23, #16]") == 1);
}

TEST_CASE("indexed pinned transfers and long alias chains preserve emitted instructions") {
    // Nops only increase IR size: the repeated survival query at emission
    // uses the index. Long chains also exercise the shared alias queue's growth.
    const auto expected = EmitTransfer(false);
    CHECK(EmitTransfer(false, false, 80) == expected);
    CHECK(EmitTransfer(false, false, 80, 12) == expected);
    CHECK(EmitTransfer(false, false, 80, 300) == expected);
    CHECK(EmitTransfer(true, false, 80) == EmitTransfer(true));
}

TEST_CASE("overwriting the resident GPR version keeps the source capture") {
    const auto lines = EmitTransfer(true);
    REQUIRE(Count(lines, "ldr x", "[x23, #8]") == 0);
    REQUIRE(Count(lines, "ldr x", "[x23, #16]") == 0);
}

TEST_CASE("published value versions feed general ALU consumers") {
    const auto lines = EmitTransfer(false, true);
    INFO([&] { std::string text; for (const auto& line : lines) text += line + '\n'; return text; }());
    REQUIRE(Count(lines, "mov x23, x1", "") == 1);
    REQUIRE(Count(lines, "add x", "x23") == 1);
}

TEST_CASE("resident helper contracts preserve pinned value versions") {
    const auto conservative = EmitHelperTransfer(HostRegisterEffect::MayTouchSIMD, 7);
    const auto resident = EmitHelperTransfer(HostRegisterEffect::PreservesPinnedState, 7);
    const auto clobbered = EmitHelperTransfer(HostRegisterEffect::PreservesPinnedState, 2);
    REQUIRE(Count(conservative, "ldr x", "[x7, #8]") == 0);
    REQUIRE(Count(resident, "ldr x", "[x7, #8]") == 1);
    REQUIRE(Count(clobbered, "ldr x", "[x2, #8]") == 0);
}

TEST_CASE("SSE4.2 string lowering preserves pinned value versions") {
    const auto native = EmitSse42Transfer(0x02);
    const auto inline_ = EmitSse42Transfer(0x00);
    REQUIRE(Count(native, "ldr x", "[x7, #8]") == 1);
    REQUIRE(Count(inline_, "ldr x", "[x7, #8]") == 1);
}

TEST_CASE("pinned CFG width facts stop at external entry roots") {
    const auto internal = EmitCrossBlockWidth(false);
    const auto external = EmitCrossBlockWidth(true);
    REQUIRE(internal.entry.KnownZeroAbove(32));
    REQUIRE_FALSE(external.entry.KnownZeroAbove(32));
    // Inspect the second block: a captured read may normalize its displaced
    // copy rather than emit a self-move. Its external entry must still
    // publish a W value, independent of the predecessor's width facts.
    REQUIRE(Count(external.code, "mov w23,", "") >= 1);
}

TEST_CASE("fixed-home coalescing stops at cross-block value ownership") {
    const auto state = AllocateCrossBlockPublication();
    REQUIRE_FALSE(state.write_coalesced);
    REQUIRE_FALSE(state.read_coalesced);
    REQUIRE_FALSE(state.width_coalesced);
    REQUIRE(state.write_home != 23);
    REQUIRE(state.read_home != 23);
}

TEST_CASE("pinned CFG width facts meet at diamonds and backedges") {
    REQUIRE(DiamondEntryFacts(false, false).KnownZeroAbove(32));
    REQUIRE_FALSE(DiamondEntryFacts(false, true).KnownZeroAbove(32));
    REQUIRE(DiamondEntryFacts(true, false).KnownSignExtended(8, 64));
    REQUIRE_FALSE(DiamondEntryFacts(true, true).KnownSignExtended(8, 64));
    REQUIRE_FALSE(DiamondEntryFacts(false, false, true).KnownZeroAbove(32));
    REQUIRE_FALSE(DiamondEntryFacts(true, false, true).KnownSignExtended(8, 64));
    REQUIRE(LoopEntryKnownZero(false));
    REQUIRE_FALSE(LoopEntryKnownZero(true));
    REQUIRE(LoopEntryKnownZero(false, true));
    REQUIRE_FALSE(LoopEntryKnownZero(true, true));
}

TEST_CASE("pinned allocation transfers a residence only after its publication") {
    IntrusivePtr<Block> block{new Block(0, Location{0x97d0})};
    const auto source = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
    block->SetHostGPR(source, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    const auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
    block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    const auto address = block->BitCast(ValueType::U64, source);
    const auto loaded = block->LoadMemory(ValueType::U64, Operand{address, Imm{8u}});
    block->StoreUniform(Uniform{64, ValueType::U64}, loaded);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    AllocationFixture alloc{block->MaxInstrId(), GPRSMask{~(1u << 14)}, FPRSMask{}, features};
    alloc.MapRegister(source.Id(), HostGPR{14});
    alloc.MapRegister(replacement.Id(), HostGPR{15});
    alloc.MapReference(source.Id(), address.Id());
    alloc.MapRegister(loaded.Id(), HostGPR{16});
    arm64::RegisterStatePreparation semantics;
    semantics.BeginBlock(block.get(), features, alloc.PrepareFacts(block.get()));
    arm64::ResidencePreparation placement{alloc, features, semantics};
    const auto generation = alloc.GPRLocationGeneration();
    placement.Prepare(block.get());
    REQUIRE(alloc.GPRLocationGeneration() == generation);
    REQUIRE_THROWS_AS(placement.PublicationFor(publication), std::logic_error);
    placement.IndexPublications();
    const auto* selected = placement.PublicationFor(publication);
    REQUIRE(selected);
    REQUIRE(selected->kind == arm64::ResidencePreparation::PublicationSelection::Kind::Transfer);
    REQUIRE(selected->source == 20);
    REQUIRE(selected->target == 23);

    const auto before = placement.UseLocation(source, publication);
    REQUIRE(before.kind == AllocationFixture::GPR);
    REQUIRE(before.slot == 20);
    REQUIRE(before.bits == 64);
    const auto after = placement.UseLocation(source, loaded.Def());
    REQUIRE(after.slot == 23);
    REQUIRE(after.bits == 64);
    REQUIRE(placement.UseLocation(address, loaded.Def()).slot == 23);
    // A transfer must not redirect the SSA definition into a home that still
    // contains the old architectural target until publication.
    REQUIRE_FALSE(placement.ValueHome(source, source.Def()));
    REQUIRE(alloc.ValueGPR(source).id == 14);

    IntrusivePtr<Block> next{new Block(1, Location{0x97e0})};
    const auto value = next->LoadImm(ValueType::U64, Imm{swift::u64{7}});
    next->StoreUniform(Uniform{64, ValueType::U64}, value);
    auto* consumer = &next->GetInstList().back();
    next->SetTerminal(terminal::ReturnToDispatch{});
    next->ReIdInstr();
    alloc.SetAllocationUnit(next.get(), nullptr);
    alloc.MapRegister(value.Id(), HostGPR{14});
    semantics.BeginBlock(next.get(), features, alloc.PrepareFacts(next.get()));
    placement.Prepare(next.get());
    REQUIRE_THROWS_AS(placement.PublicationFor(publication), std::logic_error);
    placement.IndexPublications();
    REQUIRE_FALSE(placement.PublicationFor(publication));
    REQUIRE_FALSE(placement.PublicationFor(consumer));
    const auto ordinary = placement.UseLocation(value, consumer);
    REQUIRE_FALSE(ordinary.pinned);
    REQUIRE(ordinary.slot == 14);
}

TEST_CASE("Select uses a transferred pinned value without reloading its spill") {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x97f0})};
    const auto source = block->GetHostGPR(ValueType::U8, HostRegIndex(20), Imm{0u});
    const auto widened = block->ZeroExtend32(ValueType::U32, source);
    const auto extended = block->ZeroExtend32To64(ValueType::U64, widened);
    block->SetHostGPR(extended, HostRegIndex(23), Imm{0u});
    const auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
    block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    const auto condition = block->LoadImm(ValueType::U8, Imm{1u});
    const auto otherwise = block->LoadImm(ValueType::U32, Imm{99u});
    const auto selected = block->Select(ValueType::U32, condition, widened, otherwise);
    block->StoreUniform(Uniform{64, ValueType::U32}, selected);
    // The same copy proof has an alias of the narrow extension and another
    // alias of the published extension, alongside the direct Select use.
    const auto narrow_alias = block->BitExtract(ValueType::U16, widened, Imm{0u}, Imm{16u});
    const auto narrow_sum = block->Add(ValueType::U16, narrow_alias, Operand{Imm{3u}});
    block->StoreUniform(Uniform{72, ValueType::U16}, narrow_sum);
    const auto published_alias = block->BitExtract(ValueType::U32, extended, Imm{0u}, Imm{32u});
    const auto published_sum = block->Add(ValueType::U32, published_alias, Operand{Imm{5u}});
    block->StoreUniform(Uniform{80, ValueType::U32}, published_sum);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    AllocationFixture alloc{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(block.get(), alloc, features),
            false);
    alloc.MapMemSpill(widened.Id(), SpillSlot{0});
    REQUIRE(alloc.ValueType(widened) == AllocationFixture::MEM);

    arm64::RegisterStatePreparation semantics;
    semantics.BeginBlock(block.get(), features, alloc.PrepareFacts(block.get()));
    arm64::ResidencePreparation placement{alloc, features, semantics};
    placement.Prepare(block.get());
    REQUIRE(placement.UseLocation(narrow_alias, narrow_sum.Def()).slot == 23);
    REQUIRE(placement.UseLocation(published_alias, published_sum.Def()).slot == 23);
    // Independently compiled blocks reuse instruction IDs. A local selection
    // must keep its owner identity while it is still a preparation result.
    IntrusivePtr<Block> foreign{new Block(1, Location{0x97f8})};
    const auto foreign_source = foreign->GetHostGPR(ValueType::U8, HostRegIndex(20), Imm{0u});
    const auto foreign_widened = foreign->ZeroExtend32(ValueType::U32, foreign_source);
    foreign->SetTerminal(terminal::ReturnToDispatch{});
    foreign->ReIdInstr();
    REQUIRE(foreign_widened.Id() == widened.Id());
    REQUIRE(placement.Residences().Has(widened.Def(), arm64::GPRResidenceAction::ZeroExtend));
    REQUIRE_FALSE(placement.Residences().Has(foreign_widened.Def(),
                                            arm64::GPRResidenceAction::ZeroExtend));
    placement.ElideZeroExtend(foreign_widened.Def());
    REQUIRE_FALSE(placement.Residences().Find(foreign_widened.Def()));
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                              alloc);
    const auto* residence = alloc.Actions().Residence(widened.Def());
    REQUIRE(residence);
    REQUIRE(residence->Has(arm64::GPRResidenceAction::ZeroExtend));
    REQUIRE_FALSE(alloc.Actions().Residence(foreign_widened.Def()));
    arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
    const auto before = context.CurrentBufferSize();
    REQUIRE(context.CurrentBufferSize() == before);
    REQUIRE(placement.UseLocation(widened, selected.Def()).slot == 23);

    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    const auto lines = Disassemble(context);
    INFO([&] { std::string text; for (const auto& line : lines) text += line + '\n'; return text; }());
    REQUIRE(std::ranges::any_of(lines, [](const auto& line) {
        return line.starts_with("csel w") && line.find(", w23,") != std::string::npos;
    }));
    // This block has no guest load. A scalar load here would be the old eager
    // context.R(value) spill fallback even though the selected source is w23.
    REQUIRE_FALSE(std::ranges::any_of(lines, [](const auto& line) {
        return line.starts_with("ldr w");
    }));
}

TEST_CASE("Select materializes ordinary spilled U32 operands as W registers") {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x9800})};
    const auto condition = block->LoadImm(ValueType::U8, Imm{1u});
    const auto left = block->LoadImm(ValueType::U32, Imm{41u});
    const auto right = block->LoadImm(ValueType::U32, Imm{99u});
    const auto selected = block->Select(ValueType::U32, condition, left, right);
    block->StoreUniform(Uniform{64, ValueType::U32}, selected);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    AllocationFixture alloc{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(block.get(), alloc, features),
            false);
    alloc.MapMemSpill(left.Id(), SpillSlot{0});
    alloc.MapMemSpill(right.Id(), SpillSlot{1});
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                              alloc);
    arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    const auto lines = Disassemble(context);
    REQUIRE(std::ranges::any_of(lines, [](const auto& line) {
        return line.starts_with("csel w");
    }));
}

TEST_CASE("Fixed GPR transfer is allocated before captures and survives a fault window", "[fixed-gpr]") {
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x9810})};
    auto source = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
    auto fault_address = block->LoadImm(ValueType::U64, Imm{swift::u64{0x4000}});
    auto fault = block->LoadMemory(ValueType::U64, Operand{fault_address});
    block->StoreUniform(Uniform{80, ValueType::U64}, fault);
    block->SetHostGPR(source, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x2000}});
    block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    auto alias = block->BitCast(ValueType::U64, source);
    auto loaded = block->LoadMemory(ValueType::U64, Operand{alias, Imm{8u}});
    block->StoreUniform(Uniform{88, ValueType::U64}, loaded);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    auto pool = address_space.GetTrampolines().GetGPRRegs();
    pool.Mark(20);
    pool.Mark(23);
    AllocationFixture allocation{
            block->MaxInstrId(), pool, address_space.GetTrampolines().GetFPRRegs(), features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.IsFixedGPRDefinitionElided(source.Id()));
    REQUIRE(allocation.ValueGPR(source).id == 20);
    REQUIRE(allocation.GPRLocationAt(source, fault.Id()).slot == 20);
    REQUIRE(allocation.GPRLocationAt(source, publication->Id()).slot == 20);
    REQUIRE(allocation.GPRLocationAt(source, loaded.Id()).slot == 23);
    REQUIRE(allocation.ValueGPR(alias).id == 23);
    REQUIRE(allocation.NeedsFixedGPRPublication(publication->Id()));
    REQUIRE_FALSE(allocation.IsHostWriteCoalesced(publication->Id()));

    // Retries reset physical assignments without discarding the immutable
    // residence proof. Both runs must emit the same publication at its IR point.
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.GPRLocationAt(source, loaded.Id()).slot == 23);
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                              allocation);
    arm64::JitContext context{address_space.GetDefaultModule(), allocation, allocation.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    const auto lines = Disassemble(context);
    INFO([&] { std::string text; for (const auto& line : lines) text += line + '\n'; return text; }());
    const auto copy = std::ranges::find(lines, "mov x23, x20");
    REQUIRE(copy != lines.end());
    REQUIRE(std::any_of(lines.begin(), copy, [](const auto& line) { return line.starts_with("ldr x"); }));
    REQUIRE(std::any_of(copy, lines.end(), [](const auto& line) {
        return line.starts_with("ldr x") && line.find("[x23, #8]") != std::string::npos;
    }));
    REQUIRE_FALSE(std::ranges::any_of(lines, [](const auto& line) {
        return line.starts_with("mov x") && line.ends_with(", x20") && line != "mov x23, x20";
    }));
}

TEST_CASE("Fixed GPR preallocation reduces spills under register pressure", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x9820})};
    auto source = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
    std::vector<Value> values;
    for (unsigned i = 0; i < 7; ++i)
        values.push_back(block->LoadImm(ValueType::U64, Imm{swift::u64{0x1000 + i}}));
    block->SetHostGPR(source, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->SetHostGPR(values.front(), HostRegIndex(20), Imm{0u});
    auto loaded = block->LoadMemory(ValueType::U64, Operand{source});
    block->StoreUniform(Uniform{96, ValueType::U64}, loaded);
    for (unsigned i = 0; i < values.size(); ++i)
        block->StoreUniform(Uniform{static_cast<swift::u16>(104 + 8 * i), ValueType::U64}, values[i]);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{UINT32_MAX};
    for (unsigned code : {6, 7, 8, 9, 14, 15}) pool.Clear(code);
    FPRSMask fprs{};
    AllocationFixture baseline{block->MaxInstrId(), pool, fprs, features};
    // Use the existing copy-preserving allocator for an in-process baseline.
    auto baseline_input = ::swift::runtime::backend::arm64::PrepareAllocationInputs(
            block.get(), baseline, features);
    baseline_input.fixed.clear();
    ::swift::runtime::compile::AllocateBlock(
            block.get(), &baseline, std::move(baseline_input), false);
    AllocationFixture optimized{block->MaxInstrId(), pool, fprs, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &optimized,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), optimized, features),
            false);
    auto spill_count = [&](const AllocationFixture& allocation) {
        unsigned count = 0;
        for (auto& inst : block->GetInstList()) {
            if (inst.HasValue() && !inst.IsBitCastOperation())
                count += allocation.ValueType(Value{&inst}) == AllocationFixture::MEM;
        }
        return count;
    };
    REQUIRE(spill_count(baseline) > 0);
    REQUIRE(spill_count(optimized) < spill_count(baseline));
    REQUIRE(optimized.ValueGPR(source).id == 20);
    REQUIRE(optimized.GPRLocationAt(source, loaded.Id()).slot == 23);
    REQUIRE(optimized.NeedsFixedGPRPublication(publication->Id()));
}

TEST_CASE("Narrow fixed GPR preallocation preserves the whole guest register", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x9830})};
    auto read = block->GetHostGPR(ValueType::U32, HostRegIndex(20), Imm{0u});
    auto result = block->And(ValueType::U32, read, Operand{Imm{0xffu}});
    block->StoreUniform(Uniform{64, ValueType::U32}, result);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{};
    pool.Mark(20);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.IsFixedGPRDefinitionElided(read.Id()));
    const auto location = allocation.GPRLocationAt(read, result.Id());
    REQUIRE(location.slot == 20);
    REQUIRE(location.bits == 32);
    // A W view is not a physical W write and proves nothing about X[63:32].
    arm64::RegisterStatePreparation semantics;
    semantics.BeginBlock(block.get(), features, allocation.PrepareFacts(block.get()));
    semantics.PrepareValueVersions({}, {}, true);
    const auto fact = semantics.FixedHomeForUse(read, result.Def());
    REQUIRE(fact.has_value());
    REQUIRE_FALSE(fact->extension.KnownZeroAbove(32));
}

TEST_CASE("Narrow fixed GPR preallocation covers a zero-extended read", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x98d0})};
    auto read = block->GetHostGPR(ValueType::U16, HostRegIndex(20), Imm{0u});
    auto extended = block->ZeroExtend32(ValueType::U32, read);
    block->StoreUniform(Uniform{64, ValueType::U32}, extended);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{};
    pool.Mark(20);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.IsFixedGPRDefinitionElided(read.Id()));
    REQUIRE(allocation.PreservedGPRDefinitionAt(read.Id()) == nullptr);
    REQUIRE(allocation.ValueGPR(read).id == 20);
    const auto location = allocation.GPRLocationAt(read, extended.Id());
    REQUIRE(location.slot == 20);
    REQUIRE(location.bits == 32);
}

TEST_CASE("A narrow preserved suffix keeps its displaced home through target preparation", "[fixed-gpr]") {
    for (unsigned mode : {0u, 1u, 2u}) {
        CAPTURE(mode);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        AddressSpace space{config};
        FeatureSet features{};
        HIRBuilder builder{1, true, features};
        IntrusivePtr<Block> standalone;
        HIRFunction* function = nullptr;
        Block* block;
        Value read, result;
        const auto populate = [&](auto* ir) {
            read = ir->GetHostGPR(ValueType::U16, HostRegIndex(20), Imm{0u});
            result = ir->Sub(ValueType::U16, read, Operand{Imm{1u}});
            ir->StoreUniform(Uniform{64, ValueType::U16}, result);
        };
        if (mode == 0) {
            standalone.reset(new Block(0, Location{0x98d4}));
            block = standalone.get();
            populate(block);
            block->SetTerminal(terminal::ReturnToDispatch{});
            block->ReIdInstr();
        } else {
            function = builder.AppendFunction(Location{0x98d4}, Location{0x98d8});
            populate(function);
            function->EndBlock(terminal::ReturnToDispatch{});
            function->EndFunction();
            function->ComputeRPO();
            function->IdByRPO();
            block = function->EditBlock(&function->GetHIRBlocksRPO().front()).GetBlock();
        }
        auto gprs = space.GetTrampolines().GetGPRRegs();
        gprs.Mark(20);
        AllocationFixture allocation{mode ? function->MaxInstrCount() : block->MaxInstrId(),
                                     gprs,
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        // Model a required capture supplied by fixed-input preparation. The
        // late narrow matcher accepts the immediately following subtraction,
        // but it must not replace this input with a longer elided residence.
        auto input = mode == 0 ? ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                                         block, allocation, features)
                               : ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                                         function, allocation, features);
        std::erase_if(input.fixed, [&](const auto& entry) { return entry.value == read.Id(); });
        input.fixed.push_back({read.Id(),
                               {.home = 20,
                                .end = read.Id(),
                                .transfer = read.Id(),
                                .last_use = result.Id(),
                                .owner = read.Id(),
                                .width = 2,
                                .elided = true,
                                .preserved = true,
                                .version = true}});
        if (mode == 0)
            compile::AllocateBlock(block, &allocation, std::move(input));
        else
            compile::AllocateFunction(
                    function, &allocation, std::move(input), {.block_intervals = mode == 1});
        const auto suffix = allocation.GPRLocationAt(read, result.Id());
        REQUIRE(suffix.kind == AllocationFixture::GPR);
        REQUIRE(suffix.slot != 20);
        allocation.AssignGPRLowView(read.Id(), 20);
        allocation.RegisterGPRUseHome(read.Id(), result.Id(), {.home = 20, .width = 2}, true);
        REQUIRE_FALSE(allocation.GPRLowView(read.Id()));
        REQUIRE_FALSE(allocation.GPRUseHome(read.Id(), result.Id(), true));
        REQUIRE(allocation.AdditionalGPRResidentUses(read.Id()) == 0);
        REQUIRE(allocation.GPRLocationAt(read, result.Id()).slot == suffix.slot);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(space.GetDefaultModule(),
                                                                  allocation);
        REQUIRE(allocation.FPRContractRetryCount() == 0);
        const auto* preserved = allocation.PreservedGPRDefinitionAt(read.Id());
        REQUIRE(preserved != nullptr);
        REQUIRE(preserved->transfer == read.Id());
        REQUIRE(preserved->end == read.Id());
        REQUIRE(allocation.GPRLocationAt(read, result.Id()).slot == suffix.slot);
        REQUIRE(allocation.GPRLocationAt(read, result.Id(), AllocationFixture::GPRView::WholeValue)
                        .slot == suffix.slot);
        REQUIRE(allocation.GPRPreservationsBefore(read.Id()).size() == 1);
        arm64::JitContext context{space.GetDefaultModule(), allocation, allocation.Actions()};
        arm64::JitTranslator translator{context};
        const bool translated = function ? translator.Translate(function) : translator.Translate(block);
        REQUIRE(translated);
        context.Finish();
    }
}

TEST_CASE("Narrow fixed GPR preallocation covers a sub-32 ALU read", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x98e0})};
    auto read = block->GetHostGPR(ValueType::U16, HostRegIndex(22), Imm{0u});
    auto result = block->And(ValueType::U16, read, Operand{Imm{0xffu}});
    block->StoreUniform(Uniform{64, ValueType::U16}, result);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{};
    pool.Mark(22);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.IsFixedGPRDefinitionElided(read.Id()));
    REQUIRE(allocation.PreservedGPRDefinitionAt(read.Id()) == nullptr);
    REQUIRE(allocation.ValueGPR(read).id == 22);
    const auto location = allocation.GPRLocationAt(read, result.Id());
    REQUIRE(location.slot == 22);
    REQUIRE(location.bits == 32);
}

TEST_CASE("Narrow fixed GPR preallocation covers a sign-extended read", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x98d8})};
    auto read = block->GetHostGPR(ValueType::U16, HostRegIndex(20), Imm{0u});
    auto extended = block->SignExtend(ValueType::S32, read);
    block->StoreUniform(Uniform{64, ValueType::S32}, extended);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{};
    pool.Mark(20);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.IsFixedGPRDefinitionElided(read.Id()));
    REQUIRE(allocation.PreservedGPRDefinitionAt(read.Id()) == nullptr);
    REQUIRE(allocation.ValueGPR(read).id == 20);
}

TEST_CASE("Logic fixed GPR publications bypass the temporary value class", "[fixed-gpr]") {
    for (auto op : {0, 1}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x98f0})};
        auto left = block->LoadImm(ValueType::U64, Imm{swift::u64{0x12345678}});
        auto right = block->LoadImm(ValueType::U64, Imm{swift::u64{0x0f0f0f0f}});
        auto value = op == 0 ? block->Xor(ValueType::U64, left, Operand{right})
                             : block->Or(ValueType::U64, left, Operand{right});
        block->SetHostGPR(value, HostRegIndex(23), Imm{0u});
        auto* publication = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        FeatureSet features{};
        features.ra_coalesce = true;
        GPRSMask pool{};
        pool.Mark(23);
        AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        REQUIRE(allocation.HasFixedGPRDefinition(value.Id()));
        REQUIRE(allocation.ValueGPR(value).id == 23);
        REQUIRE(allocation.IsHostWriteCoalesced(publication->Id()));
    }
}

TEST_CASE("Multiply and shifted fixed GPR publications bypass the temporary value class", "[fixed-gpr]") {
    for (auto op : {0, 1}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x98f8})};
        auto left = block->LoadImm(ValueType::U64, Imm{swift::u64{0x12345678}});
        auto right = block->LoadImm(ValueType::U64, Imm{swift::u64{0x0f0f0f0f}});
        auto value = op == 0 ? block->Mul(ValueType::U64, left, Operand{right})
                             : block->LslImm(ValueType::U64, left, Imm{5u});
        static_cast<void>(right);
        block->SetHostGPR(value, HostRegIndex(23), Imm{0u});
        auto* publication = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        FeatureSet features{};
        features.ra_coalesce = true;
        GPRSMask pool{};
        pool.Mark(23);
        AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        REQUIRE(allocation.HasFixedGPRDefinition(value.Id()));
        REQUIRE(allocation.ValueGPR(value).id == 23);
        REQUIRE(allocation.IsHostWriteCoalesced(publication->Id()));
    }
}

TEST_CASE("Shifted and carry fixed GPR publications bypass the temporary value class", "[fixed-gpr]") {
    for (auto op : {0, 1}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x9900})};
        auto left = block->LoadImm(ValueType::U64, Imm{swift::u64{0x12345678}});
        auto right = block->LoadImm(ValueType::U64, Imm{swift::u64{5}});
        auto value = op == 0 ? block->LslValue(ValueType::U64, left, right)
                             : block->Adc(ValueType::U64, left, Operand{right});
        block->SetHostGPR(value, HostRegIndex(23), Imm{0u});
        auto* publication = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        FeatureSet features{};
        features.ra_coalesce = true;
        GPRSMask pool{};
        pool.Mark(23);
        AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        REQUIRE(allocation.HasFixedGPRDefinition(value.Id()));
        REQUIRE(allocation.ValueGPR(value).id == 23);
        REQUIRE(allocation.IsHostWriteCoalesced(publication->Id()));
    }
}

TEST_CASE("Select and high multiply fixed GPR publications bypass the temporary value class", "[fixed-gpr]") {
    for (auto op : {0, 1, 2}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x9908})};
        auto left = block->LoadImm(ValueType::U64, Imm{swift::u64{0x12345678}});
        auto right = block->LoadImm(ValueType::U64, Imm{swift::u64{0x0f0f0f0f}});
        auto value = op == 0   ? block->CondSelect(ValueType::U64, Cond::EQ, left, right)
                     : op == 1 ? block->MulHigh(ValueType::U64, left, right, Imm{0u})
                               : block->LoadUniform(ValueType::U64, Uniform{64, ValueType::U64});
        static_cast<void>(left);
        static_cast<void>(right);
        block->SetHostGPR(value, HostRegIndex(23), Imm{0u});
        auto* publication = &block->GetInstList().back();
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        FeatureSet features{};
        features.ra_coalesce = true;
        GPRSMask pool{};
        pool.Mark(23);
        AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        REQUIRE(allocation.HasFixedGPRDefinition(value.Id()));
        REQUIRE(allocation.ValueGPR(value).id == 23);
        REQUIRE(allocation.IsHostWriteCoalesced(publication->Id()));
    }
}

TEST_CASE("Value select fixed GPR publication bypasses the temporary value class", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x9910})};
    auto cond = block->LoadImm(ValueType::U32, Imm{swift::u32{1}});
    auto left = block->LoadImm(ValueType::U64, Imm{swift::u64{0x12345678}});
    auto right = block->LoadImm(ValueType::U64, Imm{swift::u64{0x0f0f0f0f}});
    auto value = block->Select(ValueType::U64, cond, left, right);
    block->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    features.ra_coalesce = true;
    GPRSMask pool{};
    pool.Mark(23);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.HasFixedGPRDefinition(value.Id()));
    REQUIRE(allocation.ValueGPR(value).id == 23);
    REQUIRE(allocation.IsHostWriteCoalesced(publication->Id()));
}

TEST_CASE("Division fixed GPR publication bypasses the temporary value class", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x9918})};
    auto left = block->LoadImm(ValueType::U64, Imm{swift::u64{0x12345678}});
    auto right = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
    auto value = block->Div(ValueType::U64, left, Operand{right});
    block->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    features.ra_coalesce = true;
    GPRSMask pool{};
    pool.Mark(23);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.HasFixedGPRDefinition(value.Id()));
    REQUIRE(allocation.ValueGPR(value).id == 23);
    REQUIRE(allocation.IsHostWriteCoalesced(publication->Id()));
}

TEST_CASE("Narrow fixed GPR preallocation covers a widened read", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x9928})};
    auto read = block->GetHostGPR(ValueType::U32, HostRegIndex(22), Imm{0u});
    auto widened = block->ZeroExtend32To64(ValueType::U64, read);
    block->StoreUniform(Uniform{64, ValueType::U64}, widened);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{};
    pool.Mark(22);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.IsFixedGPRDefinitionElided(read.Id()));
    REQUIRE(allocation.PreservedGPRDefinitionAt(read.Id()) == nullptr);
    REQUIRE(allocation.ValueGPR(read).id == 22);
}

TEST_CASE("Adjacent fixed GPR publication bypasses the temporary value class", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x9840})};
    auto value = block->LoadImm(ValueType::U64, Imm{swift::u64{0x12345678}});
    block->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    features.ra_coalesce = true;
    GPRSMask pool{};
    pool.Mark(23);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.HasFixedGPRDefinition(value.Id()));
    REQUIRE_FALSE(allocation.IsFixedGPRDefinitionElided(value.Id()));
    REQUIRE(allocation.ValueGPR(value).id == 23);
    REQUIRE(allocation.IsHostWriteCoalesced(publication->Id()));
}

TEST_CASE("Fixed GPR transfer rejects source and destination clobbers", "[fixed-gpr]") {
    for (bool overwrite_source_before_publication : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x9850})};
        auto value = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
        auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x1000}});
        if (overwrite_source_before_publication)
            block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
        block->SetHostGPR(value, HostRegIndex(23), Imm{0u});
        if (!overwrite_source_before_publication) {
            block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
            // Even a partial destination write invalidates the captured address.
            auto byte = block->LoadImm(ValueType::U8, Imm{swift::u8{7}});
            block->SetHostGPR(byte, HostRegIndex(23), Imm{8u});
        }
        auto loaded = block->LoadMemory(ValueType::U64, Operand{value});
        block->StoreUniform(Uniform{64, ValueType::U64}, loaded);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        FeatureSet features{};
        GPRSMask pool{};
        pool.Mark(20);
        pool.Mark(23);
        AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        const auto* residence = allocation.FixedGPRDefinitionAt(value.Id());
        REQUIRE((!residence || residence->preserved));
        REQUIRE(allocation.ValueGPR(value).id != 20);
    }
}

TEST_CASE("Fixed GPR transfer follows exact helper preservation", "[fixed-gpr]") {
    for (bool preserved : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x9860})};
        auto value = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
        block->SetHostGPR(value, HostRegIndex(7), Imm{0u});
        auto replacement = block->LoadImm(ValueType::U64, Imm{swift::u64{0x1000}});
        block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
        block->CallLambda(ValueType::VOID,
                Lambda{DataClass{Imm{1}},
                       HelperCallTraits{.host_registers =
                                                preserved ? HostRegisterEffect::PreservesPinnedState
                                                          : HostRegisterEffect::MayTouchSIMD}});
        auto loaded = block->LoadMemory(ValueType::U64, Operand{value});
        block->StoreUniform(Uniform{64, ValueType::U64}, loaded);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        FeatureSet features{};
        GPRSMask pool{};
        pool.Mark(7);
        pool.Mark(20);
        AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        const auto* residence = allocation.FixedGPRDefinitionAt(value.Id());
        REQUIRE(residence != nullptr);
        REQUIRE(residence->preserved == !preserved);
        if (preserved) REQUIRE(allocation.GPRLocationAt(value, loaded.Id()).slot == 7);
        else REQUIRE(allocation.GPRLocationAt(value, loaded.Id()).slot != 20);
    }
}

TEST_CASE("Function collectors agree on fixed GPR residence segments", "[fixed-gpr]") {
    FeatureSet features{};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0x9870}, Location{0x9871});
    auto value = function->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
    function->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto replacement = function->LoadImm(ValueType::U64, Imm{swift::u64{0x1000}});
    function->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    auto loaded = function->LoadMemory(ValueType::U64, Operand{value});
    function->StoreUniform(Uniform{64, ValueType::U64}, loaded);
    function->EndBlock(terminal::ReturnToDispatch{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    GPRSMask pool{};
    pool.Mark(20);
    pool.Mark(23);
    AllocationFixture general{function->MaxInstrCount(), pool, FPRSMask{}, features};
    AllocationFixture fast{function->MaxInstrCount(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateFunction(
            function,
            &general,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(function, general, features),
            {.block_intervals = false});
    ::swift::runtime::compile::AllocateFunction(
            function,
            &fast,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(function, fast, features),
            {.block_intervals = true});
    REQUIRE(general.HasFixedGPRDefinition(value.Id()));
    REQUIRE(fast.HasFixedGPRDefinition(value.Id()));
    REQUIRE(general.GPRLocationAt(value, loaded.Id()).slot == 23);
    REQUIRE(fast.GPRLocationAt(value, loaded.Id()).slot == 23);
    for (swift::u32 id = 0; id < general.MapCount(); ++id) {
        INFO("allocation id " << id);
        REQUIRE(general.Mapping(id) == fast.Mapping(id));
    }
}

TEST_CASE("Fixed GPR preallocation preserves captures used in successor blocks", "[fixed-gpr]") {
    FeatureSet features{};
    HIRBuilder builder{2, true, features};
    auto* function = builder.AppendFunction(Location{0x9880}, Location{0x9882});
    auto value = function->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
    function->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto replacement = function->LoadImm(ValueType::U64, Imm{swift::u64{0x1000}});
    function->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    auto* successor = builder.LinkBlock(terminal::LinkBlock{Location{0x9881}});
    builder.SetCurBlock(successor);
    auto loaded = function->LoadMemory(ValueType::U64, Operand{value});
    function->StoreUniform(Uniform{64, ValueType::U64}, loaded);
    function->EndBlock(terminal::ReturnToDispatch{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    GPRSMask pool{};
    pool.Mark(20);
    pool.Mark(23);
    AllocationFixture allocation{function->MaxInstrCount(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateFunction(
            function,
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    function, allocation, features),
            {.block_intervals = false});
    REQUIRE(allocation.PreservedGPRDefinitionAt(value.Id()) != nullptr);
    REQUIRE(allocation.PreservedGPRDefinitionAt(value.Id())->transfer == value.Id());
    REQUIRE(allocation.ValueGPR(value).id != 20);
}

TEST_CASE("A wide arithmetic result cannot preallocate a narrow publication", "[fixed-gpr]") {
    for (auto type : {ValueType::U8, ValueType::U16}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x9890})};
        auto source = block->LoadImm(ValueType::U32, Imm{0xffu});
        auto computed = block->Add(ValueType::U32, source, Operand{Imm{1u}});
        // This is the frontend's narrow carry arithmetic shape: U32 math,
        // followed by an architectural byte/halfword update in the fixed home.
        block->SetHostGPR(computed.SetCastType(type), HostRegIndex(20), Imm{0u});
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        FeatureSet features{};
        GPRSMask pool{};
        pool.Mark(20);
        AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        REQUIRE_FALSE(allocation.HasFixedGPRDefinition(computed.Id()));
        REQUIRE(allocation.ValueGPR(computed).id != 20);
    }
}

TEST_CASE("A widened narrow capture keeps clean high bits", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x98a0})};
    auto value = block->GetHostGPR(ValueType::U32, HostRegIndex(20), Imm{0u});
    auto address = block->LoadImm(ValueType::U64, Imm{swift::u64{0x1000}});
    block->StoreMemory(Operand{address}, value.SetCastType(ValueType::U64));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{};
    pool.Mark(20);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.PreservedGPRDefinitionAt(value.Id()) != nullptr);
    REQUIRE(allocation.ValueGPR(value).id != 20);
}

TEST_CASE("Flags input reuse reads its fixed home directly", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x98b0})};
    auto value = block->GetHostGPR(ValueType::U32, HostRegIndex(3), Imm{0u});
    auto result = block->Add(ValueType::U32, value, Operand{Imm{1u}});
    block->SaveFlags(result, Flags::NZCV);
    block->StoreUniform(Uniform{64, ValueType::U32}, result);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{};
    pool.Mark(3);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.IsFixedGPRDefinitionElided(value.Id()));
    REQUIRE(allocation.PreservedGPRDefinitionAt(value.Id()) == nullptr);
    REQUIRE(allocation.ValueGPR(value).id == 3);
}

TEST_CASE("Width coalescing accepts a preallocated read input", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x98c0})};
    auto value = block->GetHostGPR(ValueType::U32, HostRegIndex(3), Imm{0u});
    auto result = block->Add(ValueType::U32, value, Operand{Imm{1u}});
    auto widened = block->ZeroExtend32To64(ValueType::U64, result);
    block->SetHostGPR(widened, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    features.ra_width_chain = true;
    GPRSMask pool{};
    for (unsigned reg : {0, 1, 2, 3, 4, 5, 19, 20, 21, 22, 23, 25, 26, 27, 28, 29, 30, 31})
        pool.Mark(reg);
    AllocationFixture allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &allocation,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                    block.get(), allocation, features),
            false);
    REQUIRE(allocation.GPRLocationAt(value, result.Id()).slot == 3);
    REQUIRE(allocation.IsFixedGPRDefinitionElided(value.Id()));
    if (!X86PinExtLevel2Enabled(pool)) {
        REQUIRE(allocation.ValueGPR(result).id != 23);
        return;
    }
    REQUIRE(allocation.ValueGPR(result).id == 23);
    REQUIRE(allocation.ValueGPR(widened).id == 23);
    REQUIRE(allocation.GPRLocationAt(widened, publication->Id()).slot == 23);
}
