#include "tests/support/rewrite_fixture.h"
using swift::runtime::test::RewriteFixture;
#include "runtime/backend/arm64/compile/inputs.h"
#include "runtime/compile/workspace.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/arm64/compile/compiler.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/actions.h"
#include "runtime/backend/arm64/compile/lower_predicates.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/svm_config.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/passes/passes.h"
#include "runtime/frontend/x86/decoder.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

struct LogicalFlagEmission {
    std::vector<std::string> instructions;
    bool extract_tied{};
};

struct DirectMemory final : swift::x86::MemoryInterface {
    bool Read(void* dest, size_t addr, size_t size) override {
        return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
    }
    bool Write(void* src, size_t addr, size_t size) override {
        return std::memcpy(reinterpret_cast<void*>(addr), src, size);
    }
    void* GetPointer(void* src) override { return src; }
};

LogicalFlagEmission EmitLogicalFlagDirect(bool observe_result) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .global_opts = Optimizations::All,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();

    IntrusivePtr<Block> block{new Block(0, Location{0x89c0})};
    auto source = block->GetHostGPR(ValueType::U32, HostRegIndex(22), Imm{0u});
    auto extract = block->BitExtract(ValueType::U8, source, Imm{0u}, Imm{8u});
    auto result = block->Or(ValueType::U8, extract, Operand{Imm{swift::u64{0}}});
    block->SaveFlags(result, Flags::Negate | Flags::Zero | Flags::Parity);
    if (observe_result) {
        block->StoreUniform(Uniform{0, ValueType::U8}, result);
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
    const bool extract_tied =
            alloc.ValueGPR(source).id == alloc.ValueGPR(extract).id;

    ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
    arm64::JitContext context{module, alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<
            const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<
            const vixl::aarch64::Instruction*>();
    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::vector<std::string> instructions;
    for (auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        instructions.emplace_back(disassembler.GetOutput());
    }
    return {std::move(instructions), extract_tied};
}

bool Contains(const std::vector<std::string>& instructions,
              std::string_view value) {
    return std::ranges::any_of(instructions, [&](const auto& instruction) {
        return instruction.find(value) != std::string::npos;
    });
}

std::size_t Count(const std::vector<std::string>& instructions,
                  std::string_view value) {
    return std::ranges::count_if(instructions, [&](const auto& instruction) {
        return instruction.find(value) != std::string::npos;
    });
}

std::size_t CountMnemonic(const std::vector<std::string>& instructions,
                         std::string_view mnemonic) {
    return std::ranges::count_if(instructions, [&](const auto& instruction) {
        return instruction.starts_with(mnemonic);
    });
}

std::vector<std::string> Disassemble(arm64::JitContext& context) {
    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::vector<std::string> instructions;
    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<
            const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<
            const vixl::aarch64::Instruction*>();
    for (auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        instructions.emplace_back(disassembler.GetOutput());
    }
    return instructions;
}

std::vector<std::string> BeforeRequestTail(
        arm64::JitContext& context, const std::vector<std::string>& instructions) {
    using namespace vixl::aarch64;
    const auto* begin = context.GetMasm().GetBuffer()->GetStartAddress<const swift::u8*>();
    const auto* end = begin + instructions.size() * sizeof(swift::u32);
    size_t cold = instructions.size();
    unsigned polls{};
    for (size_t i = 0; i + 1 < instructions.size(); ++i) {
        if (!instructions[i].starts_with("ldar x") || !instructions[i + 1].starts_with("cbz x"))
            continue;
        const auto* check = reinterpret_cast<const Instruction*>(begin + (i + 1) * sizeof(swift::u32));
        const auto* resume = reinterpret_cast<const swift::u8*>(check->GetImmPCOffsetTarget());
        REQUIRE(resume > begin + (i + 1) * sizeof(swift::u32));
        REQUIRE(resume <= end);
        const auto* exit = reinterpret_cast<const Instruction*>(resume - sizeof(swift::u32));
        REQUIRE(exit->IsUncondBranchImm());
        const auto* tail = reinterpret_cast<const swift::u8*>(exit->GetImmPCOffsetTarget());
        REQUIRE(tail >= resume);
        REQUIRE(tail < end);
        cold = std::min(cold, static_cast<size_t>(tail - begin) / sizeof(swift::u32));
        ++polls;
    }
    REQUIRE(polls == 2);
    const std::vector<std::string> hot{instructions.begin(), instructions.begin() + cold};
    REQUIRE(Count(hot, "ldar x") == 2);
    return hot;
}

std::vector<std::string> EmitNarrowArithmetic(OpCode op,
                                              bool observe_result,
                                              Flags flags,
                                              bool branch_only) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();

    IntrusivePtr<Block> block{new Block(0, Location{0x8a00})};
    auto source = block->LoadUniform<TypedValue<ValueType::U8>>(
            Uniform{0, ValueType::U8});
    Value result;
    if (op == OpCode::Add) {
        result = block->Add(ValueType::U8, source, Operand{Imm{3u}});
    } else if (op == OpCode::Sub) {
        result = block->Sub(ValueType::U8, source, Operand{Imm{3u}});
    } else {
        result = block->Neg(ValueType::U8, source);
    }
    if (branch_only) {
        block->AppendInst(OpCode::BranchOnlyFlags, result, flags);
    } else {
        block->SaveFlags(result, flags);
    }
    if (observe_result) {
        block->StoreUniform(Uniform{8, ValueType::U8}, result);
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

std::vector<std::string> EmitPackedCVAFClear(
        bool exact_clear, OpCode producer = OpCode::And) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();

    IntrusivePtr<Block> block{new Block(0, Location{0x8a20})};
    auto source = block->LoadUniform<TypedValue<ValueType::U32>>(
            Uniform{0, ValueType::U32});
    Value result;
    if (producer == OpCode::Add) {
        result = block->Add(ValueType::U32, source, Operand{Imm{1u}});
    } else if (producer == OpCode::Xor) {
        result = block->Xor(ValueType::U32, source, Operand{source});
    } else {
        result = block->And(ValueType::U32, source, Operand{source});
    }
    const auto clear = exact_clear
            ? Flags::CV | Flags::AuxiliaryCarry
            : Flags::CV;
    block->ClearFlags(clear);
    block->SaveFlags(result, Flags::NZ | Flags::Parity);
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

std::vector<std::string> EmitParityOnlyLogicalBeforeSelect() {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .global_opts = Optimizations::All,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();

    IntrusivePtr<Block> block{new Block(0, Location{0x89e0})};
    auto source = block->LoadUniform<TypedValue<ValueType::U64>>(Uniform{0, ValueType::U64});
    auto parity = block->Or(ValueType::U64, source, Operand{Imm{swift::u64{0}}});
    block->SaveFlags(parity, Flags::Parity);
    auto condition = block->LoadImm(ValueType::U8, Imm{swift::u64{1}});
    auto true_value = block->LoadImm(ValueType::U64, Imm{swift::u64{2}});
    auto false_value = block->LoadImm(ValueType::U64, Imm{swift::u64{3}});
    auto selected = block->Select(ValueType::U64, condition, true_value, false_value);
    block->StoreUniform(Uniform{8, ValueType::U64}, selected);
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

}  // namespace

TEST_CASE("dead narrow logical identities preserve NZ and parity publication") {
    const auto emission = EmitLogicalFlagDirect(false);
    REQUIRE(emission.extract_tied);
    if (GetSvmConfig().flags_regs) {
        REQUIRE(Count(emission.instructions, "lsl #24") == 1);
        REQUIRE_FALSE(Contains(emission.instructions, "sxtb"));
    } else {
        REQUIRE(Count(emission.instructions, "lsl #24") == 0);
        REQUIRE(Count(emission.instructions, "sxtb") == 1);
        REQUIRE(Contains(emission.instructions, "tst "));
    }
}

TEST_CASE("observed narrow logical identities keep their result") {
    const auto emission = EmitLogicalFlagDirect(true);
    REQUIRE_FALSE(emission.extract_tied);
}

TEST_CASE("dead narrow branch-only arithmetic omits result truncation") {
    for (const auto op : {OpCode::Add, OpCode::Sub, OpCode::Neg}) {
        CAPTURE(op);
        const auto dead =
                EmitNarrowArithmetic(op, false, Flags::NZCV, true);
        REQUIRE(Count(dead, "lsr ") == 0);

        const auto observed =
                EmitNarrowArithmetic(op, true, Flags::NZCV, true);
        REQUIRE(Count(observed, "lsr ") == 1);
    }
}

TEST_CASE("dead narrow NZCV producers omit parity result retention") {
    for (const auto op : {OpCode::Add, OpCode::Sub, OpCode::Neg}) {
        CAPTURE(op);
        const auto nzcv =
                EmitNarrowArithmetic(op, false, Flags::NZCV, false);
        REQUIRE(Count(nzcv, "lsr ") == 0);

        const auto parity = EmitNarrowArithmetic(
                op, false, Flags::NZCV | Flags::Parity, false);
        REQUIRE(Count(parity, "lsr ") == 1);
    }
}

TEST_CASE("parity-only logical publication leaves NZCV clean") {
    const auto instructions = EmitParityOnlyLogicalBeforeSelect();
    REQUIRE_FALSE(Contains(instructions, "tst "));
    REQUIRE(Contains(instructions, "cmp "));
    REQUIRE(Contains(instructions, "csel "));
}

TEST_CASE("logical NZ publication absorbs an adjacent CVAF clear") {
    const auto exact = EmitPackedCVAFClear(true);
    REQUIRE_FALSE(Contains(exact, "#26, #6"));
    if (GetSvmConfig().flags_regs) {
        REQUIRE(Contains(exact, "mrs x26, nzcv"));
        REQUIRE(Count(exact, "ubfx ") == 0);
        REQUIRE_FALSE(Contains(exact, "bfc x26, #26, #4"));
    } else {
        REQUIRE(Count(exact, "mrs ") == 1);
        REQUIRE(Count(exact, "ubfx ") == 1);
        REQUIRE(Contains(exact, "#30, #2"));
        REQUIRE(Contains(exact, "bfi x26"));
        REQUIRE(Contains(exact, "bfxil x26"));
        REQUIRE(Contains(exact, "bfc x26, #26, #4"));
    }

    const auto partial = EmitPackedCVAFClear(false);
    REQUIRE_FALSE(Contains(partial, "#26, #6"));

    const auto zero = EmitPackedCVAFClear(true, OpCode::Xor);
    REQUIRE_FALSE(Contains(zero, "orr x26"));
    if (GetSvmConfig().flags_regs) {
        REQUIRE(Contains(zero, "mov x26, #0x40000000"));
        REQUIRE_FALSE(Contains(zero, "bfc x26"));
        REQUIRE_FALSE(Contains(zero, "bfi x26"));
    } else {
        REQUIRE(Contains(zero, "wzr, wzr"));
        REQUIRE(Count(zero, "mrs ") == 1);
        REQUIRE(Contains(zero, "#30, #2"));
        REQUIRE(Contains(zero, "bfi x26"));
        REQUIRE(Contains(zero, "bfxil x26"));
        REQUIRE(Contains(zero, "bfc x26, #26, #4"));
    }

    const auto arithmetic = EmitPackedCVAFClear(true, OpCode::Add);
    if (GetSvmConfig().flags_regs) {
        REQUIRE(Contains(arithmetic, "uxtb w26"));
        REQUIRE_FALSE(Contains(arithmetic, "bfc x26, #26, #4"));
    } else {
        REQUIRE(Contains(arithmetic, "adds w"));
        REQUIRE(Count(arithmetic, "mrs ") == 1);
        REQUIRE(Contains(arithmetic, "#30, #2"));
        REQUIRE(Contains(arithmetic, "bfi x26"));
        REQUIRE(Contains(arithmetic, "bfxil x26"));
        REQUIRE(Contains(arithmetic, "bfc x26, #26, #4"));
    }
}

TEST_CASE("narrow register self tests skip the redundant AND") {
    DirectMemory memory;

    struct Case {
        std::array<swift::u8, 4> code;
        std::size_t expected_ands;
    };
    for (const auto& test : {
                 Case{{0x84, 0xc0, 0xf4, 0x00}, 0},
                 Case{{0x84, 0xe4, 0xf4, 0x00}, 1},
                 Case{{0x66, 0x85, 0xc0, 0xf4}, 0},
         }) {
        const auto& code = test.code;
        Block block{0, Location{reinterpret_cast<swift::VAddr>(code.data())}};
        swift::runtime::compile::IrBuilder assembler{&block};
        swift::x86::X64Decoder decoder{
                reinterpret_cast<swift::VAddr>(code.data()), &memory, &assembler,
                true, swift::x86::Arm64Features::None, false, false, FeatureSet{}};
        decoder.Decode();

        std::size_t and_count = 0;
        std::size_t or_count = 0;
        for (auto& inst : block.GetInstList()) {
            and_count += inst.GetOp() == OpCode::And;
            or_count += inst.GetOp() == OpCode::Or;
        }
        CAPTURE(static_cast<unsigned>(code[0]),
                static_cast<unsigned>(code[1]),
                static_cast<unsigned>(code[2]));
        REQUIRE(and_count == test.expected_ands);
        REQUIRE(or_count == 1);
    }
}

TEST_CASE("dead-edge fixed-home self tests branch without a logical result") {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .global_opts = Optimizations::All,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x8a80})};
    auto base = block->GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
    auto loaded = block->LoadMemory(ValueType::U64, Operand{base});
    auto* publication = block->AppendInst(OpCode::SetHostGPR, loaded, HostRegIndex(22), Imm{0u});
    block->AdvancePC(Imm{3u});
    auto left = block->BitCast(ValueType::U64, loaded);
    auto right = block->BitCast(ValueType::U64, loaded);
    auto tested = block->And(ValueType::U64, left, Operand{right});
    block->ClearFlags(Flags::Carry | Flags::Overflow | Flags::AuxiliaryCarry);
    block->SaveFlags(tested, Flags::Negate | Flags::Zero | Flags::Parity);
    block->AdvancePC(Imm{3u});
    block->BranchOnlyEdges();
    auto condition = block->LocalCondSet(ValueType::U8, Cond::EQ);
    block->SetTerminal(terminal::If{
            condition,
            terminal::LinkBlock{Location{0x8a90}},
            terminal::LinkBlock{Location{0x8aa0}},
    });

    FeatureSet features{};
    swift::runtime::compile::EliminateFlags(RewriteFixture{block.get(), true, nullptr}.Get(), features);
    block->ReIdInstr();
    AllocationFixture alloc{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(),
                            features};
    ::swift::runtime::compile::AllocateBlock(
            block.get(),
            &alloc,
            ::swift::runtime::backend::arm64::PrepareAllocationInputs(block.get(), alloc, features),
            false);
    alloc.MapRegister(loaded.Id(), HostGPR{22});
    alloc.MarkHostWriteCoalesced(publication->Id());
    // The independent builder never materializes a value to prove eligibility.
    compile::CompileWorkspace predicate_workspace;
    compile::BlockAnalysisIndex analysis;
    analysis.Build(block.get());
    REQUIRE(arm64::LowerIntegerBranch(block.get(),
                                      analysis,
                                      predicate_workspace,
                                      alloc,
                                      features,
                                      alloc.PrepareFixedGPRHomes()));
    alloc.MapRegister(loaded.Id(), HostGPR{23});
    REQUIRE_FALSE(arm64::LowerIntegerBranch(block.get(),
                                            analysis,
                                            predicate_workspace,
                                            alloc,
                                            features,
                                            alloc.PrepareFixedGPRHomes()));
    alloc.MapMemSpill(loaded.Id(), SpillSlot{2});
    REQUIRE_FALSE(arm64::LowerIntegerBranch(block.get(),
                                            analysis,
                                            predicate_workspace,
                                            alloc,
                                            features,
                                            alloc.PrepareFixedGPRHomes()));
    alloc.MapRegister(loaded.Id(), HostGPR{22});
    ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                              alloc);
    const auto plan = alloc.Actions().ForBlock(block.get());
    REQUIRE(plan);
    const auto* branch = alloc.Actions().IntegerBranch(block.get());
    REQUIRE(branch);
    REQUIRE(branch->ZeroTarget() == 22);
    REQUIRE(branch->condition == condition.Id());
    REQUIRE_FALSE(branch->IsNarrow());
    REQUIRE(alloc.Actions().MatchesAllocation(alloc));
    arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    const auto instructions = Disassemble(context);
    REQUIRE(Count(instructions, "ldr x22, [x22]") == 1);
    REQUIRE(Count(instructions, "cbnz x22") +
                    Count(instructions, "cbz x22") ==
            1);
    REQUIRE_FALSE(Contains(instructions, "ands "));
    REQUIRE_FALSE(Contains(instructions, "tst "));
}

