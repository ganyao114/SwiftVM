#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <array>
#include <vector>
#include <string>
#include <stdexcept>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
using swift::u32;
using swift::u64;

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

TEST_CASE("saturating add subtract emit ARM64 dword qword aliases and spills") {
    u32 fixtures{};
    for (u32 bits : {32u, 64u}) for (bool sign : {false, true})
    for (bool subtract : {false, true}) for (u32 layout : {0u, 1u, 2u}) {
        CAPTURE(bits, sign, subtract, layout); ++fixtures;
        IntrusivePtr<Block> block{new Block(0, Location{0x8840})};
        std::vector<Value> live;
        if (layout == 2) for (u32 index = 0; index < 24; ++index)
            live.push_back(block->LoadUniform(Uniform{256 + index * 16, ValueType::V128}).SetType(ValueType::V128));
        auto input = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto right = !layout ? input : block->LoadUniform(Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto result = (subtract ? block->VecSatSub(input, right, Imm{bits}, Imm{u32(sign)}) :
                                 block->VecSatAdd(input, right, Imm{bits}, Imm{u32(sign)})).SetType(ValueType::V128);
        for (u32 index = 0; index < live.size(); ++index)
            block->StoreUniform(Uniform{1024 + index * 16, ValueType::V128}, live[index]);
        if (layout) {
            block->StoreUniform(Uniform{64, ValueType::V128}, input);
            block->StoreUniform(Uniform{80, ValueType::V128}, right);
        }
        block->StoreUniform(Uniform{32, ValueType::V128}, result);
        block->SetTerminal(terminal::ReturnToDispatch{}); block->ReIdInstr();
        FeatureSet features{}; features.ra_spill_evict = false;
        RegAlloc alloc{block->MaxInstrId(), CopyTestGPRs(), FPRSMask{~((1u << 8) - 1u)}, features};
        REQUIRE_NOTHROW(RegisterAllocPass::Run(block.get(), &alloc, false, features));
        if (!layout) {
            REQUIRE(alloc.ValueType(input) == RegAlloc::FPR);
            REQUIRE(alloc.ValueType(result) == RegAlloc::FPR);
            alloc.MapReference(input.Id(), result.Id());
            const auto shared_reg = alloc.ValueFPR(input).id;
            for (auto& inst : block->GetInstList()) if (inst.Id() >= result.Id()) {
                auto fprs = alloc.DirtyFPR(inst.Id()); fprs.Mark(shared_reg);
                auto gprs = alloc.DirtyGPR(inst.Id()); alloc.SetActiveRegs(inst.Id(), gprs, fprs);
            }
        }
        if (layout == 2) {
            REQUIRE(alloc.ValueType(input) == RegAlloc::MEM);
            REQUIRE(alloc.ValueType(right) == RegAlloc::MEM);
            REQUIRE(alloc.ValueType(result) == RegAlloc::MEM);
            const auto output_slot = alloc.ValueMem(result).offset;
            for (auto source : {input, right}) {
                const auto source_slot = alloc.ValueMem(source).offset;
                REQUIRE((source_slot + 1 < output_slot || output_slot + 1 < source_slot));
            }
            REQUIRE(alloc.DirtyFPR(result.Id()).GetClearCount() >= ScratchBudget(*result.Def(), features).fpr + 3);
        }
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true, .backend_isa = kArm64};
        AddressSpace space{config}; ModuleConfig module_config{};
        auto module = space.MapModule(0x8840, 0x8850, module_config);
        arm64::JitContext context{module, alloc}; arm64::JitTranslator translator{context};
        REQUIRE_NOTHROW(translator.Translate(block.get())); context.Finish();
        vixl::aarch64::Decoder decoder; vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        const std::string mnemonic = std::string(sign ? "sq" : "uq") + (subtract ? "sub " : "add ");
        const std::string lane_format = bits == 32 ? ".4s" : ".2d";
        auto* buffer = context.GetMasm().GetBuffer();
        const auto* end = buffer->GetEndAddress<const vixl::aarch64::Instruction*>();
        u32 saturations{};
        for (const auto* instruction = buffer->GetStartAddress<const vixl::aarch64::Instruction*>();
             instruction < end; instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction); const std::string text = disassembler.GetOutput();
            if (text.find(mnemonic) != 0) continue;
            ++saturations;
            const auto first_comma = text.find(", "), second_comma = text.find(", ", first_comma + 2);
            REQUIRE(first_comma != std::string::npos); REQUIRE(second_comma != std::string::npos);
            const auto destination = text.substr(mnemonic.size(), first_comma - mnemonic.size());
            const auto left = text.substr(first_comma + 2, second_comma - first_comma - 2), right = text.substr(second_comma + 2);
            for (const auto& operand : {destination, left, right}) REQUIRE(operand.find(lane_format) != std::string::npos);
            REQUIRE((destination == left) == !layout);
            REQUIRE((destination == right) == !layout);
            REQUIRE((left == right) == !layout);
        }
        REQUIRE(saturations == 1);
    }
    REQUIRE(fixtures == 24);
}

