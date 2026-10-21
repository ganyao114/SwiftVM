#include "support/register_alloc_test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstring>
#include "runtime/backend/runtime.h"
#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/alloc/gpr_block_plan.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/div128_narrowing_analysis.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/frontend/x86/x87.h"
#include "runtime/ir/opts/register_alloc_pass.h"

TEST_CASE("Div128 uniform proofs stop at control flow joins", "[div128-cfg]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    struct Sample {
        unsigned branch;
        swift::u64 low, divisor, high, quotient, remainder;
        bool native;
    };
    const Sample samples[] = {
        {0, 100, 7, 0, 14, 2, true},
        {1, 3, 5, 1, 3689348814741910323ull, 4, false},
        {2, 3, 5, 1, 3689348814741910323ull, 4, false},
        {3, 100, 7, 0, 14, 2, true},
    };
    for (const auto& sample : samples) {
        INFO("branch=" << sample.branch);
        constexpr swift::u32 low_offset = 512, divisor_offset = 520, high_offset = 528;
        constexpr swift::u32 quotient_offset = 544, remainder_offset = 552;
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 1024;
        config.stack_alignment = 16;
        config.enable_asm_interp = false;
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block(0, Location{0x2500 + sample.branch * 4})};
        const auto low = block->LoadUniform(Uniform{low_offset, ValueType::U64}).SetType(ValueType::U64);
        const auto divisor = block->LoadUniform(Uniform{divisor_offset, ValueType::U64}).SetType(ValueType::U64);
        auto high = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
        if (sample.branch == 3) {
            block->StoreUniform(Uniform{high_offset, ValueType::U64}, high);
            high = block->LoadUniform(Uniform{high_offset, ValueType::U64}).SetType(ValueType::U64);
        } else if (sample.branch != 0) {
            // The taken branch skips the zero store, preserving incoming high=1.
            const auto condition = block->LoadImm(Imm{swift::u32{sample.branch == 1 ? 1u : 0u}})
                    .SetType(ValueType::U32);
            const auto join = sample.branch == 1 ? block->Goto(condition) : block->NotGoto(condition);
            block->StoreUniform(Uniform{high_offset, ValueType::U64}, high);
            block->BindLabel(join);
            high = block->LoadUniform(Uniform{high_offset, ValueType::U64}).SetType(ValueType::U64);
        }
        const auto quotient = block->Div128(high, low, divisor, Imm{swift::u64{0}}).SetType(ValueType::U64);
        const auto remainder = block->Div128Remainder(quotient).SetType(ValueType::U64);
        block->StoreUniform(Uniform{quotient_offset, ValueType::U64}, quotient);
        block->StoreUniform(Uniform{remainder_offset, ValueType::U64}, remainder);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        const auto features = FeatureSet{};
        RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        allocation.CompleteGPRAllocation(module);
        CHECK(arm64::Div128NarrowingAnalysis{block.get()}.CanLowerNatively(quotient.Def()) == sample.native);
        const auto plan = allocation.GPRPlanFor(block.get());
        REQUIRE(plan);
        const auto* scratch = plan->pinned->ScratchFor(quotient.Def());
        REQUIRE(scratch);
        CHECK(scratch->div128_native == sample.native);
        arm64::JitContext context{module, allocation};
        arm64::JitTranslator translator{context};
        REQUIRE(translator.Translate(block.get()));
        context.Finish();
        auto& masm = context.GetMasm();
        const auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
        const auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        unsigned divisions = 0;
        for (auto* instruction = first; instruction < last; instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction);
            const std::string line = disassembler.GetOutput();
            divisions += line.starts_with("udiv ") || line.starts_with("sdiv ");
        }
        CHECK(divisions == (sample.native ? 1u : 0u));
#if defined(__aarch64__)
        const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID);
        context.Flush(buffer);
        REQUIRE(module->Push(block.get()));
        (void)module->PublishLinkTarget(block->GetStartLocation(), buffer.exec_data, buffer.exec_data);
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        Runtime runtime{&space};
        auto uniform = runtime.GetUniformBuffer();
        std::memcpy(uniform.data() + low_offset, &sample.low, sizeof(sample.low));
        std::memcpy(uniform.data() + divisor_offset, &sample.divisor, sizeof(sample.divisor));
        std::memcpy(uniform.data() + high_offset, &sample.high, sizeof(sample.high));
        runtime.SetLocation(block->GetStartLocation().Value());
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        swift::u64 actual_quotient{}, actual_remainder{};
        std::memcpy(&actual_quotient, uniform.data() + quotient_offset, sizeof(actual_quotient));
        std::memcpy(&actual_remainder, uniform.data() + remainder_offset, sizeof(actual_remainder));
        CHECK(actual_quotient == sample.quotient);
        CHECK(actual_remainder == sample.remainder);
#endif
    }
}