TEST_CASE("compact FP compare preserves carry polarity across audited moves") {
    for (const auto& code : {
                 std::array<swift::u8, 24>{
                         0x66, 0x0f, 0x2f, 0xc1,
                         0xf2, 0x0f, 0x10, 0x15, 0x04, 0x00, 0x00, 0x00,
                         0x77, 0x01, 0xf4, 0xf4,
                 },
                 std::array<swift::u8, 24>{
                         0x66, 0x0f, 0x2f, 0xc1,
                         0x66, 0x0f, 0x28, 0xd0,
                         0x76, 0x01, 0xf4, 0xf4,
                 },
         }) {
        DirectMemory memory;
        const auto address = reinterpret_cast<swift::VAddr>(code.data());
        IntrusivePtr<Block> block{new Block(0, Location{address})};
        swift::runtime::compile::IrBuilder assembler{block.get()};
        FeatureSet features{};
        features.flags_fcmp_fuse = true;
        features.flags_fcmp_compact = true;
        const auto arm64_features =
                swift::x86::Arm64Features::AXFlag |
                swift::x86::Arm64Features::FlagM;
        swift::x86::X64Decoder decoder{
                address, &memory, &assembler, true,
                arm64_features, false, false, features};
        decoder.Decode();

        REQUIRE(std::count_if(block->GetInstList().begin(), block->GetInstList().end(),
                              [](const Inst& inst) {
                                  return inst.GetOp() == OpCode::FCmpCondSet;
                              }) == 1);
        REQUIRE(std::none_of(block->GetInstList().begin(), block->GetInstList().end(),
                             [](const Inst& inst) {
                                 return inst.GetOp() == OpCode::CondSet;
                             }));

        block->ReIdInstr();
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .global_opts = Optimizations::All,
                .arm64_features = arm64_features,
        };
        AddressSpace address_space{config};
        auto module = address_space.GetDefaultModule();
        AllocationFixture alloc{block->MaxInstrId(),
                                address_space.GetTrampolines().GetGPRRegs(),
                                address_space.GetTrampolines().GetFPRRegs(),
                                features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &alloc,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), alloc, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, alloc);
        arm64::JitContext context{module, alloc, alloc.Actions()};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();

        const auto instructions = Disassemble(context);
        // AXFLAG publishes the compact compare, then CFINV restores guest CF.
        // JA/JBE therefore need both CF and ZF after the inversion; the
        // lowering intentionally materializes the compound predicate with a
        // CSET plus CSEL/CSINC instead of using the pre-CFINV HI/LS mapping.
        REQUIRE(Count(instructions, "cset") == 2);
        REQUIRE(Count(instructions, "axflag") == 1);
        REQUIRE(Count(instructions, "cfinv") == 1);
        REQUIRE(Count(instructions, "csel") + Count(instructions, "csinc") == 1);
    }
}

