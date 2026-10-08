#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/guest_state_map.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

std::vector<std::string> Disassemble(arm64::JitContext& context) {
    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
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

    auto old_source = block->GetHostGPR(HostRegIndex(1), Imm{0u}).SetType(ValueType::U64);
    if (padding) block->Nop();  // Keep both survival intervals nonempty.
    block->SetHostGPR(old_source, HostRegIndex(23), Imm{0u});
    auto new_source = block->LoadImm(Imm{swift::u64{0x2000}}).SetType(ValueType::U64);
    block->SetHostGPR(new_source, HostRegIndex(1), Imm{0u});
    if (alu_consumer) {
        auto result = block->Add(old_source, Operand{Imm{swift::u64{7}}})
                              .SetType(ValueType::U64);
        block->StoreUniform(Uniform{64, ValueType::U64}, result);
    } else {
        auto address_source = old_source;
        for (unsigned i = 0; i < alias_depth; ++i)
            address_source = block->BitCast(address_source).SetType(ValueType::U64);
        auto first_address = block->BitCast(address_source).SetType(ValueType::U64);
        auto first = block->LoadMemory(Operand{first_address, Imm{8u}})
                             .SetType(ValueType::U64);
        block->StoreUniform(Uniform{64, ValueType::U64}, first);
        if (overwrite_target) {
            auto replacement = block->LoadImm(Imm{swift::u64{0x3000}})
                                       .SetType(ValueType::U64);
            block->SetHostGPR(replacement, HostRegIndex(23), Imm{0u});
        }
        auto second_address = block->BitCast(address_source).SetType(ValueType::U64);
        auto second = block->LoadMemory(Operand{second_address, Imm{16u}})
                              .SetType(ValueType::U64);
        block->StoreUniform(Uniform{72, ValueType::U64}, second);
    }
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(),
                   features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    arm64::JitContext context{module, alloc};
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

    auto old_source = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
    block->SetHostGPR(old_source, HostRegIndex(target), Imm{0u});
    auto replacement = block->LoadImm(Imm{swift::u64{0x2000}}).SetType(ValueType::U64);
    block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    (void)block->CallLambda(Lambda{DataClass{Imm{1}}, HelperCallTraits{.host_registers = effect}});
    auto address = block->BitCast(old_source).SetType(ValueType::U64);
    auto loaded = block->LoadMemory(Operand{address, Imm{8u}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{64, ValueType::U64}, loaded);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(),
                   features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    arm64::JitContext context{module, alloc};
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

    auto source = block->GetHostGPR(HostRegIndex(20), Imm{0u})
                          .SetType(ValueType::U64);
    block->SetHostGPR(source, HostRegIndex(7), Imm{0u});
    auto replacement = block->LoadImm(Imm{swift::u64{0x2000}})
                               .SetType(ValueType::U64);
    block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    auto left = block->LoadUniform(Uniform{0, ValueType::V128});
    auto right = block->LoadUniform(Uniform{16, ValueType::V128});
    auto result = block->Sse42Str(left, right, Imm{imm})
                          .SetType(ValueType::U64);
    block->StoreUniform(Uniform{32, ValueType::U64}, result);
    auto address = block->BitCast(source).SetType(ValueType::U64);
    auto loaded = block->LoadMemory(Operand{address, Imm{8u}})
                          .SetType(ValueType::U64);
    block->StoreUniform(Uniform{40, ValueType::U64}, loaded);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(),
                   features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    arm64::JitContext context{module, alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    return Disassemble(context);
}

std::vector<std::string> EmitCrossBlockWidth(bool external_entry) {
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

    auto value = function->LoadImm(Imm{swift::u32{7}}).SetType(ValueType::U32);
    function->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto* second = builder.LinkBlock(
            terminal::LinkBlock{Location{second_guest}});
    if (external_entry) {
        function->RegisterExternalEntryRoot(second);
    }
    builder.SetCurBlock(second);
    auto resident = function
                            ->GetHostGPR(HostRegIndex(23), Imm{0u})
                            .SetType(ValueType::U32);
    function->SetHostGPR(resident, HostRegIndex(23), Imm{0u});
    auto result = function->Add(resident, Operand{Imm{swift::u32{1}}})
                          .SetType(ValueType::U32);
    function->StoreUniform(Uniform{64, ValueType::U32}, result);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();

    RegAlloc alloc{function->MaxInstrCount(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(),
                   features};
    RegisterAllocPass::Run(function, &alloc, features);
    arm64::JitContext context{module, alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(function);
    context.Finish();
    return Disassemble(context);
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
    auto published = function->LoadImm(Imm{swift::u32{7}})
                             .SetType(ValueType::U32);
    auto* publication = function->AppendInst(
            OpCode::SetHostGPR, published, HostRegIndex(23), Imm{0u});
    auto read = function->GetHostGPR(HostRegIndex(23), Imm{0u})
                        .SetType(ValueType::U32);
    auto invariant = function->GetHostGPR(HostRegIndex(29), Imm{0u})
                             .SetType(ValueType::U32);
    Value current = function->GetHostGPR(HostRegIndex(22), Imm{0u})
                            .SetType(ValueType::U32);
    Value cross_block_bridge{};
    for (swift::u32 index = 0; index < 32; ++index) {
        Value left = current;
        Value right = invariant;
        if (index != 0) {
            left = function->BitExtract(current, Imm{0u}, Imm{32u})
                           .SetType(ValueType::U32);
            right = function->BitExtract(invariant, Imm{0u}, Imm{32u})
                            .SetType(ValueType::U32);
            if (!cross_block_bridge.Defined()) {
                cross_block_bridge = left;
            }
        }
        auto producer = (index & 1u)
                ? function->Xor(left, Operand{right}).SetType(ValueType::U32)
                : function->Add(left, Operand{right}).SetType(ValueType::U32);
        current = function->ZeroExtend32To64(producer).SetType(ValueType::U64);
        function->SetHostGPR(current, HostRegIndex(22), Imm{0u});
    }
    auto* next = builder.LinkBlock(terminal::LinkBlock{successor});
    builder.SetCurBlock(next);
    auto replacement = function->LoadImm(Imm{swift::u32{9}})
                               .SetType(ValueType::U32);
    function->SetHostGPR(replacement, HostRegIndex(23), Imm{0u});
    auto first = function->Add(published, Operand{Imm{swift::u32{1}}})
                         .SetType(ValueType::U32);
    auto second = function->Add(read, Operand{Imm{swift::u32{1}}})
                          .SetType(ValueType::U32);
    auto third = function->Add(cross_block_bridge,
                               Operand{Imm{swift::u32{1}}})
                         .SetType(ValueType::U32);
    function->StoreUniform(Uniform{64, ValueType::U32}, first);
    function->StoreUniform(Uniform{68, ValueType::U32}, second);
    function->StoreUniform(Uniform{72, ValueType::U32}, third);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();

    RegAlloc alloc{function->MaxInstrCount(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(),
                   features};
    RegisterAllocPass::Run(function, &alloc, features);
    return {
            alloc.IsHostWriteCoalesced(publication->Id()),
            alloc.IsHostReadCoalesced(read.Id()),
            alloc.IsWidthChainCoalesced(cross_block_bridge.Id()),
            alloc.ValueGPR(published).id,
            alloc.ValueGPR(read).id,
    };
}

arm64::GuestStateMap::ExtensionFacts DiamondEntryFacts(
        bool signed_value,
        bool clobber_predecessor) {
    constexpr Location entry{0x9800};
    constexpr Location left_location{0x9810};
    constexpr Location right_location{0x9820};
    constexpr Location join_location{0x9830};
    FeatureSet features{};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(entry, Location{0x9840});
    Value value;
    if (signed_value) {
        auto narrow = function->LoadImm(Imm{swift::u8{0x80}})
                              .SetType(ValueType::S8);
        value = function->SignExtend(narrow).SetType(ValueType::U64);
    } else {
        value = function->LoadImm(Imm{swift::u32{1}}).SetType(ValueType::U32);
    }
    function->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto condition = function->LoadImm(Imm{swift::u8{1}}).SetType(ValueType::U8);
    auto [left, right] = builder.If(terminal::If{
            condition,
            terminal::LinkBlock{left_location},
            terminal::LinkBlock{right_location},
    });

    builder.SetCurBlock(left);
    auto* join = builder.LinkBlock(terminal::LinkBlock{join_location});
    builder.SetCurBlock(right);
    if (clobber_predecessor) {
        auto wide = function
                            ->LoadImm(Imm{UINT64_C(0x10000000000)})
                            .SetType(ValueType::U64);
        function->SetHostGPR(wide, HostRegIndex(23), Imm{0u});
    }
    builder.LinkBlock(terminal::LinkBlock{join_location});
    builder.SetCurBlock(join);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();

    arm64::GuestStateMap state_map;
    state_map.AnalyzeFunction(function, features);
    return state_map.EntryExtensionFacts(join->GetBlock(), 23);
}

bool LoopEntryKnownZero(bool clobber_backedge) {
    constexpr Location entry{0x9850};
    constexpr Location loop_location{0x9860};
    FeatureSet features{};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(entry, Location{0x9870});
    auto value = function->LoadImm(Imm{swift::u32{1}}).SetType(ValueType::U32);
    function->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto* loop = builder.LinkBlock(terminal::LinkBlock{loop_location});
    builder.SetCurBlock(loop);
    if (clobber_backedge) {
        auto wide = function
                            ->LoadImm(Imm{UINT64_C(0x10000000000)})
                            .SetType(ValueType::U64);
        function->SetHostGPR(wide, HostRegIndex(23), Imm{0u});
    }
    builder.LinkBlock(terminal::LinkBlock{loop_location});
    function->EndFunction();
    function->ComputeRPO();

    arm64::GuestStateMap state_map;
    state_map.AnalyzeFunction(function, features);
    return state_map.EntryExtensionFacts(loop->GetBlock(), 23)
            .KnownZeroAbove(32);
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
    // uses the index. More than eight aliases also exercise worklist spill.
    const auto expected = EmitTransfer(false);
    CHECK(EmitTransfer(false, false, 80) == expected);
    CHECK(EmitTransfer(false, false, 80, 12) == expected);
    CHECK(EmitTransfer(true, false, 80) == EmitTransfer(true));
}

TEST_CASE("overwriting the resident GPR version keeps the source capture") {
    const auto lines = EmitTransfer(true);
    REQUIRE(Count(lines, "ldr x", "[x23, #8]") == 0);
    REQUIRE(Count(lines, "ldr x", "[x23, #16]") == 0);
}

TEST_CASE("published value versions feed general ALU consumers") {
    const auto lines = EmitTransfer(false, true);
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
    REQUIRE(Count(internal, "mov w23, w23", "") == 0);
    REQUIRE(Count(external, "mov w23, w23", "") == 1);
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
    REQUIRE(LoopEntryKnownZero(false));
    REQUIRE_FALSE(LoopEntryKnownZero(true));
}

TEST_CASE("pinned allocation transfers a residence only after its publication") {
    IntrusivePtr<Block> block{new Block(0, Location{0x97d0})};
    const auto source = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
    block->SetHostGPR(source, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    const auto replacement = block->LoadImm(Imm{swift::u64{0x2000}}).SetType(ValueType::U64);
    block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    const auto address = block->BitCast(source).SetType(ValueType::U64);
    const auto loaded = block->LoadMemory(Operand{address, Imm{8u}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{64, ValueType::U64}, loaded);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    RegAlloc alloc{block->MaxInstrId(), GPRSMask{~(1u << 14)}, FPRSMask{}, features};
    alloc.MapRegister(source.Id(), HostGPR{14});
    alloc.MapRegister(replacement.Id(), HostGPR{15});
    alloc.MapReference(source.Id(), address.Id());
    alloc.MapRegister(loaded.Id(), HostGPR{16});
    arm64::GuestStateMap semantics;
    semantics.Analyze(block.get(), features);
    arm64::PinnedGPRAllocation placement{alloc, features, semantics};
    placement.Prepare(block.get());
    REQUIRE(placement.GetRecipes().transfers.size() == 1);

    const auto before = placement.UseLocation(source, publication);
    REQUIRE(before.kind == RegAlloc::GPR);
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
    const auto value = next->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
    next->StoreUniform(Uniform{64, ValueType::U64}, value);
    auto* consumer = &next->GetInstList().back();
    next->SetTerminal(terminal::ReturnToDispatch{});
    next->ReIdInstr();
    alloc.MapRegister(value.Id(), HostGPR{14});
    semantics.Analyze(next.get(), features);
    placement.Prepare(next.get());
    REQUIRE(placement.GetRecipes().transfers.empty());
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
    const auto source = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U8);
    const auto widened = block->ZeroExtend32(source).SetType(ValueType::U32);
    const auto extended = block->ZeroExtend32To64(widened).SetType(ValueType::U64);
    block->SetHostGPR(extended, HostRegIndex(23), Imm{0u});
    const auto replacement = block->LoadImm(Imm{swift::u64{0x2000}}).SetType(ValueType::U64);
    block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    const auto condition = block->LoadImm(Imm{1u}).SetType(ValueType::U8);
    const auto otherwise = block->LoadImm(Imm{99u}).SetType(ValueType::U32);
    const auto selected = block->Select(condition, widened, otherwise).SetType(ValueType::U32);
    block->StoreUniform(Uniform{64, ValueType::U32}, selected);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    alloc.MapMemSpill(widened.Id(), SpillSlot{0});
    REQUIRE(alloc.ValueType(widened) == RegAlloc::MEM);

    arm64::GuestStateMap semantics;
    semantics.Analyze(block.get(), features);
    arm64::PinnedGPRAllocation placement{alloc, features, semantics};
    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
    const auto before = context.CurrentBufferSize();
    placement.Prepare(block.get());
    REQUIRE(context.CurrentBufferSize() == before);
    REQUIRE(placement.GetRecipes().copies.size() == 1);
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
    const auto condition = block->LoadImm(Imm{1u}).SetType(ValueType::U8);
    const auto left = block->LoadImm(Imm{41u}).SetType(ValueType::U32);
    const auto right = block->LoadImm(Imm{99u}).SetType(ValueType::U32);
    const auto selected = block->Select(condition, left, right).SetType(ValueType::U32);
    block->StoreUniform(Uniform{64, ValueType::U32}, selected);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    alloc.MapMemSpill(left.Id(), SpillSlot{0});
    alloc.MapMemSpill(right.Id(), SpillSlot{1});
    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
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
    auto source = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
    auto fault_address = block->LoadImm(Imm{swift::u64{0x4000}}).SetType(ValueType::U64);
    auto fault = block->LoadMemory(Operand{fault_address}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{80, ValueType::U64}, fault);
    block->SetHostGPR(source, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    auto replacement = block->LoadImm(Imm{swift::u64{0x2000}}).SetType(ValueType::U64);
    block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    auto alias = block->BitCast(source).SetType(ValueType::U64);
    auto loaded = block->LoadMemory(Operand{alias, Imm{8u}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{88, ValueType::U64}, loaded);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    auto pool = address_space.GetTrampolines().GetGPRRegs();
    pool.Mark(20);
    pool.Mark(23);
    RegAlloc allocation{block->MaxInstrId(), pool,
                        address_space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
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
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE(allocation.GPRLocationAt(source, loaded.Id()).slot == 23);
    arm64::JitContext context{address_space.GetDefaultModule(), allocation};
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
    auto source = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
    std::vector<Value> values;
    for (unsigned i = 0; i < 7; ++i)
        values.push_back(block->LoadImm(Imm{swift::u64{0x1000 + i}}).SetType(ValueType::U64));
    block->SetHostGPR(source, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->SetHostGPR(values.front(), HostRegIndex(20), Imm{0u});
    auto loaded = block->LoadMemory(Operand{source}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{96, ValueType::U64}, loaded);
    for (unsigned i = 0; i < values.size(); ++i)
        block->StoreUniform(Uniform{static_cast<swift::u16>(104 + 8 * i), ValueType::U64}, values[i]);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{UINT32_MAX};
    for (unsigned code : {6, 7, 8, 9, 14, 15}) pool.Clear(code);
    FPRSMask fprs{};
    RegAlloc baseline{block->MaxInstrId(), pool, fprs, features};
    // Use the existing copy-preserving allocator for an in-process baseline.
    baseline.FinishFixedGPRConstraints();
    RegisterAllocPass::Run(block.get(), &baseline, false, features);
    RegAlloc optimized{block->MaxInstrId(), pool, fprs, features};
    RegisterAllocPass::Run(block.get(), &optimized, false, features);
    auto spill_count = [&](const RegAlloc& allocation) {
        unsigned count = 0;
        for (auto& inst : block->GetInstList()) {
            if (inst.HasValue() && !inst.IsBitCastOperation())
                count += allocation.ValueType(Value{&inst}) == RegAlloc::MEM;
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
    auto read = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U32);
    auto result = block->And(read, Operand{Imm{0xffu}}).SetType(ValueType::U32);
    block->StoreUniform(Uniform{64, ValueType::U32}, result);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{};
    pool.Mark(20);
    RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE(allocation.IsFixedGPRDefinitionElided(read.Id()));
    const auto location = allocation.GPRLocationAt(read, result.Id());
    REQUIRE(location.slot == 20);
    REQUIRE(location.bits == 32);
    // A W view is not a physical W write and proves nothing about X[63:32].
    arm64::GuestStateMap semantics;
    semantics.Analyze(block.get(), features);
    semantics.BuildValueVersions({}, {}, true);
    const auto fact = semantics.FixedHomeForUse(read, result.Def());
    REQUIRE(fact.has_value());
    REQUIRE_FALSE(fact->extension.KnownZeroAbove(32));
}

TEST_CASE("Adjacent fixed GPR publication bypasses the temporary value class", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x9840})};
    auto value = block->LoadImm(Imm{swift::u64{0x12345678}}).SetType(ValueType::U64);
    block->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    features.ra_coalesce = true;
    GPRSMask pool{};
    pool.Mark(23);
    RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE(allocation.HasFixedGPRDefinition(value.Id()));
    REQUIRE_FALSE(allocation.IsFixedGPRDefinitionElided(value.Id()));
    REQUIRE(allocation.ValueGPR(value).id == 23);
    REQUIRE(allocation.IsHostWriteCoalesced(publication->Id()));
}

TEST_CASE("Fixed GPR transfer rejects source and destination clobbers", "[fixed-gpr]") {
    for (bool overwrite_source_before_publication : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x9850})};
        auto value = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
        auto replacement = block->LoadImm(Imm{swift::u64{0x1000}}).SetType(ValueType::U64);
        if (overwrite_source_before_publication)
            block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
        block->SetHostGPR(value, HostRegIndex(23), Imm{0u});
        if (!overwrite_source_before_publication) {
            block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
            // Even a partial destination write invalidates the captured address.
            auto byte = block->LoadImm(Imm{swift::u8{7}}).SetType(ValueType::U8);
            block->SetHostGPR(byte, HostRegIndex(23), Imm{8u});
        }
        auto loaded = block->LoadMemory(Operand{value}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{64, ValueType::U64}, loaded);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        FeatureSet features{};
        GPRSMask pool{};
        pool.Mark(20);
        pool.Mark(23);
        RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        REQUIRE_FALSE(allocation.HasFixedGPRDefinition(value.Id()));
        REQUIRE(allocation.ValueGPR(value).id != 20);
    }
}

TEST_CASE("Fixed GPR transfer follows exact helper preservation", "[fixed-gpr]") {
    for (bool preserved : {false, true}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x9860})};
        auto value = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
        block->SetHostGPR(value, HostRegIndex(7), Imm{0u});
        auto replacement = block->LoadImm(Imm{swift::u64{0x1000}}).SetType(ValueType::U64);
        block->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
        block->CallLambda(Lambda{DataClass{Imm{1}}, HelperCallTraits{
                .host_registers = preserved ? HostRegisterEffect::PreservesPinnedState
                                            : HostRegisterEffect::MayTouchSIMD}});
        auto loaded = block->LoadMemory(Operand{value}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{64, ValueType::U64}, loaded);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        FeatureSet features{};
        GPRSMask pool{};
        pool.Mark(7);
        pool.Mark(20);
        RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        REQUIRE(allocation.HasFixedGPRDefinition(value.Id()) == preserved);
        if (preserved) REQUIRE(allocation.GPRLocationAt(value, loaded.Id()).slot == 7);
    }
}

TEST_CASE("Function collectors agree on fixed GPR residence segments", "[fixed-gpr]") {
    FeatureSet features{};
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{0x9870}, Location{0x9871});
    auto value = function->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
    function->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto replacement = function->LoadImm(Imm{swift::u64{0x1000}}).SetType(ValueType::U64);
    function->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    auto loaded = function->LoadMemory(Operand{value}).SetType(ValueType::U64);
    function->StoreUniform(Uniform{64, ValueType::U64}, loaded);
    function->EndBlock(terminal::ReturnToDispatch{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    GPRSMask pool{};
    pool.Mark(20);
    pool.Mark(23);
    RegAlloc general{function->MaxInstrCount(), pool, FPRSMask{}, features};
    RegAlloc fast{function->MaxInstrCount(), pool, FPRSMask{}, features};
    RegisterAllocPass::Run(function, &general, false, features);
    RegisterAllocPass::Run(function, &fast, true, features);
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
    auto value = function->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U64);
    function->SetHostGPR(value, HostRegIndex(23), Imm{0u});
    auto replacement = function->LoadImm(Imm{swift::u64{0x1000}}).SetType(ValueType::U64);
    function->SetHostGPR(replacement, HostRegIndex(20), Imm{0u});
    auto* successor = builder.LinkBlock(terminal::LinkBlock{Location{0x9881}});
    builder.SetCurBlock(successor);
    auto loaded = function->LoadMemory(Operand{value}).SetType(ValueType::U64);
    function->StoreUniform(Uniform{64, ValueType::U64}, loaded);
    function->EndBlock(terminal::ReturnToDispatch{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    GPRSMask pool{};
    pool.Mark(20);
    pool.Mark(23);
    RegAlloc allocation{function->MaxInstrCount(), pool, FPRSMask{}, features};
    RegisterAllocPass::Run(function, &allocation, false, features);
    REQUIRE_FALSE(allocation.HasFixedGPRDefinition(value.Id()));
    REQUIRE(allocation.ValueGPR(value).id != 20);
}

TEST_CASE("A wide arithmetic result cannot preallocate a narrow publication", "[fixed-gpr]") {
    for (auto type : {ValueType::U8, ValueType::U16}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x9890})};
        auto source = block->LoadImm(Imm{0xffu}).SetType(ValueType::U32);
        auto computed = block->Add(source, Operand{Imm{1u}}).SetType(ValueType::U32);
        // This is the frontend's narrow carry arithmetic shape: U32 math,
        // followed by an architectural byte/halfword update in the fixed home.
        block->SetHostGPR(computed.SetCastType(type), HostRegIndex(20), Imm{0u});
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        FeatureSet features{};
        GPRSMask pool{};
        pool.Mark(20);
        RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        REQUIRE_FALSE(allocation.HasFixedGPRDefinition(computed.Id()));
        REQUIRE(allocation.ValueGPR(computed).id != 20);
    }
}

TEST_CASE("A widened narrow capture keeps clean high bits", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x98a0})};
    auto value = block->GetHostGPR(HostRegIndex(20), Imm{0u}).SetType(ValueType::U32);
    auto address = block->LoadImm(Imm{swift::u64{0x1000}}).SetType(ValueType::U64);
    block->StoreMemory(Operand{address}, value.SetCastType(ValueType::U64));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{};
    pool.Mark(20);
    RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE_FALSE(allocation.HasFixedGPRDefinition(value.Id()));
    REQUIRE(allocation.ValueGPR(value).id != 20);
}