TEST_CASE("Div128 scratch proofs require local uniform membership", "[scratch-facts][div128-cfg]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    for (bool sign : {false, true}) for (bool alias : {false, true}) {
        CAPTURE(sign, alias);
        IntrusivePtr<Block> parent{new Block(0, Location{0x2510})};
        IntrusivePtr<Block> block{new Block(0, Location{0x2514})};
        const auto low = block->LoadUniform(Uniform{512, ValueType::U64}).SetType(ValueType::U64);
        const auto divisor = block->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
        const auto source = sign ? block->AsrImm(low, Imm{swift::u32{63}}).SetType(ValueType::U64) :
                block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{528, ValueType::U64}, source);
        auto local = block->LoadUniform(Uniform{528, ValueType::U64}).SetType(ValueType::U64);
        auto incoming = parent->LoadUniform(Uniform{528, ValueType::U64}).SetType(ValueType::U64);
        auto* local_load = local.Def();
        auto* incoming_load = incoming.Def();
        if (alias) {
            local = block->BitCast(local).SetType(ValueType::U64);
            incoming = block->BitCast(incoming).SetType(ValueType::U64);
        }
        const auto local_div = block->Div128(local, low, divisor, Imm{swift::u64{sign}}).SetType(ValueType::U64);
        const auto incoming_div = block->Div128(incoming, low, divisor, Imm{swift::u64{sign}}).SetType(ValueType::U64);
        const auto pure_div = block->Div128(source, low, divisor, Imm{swift::u64{sign}}).SetType(ValueType::U64);
        block->ReIdInstr();
        incoming_load->SetId(local_load->Id());
        const arm64::Div128NarrowingAnalysis proof{block.get()};
        CHECK(proof.CanLowerNatively(local_div.Def()));
        CHECK_FALSE(proof.CanLowerNatively(incoming_div.Def()));
        const GPRSMask pool{};
        const FeatureSet features{};
        CHECK_FALSE(arm64::PrepareInstructionScratch(*local_div.Def(), pool, features).div128_native);
        CHECK_FALSE(arm64::PrepareInstructionScratch(*incoming_div.Def(), pool, features).div128_native);
        CHECK(arm64::PrepareInstructionScratch(*pure_div.Def(), pool, features).div128_native);
        CHECK(arm64::PrepareInstructionScratchWithBlock(*local_div.Def(), pool, features, block.get()).div128_native);
        CHECK_FALSE(arm64::PrepareInstructionScratchWithBlock(*incoming_div.Def(), pool, features, block.get()).div128_native);
    }
}

TEST_CASE("Div128 uniform scratch proofs stop at partial stores and barriers", "[scratch-facts][div128-cfg]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    for (unsigned boundary = 0; boundary < 3; ++boundary) {
        CAPTURE(boundary);
        IntrusivePtr<Block> block{new Block(0, Location{0x2518})};
        const auto low = block->LoadImm(Imm{swift::u64{100}}).SetType(ValueType::U64);
        const auto divisor = block->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
        const auto zero = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{528, ValueType::U64}, zero);
        if (boundary == 1) block->StoreUniform(Uniform{532, ValueType::U32}, Value{divisor}.SetCastType(ValueType::U32));
        if (boundary == 2) block->AppendInst(OpCode::UniformBarrier);
        const auto high = block->LoadUniform(Uniform{528, ValueType::U64}).SetType(ValueType::U64);
        const auto quotient = block->Div128(high, low, divisor, Imm{swift::u64{0}}).SetType(ValueType::U64);
        block->ReIdInstr();
        CHECK(arm64::Div128NarrowingAnalysis{block.get()}.CanLowerNatively(quotient.Def()) == (boundary == 0));
    }
}