TEST_CASE("dead-edge FP compares branch on raw host flags") {
    struct Case {
        swift::u8 opcode;
        std::string_view condition;
        std::string_view inverse;
    };
    for (const auto& test : {
                 Case{0x72, "b.lt", "b.ge"},
                 Case{0x73, "b.ge", "b.lt"},
                 Case{0x77, "b.gt", "b.le"},
                 Case{0x76, "b.le", "b.gt"},
                 Case{0x7a, "b.vs", "b.vc"},
                 Case{0x7b, "b.vc", "b.vs"},
         }) {
        const std::array<swift::u8, 16> code{
                0x66, 0x0f, 0x2f, 0xc1,
                test.opcode, 0x04,
                0x39, 0xc0,
                0xf4, 0x90,
                0x39, 0xc9,
                0xf4,
        };
        DirectMemory memory;
        const auto address = reinterpret_cast<swift::VAddr>(code.data());
        IntrusivePtr<Block> block{new Block(0, Location{address})};
        swift::runtime::compile::IrBuilder assembler{block.get()};
        FeatureSet features{};
        const auto arm64_features = Arm64Features::AXFlag | Arm64Features::FlagM;
        swift::x86::X64Decoder decoder{
                address, &memory, &assembler, true, arm64_features, false, false,
                features};
        decoder.Decode();

        auto count_op = [&](OpCode op) {
            std::size_t count{};
            for (const auto& inst : block->GetInstList()) {
                count += inst.GetOp() == op;
            }
            return count;
        };
        CAPTURE(test.opcode);
        REQUIRE(count_op(OpCode::BranchOnlyEdges) == 1);
        REQUIRE(count_op(OpCode::PublishFCmpFlags) == 1);
        REQUIRE(count_op(OpCode::InvertCarry) == 1);

        swift::runtime::compile::EliminateFlags(RewriteFixture{block.get(), true, nullptr}.Get(), features);
        REQUIRE(count_op(OpCode::BranchOnlyEdges) == 0);
        REQUIRE(count_op(OpCode::PublishFCmpFlags) == 0);
        REQUIRE(count_op(OpCode::InvertCarry) == 0);
        REQUIRE(count_op(OpCode::FCmpCondSet) == 1);

        block->ReIdInstr();
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .global_opts = Optimizations::All,
                .arm64_features = arm64_features,
        };
        AddressSpace address_space{config};
        AllocationFixture alloc{block->MaxInstrId(),
                                address_space.GetTrampolines().GetGPRRegs(),
                                address_space.GetTrampolines().GetFPRRegs(),
                                features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &alloc,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), alloc, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                                  alloc);
        arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();

        const auto instructions = Disassemble(context);
        REQUIRE(Count(instructions, "fcmp") == 1);
        REQUIRE(Count(instructions, "cset") == 0);
        REQUIRE(Count(instructions, "axflag") == 0);
        REQUIRE(Count(instructions, "cfinv") == 0);
        REQUIRE((Contains(instructions, test.condition) ||
                 Contains(instructions, test.inverse)));
    }
}