TEST_CASE("saturating add subtract reject malformed ARM64 whole blocks") {
    u32 fixtures{};
    for (bool subtract : {false, true}) for (bool skipped : {false, true}) for (u32 fault = 0; fault < 17; ++fault) {
        CAPTURE(subtract, skipped, fault); ++fixtures;
        IntrusivePtr<Block> block{new Block(0, Location{0x8860})};
        auto source = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128); Value skip;
        if (skipped) skip = block->Goto(BOOL{block->LoadImm(Imm{swift::u8{1}}).SetType(ValueType::U8)});
        auto result = (subtract ? block->VecSatSub(source, source, Imm{32u}, Imm{0u}) :
                                 block->VecSatAdd(source, source, Imm{32u}, Imm{0u})).SetType(ValueType::V128);
        if (skipped) block->BindLabel(skip);
        block->SetTerminal(terminal::ReturnToDispatch{}); block->ReIdInstr();
        FeatureSet features{};
        RegAlloc alloc{block->MaxInstrId(), CopyTestGPRs(), FPRSMask{~((1u << 8) - 1u)}, features};
        REQUIRE_NOTHROW(RegisterAllocPass::Run(block.get(), &alloc, false, features));
        if (fault < 5) result.Def()->SetArg(2, Imm{u64(fault == 0 ? 0 : fault == 1 ? 24 : fault == 2 ? UINT64_MAX :
                                                     fault == 3 ? 0x100000020ULL : 0x100000040ULL)});
        if (fault >= 5 && fault < 9) result.Def()->SetArg(3, Imm{u64(fault == 5 ? 2 : fault == 6 ? UINT64_MAX :
                                                                   fault == 7 ? 0x100000000ULL : 0x100000001ULL)});
        if (fault == 9) result.Def()->SetArg(2, source);
        if (fault == 10) result.Def()->SetArg(3, source);
        if (fault == 11) { result.Def()->SetArg(0, Void{nullptr}); result.Def()->ArgAt(0) = Value{}; }
        if (fault == 12) result.Def()->SetArg(1, Imm{0u});
        if (fault == 13) result.Def()->SetArg(0, source.SetCastType(ValueType::V256));
        if (fault == 14) source.SetType(ValueType::VOID);
        if (fault == 15) source.SetType(ValueType::V256);
        if (fault == 16) result.SetType(ValueType::VOID);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true, .backend_isa = kArm64};
        AddressSpace space{config}; ModuleConfig module_config{};
        auto module = space.MapModule(0x8860, 0x8870, module_config);
        arm64::JitContext context{module, alloc}; arm64::JitTranslator translator{context};
        REQUIRE_THROWS_AS(translator.Translate(block.get()), std::runtime_error);
        REQUIRE(context.CurrentBufferSize() == 0);
        if (fault == 11) result.Def()->ArgAt(0) = Void{nullptr};
    }
    REQUIRE(fixtures == 68);
}

