#include "support/register_alloc_test_support.h"
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
#include "runtime/common/svm_config.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "translator/x86/translator.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

constexpr swift::u32 kTarget = 17;

IntrusivePtr<Block> MakeScalarLoad(ValueType load_type, bool zero_high,
                                   bool fault_observer = false) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8810})};
    auto address = block->LoadUniform(Uniform{0, ValueType::U64});
    auto loaded = block->LoadMemory(Operand{address}).SetType(load_type);
    auto published = load_type == ValueType::U32 ? block->ZeroExtend64(loaded) : loaded;
    auto high = block->LoadImm(Imm{static_cast<swift::u64>(zero_high ? 0 : 1)})
                        .SetType(ValueType::U64);
    if (fault_observer) {
        auto observer_address = block->LoadUniform(Uniform{8, ValueType::U64});
        (void)block->LoadMemory(Operand{observer_address}).SetType(ValueType::U64);
    }
    block->AppendInst(
            OpCode::SetHostFPR, published, HostRegIndex(kTarget), Imm{0u});
    block->AppendInst(
            OpCode::SetHostFPR, high, HostRegIndex(kTarget), Imm{8u});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

IntrusivePtr<Block> MakeZeroScalarValue() {
    IntrusivePtr<Block> block{new Block(0, Location{0x8810})};
    auto low = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
    auto high = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
    block->AppendInst(
            OpCode::SetHostFPR, low, HostRegIndex(kTarget), Imm{0u});
    block->AppendInst(
            OpCode::SetHostFPR, high, HostRegIndex(kTarget), Imm{8u});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

IntrusivePtr<Block> MakeSharedHighZero() {
    IntrusivePtr<Block> block{new Block(0, Location{0x8810})};
    auto low0 = block->LoadUniform(Uniform{0, ValueType::U64});
    auto low1 = block->LoadUniform(Uniform{8, ValueType::U64});
    auto high = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
    block->AppendInst(
            OpCode::SetHostFPR, low0, HostRegIndex(kTarget), Imm{0u});
    block->AppendInst(
            OpCode::SetHostFPR, high, HostRegIndex(kTarget), Imm{8u});
    block->AppendInst(
            OpCode::SetHostFPR, low1, HostRegIndex(kTarget + 1), Imm{0u});
    block->AppendInst(
            OpCode::SetHostFPR, high, HostRegIndex(kTarget + 1), Imm{8u});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

IntrusivePtr<Block> MakeSharedHighZeroLoads() {
    IntrusivePtr<Block> block{new Block(0, Location{0x8810})};
    auto high = block->LoadImm(Imm{swift::u64{0}}).SetType(ValueType::U64);
    auto address0 = block->LoadUniform(Uniform{0, ValueType::U64});
    auto low0 = block->LoadMemory(Operand{address0}).SetType(ValueType::U64);
    block->AppendInst(
            OpCode::SetHostFPR, low0, HostRegIndex(kTarget), Imm{0u});
    block->AppendInst(
            OpCode::SetHostFPR, high, HostRegIndex(kTarget), Imm{8u});
    auto address1 = block->LoadUniform(Uniform{8, ValueType::U64});
    auto low1 = block->LoadMemory(Operand{address1}).SetType(ValueType::U64);
    block->AppendInst(
            OpCode::SetHostFPR, low1, HostRegIndex(kTarget + 1), Imm{0u});
    block->AppendInst(
            OpCode::SetHostFPR, high, HostRegIndex(kTarget + 1), Imm{8u});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

std::vector<std::string> Emit(IntrusivePtr<Block> block) {
    GPRSMask gprs{~((1u << 8) - 1u)};
    FPRSMask fprs{0};
    fprs.Mark(kTarget);
    fprs.Mark(kTarget + 1);
    RegAlloc alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocTestSupport::RunForXmmResidentTest(block.get(), &alloc, true);

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.MapModule(
            LocationDescriptor{0x8810}, LocationDescriptor{0x8830},
            ModuleConfig{});
    arm64::JitContext context{module, alloc};
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

TEST_CASE("scalar memory load and zero-high publication use one D-register load") {
    const auto fused = Emit(MakeScalarLoad(ValueType::U64, true));
    const auto fallback = Emit(MakeScalarLoad(ValueType::U64, false));
    const auto observed = Emit(MakeScalarLoad(ValueType::U64, true, true));

    REQUIRE(Count(fused, "ldr d17") == 1);
    REQUIRE(Count(fused, "mov v17.d") == 0);
    REQUIRE(Count(fallback, "ldr d17") == 0);
    REQUIRE(Count(fallback, "mov v17.d") == 2);
    REQUIRE(Count(fallback, "fmov d17") == 0);
    REQUIRE(fused.size() + 3 == fallback.size());
    REQUIRE(Count(observed, "ldr d17") == 0);
    REQUIRE(Count(observed, "mov v17.d") == 0);
    REQUIRE(Count(observed, "fmov d17") == 1);
}

TEST_CASE("narrow scalar memory load and zero-high publication use one S-register load") {
    const auto fused = Emit(MakeScalarLoad(ValueType::U32, true));
    const auto fallback = Emit(MakeScalarLoad(ValueType::U32, false));
    const auto observed = Emit(MakeScalarLoad(ValueType::U32, true, true));

    REQUIRE(Count(fused, "ldr s17") == 1);
    REQUIRE(Count(fused, "fmov d17") == 0);
    REQUIRE(Count(fused, "mov v17.d") == 0);
    REQUIRE(Count(fallback, "ldr s17") == 0);
    REQUIRE(Count(fallback, "mov v17.d") == 2);
    REQUIRE(Count(observed, "ldr s17") == 0);
    REQUIRE(Count(observed, "fmov d17") == 1);
}

TEST_CASE("zero-low scalar publication clears the full resident home") {
    const auto emitted = Emit(MakeZeroScalarValue());
    REQUIRE(Count(emitted, "eor v17.16b") == 1);
    REQUIRE(Count(emitted, "fmov d17") == 0);
    REQUIRE(Count(emitted, "mov v17.d") == 0);
}

TEST_CASE("shared high zero is removed only after every scalar publication fuses") {
    const auto emitted = Emit(MakeSharedHighZero());
    REQUIRE(Count(emitted, "fmov d17") == 1);
    REQUIRE(Count(emitted, "fmov d18") == 1);
    REQUIRE(Count(emitted, "mov x") == 0);

    const auto loads = Emit(MakeSharedHighZeroLoads());
    REQUIRE(Count(loads, "ldr d17") == 1);
    REQUIRE(Count(loads, "ldr d18") == 1);
    REQUIRE(Count(loads, "fmov d17") == 0);
    REQUIRE(Count(loads, "fmov d18") == 0);
}

TEST_CASE("faulting scalar memory load leaves the resident XMM home unchanged") {
    if (!GetSvmConfig().xmm_resident) {
        SUCCEED("resident XMM integration is disabled");
        return;
    }
    for (const auto prefix : {swift::u8{0xf2}, swift::u8{0xf3}}) {
        CAPTURE(prefix);
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

        const std::array<swift::u8, 6> guest{
                prefix, 0x0f, 0x10, 0x48, 0x08, 0xf4,
        };
        const std::array<swift::u8, 16> expected{
                0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
                0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f,
        };
        std::memcpy(code, guest.data(), guest.size());

        backend::SmcTracker::SetEnabled(false);
        auto* instance = swift::translator::x86::X86Instance::Make();
        auto* core = swift::translator::x86::X86Core::Make(instance);
        auto& state = core->GetContext();
        state.rip.qword = reinterpret_cast<swift::u64>(code);
        state.rax.qword = reinterpret_cast<swift::u64>(data);
        std::memcpy(&state.xmm1, expected.data(), expected.size());

        const auto reason = core->Run();
        const bool unchanged =
                std::memcmp(&state.xmm1, expected.data(), expected.size()) == 0;

        swift::translator::x86::X86Core::Destroy(core);
        swift::translator::x86::X86Instance::Destroy(instance);
        backend::SmcTracker::SetEnabled(true);
        munmap(data, page);
        munmap(code, page);

        REQUIRE(reason == swift::translator::ExitReason::PageFatal);
        REQUIRE(unchanged);
    }
}
