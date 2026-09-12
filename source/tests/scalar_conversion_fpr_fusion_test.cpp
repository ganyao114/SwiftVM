#include "support/register_alloc_test_support.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
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

constexpr swift::u32 kTarget = 17;

IntrusivePtr<Block> MakeConversion(bool faulting_source) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8b80})};
    auto current = block->GetHostFPR(HostRegIndex(kTarget), Imm{0u}).SetType(ValueType::V128);
    auto view = block->BitCast(current).SetType(ValueType::V128);
    auto zero = block->VecXor(current, view).SetType(ValueType::V128);
    block->SetHostFPR(zero, HostRegIndex(kTarget), Imm{0u});
    Value source;
    if (faulting_source) {
        auto address = block->LoadUniform(Uniform{0, ValueType::U64});
        source = block->LoadMemory(Operand{address}).SetType(ValueType::U32);
    } else {
        source = block->LoadUniform(Uniform{8, ValueType::U32});
    }
    auto converted = block->VecFCvtIntToFloat(source, Imm{32u}, Imm{64u}).SetType(ValueType::U64);
    block->SetHostFPR(converted, HostRegIndex(kTarget), Imm{0u});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

IntrusivePtr<Block> MakeResidentStore(bool overwrite) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8b80})};
    auto value = block->GetHostFPR(HostRegIndex(kTarget), Imm{0u}).SetType(ValueType::U64);
    if (overwrite) {
        auto replacement = block->LoadUniform(Uniform{16, ValueType::U64});
        block->SetHostFPR(replacement, HostRegIndex(kTarget), Imm{0u});
    }
    auto address = block->LoadUniform(Uniform{0, ValueType::U64});
    block->StoreMemory(Operand{address}, value);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

IntrusivePtr<Block> MakeNarrowResidentStore(bool overwrite) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8b80})};
    auto value = block->GetHostFPR(HostRegIndex(kTarget), Imm{0u}).SetType(ValueType::U64);
    auto narrow = block->ZeroExtend32(value);
    if (overwrite) {
        auto replacement = block->LoadUniform(Uniform{16, ValueType::U64});
        block->SetHostFPR(replacement, HostRegIndex(kTarget), Imm{0u});
    }
    auto address = block->LoadUniform(Uniform{0, ValueType::U64});
    block->StoreMemory(Operand{address}, narrow);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

IntrusivePtr<Block> MakePublishedExtractStore(bool overwrite) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8b80})};
    auto source = block->LoadUniform(Uniform{16, ValueType::V128});
    auto extracted = block->VecExtract64(source, Imm{0u}).SetType(ValueType::U64);
    block->SetHostFPR(source, HostRegIndex(kTarget), Imm{0u});
    if (overwrite) {
        auto replacement = block->LoadUniform(Uniform{32, ValueType::V128});
        block->SetHostFPR(replacement, HostRegIndex(kTarget), Imm{0u});
    }
    auto address = block->LoadUniform(Uniform{0, ValueType::U64});
    block->StoreMemory(Operand{address}, extracted);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

IntrusivePtr<Block> MakeConversionStore(bool overwrite) {
    IntrusivePtr<Block> block{new Block(0, Location{0x8b80})};
    auto current = block->GetHostFPR(HostRegIndex(kTarget), Imm{0u}).SetType(ValueType::V128);
    auto view = block->BitCast(current).SetType(ValueType::V128);
    auto zero = block->VecXor(current, view).SetType(ValueType::V128);
    block->SetHostFPR(zero, HostRegIndex(kTarget), Imm{0u});
    auto source = block->LoadUniform(Uniform{8, ValueType::U32});
    auto converted = block->VecFCvtIntToFloat(source, Imm{32u}, Imm{64u}).SetType(ValueType::U64);
    block->SetHostFPR(converted, HostRegIndex(kTarget), Imm{0u});
    if (overwrite) {
        auto replacement = block->LoadUniform(Uniform{16, ValueType::U64});
        block->SetHostFPR(replacement, HostRegIndex(kTarget), Imm{0u});
    }
    auto address = block->LoadUniform(Uniform{0, ValueType::U64});
    block->StoreMemory(Operand{address}, converted);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

std::vector<std::string> Emit(IntrusivePtr<Block> block) {
    GPRSMask gprs{~((1u << 8) - 1u)};
    FPRSMask fprs{0};
    fprs.Mark(kTarget);
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
            LocationDescriptor{0x8b80}, LocationDescriptor{0x8ba0}, ModuleConfig{});
    arm64::JitContext context{module, alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::vector<std::string> instructions;
    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
    for (auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        instructions.emplace_back(disassembler.GetOutput());
    }
    return instructions;
}

std::size_t Count(const std::vector<std::string>& lines, std::string_view text) {
    return std::ranges::count_if(
            lines, [&](const auto& line) { return line.find(text) != std::string::npos; });
}

}  // namespace

