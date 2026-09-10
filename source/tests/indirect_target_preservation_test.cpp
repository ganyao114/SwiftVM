#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/ir/opts/register_alloc_pass.h"

TEST_CASE("indirect L1 cold lookup preserves its target register",
          "[indirect-l1][continuation]") {
    using namespace swift;
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

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
    module_config.feature_overrides.Set(FeatureId::indirect_l1, true);
    auto module = address_space.MapModule(
            LocationDescriptor{0x7000}, LocationDescriptor{0x7100}, module_config);

    IntrusivePtr<Block> block{new Block(0, Location{0x7000})};
    block->AppendInst(OpCode::Nop);
    block->ReIdInstr();

    GPRSMask gprs{~u32{0}};
    for (u32 code : {8u, 9u, 10u}) {
        gprs.Clear(code);
    }
    FPRSMask fprs{~u32{0}};
    const auto features = ResolveFeatureSet(module_config);
    RegAlloc alloc{block->MaxInstrId(), gprs, fprs, features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);

    arm64::JitContext context{module, alloc};
    context.SetCurrent(block.get());
    context.BeginColdScratch();
    vixl::aarch64::Label miss;
    (void)context.ForwardIndirectL1(vixl::aarch64::x8, &miss);
    context.GetMasm().Bind(&miss);
    context.ReturnHost();
    vixl::aarch64::Label continuation_miss;
    vixl::aarch64::Label continuation_dispatch;
    (void)context.ForwardContinuation(vixl::aarch64::x8,
                                      &continuation_miss,
                                      &continuation_dispatch);
    context.GetMasm().Bind(&continuation_miss);
    context.GetMasm().Bind(&continuation_dispatch);
    context.ReturnHost();
    context.EndColdScratch();
    context.Finish();

    std::vector<std::string> instructions;
    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<
            const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<
            const vixl::aarch64::Instruction*>();
    for (auto* instruction = first; instruction < last; ++instruction) {
        decoder.Decode(instruction);
        instructions.emplace_back(disassembler.GetOutput());
    }

    REQUIRE(std::ranges::none_of(instructions, [](const std::string& text) {
        return text.find("ldp x8,") != std::string::npos;
    }));
    REQUIRE(std::ranges::any_of(instructions, [](const std::string& text) {
        return text.find("cmp") != std::string::npos &&
               text.find(", x8") != std::string::npos &&
               text.find("x8, x8") == std::string::npos;
    }));
    // External call-miss frames carry a null continuation: the pop must guard
    // it before blr rather than un-pop, since the mismatch path resets rsb.
    REQUIRE(std::ranges::none_of(instructions, [](const std::string& text) {
        return text.find("sub x25, x25, #0x10") != std::string::npos;
    }));
    REQUIRE(std::ranges::any_of(instructions, [](const std::string& text) {
        return text.find("cbz") != std::string::npos;
    }));
}
