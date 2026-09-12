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
                                      bool alu_consumer = false) {
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

    auto old_source = block->GetHostGPR(HostRegIndex(1), Imm{0u}).SetType(ValueType::U64);
    block->SetHostGPR(old_source, HostRegIndex(23), Imm{0u});
    auto new_source = block->LoadImm(Imm{swift::u64{0x2000}}).SetType(ValueType::U64);
    block->SetHostGPR(new_source, HostRegIndex(1), Imm{0u});
    if (alu_consumer) {
        auto result = block->Add(old_source, Operand{Imm{swift::u64{7}}})
                              .SetType(ValueType::U64);
        block->StoreUniform(Uniform{64, ValueType::U64}, result);
    } else {
        auto first_address = block->BitCast(old_source).SetType(ValueType::U64);
        auto first = block->LoadMemory(Operand{first_address, Imm{8u}})
                             .SetType(ValueType::U64);
        block->StoreUniform(Uniform{64, ValueType::U64}, first);
        if (overwrite_target) {
            auto replacement = block->LoadImm(Imm{swift::u64{0x3000}})
                                       .SetType(ValueType::U64);
            block->SetHostGPR(replacement, HostRegIndex(23), Imm{0u});
        }
        auto second_address = block->BitCast(old_source).SetType(ValueType::U64);
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