TEST_CASE("prepared scratch preserves fixed and audit exclusions", "[scratch-facts][regression]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    constexpr swift::u32 x11 = 1u << 11, x12 = 1u << 12, x13 = 1u << 13;
    constexpr swift::u32 ip0 = 1u << 16, ip1 = 1u << 17;
    for (bool requested_xpool : {false, true}) for (int missing : {-1, 0, 9, 10, 12}) {
        const bool xpool = requested_xpool || GetSvmConfig().x86_pin_ext >= 3;
        CAPTURE(requested_xpool, xpool, missing);
        GPRSMask pool{(1u << 14) - 1};
        if (missing >= 0) pool.Clear(missing);
        FeatureSet features{};
        features.jit_scratch_xpool = requested_xpool;
        IntrusivePtr<Block> block{new Block(0, Location{0x251a})};
        const auto scalar = block->LoadImm(Imm{swift::u64{1}}).SetType(ValueType::U64);
        const auto vector = block->LoadUniform(Uniform{512, ValueType::V128}).SetType(ValueType::V128);
        const auto add = block->Add(scalar, Operand{scalar}).SetType(ValueType::U64);
        const auto atomic = block->CompareAndSwap(scalar, scalar, scalar).SetType(ValueType::U64);
        const auto fp = block->VecFAdd(vector, vector, Imm{swift::u32{32}}).SetType(ValueType::V128);
        const auto x87 = block->X87Op(scalar, Imm{static_cast<swift::u64>(swift::x86::X87Action::Compare)}, scalar)
                .SetType(ValueType::U64);
        auto* xchg = block->AppendInst(OpCode::XchgBarrier, Imm{swift::u32{3}});
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        RegAlloc allocation{block->MaxInstrId(), pool, FPRSMask{}, features};
        const auto plan = arm64::FrozenGPRPlan::SealWithSpillClobbers(
                allocation, {block.get(), {}, {}}, features);
        const bool scratch_only = GetSvmConfig().x86_pin_ext >= 2 && !xpool &&
                missing != 0 && missing != 12;
        const swift::u32 profile = xpool && GetSvmConfig().exec_prof ? ip0 | ip1 : 0;
        const bool planned_scratch_only = GetSvmConfig().x86_pin_ext >= 2 && !xpool && missing != 0;
        const bool alu_lease = GetSvmConfig().x86_pin_ext >= 3 &&
                missing != 0 && missing != 9 && missing != 10;
        struct Expected {
            const Inst* inst;
            swift::u32 xpool_clobbers, scratch_clobbers, published_home;
            bool alu{};
        };
        const swift::u32 published_home = GetSvmConfig().static_regs ? 1u << 20 : 0;
        const Expected expected[] = {
            {add.Def(), 0, 0, 0, true},
            {atomic.Def(), x11 | x12 | x13, x11 | x12 | x13, 0},
            {fp.Def(), x13, x13, 0},
            {x87.Def(), ip0 | x12 | x13, 0, 0},
            {xchg, 0, 0, published_home},
        };
        for (const auto& row : expected) {
            const auto* frozen = plan->ScratchFor(row.inst);
            REQUIRE(frozen);
            const auto manual = arm64::PrepareInstructionScratch(*row.inst, pool, features);
            // RegAlloc restores x12/x13 reservations without XPOOL. Manual
            // preparation receives the supplied mask without normalization.
            for (const auto [facts, scratch] : {
                    std::pair{frozen, planned_scratch_only}, std::pair{&manual, scratch_only}}) {
                const swift::u32 fixed = profile | row.published_home |
                        (xpool ? row.xpool_clobbers : (scratch ? row.scratch_clobbers : 0));
                const swift::u32 audit = profile | row.published_home | (xpool ? row.xpool_clobbers : 0);
                const swift::u32 terminal = scratch ? x13 | (GetSvmConfig().flags_regs ? 0 : x12) : 0;
                CHECK(facts->fixed_clobbers == fixed);
                CHECK(facts->audit_clobbers == audit);
                CHECK(facts->leases == ((terminal & ~fixed) | (row.alu && alu_lease ? 1u << 10 : 0)));
            }
        }
    }
}

TEST_CASE("unit scratch admission rejects changed feature policy", "[scratch-facts][pin-plan][regression]") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                  .has_local_operation = false, .backend_isa = kArm64};
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<Block> block{new Block(0, Location{0x251c})};
    const auto value = block->LoadImm(Imm{swift::u64{42}}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, value);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    const FeatureSet features{};
    RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                        space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &allocation, false, features);
    allocation.CompleteGPRAllocation(module);
    const auto plan = allocation.GPRPlanFor(block.get());
    REQUIRE(plan);
    REQUIRE(plan->pinned->UsesUnitScratchInputs());
    REQUIRE(plan->pinned->MatchesScratchInputs(allocation.GetGprs(), features));
    REQUIRE(plan->region->MatchesUnit(block.get(), nullptr, allocation, config, features));
    for (auto field : {&FeatureSet::jit_scratch_xpool, &FeatureSet::scratch_precise,
                       &FeatureSet::fpr_scratch_precise, &FeatureSet::sse_nan_coldpath}) {
        auto changed = features;
        changed.*field = !(changed.*field);
        CHECK_FALSE(plan->pinned->MatchesScratchInputs(allocation.GetGprs(), changed));
        CHECK_FALSE(plan->region->MatchesUnit(block.get(), nullptr, allocation, config, changed));
    }
    CHECK(plan->pinned->MatchesScratchInputs(allocation.GetGprs(), features));
}

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

struct SpillForwardingBlock {
    IntrusivePtr<Block> block;
    Value arriving;
};

enum class WidthSpillShape {
    FinalUse,
    MultiUse,
    DeferredPublication,
};