TEST_CASE("unsigned estimates emit native ARM64 under aliases and FPR pressure") {
    // Decode-only verification on the build host; no ARM64 code execution.
    u32 fixtures{};
    for (u32 operation : {0u, 1u}) for (u32 layout : {0u, 1u, 2u}) {
        CAPTURE(operation, layout); ++fixtures;
        IntrusivePtr<Block> block{new Block(0, Location{0x8810})};
        std::vector<Value> live;
        if (layout == 2) for (u32 index = 0; index < 24; ++index)
            live.push_back(block->LoadUniform(Uniform{256 + index * 16, ValueType::V128}).SetType(ValueType::V128));
        auto input = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto result = block->VecUnsignedEstimate(input, Imm{operation}).SetType(ValueType::V128);
        for (u32 index = 0; index < live.size(); ++index)
            block->StoreUniform(Uniform{1024 + index * 16, ValueType::V128}, live[index]);
        if (layout) block->StoreUniform(Uniform{64, ValueType::V128}, input);
        block->StoreUniform(Uniform{32, ValueType::V128}, result);
        block->SetTerminal(terminal::ReturnToDispatch{}); block->ReIdInstr();
        FeatureSet features{}; features.ra_spill_evict = false;
        RegAlloc alloc{block->MaxInstrId(), CopyTestGPRs(), FPRSMask{~((1u << 8) - 1u)}, features};
        REQUIRE_NOTHROW(RegisterAllocPass::Run(block.get(), &alloc, false, features));
        if (!layout) {
            REQUIRE(alloc.ValueType(input) == RegAlloc::FPR);
            REQUIRE(alloc.ValueType(result) == RegAlloc::FPR);
            // Supply the permitted last-use register handoff explicitly.
            alloc.MapReference(input.Id(), result.Id());
            const auto shared_reg = alloc.ValueFPR(result).id;
            for (auto& inst : block->GetInstList()) if (inst.Id() >= result.Id()) {
                auto fprs = alloc.DirtyFPR(inst.Id()); fprs.Mark(shared_reg);
                auto gprs = alloc.DirtyGPR(inst.Id());
                alloc.SetActiveRegs(inst.Id(), gprs, fprs);
            }
            REQUIRE(alloc.ValueFPR(input).id == shared_reg);
        }
        if (layout == 2) {
            REQUIRE(alloc.ValueType(input) == RegAlloc::MEM);
            REQUIRE(alloc.ValueType(result) == RegAlloc::MEM);
            const auto source_slot = alloc.ValueMem(input).offset, result_slot = alloc.ValueMem(result).offset;
            REQUIRE((source_slot + 1 < result_slot || result_slot + 1 < source_slot));
            REQUIRE(alloc.DirtyFPR(result.Id()).GetClearCount() >= ScratchBudget(*result.Def(), features).fpr + 2);
        }
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true, .backend_isa = kArm64};
        AddressSpace space{config}; ModuleConfig module_config{};
        auto module = space.MapModule(0x8810, 0x8820, module_config);
        arm64::JitContext context{module, alloc}; arm64::JitTranslator translator{context};
        REQUIRE_NOTHROW(translator.Translate(block.get())); context.Finish();
        vixl::aarch64::Decoder decoder; vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        const std::string mnemonic = operation ? "ursqrte " : "urecpe ";
        auto* buffer = context.GetMasm().GetBuffer();
        const auto* end = buffer->GetEndAddress<const vixl::aarch64::Instruction*>();
        u32 estimates{};
        for (const auto* instruction = buffer->GetStartAddress<const vixl::aarch64::Instruction*>();
             instruction < end; instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction); const std::string text = disassembler.GetOutput();
            if (text.find(mnemonic) != 0) continue;
            ++estimates;
            const auto comma = text.find(", "); REQUIRE(comma != std::string::npos);
            const auto destination = text.substr(mnemonic.size(), comma - mnemonic.size());
            const auto source = text.substr(comma + 2);
            REQUIRE(destination.find(".4s") != std::string::npos);
            REQUIRE(source.find(".4s") != std::string::npos);
            REQUIRE((destination == source) == !layout);
        }
        REQUIRE(estimates == 1);
    }
    REQUIRE(fixtures == 6);
}

