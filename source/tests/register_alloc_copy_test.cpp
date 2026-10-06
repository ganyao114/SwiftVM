#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <array>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

GPRSMask CopyTestGPRs() {
    GPRSMask result{swift::u32{0}};
    for (swift::u32 code : {0u, 1u, 2u, 3u, 4u, 5u, 19u, 20u, 21u,
                            22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u}) {
        result.Mark(code);
    }
    return result;
}

struct CopyAllocation {
    IntrusivePtr<Block> block;
    std::unique_ptr<RegAlloc> alloc;
    Value source;
    Value bridge;
    Value wrapper;
};

CopyAllocation AllocateCopyChain(bool separate_nodes,
                                 bool reuse_bridge,
                                 bool keep_source_live) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8680})};
    auto source = block->LoadUniform<TypedValue<ValueType::U64>>(
            Uniform{0, ValueType::U64});
    auto bridge = block->BitExtract(source, Imm{0u}, Imm{32u})
                          .SetType(ValueType::U32);
    if (separate_nodes) {
        block->AdvancePC(Imm{1u});
    }
    auto wrapper = block->ZeroExtend32To64(bridge).SetType(ValueType::U64);
    block->StoreUniform(Uniform{8, ValueType::U64}, wrapper);
    block->StoreUniform(Uniform{16, ValueType::U64}, wrapper);
    if (reuse_bridge) {
        block->StoreUniform(Uniform{32, ValueType::U32}, bridge);
    }
    if (keep_source_live) {
        block->StoreUniform(Uniform{24, ValueType::U64}, source);
    }
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    auto features = FeatureSet{};
    auto alloc = std::make_unique<RegAlloc>(
            block->MaxInstrId(), CopyTestGPRs(), FPRSMask{~((1u << 8) - 1u)},
            features);
    RegisterAllocPass::Run(block.get(), alloc.get(), false, features);
    return {std::move(block), std::move(alloc), source, bridge, wrapper};
}

}  // namespace

TEST_CASE("low32 copies support separated and repeated wrapper uses") {
    auto accepted = AllocateCopyChain(false, false, true);
    REQUIRE(accepted.alloc->IsLow32CopyCoalesced(accepted.bridge.Id()));
    REQUIRE(accepted.alloc->Low32CopySource(accepted.bridge.Id()) ==
            accepted.source.Id());
    REQUIRE(accepted.alloc->ValueGPR(accepted.bridge).id ==
            accepted.alloc->ValueGPR(accepted.source).id);
    REQUIRE(accepted.alloc->ValueGPR(accepted.wrapper).id !=
            accepted.alloc->ValueGPR(accepted.source).id);

    auto separated = AllocateCopyChain(true, false, true);
    REQUIRE(separated.alloc->IsLow32CopyCoalesced(separated.bridge.Id()));

    auto reused = AllocateCopyChain(false, true, true);
    REQUIRE(reused.alloc->IsLow32CopyCoalesced(reused.bridge.Id()));

    auto dead_source = AllocateCopyChain(false, false, false);
    REQUIRE(dead_source.alloc->IsLow32CopyCoalesced(dead_source.bridge.Id()));
}

TEST_CASE("ordered stores accept coalesced low32 views during emission") {
    for (unsigned variant : {0u, 1u, 2u, 3u}) {
        const bool ordered = (variant & 1u) != 0;
        const bool keep_source_live = (variant & 2u) != 0;
        CAPTURE(ordered, keep_source_live);
        IntrusivePtr<Block> block{new Block(0, Location{0x86c0})};
        auto source = block->LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{0, ValueType::U64});
        auto bridge = block->BitExtract(source, Imm{0u}, Imm{32u})
                              .SetType(ValueType::U32);
        auto reused_home = block->LoadImm(Imm{7u}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{8, ValueType::U64}, reused_home);
        if (ordered) {
            block->StoreMemoryTSO(Operand{Imm{0x10000u}}, bridge);
        } else {
            block->StoreMemory(Operand{Imm{0x10000u}}, bridge);
        }
        if (keep_source_live) {
            block->StoreUniform(Uniform{16, ValueType::U64}, source);
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        ModuleConfig module_config{};
        auto module = address_space.MapModule(
                LocationDescriptor{0x86c0}, LocationDescriptor{0x86d0}, module_config);
        auto features = ResolveFeatureSet(module_config);
        RegAlloc alloc{block->MaxInstrId(), CopyTestGPRs(),
                       FPRSMask{~((1u << 8) - 1u)}, features};
        RegisterAllocPass::Run(block.get(), &alloc, false, features);
        REQUIRE(alloc.IsLow32CopyCoalesced(bridge.Id()));
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        REQUIRE_NOTHROW(translator.Translate(block.get()));
        context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
    }
}