TEST_CASE("dead-edge integer compares branch on raw host flags") {
    struct Case {
        swift::u8 opcode;
        std::string_view condition;
        std::string_view inverse;
    };
    for (const auto& test : {
                 Case{0x74, "b.eq", "b.ne"},
                 Case{0x75, "b.ne", "b.eq"},
                 Case{0x72, "b.lo", "b.hs"},
                 Case{0x73, "b.hs", "b.lo"},
                 Case{0x77, "b.hi", "b.ls"},
                 Case{0x76, "b.ls", "b.hi"},
         }) {
        const std::array<swift::u8, 12> code{
                0x3c, 0x50,
                test.opcode, 0x04,
                0x39, 0xc0,
                0xf4, 0x90,
                0x39, 0xc9,
                0xf4,
        };
        DirectMemory memory;
        const auto address = reinterpret_cast<swift::VAddr>(code.data());
        IntrusivePtr<Block> block{new Block(0, Location{address})};
        swift::runtime::compile::IrBuilder assembler{block.get()};
        FeatureSet features{};
        constexpr auto arm64_features = Arm64Features::FlagM;
        swift::x86::X64Decoder decoder{
                address, &memory, &assembler, true, arm64_features, false, false,
                features};
        decoder.Decode();

        auto count_op = [&](OpCode op) {
            std::size_t count{};
            for (const auto& inst : block->GetInstList()) {
                count += inst.GetOp() == op;
            }
            return count;
        };
        CAPTURE(test.opcode);
        REQUIRE(count_op(OpCode::BranchOnlyEdges) == 1);
        REQUIRE(count_op(OpCode::InvertCarry) == 1);

        swift::runtime::compile::EliminateFlags(RewriteFixture{block.get(), true, nullptr}.Get(), features);
        REQUIRE(count_op(OpCode::BranchOnlyEdges) == 0);
        const bool zero_only = test.opcode == 0x74 || test.opcode == 0x75;
        if (zero_only) {
            auto invert = std::find_if(block->GetInstList().begin(),
                                       block->GetInstList().end(),
                                       [](const Inst& inst) {
                                           return inst.GetOp() == OpCode::InvertCarry;
                                       });
            REQUIRE(invert != block->GetInstList().end());
            block->DestroyInst(&*invert);
        }
        REQUIRE(count_op(OpCode::InvertCarry) == (zero_only ? 0 : 1));
        REQUIRE(block->HasDeadEdgeIntegerBranchProof());

        block->ReIdInstr();
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .global_opts = Optimizations::All,
                .arm64_features = arm64_features,
        };
        AddressSpace address_space{config};
        AllocationFixture alloc{block->MaxInstrId(),
                                address_space.GetTrampolines().GetGPRRegs(),
                                address_space.GetTrampolines().GetFPRRegs(),
                                features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &alloc,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), alloc, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                                  alloc);
        arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();

        const auto instructions = BeforeRequestTail(context, Disassemble(context));
        CAPTURE(instructions);
        REQUIRE(Count(instructions, "subs") == 0);
        REQUIRE(Count(instructions, "uxtb ") == 1);
        REQUIRE(Count(instructions, "cmp ") == 1);
        REQUIRE(CountMnemonic(instructions, "lsl ") == 0);
        REQUIRE(Count(instructions, "ldar x") == 2);
        REQUIRE(Count(instructions, "lsr ") == 0);
        REQUIRE(std::ranges::count_if(instructions, [](const auto& instruction) {
            return instruction.starts_with("cmp ") &&
                   instruction.find("#0x50 (80)") != std::string::npos;
        }) == 1);
        REQUIRE(Count(instructions, "cfinv") == 0);
        REQUIRE(Count(instructions, "cset") == 0);
        REQUIRE(Count(instructions, "mrs") == 0);
        REQUIRE(Count(instructions, "bfi") == 0);
        REQUIRE((Contains(instructions, test.condition) ||
                 Contains(instructions, test.inverse)));
    }
}