SpillForwardingBlock MakeSpillForwardingBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x2400})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto first = block->Add(arriving, Operand{shortest});
    const auto second = block->Add(first, Operand{middle});
    const auto total = block->Add(second, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillWidthTransferBlock(
        WidthSpillShape shape = WidthSpillShape::FinalUse) {
    IntrusivePtr<Block> block{new Block(0, Location{0x2420})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}})
                                  .SetType(ValueType::U32);
    const auto extended = block->ZeroExtend32To64(arriving)
                                  .SetType(ValueType::U64);
    Value total;
    if (shape == WidthSpillShape::DeferredPublication) {
        block->SetHostGPR(extended, HostRegIndex(22), Imm{0u});
        const auto first = block->Add(shortest, Operand{middle});
        total = block->Add(first, Operand{longest});
    } else {
        if (shape == WidthSpillShape::MultiUse) {
            block->StoreUniform(Uniform{8, ValueType::U32}, arriving);
        }
        const auto first = block->Add(extended, Operand{shortest});
        const auto second = block->Add(first, Operand{middle});
        total = block->Add(second, Operand{longest});
    }
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillFlagsOnlyBlock(OpCode consumer) {
    IntrusivePtr<Block> block{new Block(0, Location{0x2430})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    block->AppendInst(consumer, arriving, Flags::NZCV);
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillAddressBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x2438})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto address = block->GetOperand(
            Operand{arriving, Imm{swift::u64{16}}, OperandPlus})
                                 .SetType(ValueType::U64);
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, address);
    block->StoreUniform(Uniform{8, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillMemoryAddressBlock(bool multi_use,
                                                 bool spill_result = false) {
    IntrusivePtr<Block> block{new Block(0, Location{0x243a})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto loaded = block->LoadMemory(Operand{arriving})
                                .SetType(ValueType::U64);
    if (spill_result) {
        block->StoreUniform(Uniform{8, ValueType::U64}, loaded);
    } else {
        block->SetHostGPR(loaded, HostRegIndex(22), Imm{0u});
    }
    if (multi_use) {
        block->StoreUniform(Uniform{16, ValueType::U64}, arriving);
    }
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillMemoryAddressNoRegionBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x243e})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto loaded = block->LoadMemory(Operand{arriving})
                                .SetType(ValueType::U64);
    block->AppendInst(OpCode::TestNotZero, loaded);
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillMemoryOwnedAddressBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x243f})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto base = block->LoadImm(Imm{swift::u64{4}});
    const auto address = block->GetOperand(
            Operand{base, Imm{swift::u64{16}}, OperandPlus})
                                 .SetType(ValueType::U64);
    const auto loaded = block->LoadMemory(Operand{address})
                                .SetType(ValueType::U64);
    block->StoreUniform(Uniform{8, ValueType::U64}, loaded);
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), address};
}