TEST_CASE("in-place low32 extensions retain their high-bit clearing write") {
    for (const bool keep_parent_live : {false, true}) {
        CAPTURE(keep_parent_live);
        IntrusivePtr<Block> block{new Block(0, Location{0x86e0})};
        auto source = block->LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{0, ValueType::U64});
        block->StoreUniform(Uniform{16, ValueType::U64}, source);
        auto bridge = block->BitExtract(source, Imm{0u}, Imm{32u})
                              .SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(bridge).SetType(ValueType::U64);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        if (keep_parent_live) {
            block->StoreUniform(Uniform{24, ValueType::U64}, source);
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        auto module = address_space.GetDefaultModule();
        const auto features = ResolveFeatureSet(module->GetModuleConfig());
        RegAlloc allocation{block->MaxInstrId(), CopyTestGPRs(),
                            FPRSMask{~((1u << 8) - 1u)}, features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        REQUIRE(allocation.IsLow32CopyCoalesced(bridge.Id()));
        const auto source_reg = allocation.ValueGPR(source).id;
        if (!keep_parent_live) {
            // Supply the final-use handoff accepted by the allocator, with
            // an arbitrary U64 input so the emitter cannot assume clean high
            // bits. Retain conservative liveness through the result's store.
            allocation.MapReference(source.Id(), result.Id());
            for (auto& inst : block->GetInstList()) {
                if (inst.Id() >= bridge.Id()) {
                    auto gprs = allocation.DirtyGPR(inst.Id());
                    auto fprs = allocation.DirtyFPR(inst.Id());
                    gprs.Mark(source_reg);
                    allocation.SetActiveRegs(inst.Id(), gprs, fprs);
                }
            }
        }
        const auto result_reg = allocation.ValueGPR(result).id;
        REQUIRE((source_reg == result_reg) == !keep_parent_live);

        arm64::JitContext context{module, allocation};
        arm64::JitTranslator translator{context};
        REQUIRE_NOTHROW(translator.Translate(block.get()));
        context.Finish();
        const auto expected = "mov w" + std::to_string(result_reg) +
                              ", w" + std::to_string(source_reg);
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        auto* buffer = context.GetMasm().GetBuffer();
        const auto* end = buffer->GetEndAddress<const vixl::aarch64::Instruction*>();
        bool clears_high_bits = false;
        for (const auto* instruction =
                     buffer->GetStartAddress<const vixl::aarch64::Instruction*>();
             instruction < end; instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction);
            clears_high_bits |= disassembler.GetOutput() == expected;
        }
        REQUIRE(clears_high_bits);
    }
}

TEST_CASE("low32 views reuse compatible source and result ownership") {
    auto allocate = [](bool keep_source_live) {
        IntrusivePtr<Block> block{new Block(0, Location{0x8690})};
        auto source = block->LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{0, ValueType::U64});
        auto bridge = block->BitExtract(source, Imm{0u}, Imm{32u})
                              .SetType(ValueType::U32);
        auto result = block->Add(bridge, Operand{Imm{1u}})
                              .SetType(ValueType::U32);
        block->StoreUniform(Uniform{8, ValueType::U32}, result);
        block->StoreUniform(Uniform{16, ValueType::U32}, bridge);
        if (keep_source_live) {
            block->StoreUniform(Uniform{24, ValueType::U64}, source);
        }
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        auto features = FeatureSet{};
        auto alloc = std::make_unique<RegAlloc>(
                block->MaxInstrId(), CopyTestGPRs(),
                FPRSMask{~((1u << 8) - 1u)}, features);
        RegisterAllocPass::Run(block.get(), alloc.get(), false, features);
        return CopyAllocation{
                std::move(block), std::move(alloc), source, bridge, result};
    };

    auto accepted = allocate(true);
    REQUIRE(accepted.alloc->IsLow32CopyCoalesced(accepted.bridge.Id()));
    REQUIRE(accepted.alloc->Low32CopySource(accepted.bridge.Id()) ==
            accepted.source.Id());
    REQUIRE(accepted.alloc->ValueGPR(accepted.bridge).id ==
            accepted.alloc->ValueGPR(accepted.source).id);
    REQUIRE(accepted.alloc->ValueGPR(accepted.wrapper).id !=
            accepted.alloc->ValueGPR(accepted.source).id);

    auto dead_source = allocate(false);
    REQUIRE(dead_source.alloc->IsLow32CopyCoalesced(dead_source.bridge.Id()));
}