TEST_CASE("unsigned estimates reject noncanonical ARM64 IR before emission") {
    // Allocate valid IR first, then corrupt it. The translator's complete
    // block validation must reject even a malformed instruction skipped by
    // a local jump, before consulting stale allocation metadata.
    u32 fixtures{};
    for (bool skipped : {false, true}) for (u32 corruption = 0; corruption < 15; ++corruption) {
        CAPTURE(skipped, corruption); ++fixtures;
        IntrusivePtr<Block> block{new Block(0, Location{0x8820})};
        auto source = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        Value skip;
        if (skipped) skip = block->Goto(BOOL{block->LoadImm(Imm{swift::u8{1}}).SetType(ValueType::U8)});
        auto result = block->VecUnsignedEstimate(source, Imm{0u}).SetType(ValueType::V128);
        if (skipped) block->BindLabel(skip);
        block->SetTerminal(terminal::ReturnToDispatch{}); block->ReIdInstr();
        FeatureSet features{};
        RegAlloc alloc{block->MaxInstrId(), CopyTestGPRs(), FPRSMask{~((1u << 8) - 1u)}, features};
        REQUIRE_NOTHROW(RegisterAllocPass::Run(block.get(), &alloc, false, features));
        if (corruption < 4) result.Def()->SetArg(1, Imm{u64(corruption == 0 ? 2ull : corruption == 1 ? UINT64_MAX :
                                                         corruption == 2 ? 0x100000000ull : 0x100000001ull)});
        if (corruption == 4) result.Def()->SetArg(0, source.SetCastType(ValueType::V64));
        if (corruption == 5) source.SetType(ValueType::V64);
        if (corruption == 6) result.Def()->SetArg(0, source.SetCastType(ValueType::U64));
        if (corruption == 7) source.SetType(ValueType::VOID);
        if (corruption == 8) result.Def()->SetArg(0, source.SetCastType(ValueType::V256));
        if (corruption == 9) source.SetType(ValueType::V256);
        if (corruption == 10) result.SetType(ValueType::V64);
        if (corruption == 11) result.SetType(ValueType::VOID);
        if (corruption == 12) result.Def()->SetArg(0, Imm{0u});
        if (corruption == 13) result.Def()->SetArg(1, source);
        if (corruption == 14) {
            result.Def()->SetArg(0, Void{nullptr});
            result.Def()->ArgAt(0) = Value{};
        }
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true, .backend_isa = kArm64};
        AddressSpace space{config}; ModuleConfig module_config{};
        auto module = space.MapModule(0x8820, 0x8830, module_config);
        arm64::JitContext context{module, alloc}; arm64::JitTranslator translator{context};
        REQUIRE_THROWS_AS(translator.Translate(block.get()), std::runtime_error);
        if (corruption == 14) result.Def()->ArgAt(0) = Void{nullptr};
        REQUIRE(context.CurrentBufferSize() == 0);
    }
    REQUIRE(fixtures == 30);
}

TEST_CASE("saturating multiply flat-block QC is reserved at its producer") {
    for (unsigned pressure : {1u, 24u}) {
        IntrusivePtr<Block> block{new Block(0, Location{0x86e0})};
        std::vector<Value> live;
        for (unsigned index = 0; index < pressure; ++index)
            live.push_back(block->LoadUniform(Uniform{256 + index * 8, ValueType::U64}).SetType(ValueType::U64));
        auto input = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto producer = block->VecSatMulHigh(input, input, input, Imm{32u}, Imm{18u}).SetType(ValueType::V128);
        for (unsigned index = 0; index < pressure; ++index)
            block->StoreUniform(Uniform{512 + index * 8, ValueType::U64}, live[index]);
        auto qc = block->VecSatMulQC(producer).SetType(ValueType::U8);
        auto repeated = block->VecSatMulQC(producer).SetType(ValueType::U8);
        block->StoreUniform(Uniform{32, ValueType::V128}, producer);
        block->StoreUniform(Uniform{48, ValueType::U8}, qc);
        block->StoreUniform(Uniform{49, ValueType::U8}, repeated);
        block->SetTerminal(terminal::ReturnToDispatch{}); block->ReIdInstr();
        FeatureSet features{};
        RegAlloc alloc{block->MaxInstrId(), CopyTestGPRs(), FPRSMask{~((1u << 8) - 1u)}, features};
        REQUIRE_NOTHROW(RegisterAllocPass::Run(block.get(), &alloc, false, features));
        for (auto output : {qc, repeated}) {
            REQUIRE((alloc.ValueType(output) == RegAlloc::GPR || alloc.ValueType(output) == RegAlloc::MEM));
            for (auto old : live) {
                if (alloc.ValueType(output) == RegAlloc::GPR && alloc.ValueType(old) == RegAlloc::GPR)
                    REQUIRE(alloc.ValueGPR(output).id != alloc.ValueGPR(old).id);
                if (alloc.ValueType(output) == RegAlloc::MEM && alloc.ValueType(old) == RegAlloc::MEM)
                    REQUIRE(alloc.ValueMem(output).offset != alloc.ValueMem(old).offset);
            }
        }
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true, .backend_isa = kArm64};
        AddressSpace space{config};
        ModuleConfig module_config{};
        auto module = space.MapModule(0x86e0, 0x86f0, module_config);
        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        REQUIRE_NOTHROW(translator.Translate(block.get()));
        context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
    }
}