SpillForwardingBlock MakeSpillMemoryStoreAddressBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x243d})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto stored = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    block->StoreMemory(Operand{arriving}, stored);
    const auto first = block->Add(stored, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillMemoryValueBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x243b})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    block->StoreMemory(Operand{Imm{swift::u64{0x1000}}}, arriving);
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillPublicationBlock(bool fixed_home) {
    IntrusivePtr<Block> block{new Block(0, Location{0x243c})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    if (fixed_home) {
        block->SetHostGPR(arriving, HostRegIndex(15), Imm{0u});
    } else {
        block->StoreUniform(Uniform{0, ValueType::U64}, arriving);
    }
    const auto first = block->Add(shortest, Operand{middle});
    const auto total = block->Add(first, Operand{longest});
    block->StoreUniform(Uniform{8, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeRepeatedSpillUseBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x2440})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto first = block->Add(arriving, Operand{shortest});
    const auto bridge = block->Add(first, Operand{middle});
    const auto second = block->Add(arriving, Operand{longest});
    const auto total = block->Add(second, Operand{bridge});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpillPublicationRegionBlock(
        bool overwritten = false) {
    IntrusivePtr<Block> block{new Block(0, Location{0x2448})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto target = overwritten ? HostRegIndex(22) : HostRegIndex(15);
    block->SetHostGPR(arriving, target, Imm{0u});
    if (overwritten) {
        block->SetHostGPR(shortest, target, Imm{0u});
    }
    const auto first = block->Add(arriving, Operand{shortest});
    const auto second = block->Add(first, Operand{middle});
    const auto total = block->Add(second, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), arriving};
}

SpillForwardingBlock MakeSpilledPublicationWindowBlock(bool faulting = false) {
    IntrusivePtr<Block> block{new Block(0, Location{0x2480})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto published = block->GetOperand(Operand{Imm{swift::u64{0x1234}}})
                                   .SetType(ValueType::U64);
    if (faulting) {
        (void)block->LoadMemory(Operand{Imm{swift::u64{0x1000}}})
                .SetType(ValueType::U64);
    } else {
        block->ClearFlags(Flags::Carry | Flags::Overflow);
    }
    block->AppendInst(OpCode::SetHostGPR, published, HostRegIndex(29), Imm{0u});
    const auto first = block->Add(longest, Operand{middle});
    const auto total = block->Add(first, Operand{shortest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), published};
}

SpillForwardingBlock MakeSpilledBitExtractPublicationBlock() {
    IntrusivePtr<Block> block{new Block(0, Location{0x24a0})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto source = block->LoadImm(Imm{swift::u64{0x123400}});
    const auto published = block->BitExtract(source, Imm{16u}, Imm{8u})
                                   .SetType(ValueType::U64);
    block->AppendInst(OpCode::SetHostGPR, published, HostRegIndex(29), Imm{0u});
    const auto first = block->Add(longest, Operand{middle});
    const auto total = block->Add(first, Operand{shortest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {std::move(block), published};
}

std::vector<std::string> Emit(SpillForwardingBlock input, GPRSMask gprs) {
    FPRSMask fprs{~((1u << 8) - 1u)};
    RegAlloc alloc{input.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    // These fixtures explicitly exercise arriving spill backing; the initial
    // target-home optimization is covered separately by the pin-plan tests.
    alloc.FinishFixedGPRConstraints();
    RegisterAllocTestSupport::RunForSpillEvictTest(input.block.get(), &alloc, false);
    REQUIRE(alloc.ValueType(input.arriving) == RegAlloc::MEM);

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.MapModule(
            LocationDescriptor{0x2400}, LocationDescriptor{0x2500}, ModuleConfig{});
    alloc.CompleteGPRAllocation(module);
    arm64::JitContext context{module, alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(input.block.get());
    context.Finish();

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

void RequireCanonicalSpillRoundTrip(const std::vector<std::string>& emitted,
                                    bool wide_definition = false) {
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    REQUIRE(definition->find(wide_definition ? "mov x" : "mov w") !=
            std::string::npos);
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = wide_definition
            ? definition->substr(4, delimiter - 4)
            : "x" + definition->substr(5, delimiter - 5);
    const auto store = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                       std::string::npos;
            });
    REQUIRE(store != emitted.end());
    const auto address_begin = store->find("[x28");
    const auto address_end = store->find(']', address_begin);
    REQUIRE(address_begin != std::string::npos);
    REQUIRE(address_end != std::string::npos);
    const auto address = store->substr(
            address_begin, address_end - address_begin + 1);
    REQUIRE(std::any_of(
            std::next(store), emitted.end(), [&](const auto& line) {
                return line.find("ldr x") != std::string::npos &&
                       line.find(address) != std::string::npos;
            }));
}

}  // namespace

TEST_CASE("adjacent spilled scalar def-use retains its free scratch") {
    const auto emitted = Emit(MakeSpillForwardingBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(std::next(definition), emitted.end(), [&](const auto& line) {
        return line.find("add ") != std::string::npos &&
               line.find(scratch) != std::string::npos;
    });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(std::next(definition), consumer, [&](const auto& line) {
        return (line.find("str " + scratch + ", [x28") != std::string::npos) ||
               (line.find("ldr " + scratch + ", [x28") != std::string::npos);
    }));
    const auto after_consumer = std::next(consumer);
    REQUIRE(after_consumer != emitted.end());
    REQUIRE(after_consumer->find("str " + scratch + ", [x28") == std::string::npos);
}

TEST_CASE("x18 spill forwarding yields to a live allocated value") {
#if defined(__linux__) && !defined(__ANDROID__)
    const auto emitted = Emit(MakeSpillForwardingBlock(),
                              GPRSMask{~(((1u << 6) - 1u) << 18)});
    REQUIRE(std::ranges::any_of(emitted, [](const auto& line) {
        return line.find("mov x18, #0x1") != std::string::npos;
    }));
    REQUIRE(std::ranges::none_of(emitted, [](const auto& line) {
        return line.find("mov x18, #0x4") != std::string::npos;
    }));
#else
    SUCCEED("x18 spill scratch is Linux-only");
#endif
}

TEST_CASE("final-use width consumers retain spilled inputs") {
    const auto emitted = Emit(MakeSpillWidthTransferBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov w") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch_code = definition->substr(5, delimiter - 5);
    const auto scratch = "x" + scratch_code;
    const auto source_register = "w" + scratch_code;
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("mov w") != std::string::npos &&
                       line.find(source_register) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(std::next(definition), consumer,
                         [&](const auto& line) {
                             return line.find("str " + scratch + ", [x28") !=
                                            std::string::npos ||
                                    line.find("ldr " + scratch + ", [x28") !=
                                            std::string::npos;
                         }));
}

TEST_CASE("multi-use width sources retain canonical spill backing") {
    RequireCanonicalSpillRoundTrip(Emit(
            MakeSpillWidthTransferBlock(WidthSpillShape::MultiUse),
            GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)}));
}

TEST_CASE("deferred pinned width publication retains spill backing") {
    RequireCanonicalSpillRoundTrip(Emit(
            MakeSpillWidthTransferBlock(
                    WidthSpillShape::DeferredPublication),
            GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)}));
}

TEST_CASE("flags-only consumers discard dead spill results") {
    auto check = [](OpCode consumer) {
        const auto emitted = Emit(
                MakeSpillFlagsOnlyBlock(consumer),
                GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
        const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
            return line.find("mov x") != std::string::npos &&
                   line.find("#0x4") != std::string::npos;
        });
        REQUIRE(definition != emitted.end());
        const auto delimiter = definition->find(',');
        REQUIRE(delimiter != std::string::npos);
        const auto scratch = definition->substr(4, delimiter - 4);
        const auto next_value = std::find_if(std::next(definition), emitted.end(),
                                             [](const auto& line) {
            return line.find("add ") != std::string::npos;
        });
        REQUIRE(next_value != emitted.end());
        REQUIRE(std::none_of(std::next(definition), next_value,
                             [&](const auto& line) {
            return line.find("str " + scratch + ", [x28") != std::string::npos ||
                   line.find("ldr " + scratch + ", [x28") != std::string::npos;
        }));
    };

    SECTION("branch-only marker") { check(OpCode::BranchOnlyFlags); }
    SECTION("architectural flags marker") { check(OpCode::SaveFlags); }
}

TEST_CASE("address consumers retain final-use spilled inputs") {
    const auto emitted = Emit(MakeSpillAddressBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(std::next(definition), emitted.end(),
                                       [&](const auto& line) {
        return line.find("add ") != std::string::npos &&
               line.find(scratch) != std::string::npos &&
               line.find("#0x10") != std::string::npos;
    });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(std::next(definition), consumer,
                         [&](const auto& line) {
        return line.find("str " + scratch + ", [x28") != std::string::npos ||
               line.find("ldr " + scratch + ", [x28") != std::string::npos;
    }));
}

TEST_CASE("faulting load with a direct result consumes a pending address") {
    const auto emitted = Emit(MakeSpillMemoryAddressBlock(false),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("ldr x") != std::string::npos &&
                       line.find("[" + scratch) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                               std::string::npos ||
                       line.find("ldr " + scratch + ", [x28") !=
                               std::string::npos;
            }));
}

