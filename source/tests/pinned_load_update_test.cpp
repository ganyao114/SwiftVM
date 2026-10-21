#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "translator/x86/translator.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

std::vector<std::string> EmitPinnedLoadUpdate() {
    IntrusivePtr<Block> block{new Block(0, Location{0x9210})};
    auto base = block->GetHostGPR(HostRegIndex(23), Imm{0u})
                        .SetType(ValueType::U64);
    auto loaded = block->LoadMemory(Operand{base, Imm{1u}})
                          .SetType(ValueType::U8);
    auto widened = block->ZeroExtend32(loaded).SetType(ValueType::U32);
    auto published = block->ZeroExtend32To64(widened).SetType(ValueType::U64);
    block->SetHostGPR(published, HostRegIndex(29), Imm{0u});
    auto updated = block->Add(base, Operand{Imm{1u}}).SetType(ValueType::U64);
    block->SetHostGPR(updated, HostRegIndex(23), Imm{0u});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .global_opts = Optimizations::All,
    };
    AddressSpace address_space{config};
    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(), FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &alloc, false, FeatureSet{});
    alloc.CompleteGPRAllocation(address_space.GetDefaultModule());
    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
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

std::size_t Count(const std::vector<std::string>& lines, std::string_view text) {
    return std::ranges::count_if(lines, [&](const auto& line) {
        return line.find(text) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("pinned byte load folds a following base update into writeback") {
    const auto instructions = EmitPinnedLoadUpdate();
    REQUIRE(Count(instructions, "ldrb w29, [x23, #1]!") == 1);
    REQUIRE(Count(instructions, "add x23, x23, #1") == 0);
}

TEST_CASE("faulting pinned load writeback leaves the guest base unchanged") {
    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<std::size_t>(page_long);
    auto* data = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* code = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(data != MAP_FAILED);
    REQUIRE(code != MAP_FAILED);

    const std::array<swift::u8, 11> guest{
            0x0f, 0xb6, 0x51, 0x01,
            0x48, 0x83, 0xc1, 0x01,
            0x84, 0xd2,
            0xf4,
    };
    std::memcpy(code, guest.data(), guest.size());

    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();
    auto* core = swift::translator::x86::X86Core::Make(instance);
    auto& state = core->GetContext();
    const auto original = reinterpret_cast<swift::u64>(data);
    state.rip.qword = reinterpret_cast<swift::u64>(code);
    state.rcx.qword = original;
    state.rdx.qword = 0x1122334455667788ull;

    const auto reason = core->Run();
    const auto observed_base = state.rcx.qword;

    swift::translator::x86::X86Core::Destroy(core);
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(data, page);
    munmap(code, page);

    REQUIRE(reason == swift::translator::ExitReason::PageFatal);
    REQUIRE(observed_base == original);
}