TEST_CASE("saturating shift flat-block QC is reserved at its producer") {
    u32 fixtures = 0;
    for (unsigned pressure : {1u, 24u}) for (u32 mode = 0; mode < 9; ++mode)
    for (u32 bits : {8u, 16u, 32u, 64u}) for (bool q : {false, true}) {
        const bool left = mode < 3;
        if ((!left && (bits == 64 || q)) || (left && bits == 64 && !q)) continue;
        ++fixtures;
        IntrusivePtr<Block> block{new Block(0, Location{0x87e0})};
        std::vector<Value> live;
        for (unsigned index = 0; index < pressure; ++index)
            live.push_back(block->LoadUniform(Uniform{256 + index * 8, ValueType::U64}).SetType(ValueType::U64));
        auto input = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto negative = block->LoadImm(Imm{u64{UINT64_MAX}}).SetType(ValueType::U64);
        block->SaveFlags(negative, Flags::NZCV);
        const u32 lanes = (left ? q ? 128u : 64u : 64u) / bits;
        auto producer = block->VecSatShiftImm(input, Imm{bits}, Imm{left ? bits - 1 : bits},
                                              Imm{(lanes << 4) | mode}).SetType(ValueType::V128);
        for (unsigned index = 0; index < pressure; ++index)
            block->StoreUniform(Uniform{512 + index * 8, ValueType::U64}, live[index]);
        auto qc = block->VecSatShiftQC(producer).SetType(ValueType::U8);
        auto repeated = block->VecSatShiftQC(producer).SetType(ValueType::U8);
        block->StoreUniform(Uniform{32, ValueType::V128}, producer);
        block->StoreUniform(Uniform{48, ValueType::U8}, qc);
        block->StoreUniform(Uniform{49, ValueType::U8}, repeated);
        block->StoreUniform(Uniform{56, ValueType::U8}, block->TestFlags(Flags::Negate).SetType(ValueType::U8));
        block->SetTerminal(terminal::ReturnToDispatch{}); block->ReIdInstr();
        FeatureSet features{};
        RegAlloc alloc{block->MaxInstrId(), CopyTestGPRs(), FPRSMask{~((1u << 8) - 1u)}, features};
        REQUIRE_NOTHROW(RegisterAllocPass::Run(block.get(), &alloc, false, features));
        for (auto output : {qc, repeated}) {
            REQUIRE((alloc.ValueType(output) == RegAlloc::GPR || alloc.ValueType(output) == RegAlloc::MEM));
            for (auto old : live) {
                if (alloc.ValueType(output) == RegAlloc::GPR && alloc.ValueType(old) == RegAlloc::GPR)
                    REQUIRE(alloc.ValueGPR(output).id != alloc.ValueGPR(old).id);
                if (alloc.ValueType(output) == RegAlloc::MEM && alloc.ValueType(old) == RegAlloc::MEM)
                    REQUIRE(alloc.ValueMem(output).offset != alloc.ValueMem(old).offset);
            }
        }
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true, .backend_isa = kArm64};
        AddressSpace space{config}; ModuleConfig module_config{};
        auto module = space.MapModule(0x87e0, 0x87f0, module_config);
        arm64::JitContext context{module, alloc}; arm64::JitTranslator translator{context};
        REQUIRE_NOTHROW(translator.Translate(block.get())); context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
    }
    REQUIRE(fixtures == 78);
}