TEST_CASE("incoming packed carry-zero branches test the flags carrier") {
    for (const auto opcode : {swift::u8{0x77}, swift::u8{0x76}}) {
        const std::array<swift::u8, 9> code{
                opcode, 0x04,
                0x39, 0xc0,
                0xf4, 0x90,
                0x39, 0xc9,
                0xf4,
        };
        DirectMemory memory;
        const auto address = reinterpret_cast<swift::VAddr>(code.data());
        IntrusivePtr<Block> block{new Block(0, Location{address})};
        swift::runtime::compile::IrBuilder assembler{block.get()};
        FeatureSet features{};
        constexpr auto arm64_features = Arm64Features::FlagM;
        swift::x86::X64Decoder decoder{
                address, &memory, &assembler, true, arm64_features, false, false,
                features};
        decoder.Decode();

        block->ReIdInstr();
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .global_opts = Optimizations::All,
                .arm64_features = arm64_features,
        };
        AddressSpace address_space{config};
        AllocationFixture alloc{block->MaxInstrId(),
                                address_space.GetTrampolines().GetGPRRegs(),
                                address_space.GetTrampolines().GetFPRRegs(),
                                features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &alloc,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), alloc, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                                  alloc);
        arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();

        const auto instructions = BeforeRequestTail(context, Disassemble(context));
        CAPTURE(opcode, instructions);
        if (GetSvmConfig().flags_regs) {
            REQUIRE(Count(instructions, "tst ") == 1);
            REQUIRE(Contains(instructions, "tst x26, #0x60000000"));
            REQUIRE(Count(instructions, "ubfx") == 0);
            REQUIRE(Count(instructions, "cset") == 0);
            REQUIRE((Contains(instructions, "b.eq") || Contains(instructions, "b.ne")));
            REQUIRE(Contains(instructions, opcode == 0x77 ? "b.ne" : "b.eq"));
        } else {
            REQUIRE(Count(instructions, "tst ") == 0);
            REQUIRE(Count(instructions, "ubfx") == 2);
            REQUIRE(Contains(instructions, "w26, #29, #1"));
            REQUIRE(Contains(instructions, "x26, #30, #1"));
            REQUIRE(Count(instructions, "cset") == 1);
            REQUIRE(std::ranges::count_if(instructions, [opcode](const auto& instruction) {
                        return instruction.find("cset ") != std::string::npos &&
                               instruction.ends_with(opcode == 0x77 ? ", eq" : ", ne");
                    }) == 1);
            REQUIRE(Count(instructions, "eor ") == (opcode == 0x77 ? 1 : 0));
            if (opcode == 0x77) {
                REQUIRE(Contains(instructions, "#0x1"));
            }
            REQUIRE(Count(instructions, "cbz ") == 3);
            REQUIRE(Contains(instructions, opcode == 0x77 ? "and w" : "orr w"));
        }
    }
}

