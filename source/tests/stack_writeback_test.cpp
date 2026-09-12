#include "support/register_alloc_test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "translator/x86/translator.h"

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

std::vector<std::string> EmitPushShape(bool store_rsp, bool biased_memory) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .page_table = biased_memory ? reinterpret_cast<void*>(0x1000) : nullptr,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();

    IntrusivePtr<Block> block{new Block(0, Location{0x8920})};
    auto rsp = block->GetHostGPR(HostRegIndex(4), Imm{0u}).SetType(ValueType::U64);
    auto stored =
            store_rsp ? rsp : block->GetHostGPR(HostRegIndex(3), Imm{0u}).SetType(ValueType::U64);
    auto updated = block->Sub(rsp, Operand{Imm{swift::u64{8}}}).SetType(ValueType::U64);
    block->StoreMemory(Operand{updated}, stored);
    block->SetHostGPR(updated, HostRegIndex(4), Imm{0u});
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

std::vector<std::string> EmitSpilledPushShape() {
    IntrusivePtr<Block> block{new Block(0, Location{0x8940})};
    std::vector<Value> retained;
    for (swift::u64 value = 1; value <= 6; ++value) {
        retained.push_back(block->LoadImm(Imm{value}).SetType(ValueType::U64));
    }
    const auto rsp = block->GetHostGPR(HostRegIndex(4), Imm{0u})
                             .SetType(ValueType::U64);
    const auto stored = block->GetHostGPR(HostRegIndex(3), Imm{0u})
                                .SetType(ValueType::U64);
    const auto updated = block->Sub(rsp, Operand{Imm{swift::u64{8}}})
                                 .SetType(ValueType::U64);
    block->StoreMemory(Operand{updated}, stored);
    block->SetHostGPR(updated, HostRegIndex(4), Imm{0u});
    auto total = retained.front();
    for (std::size_t i = 1; i < retained.size(); ++i) {
        total = block->Add(total, Operand{retained[i]}).SetType(ValueType::U64);
    }
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    const GPRSMask gprs{~((1u << 6) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    RegAlloc alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocTestSupport::RunForSpillEvictTest(block.get(), &alloc, false);
    REQUIRE(alloc.ValueType(updated) == RegAlloc::MEM);

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .page_table = reinterpret_cast<void*>(0x1000),
    };
    AddressSpace address_space{config};
    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    return Disassemble(context);
}

std::size_t Count(const std::vector<std::string>& lines, std::string_view text) {
    return std::ranges::count_if(
            lines, [&](const auto& line) { return line.find(text) != std::string::npos; });
}

}  // namespace

TEST_CASE("stack push update folds into an direct-memory pre-index store") {
    const auto folded = EmitPushShape(false, false);
    REQUIRE(Count(folded, "str x3, [x4, #-8]!") == 1);
    REQUIRE(Count(folded, "sub ") == 0);
    REQUIRE(Count(folded, "mov x4") == 0);
}

TEST_CASE("stack push pre-index rejects address bias and base-data overlap") {
    const auto biased = EmitPushShape(false, true);
    const auto overlap = EmitPushShape(true, false);
    REQUIRE(Count(biased, "]!") == 0);
    REQUIRE(Count(biased, "sub ") == 1);
    REQUIRE(Count(overlap, "]!") == 0);
    REQUIRE(Count(overlap, "sub ") == 1);
}

TEST_CASE("spilled biased stack update publishes only after the store") {
    const auto instructions = EmitSpilledPushShape();
    const auto address = std::ranges::find_if(instructions, [](const auto& instruction) {
        return instruction.find("sub x10, x4, #0x8") != std::string::npos;
    });
    const auto store = std::ranges::find_if(instructions, [](const auto& instruction) {
        return instruction.find("str x3, [x10, x24]") != std::string::npos;
    });
    const auto publication = std::ranges::find_if(instructions, [](const auto& instruction) {
        return instruction.find("sub x4, x4, #0x8") != std::string::npos;
    });
    REQUIRE(address != instructions.end());
    REQUIRE(store != instructions.end());
    REQUIRE(publication != instructions.end());
    REQUIRE(address < store);
    REQUIRE(store < publication);
}

TEST_CASE("faulting stack push preserves the pre-instruction RSP") {
    using namespace swift::translator;
    using namespace swift::translator::x86;

    const auto page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<std::size_t>(page_long);
    auto* fault =
            static_cast<swift::u8*>(mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* code = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(fault != MAP_FAILED);
    REQUIRE(code != MAP_FAILED);
    const std::array<swift::u8, 2> guest{0x53, 0xf4};
    std::memcpy(code, guest.data(), guest.size());

    backend::SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make();
    auto* core = X86Core::Make(instance);
    auto& state = core->GetContext();
    const auto original_rsp = reinterpret_cast<swift::u64>(fault + 8);
    state.rip.qword = reinterpret_cast<swift::u64>(code);
    state.rsp.qword = original_rsp;
    state.rbx.qword = 0x123456789abcdef0;

    REQUIRE(core->Run() == ExitReason::PageFatal);
    REQUIRE(state.rsp.qword == original_rsp);
    REQUIRE(state.rbx.qword == 0x123456789abcdef0);

    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(fault, page);
    munmap(code, page);
}