TEST_CASE("saturating shift spilled input and result retain independent FPR leases") {
    // Emission-only ARM64 verification: the generated instructions are never
    // executed on this host. Live old vectors naturally force both the later
    // source and result into MEM; avoid hand-editing allocator spill maps.
    u32 fixtures = 0;
    for (u32 pressure : {8u, 24u}) for (u32 mode = 0; mode < 9; ++mode)
    for (u32 bits : {8u, 16u, 32u, 64u}) for (bool q : {false, true}) {
        const bool left = mode < 3;
        if ((!left && (bits == 64 || q)) || (left && bits == 64 && !q)) continue;
        CAPTURE(pressure, mode, bits, q); ++fixtures;
        IntrusivePtr<Block> block{new Block(0, Location{0x87c0})};
        std::vector<Value> live;
        for (u32 index = 0; index < pressure; ++index)
            live.push_back(block->LoadUniform(Uniform{256 + index * 16, ValueType::V128}).SetType(ValueType::V128));
        auto input = block->LoadUniform(Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        const u32 lanes = (left ? q ? 128u : 64u : 64u) / bits;
        auto producer = block->VecSatShiftImm(input, Imm{bits}, Imm{left ? bits - 1 : 1u},
                                             Imm{(lanes << 4) | mode}).SetType(ValueType::V128);
        for (u32 index = 0; index < pressure; ++index)
            block->StoreUniform(Uniform{1024 + index * 16, ValueType::V128}, live[index]);
        // Retain the source through every old vector use, preventing the
        // source and destination spill intervals from sharing one slot pair.
        block->StoreUniform(Uniform{80, ValueType::V128}, input);
        auto qc = block->VecSatShiftQC(producer).SetType(ValueType::U8);
        auto repeated = block->VecSatShiftQC(producer).SetType(ValueType::U8);
        block->StoreUniform(Uniform{32, ValueType::V128}, producer);
        block->StoreUniform(Uniform{64, ValueType::U8}, qc);
        block->StoreUniform(Uniform{65, ValueType::U8}, repeated);
        block->SetTerminal(terminal::ReturnToDispatch{}); block->ReIdInstr();
        FeatureSet features{};
        // Use the deterministic baseline scan: earlier live vectors retain
        // their homes and the later source/result spill under FPR pressure.
        features.ra_spill_evict = false;
        RegAlloc alloc{block->MaxInstrId(), CopyTestGPRs(), FPRSMask{~((1u << 8) - 1u)}, features};
        REQUIRE_NOTHROW(RegisterAllocPass::Run(block.get(), &alloc, false, features));
        REQUIRE(alloc.ValueType(input) == RegAlloc::MEM);
        REQUIRE(alloc.ValueType(producer) == RegAlloc::MEM);
        const u32 input_slot = alloc.ValueMem(input).offset, result_slot = alloc.ValueMem(producer).offset;
        REQUIRE((input_slot & 1) == 0);
        REQUIRE((result_slot & 1) == 0);
        REQUIRE((input_slot + 1 < kMaxSpillSlots && result_slot + 1 < kMaxSpillSlots));
        REQUIRE((input_slot + 1 < result_slot || result_slot + 1 < input_slot));
        // ScratchBudget describes ordinary temporaries only. The verifier
        // additionally prices the two distinct source/result spill leases.
        const auto need = ScratchBudget(*producer.Def(), features);
        REQUIRE(alloc.DirtyFPR(producer.Id()).GetClearCount() >= need.fpr + 2);
        REQUIRE(alloc.DirtyGPR(producer.Id()).GetClearCount() >= need.gpr);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true, .backend_isa = kArm64};
        AddressSpace space{config}; ModuleConfig module_config{};
        auto module = space.MapModule(0x87c0, 0x87d0, module_config);
        arm64::JitContext context{module, alloc}; arm64::JitTranslator translator{context};
        REQUIRE_NOTHROW(translator.Translate(block.get())); context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
    }
    REQUIRE(fixtures == 78);
}

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