TEST_CASE("faulting load with a local spill result consumes a pending address") {
    const auto emitted = Emit(MakeSpillMemoryAddressNoRegionBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("ldr x") != std::string::npos &&
                       line.find("[" + scratch) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                               std::string::npos ||
                       line.find("ldr " + scratch + ", [x28") !=
                               std::string::npos;
            }));
}

TEST_CASE("faulting store consumes a final-use pending address") {
    const auto emitted = Emit(MakeSpillMemoryStoreAddressBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("str x") != std::string::npos &&
                       line.find("[" + scratch) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                               std::string::npos ||
                       line.find("ldr " + scratch + ", [x28") !=
                               std::string::npos;
            }));
}

TEST_CASE("faulting store consumes a final-use pending value") {
    const auto emitted = Emit(MakeSpillMemoryValueBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("str " + scratch + ", [") !=
                               std::string::npos &&
                       line.find("[x28") == std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                               std::string::npos ||
                       line.find("ldr " + scratch + ", [x28") !=
                               std::string::npos;
            }));
}

TEST_CASE("faulting memory retains backing for multi-use pending inputs") {
    RequireCanonicalSpillRoundTrip(
            Emit(MakeSpillMemoryAddressBlock(true),
                 GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)}),
            true);
}

TEST_CASE("faulting region-owned loads retain backing without reloading addresses") {
    const auto emitted = Emit(MakeSpillMemoryOwnedAddressBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("add x") != std::string::npos &&
               line.find("#0x10") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto address = definition->substr(4, delimiter - 4);
    const auto consumer = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("ldr x") != std::string::npos &&
                       line.find("[" + address) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(consumer->find("ldr " + address + ",") == std::string::npos);
    REQUIRE(std::count_if(
                    std::next(definition), consumer, [&](const auto& line) {
                        return line.find("str " + address + ", [x28") !=
                               std::string::npos;
                    }) == 1);
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("ldr " + address + ", [x28") !=
                       std::string::npos;
            }));
}

TEST_CASE("publication consumers retain final-use spilled inputs") {
    auto check = [](bool fixed_home) {
        const auto emitted = Emit(
                MakeSpillPublicationBlock(fixed_home),
                GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
        const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
            return line.find("mov x") != std::string::npos &&
                   line.find("#0x4") != std::string::npos;
        });
        REQUIRE(definition != emitted.end());
        const auto delimiter = definition->find(',');
        REQUIRE(delimiter != std::string::npos);
        const auto scratch = definition->substr(4, delimiter - 4);
        const auto consumer = std::find_if(std::next(definition), emitted.end(),
                                           [&](const auto& line) {
            return fixed_home
                    ? line.find("mov x15, " + scratch) != std::string::npos
                    : line.find("str " + scratch + ", [x28") != std::string::npos;
        });
        REQUIRE(consumer != emitted.end());
        REQUIRE(std::none_of(std::next(definition), consumer,
                             [&](const auto& line) {
            return line.find("str " + scratch + ", [x28") != std::string::npos ||
                   line.find("ldr " + scratch + ", [x28") != std::string::npos;
        }));
    };

    SECTION("guest register publication") { check(true); }
    SECTION("uniform memory") { check(false); }
}