TEST_CASE("branch-only narrow equality compares extend the register operand") {
    for (const auto opcode : {swift::u8{0x74}, swift::u8{0x75}}) {
        const std::array<swift::u8, 14> code{
                0x66, 0x44, 0x3b, 0x62, 0x02,
                opcode, 0x04,
                0x39, 0xc0,
                0xf4, 0x90,
                0x39, 0xc9,
                0xf4,
        };
        DirectMemory memory;
        const auto address = reinterpret_cast<swift::VAddr>(code.data());
        IntrusivePtr<Block> block{new Block(0, Location{address})};
        swift::runtime::compile::IrBuilder assembler{block.get()};
        FeatureSet features{};
        constexpr auto arm64_features = Arm64Features::FlagM;
        swift::x86::X64Decoder decoder{
                address, &memory, &assembler, true, arm64_features, false, false,
                features};
        decoder.Decode();

        swift::runtime::compile::EliminateFlags(RewriteFixture{block.get(), true, nullptr}.Get(), features);
        block->ReIdInstr();
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .global_opts = Optimizations::All,
                .arm64_features = arm64_features,
        };
        AddressSpace address_space{config};
        AllocationFixture alloc{block->MaxInstrId(),
                                address_space.GetTrampolines().GetGPRRegs(),
                                address_space.GetTrampolines().GetFPRRegs(),
                                features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &alloc,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), alloc, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                                  alloc);
        arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();

        const auto instructions = BeforeRequestTail(context, Disassemble(context));
        CAPTURE(opcode, instructions);
        REQUIRE(Count(instructions, "cmp ") == 1);
        REQUIRE(Count(instructions, "uxth") == 1);
        REQUIRE(CountMnemonic(instructions, "lsl ") == 0);
        REQUIRE(Count(instructions, "subs") == 0);
    }
}

TEST_CASE("dead-edge proof rejects a call before a dominating flag write", "[flags][fault]") {
    auto decode_marker = [](const std::array<swift::u8, 36>& code) {
        DirectMemory memory;
        const auto address = reinterpret_cast<swift::VAddr>(code.data());
        IntrusivePtr<Block> block{new Block(0, Location{address})};
        swift::runtime::compile::IrBuilder assembler{block.get()};
        FeatureSet features{};
        swift::x86::X64Decoder decoder{
                address, &memory, &assembler, true, Arm64Features::FlagM,
                false, false, features};
        decoder.Decode();
        const auto markers = std::count_if(
                block->GetInstList().begin(), block->GetInstList().end(),
                [](const Inst& inst) {
                    return inst.GetOp() == OpCode::BranchOnlyEdges;
                });
        return std::pair{markers, block->GetGuestCodeDependencies()};
    };

    std::array<swift::u8, 36> code{
            0x3c, 0x50, 0x75, 0x04, 0x39, 0xc0, 0xf4, 0x90,
            0x48, 0x89, 0xee, 0xe8, 0x04, 0x00, 0x00, 0x00,
            0xf4, 0x90, 0x90, 0x90, 0xf3, 0x0f, 0x1e, 0xfa,
            0x48, 0x8b, 0x17, 0x48, 0x89, 0xf0, 0x0f, 0xb6,
            0x0a, 0x84, 0xc9, 0xf4,
    };
    const auto [markers, dependencies] = decode_marker(code);
    // CALL can fault while pushing its return address, and this callee
    // loads memory before TEST overwrites flags. Neither path proves that
    // the incoming flags are unobservable before the full overwrite.
    REQUIRE(markers == 0);
    REQUIRE(dependencies.empty());

    code[33] = 0x74;
    code[34] = 0x00;
    const auto [rejected_markers, rejected_dependencies] = decode_marker(code);
    REQUIRE(rejected_markers == 0);
    REQUIRE(rejected_dependencies.empty());
}