TEST_CASE("integer conversion writes a zero-high resident FPR directly") {
    const auto direct = Emit(MakeConversion(false));
    REQUIRE(Count(direct, "scvtf d17") == 1);
    REQUIRE(Count(direct, "eor v17.16b") == 1);
    REQUIRE(Count(direct, "mov v17.d[0]") == 0);
    REQUIRE(Count(direct, "fmov x") == 0);

    const auto faulting = Emit(MakeConversion(true));
    REQUIRE(Count(faulting, "scvtf d17") == 1);
    REQUIRE(Count(faulting, "eor v17.16b") == 1);
    REQUIRE(Count(faulting, "mov v17.d[0]") == 0);
}

TEST_CASE("resident scalar FPR reads store to memory without a GPR bridge") {
    const auto direct = Emit(MakeResidentStore(false));
    REQUIRE(Count(direct, "str d17") == 1);
    REQUIRE(Count(direct, "mov x") == 0);

    const auto overwritten = Emit(MakeResidentStore(true));
    REQUIRE(Count(overwritten, "str d17") == 0);
    REQUIRE(Count(overwritten, "mov x") == 1);
    REQUIRE(Count(overwritten, "str x") == 1);
}

TEST_CASE("resident narrow FPR reads store to memory without a GPR bridge") {
    const auto direct = Emit(MakeNarrowResidentStore(false));
    REQUIRE(Count(direct, "str s17") == 1);
    REQUIRE(Count(direct, "mov x") == 0);
    REQUIRE(Count(direct, "mov w") == 0);

    const auto overwritten = Emit(MakeNarrowResidentStore(true));
    REQUIRE(Count(overwritten, "str s17") == 0);
    REQUIRE(Count(overwritten, "mov x") == 1);
    REQUIRE(Count(overwritten, "mov w") == 1);
    REQUIRE(Count(overwritten, "str w") == 1);
}

TEST_CASE("published low64 extracts store from the resident FPR home") {
    SECTION("direct") {
        const auto emitted = Emit(MakePublishedExtractStore(false));
        REQUIRE(Count(emitted, "str d17") == 1);
        REQUIRE(Count(emitted, "mov x") == 0);
    }
    SECTION("overwritten") {
        const auto emitted = Emit(MakePublishedExtractStore(true));
        REQUIRE(Count(emitted, "str d17") == 0);
        REQUIRE(Count(emitted, "mov x") == 1);
        REQUIRE(Count(emitted, "str x") == 1);
    }
}

TEST_CASE("resident scalar conversions store to memory without a GPR bridge") {
    const auto direct = Emit(MakeConversionStore(false));
    REQUIRE(Count(direct, "scvtf d17") == 1);
    REQUIRE(Count(direct, "str d17") == 1);
    REQUIRE(Count(direct, "fmov x") == 0);
    REQUIRE(Count(direct, "str x") == 0);

    const auto overwritten = Emit(MakeConversionStore(true));
    REQUIRE(Count(overwritten, "scvtf d17") == 1);
    REQUIRE(Count(overwritten, "str d17") == 0);
    REQUIRE(Count(overwritten, "fmov x") == 1);
    REQUIRE(Count(overwritten, "str x") == 1);
}