TEST_CASE("spilled scalar definitions transfer into multi-use reload regions") {
    const auto emitted = Emit(MakeRepeatedSpillUseBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto resident = definition->substr(4, delimiter - 4);
    REQUIRE(std::count_if(std::next(definition), emitted.end(), [&](const auto& line) {
        return line.find("add ") != std::string::npos &&
               line.find(resident) != std::string::npos;
    }) >= 2);
    REQUIRE(std::none_of(std::next(definition), emitted.end(), [&](const auto& line) {
        return (line.find("str " + resident + ", [x28") != std::string::npos) ||
               (line.find("ldr " + resident + ", [x28") != std::string::npos);
    }));
}

TEST_CASE("pending spill definitions hand off to complete reload regions") {
    const auto emitted = Emit(
            MakeSpillPublicationRegionBlock(),
            GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto definition_register = definition->substr(4, delimiter - 4);
    const auto publication = std::find_if(
            std::next(definition), emitted.end(), [](const auto& line) {
                return line.find("mov x15, x") != std::string::npos;
            });
    REQUIRE(publication != emitted.end());
    const auto source_delimiter = publication->find(',');
    REQUIRE(source_delimiter != std::string::npos);
    const auto resident = publication->substr(source_delimiter + 2);
    const auto consumer = std::find_if(
            std::next(publication), emitted.end(), [&](const auto& line) {
                return line.find("add ") != std::string::npos &&
                       line.find(resident) != std::string::npos;
            });
    REQUIRE(consumer != emitted.end());
    REQUIRE(std::none_of(
            std::next(definition), consumer, [&](const auto& line) {
                return line.find("str " + definition_register + ", [x28") !=
                               std::string::npos ||
                       line.find("ldr " + resident + ", [x28") !=
                               std::string::npos;
            }));
}

TEST_CASE("elided publications retain complete-region spill backing") {
    const auto emitted = Emit(
            MakeSpillPublicationRegionBlock(true),
            GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    const auto definition = std::ranges::find_if(emitted, [](const auto& line) {
        return line.find("mov x") != std::string::npos &&
               line.find("#0x4") != std::string::npos;
    });
    REQUIRE(definition != emitted.end());
    const auto delimiter = definition->find(',');
    REQUIRE(delimiter != std::string::npos);
    const auto scratch = definition->substr(4, delimiter - 4);
    const auto store = std::find_if(
            std::next(definition), emitted.end(), [&](const auto& line) {
                return line.find("str " + scratch + ", [x28") !=
                       std::string::npos;
            });
    REQUIRE(store != emitted.end());
    const auto address_begin = store->find("[x28");
    const auto address_end = store->find(']', address_begin);
    REQUIRE(address_begin != std::string::npos);
    REQUIRE(address_end != std::string::npos);
    const auto address = store->substr(
            address_begin, address_end - address_begin + 1);
    REQUIRE(std::any_of(
            std::next(store), emitted.end(), [&](const auto& line) {
                return line.find("ldr x") != std::string::npos &&
                       line.find(address) != std::string::npos;
            }));
}

TEST_CASE("spilled fixed publications cross a fault-safe instruction window") {
    const auto emitted = Emit(MakeSpilledPublicationWindowBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    REQUIRE(std::ranges::any_of(emitted, [](const auto& line) {
        return line.find("mov x29, #0x1234") != std::string::npos;
    }));
    REQUIRE(std::ranges::none_of(emitted, [](const auto& line) {
        return line.find("str x29, [x28") != std::string::npos ||
               line.find("ldr x29, [x28") != std::string::npos;
    }));
}

TEST_CASE("spilled fixed publications retain canonical state across an untracked fault") {
    const auto emitted = Emit(MakeSpilledPublicationWindowBlock(true),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    REQUIRE(std::ranges::none_of(emitted, [](const auto& line) {
        return line.find("mov x29, #0x1234") != std::string::npos;
    }));
}

TEST_CASE("spilled bit extracts publish directly to a fixed home") {
    const auto emitted = Emit(MakeSpilledBitExtractPublicationBlock(),
                              GPRSMask{~((1u << 5) - 1u) & ~(1u << 18)});
    REQUIRE(std::ranges::any_of(emitted, [](const auto& line) {
        return line.find("ubfx x29") != std::string::npos;
    }));
    REQUIRE(std::ranges::none_of(emitted, [](const auto& line) {
        return line.find("str x29, [x28") != std::string::npos ||
               line.find("ldr x29, [x28") != std::string::npos;
    }));
}

TEST_CASE("spill reload regions stop at local control flow") {
    IntrusivePtr<Block> block{new Block(0, Location{0x2460})};
    const auto longest = block->LoadImm(Imm{swift::u64{1}});
    const auto middle = block->LoadImm(Imm{swift::u64{2}});
    const auto shortest = block->LoadImm(Imm{swift::u64{3}});
    const auto arriving = block->LoadImm(Imm{swift::u64{4}});
    const auto first = block->Add(arriving, Operand{shortest});
    const auto condition = block->LoadImm<BOOL>(Imm{swift::u64{1}});
    const auto skip = block->NotGoto(condition);
    block->Add(first, Operand{middle});
    block->BindLabel(skip);
    const auto second = block->Add(arriving, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, second);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    GPRSMask gprs{~((1u << 5) - 1u) & ~(1u << 18)};
    FPRSMask fprs{~((1u << 8) - 1u)};
    RegAlloc alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocTestSupport::RunForSpillEvictTest(block.get(), &alloc, false);

    REQUIRE(alloc.ValueType(arriving) == RegAlloc::MEM);
    REQUIRE_FALSE(alloc.HasSpillReload(arriving.Id(), first.Id()));
    REQUIRE_FALSE(alloc.HasSpillReload(arriving.Id(), second.Id()));
}

// Proposal only: append to the existing ARM spill_forwarding_test.cpp.
// No production or test source has been changed, compiled, or executed.
TEST_CASE("normal allocation preserves flags-only spilled XOR and surviving uses", "[spill-flags-only][regression]") {
    for (unsigned shape : {0u, 1u, 2u}) {
        Config config{.loc_start = 0, .loc_end = 1ull << 48, .enable_jit = true,
                      .has_local_operation = false, .backend_isa = kArm64};
        config.uniform_buffer_size = 1024;
        config.stack_alignment = 16;
        config.enable_asm_interp = false;
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block(0, Location{0x24b0})};
        // AF is explicitly initialized then preserved; XOR's undefined x86 AF
        // is not used as an oracle for a new AF publication contract.
        block->ClearFlags(Flags::AuxiliaryCarry);
        const auto left = block->LoadUniform(Uniform{64, ValueType::U64}).SetType(ValueType::U64);
        const auto right = block->LoadUniform(Uniform{72, ValueType::U64}).SetType(ValueType::U64);
        std::vector<Value> live;
        for (unsigned i = 0; i < 36; ++i)
            live.push_back(block->LoadUniform(Uniform{128 + i * 8, ValueType::U64}).SetType(ValueType::U64));
        const auto result = block->Xor(left, Operand{right}).SetType(ValueType::U64);
        block->SaveFlags(result, Flags::NZCV | Flags::Parity);
        const auto* flags_consumer = &block->GetInstList().back();
        const Flags tests[] = {Flags::Parity, Flags::AuxiliaryCarry, Flags::Carry,
                               Flags::Overflow, Flags::Zero, Flags::Negate};
        for (unsigned i = 0; i < 6; ++i) {
            const auto bit = block->TestFlags(tests[i]).SetType(ValueType::U8);
            block->StoreUniform(Uniform{i, ValueType::U8}, bit);
        }
        if (shape == 1) block->StoreUniform(Uniform{8, ValueType::U64}, result);
        if (shape == 2) {
            const auto alias = block->BitCast(result).SetType(ValueType::U64);
            block->StoreUniform(Uniform{8, ValueType::U64}, alias);
        }
        auto total = block->Add(left, Operand{right}).SetType(ValueType::U64);
        for (const auto value : live) total = block->Add(total, Operand{value}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{16, ValueType::U64}, total);
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        auto features = FeatureSet{};
        features.ra_spill_evict = false;
        RegAlloc allocation{block->MaxInstrId(), space.GetTrampolines().GetGPRRegs(),
                            space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(block.get(), &allocation, false, features);
        allocation.CompleteGPRAllocation(module);
        INFO("shape=" << shape);
        REQUIRE(allocation.ValueType(result) == RegAlloc::MEM);
        REQUIRE(allocation.GPRLocationAt(result, result.Id(), RegAlloc::GPRView::WholeValue).kind == RegAlloc::MEM);
        const auto actions = allocation.GPRPlanFor(block.get()); REQUIRE(actions);
        [&]<class Actions>(const Actions* selected) {
            if constexpr (requires { selected->flags_only_spills.Discards(flags_consumer, result.Id()); }) {
                CHECK(selected->flags_only_spills.Discards(flags_consumer, result.Id()) == (shape == 0));
                CHECK_FALSE(selected->flags_only_spills.Discards(flags_consumer, left.Id()));
            }
        }(actions.get());

        arm64::JitContext context{module, allocation};
        arm64::JitTranslator translator{context};
        REQUIRE(translator.Translate(block.get()));
        context.Finish();
#if defined(__aarch64__)
        const auto [index, buffer] = module->AllocCodeCache(context.CurrentBufferSize(), context.RequiresRegionTrampoline());
        REQUIRE(index != INVALID_CACHE_ID);
        context.Flush(buffer);
        REQUIRE(module->Push(block.get()));
        (void)module->PublishLinkTarget(block->GetStartLocation(), buffer.exec_data, buffer.exec_data);
        space.PushCodeCache(block->GetStartLocation(), buffer.exec_data);
        for (const auto inputs : {std::pair<swift::u64, swift::u64>{0x55, 0x55},
                                  {0x55, 0x54}, {0, 0x8000000000000000ull}}) {
            Runtime runtime{&space};
            auto uniform = runtime.GetUniformBuffer();
            std::memcpy(uniform.data() + 64, &inputs.first, sizeof(inputs.first));
            std::memcpy(uniform.data() + 72, &inputs.second, sizeof(inputs.second));
            swift::u64 expected_sum = inputs.first + inputs.second;
            for (unsigned i = 0; i < 36; ++i) {
                const swift::u64 v = 100 + i;
                std::memcpy(uniform.data() + 128 + i * 8, &v, sizeof(v));
                expected_sum += v;
            }
            runtime.SetLocation(block->GetStartLocation().Value());
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            const swift::u64 expected = inputs.first ^ inputs.second;
            CHECK(uniform[0] == (std::popcount(static_cast<unsigned>(expected & 0xff)) % 2 == 0));
            CHECK(uniform[1] == 0); // explicit incoming AF preserved
            CHECK(uniform[2] == 0);
            CHECK(uniform[3] == 0);
            CHECK(uniform[4] == (expected == 0));
            CHECK(uniform[5] == (expected >> 63));
            swift::u64 actual_sum{};
            std::memcpy(&actual_sum, uniform.data() + 16, sizeof(actual_sum));
            CHECK(actual_sum == expected_sum);
            if (shape != 0) {
                swift::u64 actual{};
                std::memcpy(&actual, uniform.data() + 8, sizeof(actual));
                CHECK(actual == expected);
            }
        }
#endif
    }
}