TEST_CASE("dead-edge proof follows one direct jump to a dominating flag write") {
    auto decode_marker = [](const std::array<swift::u8, 20>& code) {
        DirectMemory memory;
        const auto address = reinterpret_cast<swift::VAddr>(code.data());
        IntrusivePtr<Block> block{new Block(0, Location{address})};
        swift::runtime::compile::IrBuilder assembler{block.get()};
        FeatureSet features{};
        swift::x86::X64Decoder decoder{
                address, &memory, &assembler, true, Arm64Features::FlagM,
                false, false, features};
        decoder.Decode();
        const auto markers = std::count_if(
                block->GetInstList().begin(), block->GetInstList().end(),
                [](const Inst& inst) {
                    return inst.GetOp() == OpCode::BranchOnlyEdges;
                });
        return std::pair{markers, block->GetGuestCodeDependencies()};
    };

    std::array<swift::u8, 20> code{
            0x3c, 0x50, 0x75, 0x04, 0xeb, 0x0a, 0x90, 0x90,
            0x48, 0x85, 0xc0, 0xf4, 0x90, 0x90, 0x90, 0x90,
            0x31, 0xc9, 0xf4, 0x90,
    };
    const auto [markers, dependencies] = decode_marker(code);
    REQUIRE(markers == 1);
    REQUIRE(dependencies.size() == 1);
    REQUIRE(dependencies[0].start.Value() ==
            reinterpret_cast<swift::VAddr>(code.data()) + 4);
    REQUIRE(dependencies[0].end.Value() ==
            reinterpret_cast<swift::VAddr>(code.data()) + 18);

    code[16] = 0x74;
    code[17] = 0x00;
    const auto [rejected_markers, rejected_dependencies] = decode_marker(code);
    REQUIRE(rejected_markers == 0);
    REQUIRE(rejected_dependencies.empty());
}