TEST_CASE("low32 ownership transfer preserves guest homes across publication") {
    for (const bool overwrite_home : {false, true}) {
        CAPTURE(overwrite_home);
        IntrusivePtr<Block> block{new Block(0, Location{0x86d0})};
        auto replacement = block->GetHostGPR(HostRegIndex(0), Imm{0u})
                                   .SetType(ValueType::U64);
        auto initial = block->LoadImm(Imm{0x12345678u}).SetType(ValueType::U32);
        block->SetHostGPR(initial, HostRegIndex(22), Imm{0u});
        auto source = block->GetHostGPR(HostRegIndex(22), Imm{0u})
                              .SetType(ValueType::U32);
        auto bridge = block->BitExtract(source, Imm{0u}, Imm{32u})
                              .SetType(ValueType::U32);
        if (overwrite_home) {
            block->SetHostGPR(replacement, HostRegIndex(22), Imm{0u});
        }
        block->StoreUniform(Uniform{16, ValueType::U32}, bridge);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        const auto features = FeatureSet{};
        RegAlloc allocation{block->MaxInstrId(), CopyTestGPRs(),
                            FPRSMask{~((1u << 8) - 1u)}, features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        REQUIRE(allocation.IsHostReadCoalesced(source.Id()));
        REQUIRE(allocation.ValueGPR(source).id == 22);
        REQUIRE(allocation.IsLow32CopyCoalesced(bridge.Id()) == !overwrite_home);
        if (overwrite_home) {
            REQUIRE(allocation.ValueGPR(bridge).id != 22);
        }
    }
}

TEST_CASE("low32 recoloring preserves the parent across earlier atomic clobbers") {
    for (const bool atomic : {false, true}) {
        CAPTURE(atomic);
        IntrusivePtr<Block> block{new Block(0, Location{0x86e0})};
        auto source = block->LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{0, ValueType::U64});
        std::array<Value, 4> keep;
        for (unsigned i = 0; i < keep.size(); ++i) {
            keep[i] = block->LoadUniform<TypedValue<ValueType::U64>>(
                    Uniform{32 + i * 8, ValueType::U64});
        }
        Value exchanged;
        if (atomic) {
            auto address = block->LoadUniform<TypedValue<ValueType::U64>>(
                    Uniform{128, ValueType::U64});
            auto input = block->LoadImm(Imm{17u}).SetType(ValueType::U64);
            exchanged = block->AtomicExchange(address, input).SetType(ValueType::U64);
            block->StoreUniform(Uniform{136, ValueType::U64}, exchanged);
        }
        auto bridge = block->BitExtract(source, Imm{0u}, Imm{32u})
                              .SetType(ValueType::U32);
        auto reused_home = block->LoadImm(Imm{7u}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{8, ValueType::U64}, reused_home);
        for (unsigned i = 0; i < keep.size(); ++i) {
            block->StoreUniform(Uniform{160 + i * 8, ValueType::U64}, keep[i]);
        }
        block->StoreUniform(Uniform{16, ValueType::U32}, bridge);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        FeatureSet features{};
        features.jit_scratch_xpool = true;
        RegAlloc allocation{block->MaxInstrId(), CopyTestGPRs(),
                            FPRSMask{~((1u << 8) - 1u)}, features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        REQUIRE(allocation.ValueType(source) == RegAlloc::GPR);
        if (atomic) {
            const auto source_reg = allocation.ValueGPR(source).id;
            REQUIRE((FixedGPRClobbers(*exchanged.Def(), features) &
                     (1u << source_reg)) == 0);
        } else {
            REQUIRE(allocation.IsLow32CopyCoalesced(bridge.Id()));
            REQUIRE(allocation.AllocationId(source) == bridge.Id());
        }
    }
}

TEST_CASE("final-use low32 views transfer or recolor ownership") {
    IntrusivePtr<Block> block{new Block(0, Location{0x86a0})};
    auto source = block->LoadUniform<TypedValue<ValueType::U64>>(
            Uniform{0, ValueType::U64});
    block->StoreUniform(Uniform{16, ValueType::U64}, source);
    auto bridge = block->BitExtract(source, Imm{0u}, Imm{32u})
                          .SetType(ValueType::U32);
    block->StoreUniform(Uniform{8, ValueType::U32}, bridge);
    block->StoreUniform(Uniform{12, ValueType::U32}, bridge);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    auto features = FeatureSet{};
    RegAlloc alloc{block->MaxInstrId(), CopyTestGPRs(),
                   FPRSMask{~((1u << 8) - 1u)}, features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);

    REQUIRE(alloc.IsLow32CopyCoalesced(bridge.Id()));
    REQUIRE(alloc.Low32CopySource(bridge.Id()) == source.Id());
    REQUIRE(alloc.ValueGPR(bridge).id == alloc.ValueGPR(source).id);

    IntrusivePtr<Block> recolor_block{new Block(0, Location{0x86a8})};
    auto recolor_source = recolor_block->LoadUniform<TypedValue<ValueType::U64>>(
            Uniform{0, ValueType::U64});
    auto recolor_bridge = recolor_block->BitExtract(
            recolor_source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    auto reused_home = recolor_block->LoadImm(Imm{7u}).SetType(ValueType::U64);
    recolor_block->StoreUniform(
            Uniform{8, ValueType::U64}, reused_home);
    recolor_block->StoreUniform(
            Uniform{16, ValueType::U32}, recolor_bridge);
    recolor_block->SetTerminal(terminal::ReturnToDispatch{});
    recolor_block->ReIdInstr();

    RegAlloc recolor_alloc{recolor_block->MaxInstrId(), CopyTestGPRs(),
                           FPRSMask{~((1u << 8) - 1u)}, features};
    RegisterAllocPass::Run(
            recolor_block.get(), &recolor_alloc, false, features);
    REQUIRE(recolor_alloc.IsLow32CopyCoalesced(recolor_bridge.Id()));
    REQUIRE(recolor_alloc.AllocationId(recolor_source) == recolor_bridge.Id());
    REQUIRE(recolor_alloc.ValueGPR(recolor_bridge).id ==
            recolor_alloc.ValueGPR(recolor_source).id);
    REQUIRE(recolor_alloc.ValueGPR(reused_home).id !=
            recolor_alloc.ValueGPR(recolor_bridge).id);

    IntrusivePtr<Block> atomic_block{new Block(0, Location{0x86b0})};
    auto address = atomic_block->LoadUniform<TypedValue<ValueType::U64>>(
            Uniform{0, ValueType::U64});
    auto atomic_source = atomic_block->LoadUniform<TypedValue<ValueType::U64>>(
            Uniform{8, ValueType::U64});
    auto atomic_bridge = atomic_block->BitExtract(
            atomic_source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    auto previous = atomic_block->AtomicExchange(address, atomic_bridge)
                            .SetType(ValueType::U32);
    atomic_block->StoreUniform(Uniform{16, ValueType::U32}, previous);
    atomic_block->SetTerminal(terminal::ReturnToDispatch{});
    atomic_block->ReIdInstr();

    RegAlloc atomic_alloc{atomic_block->MaxInstrId(), CopyTestGPRs(),
                          FPRSMask{~((1u << 8) - 1u)}, features};
    RegisterAllocPass::Run(
            atomic_block.get(), &atomic_alloc, false, features);
    REQUIRE(atomic_alloc.IsLow32CopyCoalesced(atomic_bridge.Id()));
    REQUIRE(atomic_alloc.AllocationId(atomic_source) == atomic_bridge.Id());

    IntrusivePtr<Block> host_block{new Block(0, Location{0x86b8})};
    auto host_source = host_block->GetHostGPR(HostRegIndex(6), Imm{0u})
                               .SetType(ValueType::U64);
    auto host_bridge = host_block->BitExtract(
            host_source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    auto host_reuse = host_block->LoadImm(Imm{7u}).SetType(ValueType::U64);
    host_block->StoreUniform(Uniform{8, ValueType::U64}, host_reuse);
    host_block->StoreUniform(Uniform{16, ValueType::U32}, host_bridge);
    host_block->SetTerminal(terminal::ReturnToDispatch{});
    host_block->ReIdInstr();

    RegAlloc host_alloc{host_block->MaxInstrId(), CopyTestGPRs(),
                        FPRSMask{~((1u << 8) - 1u)}, features};
    RegisterAllocPass::Run(host_block.get(), &host_alloc, false, features);
    REQUIRE_FALSE(host_alloc.IsLow32CopyCoalesced(host_bridge.Id()));
}