TEST_CASE("direct resident integer conversion preserves signed scalar results") {
    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<std::size_t>(page_long);
    auto* code = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(code != MAP_FAILED);

    const std::array<swift::u8, 9> convert32{
            0x66,
            0x0f,
            0xef,
            0xc9,
            0xf2,
            0x0f,
            0x2a,
            0xc8,
            0xf4,
    };
    const std::array<swift::u8, 10> convert64{
            0x66,
            0x0f,
            0xef,
            0xc9,
            0xf2,
            0x48,
            0x0f,
            0x2a,
            0xc8,
            0xf4,
    };
    std::memcpy(code, convert32.data(), convert32.size());
    std::memcpy(code + 32, convert64.data(), convert64.size());

    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();
    for (const auto value : std::array<swift::s64, 7>{
                 0,
                 1,
                 -1,
                 INT32_MIN,
                 INT32_MAX,
                 INT64_MIN,
                 INT64_MAX,
         }) {
        for (const bool wide : {false, true}) {
            if (!wide && (value < INT32_MIN || value > INT32_MAX)) {
                continue;
            }
            auto* core = swift::translator::x86::X86Core::Make(instance);
            auto& state = core->GetContext();
            state.rip.qword = reinterpret_cast<swift::u64>(code + (wide ? 32 : 0));
            state.rax.qword = static_cast<swift::u64>(value);
            std::memset(&state.xmm1, 0xa5, sizeof(state.xmm1));

            REQUIRE(core->Run() == swift::translator::ExitReason::None);
            const auto expected = wide ? static_cast<double>(value)
                                       : static_cast<double>(static_cast<swift::s32>(value));
            double actual{};
            std::memcpy(&actual, &state.xmm1, sizeof(actual));
            std::array<swift::u8, 8> high{};
            const bool high_zero = std::memcmp(reinterpret_cast<const swift::u8*>(&state.xmm1) + 8,
                                               high.data(),
                                               high.size()) == 0;

            swift::translator::x86::X86Core::Destroy(core);
            CAPTURE(value, wide, expected, actual);
            REQUIRE(actual == expected);
            REQUIRE(high_zero);
        }
    }
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(code, page);
}

TEST_CASE("fault before scalar conversion retains the completed vector zero") {
    if (!GetSvmConfig().xmm_resident) {
        SUCCEED("resident XMM integration is disabled");
        return;
    }
    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<std::size_t>(page_long);
    auto* data =
            static_cast<swift::u8*>(mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* code = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(data != MAP_FAILED);
    REQUIRE(code != MAP_FAILED);

    const std::array<swift::u8, 9> guest{
            0x66,
            0x0f,
            0xef,
            0xc9,
            0xf2,
            0x0f,
            0x2a,
            0x08,
            0xf4,
    };
    const std::array<swift::u8, 16> initial{
            0x10,
            0x21,
            0x32,
            0x43,
            0x54,
            0x65,
            0x76,
            0x87,
            0x98,
            0xa9,
            0xba,
            0xcb,
            0xdc,
            0xed,
            0xfe,
            0x0f,
    };
    const std::array<swift::u8, 16> zero{};
    std::memcpy(code, guest.data(), guest.size());

    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();
    auto* core = swift::translator::x86::X86Core::Make(instance);
    auto& state = core->GetContext();
    state.rip.qword = reinterpret_cast<swift::u64>(code);
    state.rax.qword = reinterpret_cast<swift::u64>(data);
    std::memcpy(&state.xmm1, initial.data(), initial.size());

    const auto reason = core->Run();
    const bool cleared = std::memcmp(&state.xmm1, zero.data(), zero.size()) == 0;

    swift::translator::x86::X86Core::Destroy(core);
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(data, page);
    munmap(code, page);

    REQUIRE(reason == swift::translator::ExitReason::PageFatal);
    REQUIRE(cleared);
}