TEST_CASE("dead-edge zero home remains intact until terminal consumption", "[integer-branch]") {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    for (unsigned mode : {0u, 1u, 2u, 3u, 4u, 5u}) {
        CAPTURE(mode);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        AddressSpace space{config}; FeatureSet features{};
        IntrusivePtr<Block> block{new Block(0, Location{0x8ab0})};
        const auto base = block->GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
        const auto loaded = block->LoadMemory(ValueType::U64, Operand{base});
        auto* publication =
                block->AppendInst(OpCode::SetHostGPR, loaded, HostRegIndex(22), Imm{0u});
        block->AdvancePC(Imm{1u});
        const auto result = block->And(ValueType::U64, loaded, Operand{loaded});
        block->ClearFlags(Flags::Carry | Flags::Overflow | Flags::AuxiliaryCarry);
        block->SaveFlags(result, Flags::Negate | Flags::Zero | Flags::Parity);
        block->AdvancePC(Imm{1u});
        block->BranchOnlyEdges();
        const auto condition = block->LocalCondSet(ValueType::U8, Cond::EQ);
        const auto intruder = block->LoadImm(ValueType::U64, Imm{swift::u64{7}});
        if (mode == 3 || mode == 5) {
            const auto changed = block->Add(ValueType::U64, loaded, Operand{Imm{1u}});
            auto* save = block->AppendInst(OpCode::SaveFlags, changed, Flags::NZCV);
            if (mode == 5) {
                compile::RewriteContext{block.get(), compilation_workspace, nullptr, false}.Change(
                        block.get(), changed.Def(), [&](Inst& target) {
                            block->RemoveInst(&target);
                            block->InsertBefore(&target, condition.Def());
                        });
                block->RemoveInst(save);
                block->InsertBefore(save, condition.Def());
            }
        }
        if (mode == 4) {
            const auto observed = block->LoadMemory(ValueType::U64, Operand{base});
            block->StoreUniform(Uniform{8, ValueType::U64}, observed);
        }
        if (mode == 2) block->SetHostGPR(intruder, HostRegIndex(22), Imm{0u});
        else block->StoreUniform(Uniform{0, ValueType::U64}, intruder);
        block->SetTerminal(terminal::If{condition, terminal::LinkBlock{Location{0x8ac0}},
                                      terminal::LinkBlock{Location{0x8ad0}}});
        swift::runtime::compile::EliminateFlags(RewriteFixture{block.get(), true, nullptr}.Get(), features); block->ReIdInstr();
        REQUIRE(block->HasDeadEdgeIntegerBranchProof());
        AllocationFixture allocation{block->MaxInstrId(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        allocation.MapRegister(loaded.Id(), HostGPR{22});
        allocation.MarkHostWriteCoalesced(publication->Id());
        allocation.MapRegister(intruder.Id(), HostGPR{static_cast<swift::u16>(mode == 1 ? 22 : 23)});
        compile::CompileWorkspace predicate_workspace;
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        const auto direct = arm64::LowerIntegerBranch(block.get(),
                                                      analysis,
                                                      predicate_workspace,
                                                      allocation,
                                                      features,
                                                      allocation.PrepareFixedGPRHomes());
        if (mode == 0) REQUIRE(direct);
        else REQUIRE_FALSE(direct);
        // The ordinary-writer cell proves the pure builder without imposing an
        // invalid manual overlap on the allocator's final target replay.
        if (mode == 1 || mode >= 3) continue;
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(space.GetDefaultModule(),
                                                                  allocation);
        const auto frozen = allocation.Actions().ForBlock(block.get());
        REQUIRE(frozen);
        if (mode == 0)
            REQUIRE(allocation.Actions().IntegerBranch(block.get()));
        else
            REQUIRE_FALSE(allocation.Actions().IntegerBranch(block.get()));
        arm64::JitContext context{space.GetDefaultModule(), allocation, allocation.Actions()};
        arm64::JitTranslator translator{context}; translator.Translate(block.get()); context.Finish();
        const auto instructions = Disassemble(context);
        const auto zero_branches = Count(instructions, "cbnz x22") + Count(instructions, "cbz x22");
        REQUIRE(zero_branches == (mode == 0 ? 1 : 0));
        if (mode == 2) REQUIRE((Contains(instructions, "b.eq") || Contains(instructions, "b.ne")));
    }
}

TEST_CASE("dead-edge CBZ requires a complete 32 or 64 bit home", "[integer-branch]") {
    for (auto type : {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64}) {
        CAPTURE(type);
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        AddressSpace space{config}; FeatureSet features{};
        IntrusivePtr<Block> block{new Block(0, Location{0x8ae0})};
        const auto base = block->GetHostGPR(ValueType::U64, HostRegIndex(22), Imm{0u});
        const auto loaded = block->LoadMemory(type, Operand{base});
        auto* publication =
                block->AppendInst(OpCode::SetHostGPR, loaded, HostRegIndex(22), Imm{0u});
        block->AdvancePC(Imm{1u});
        const auto result = block->And(type, loaded, Operand{loaded});
        block->ClearFlags(Flags::Carry | Flags::Overflow | Flags::AuxiliaryCarry);
        block->SaveFlags(result, Flags::Negate | Flags::Zero | Flags::Parity);
        block->AdvancePC(Imm{1u});
        block->BranchOnlyEdges();
        const auto condition = block->LocalCondSet(ValueType::U8, Cond::EQ);
        block->SetTerminal(terminal::If{condition,
                                        terminal::LinkBlock{Location{0x8af0}},
                                        terminal::LinkBlock{Location{0x8b00}}});
        swift::runtime::compile::EliminateFlags(RewriteFixture{block.get(), true, nullptr}.Get(), features); block->ReIdInstr();
        REQUIRE(block->HasDeadEdgeIntegerBranchProof());
        AllocationFixture allocation{block->MaxInstrId(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &allocation,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), allocation, features),
                false);
        allocation.MapRegister(loaded.Id(), HostGPR{22});
        allocation.MarkHostWriteCoalesced(publication->Id());
        compile::CompileWorkspace predicate_workspace;
        compile::BlockAnalysisIndex analysis;
        analysis.Build(block.get());
        const auto direct = arm64::LowerIntegerBranch(block.get(),
                                                      analysis,
                                                      predicate_workspace,
                                                      allocation,
                                                      features,
                                                      allocation.PrepareFixedGPRHomes());
        REQUIRE(direct.has_value() == (ir::GetValueSizeByte(type) >= 4));
    }
}
TEST_CASE("dead-edge terminal fallback preserves the captured zero relation", "[integer-branch][runtime]") {
#if defined(__aarch64__)
    for (const auto type : {ValueType::U8, ValueType::U16, ValueType::U64}) {
        for (const swift::u64 low : {0ull, 3ull}) {
            CAPTURE(type, low);
            Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                          .has_local_operation = false, .backend_isa = kArm64};
            config.uniform_buffer_size = 64; config.stack_alignment = 16;
            AddressSpace space{config}; auto module = space.GetDefaultModule(); FeatureSet features{};
            constexpr swift::u64 start = 0xfa00, yes = 0xfa10, no = 0xfa20;
            IntrusivePtr<Block> entry{new Block(0, Location{start})};
            const auto width = ir::GetValueSizeByte(type);
            const auto initial = entry->LoadImm(
                    ValueType::U64,
                    Imm{static_cast<swift::u64>(low | (width < 8 ? 1ull << (width * 8) : 0ull))});
            entry->SetHostGPR(initial, HostRegIndex(22), Imm{0u});
            const auto captured = entry->GetHostGPR(type, HostRegIndex(22), Imm{0u});
            entry->SetHostGPR(captured, HostRegIndex(22), Imm{0u});
            entry->AdvancePC(Imm{1u});
            const auto result = entry->And(type, captured, Operand{captured});
            entry->ClearFlags(Flags::Carry | Flags::Overflow | Flags::AuxiliaryCarry);
            entry->SaveFlags(result, Flags::Negate | Flags::Zero | Flags::Parity);
            entry->AdvancePC(Imm{1u});
            entry->BranchOnlyEdges();
            const auto condition = entry->LocalCondSet(ValueType::U8, Cond::EQ);
            const auto changed = entry->LoadImm(ValueType::U64, Imm{swift::u64{7}});
            entry->SetHostGPR(changed, HostRegIndex(22), Imm{0u});
            entry->SetTerminal(terminal::If{condition, terminal::LinkBlock{Location{yes}},
                                          terminal::LinkBlock{Location{no}}});
            swift::runtime::compile::EliminateFlags(RewriteFixture{entry.get(), true, nullptr}.Get(), features);
            REQUIRE(entry->HasDeadEdgeIntegerBranchProof());
            auto publish = [&](Block* block, bool inspect) {
                block->ReIdInstr();
                AllocationFixture allocation{block->MaxInstrId(),
                                             space.GetTrampolines().GetGPRRegs(),
                                             space.GetTrampolines().GetFPRRegs(),
                                             features};
                ::swift::runtime::compile::AllocateBlock(
                        block,
                        &allocation,
                        ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                                block, allocation, features),
                        false);
                ::swift::runtime::backend::arm64::CompleteARM64Allocation(module, allocation);
                if (inspect) REQUIRE_FALSE(allocation.Actions().IntegerBranch(block));
                arm64::JitContext context{module, allocation, allocation.Actions()};
                arm64::JitTranslator translator{context};
                translator.Translate(block); context.Finish();
                const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
                REQUIRE(index != INVALID_CACHE_ID); context.Flush(buffer);
                space.PushCodeCache(block->GetStartLocation(), buffer.exec_data + context.GetCodeOffset(block->GetStartLocation().Value()));
            };
            publish(entry.get(), true);
            for (const auto target : {yes, no}) {
                IntrusivePtr<Block> outcome{new Block(0, Location{target})};
                const auto answer = outcome->LoadImm(
                        ValueType::U64, Imm{target == yes ? swift::u64{111} : swift::u64{222}});
                outcome->StoreUniform(Uniform{0, ValueType::U64}, answer);
                outcome->SetTerminal(terminal::ReturnToHost{}); publish(outcome.get(), false);
            }
            Runtime runtime{&space}; runtime.SetLocation(start);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            swift::u64 answer{}; std::memcpy(&answer, runtime.GetUniformBuffer().data(), sizeof(answer));
            REQUIRE(answer == (low == 0 ? 111 : 222));
        }
    }
#endif
}