TEST_CASE("Flags input reuse retains its ordinary capture placement", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x98b0})};
    auto value = block->GetHostGPR(HostRegIndex(3), Imm{0u}).SetType(ValueType::U32);
    auto result = block->Add(value, Operand{Imm{1u}}).SetType(ValueType::U32);
    block->SaveFlags(result, Flags::NZCV);
    block->StoreUniform(Uniform{64, ValueType::U32}, result);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    GPRSMask pool{};
    pool.Mark(3);
    RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    REQUIRE_FALSE(allocation.HasFixedGPRDefinition(value.Id()));
    REQUIRE(allocation.ValueGPR(value).id != 3);
}

TEST_CASE("Width coalescing accepts a preallocated read input", "[fixed-gpr]") {
    IntrusivePtr<Block> block{new Block(0, Location{0x98c0})};
    auto value = block->GetHostGPR(HostRegIndex(3), Imm{0u}).SetType(ValueType::U32);
    auto result = block->Add(value, Operand{Imm{1u}}).SetType(ValueType::U32);
    auto widened = block->ZeroExtend32To64(result).SetType(ValueType::U64);
    block->SetHostGPR(widened, HostRegIndex(23), Imm{0u});
    auto* publication = &block->GetInstList().back();
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    FeatureSet features{};
    features.ra_width_chain = true;
    GPRSMask pool{};
    for (unsigned reg : {0, 1, 2, 3, 4, 5, 19, 20, 21, 22, 23, 25, 26, 27, 28, 29, 30, 31})
        pool.Mark(reg);
    RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
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
