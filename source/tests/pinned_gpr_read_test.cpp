#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/mman.h>
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

enum class PinnedReadShape {
    Or,
    SelfAnd,
    SelfWrite,
    SelfWriteAddress,
    Copy,
    TransferredCopy,
    CopyU16,
    LoadU16,
    SignedLoadU16,
    LoadU8Flags,
    LoadU8SavedFlags,
    LoadNarrowSubtract,
    LoadCallerNarrowSubtract,
    FullAddAlias,
    OverwrittenWrite,
    OverwrittenWriteFault,
    MemoryAddress,
    SignExtend,
    StoreMemory,
    NarrowStoreMemory,
    Subtract,
    CallerNarrowSubtract,
};

std::vector<std::string> EmitPinnedRead(PinnedReadShape shape, bool reuse_read,
                                        ValueType type = ValueType::U32) {
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();

    IntrusivePtr<Block> block{new Block(0, Location{0x8940})};
    const bool overwritten_write = shape == PinnedReadShape::OverwrittenWrite ||
                                   shape == PinnedReadShape::OverwrittenWriteFault;
    const bool copy = shape == PinnedReadShape::Copy ||
                      shape == PinnedReadShape::TransferredCopy ||
                      shape == PinnedReadShape::CopyU16 ||
                      shape == PinnedReadShape::LoadU16 ||
                      shape == PinnedReadShape::NarrowStoreMemory;
    const auto source_index = overwritten_write
            ? 1u
            : (shape == PinnedReadShape::CallerNarrowSubtract
                       ? 7u
                       : (copy ? 20u : 22u));
    if (shape == PinnedReadShape::CopyU16 || shape == PinnedReadShape::LoadU16 ||
        shape == PinnedReadShape::SignedLoadU16) {
        type = ValueType::U16;
    } else if (shape == PinnedReadShape::LoadU8Flags ||
               shape == PinnedReadShape::LoadU8SavedFlags) {
        type = ValueType::U8;
    }
    auto value = shape == PinnedReadShape::LoadU16 ||
                         shape == PinnedReadShape::SignedLoadU16 ||
                         shape == PinnedReadShape::LoadU8Flags ||
                         shape == PinnedReadShape::LoadU8SavedFlags ||
                         shape == PinnedReadShape::LoadNarrowSubtract ||
                         shape == PinnedReadShape::LoadCallerNarrowSubtract
            ? block->LoadMemory(Operand{block->LoadImm(Imm{swift::u64{0x1000}})
                                                .SetType(ValueType::U64)})
                      .SetType(type)
            : block->GetHostGPR(HostRegIndex(source_index), Imm{0u})
                      .SetType(type);
    if (shape == PinnedReadShape::SignedLoadU16) {
        auto signed_value = block->SignExtend(value).SetType(ValueType::S32);
        auto extended = block->ZeroExtend32To64(signed_value)
                                .SetType(ValueType::U64);
        block->SetHostGPR(extended, HostRegIndex(22), Imm{0u});
        auto alias = block->BitExtract(extended, Imm{0u}, Imm{32u})
                             .SetType(ValueType::U32);
        auto widened = block->SignExtend(alias).SetType(ValueType::U64);
        auto product = block->Mul(alias, Operand{Imm{3u}})
                               .SetType(ValueType::U32);
        block->StoreUniform(Uniform{64, ValueType::U64}, widened);
        block->StoreUniform(Uniform{72, ValueType::U32}, product);
    } else if (shape == PinnedReadShape::LoadNarrowSubtract ||
               shape == PinnedReadShape::LoadCallerNarrowSubtract) {
        auto extended = block->ZeroExtend32(value).SetType(ValueType::U32);
        auto published = block->ZeroExtend32To64(extended)
                                 .SetType(ValueType::U64);
        const auto target = shape == PinnedReadShape::LoadCallerNarrowSubtract
                ? 2u
                : 22u;
        block->SetHostGPR(published, HostRegIndex(target), Imm{0u});
        const auto width = ir::GetValueSizeByte(type) * 8;
        auto alias = block->BitExtract(published, Imm{0u}, Imm{width})
                             .SetType(type);
        auto left = block->GetHostGPR(HostRegIndex(7), Imm{0u})
                            .SetType(type);
        auto result = block->Sub(left, Operand{alias}).SetType(type);
        block->SaveFlags(result, Flags::All);
    } else if (shape == PinnedReadShape::LoadU8Flags ||
               shape == PinnedReadShape::LoadU8SavedFlags) {
        auto extended = block->ZeroExtend32To64(value).SetType(ValueType::U64);
        block->SetHostGPR(extended, HostRegIndex(23), Imm{0u});
        auto alias = block->BitExtract(extended, Imm{0u}, Imm{8u})
                             .SetType(ValueType::U8);
        auto result = block->Or(alias, Operand{Imm{0u}})
                               .SetType(ValueType::U8);
        if (shape == PinnedReadShape::LoadU8Flags) {
            block->AppendInst(OpCode::BranchOnlyFlags, result, Flags::Zero);
        } else {
            block->SaveFlags(result, Flags::Zero | Flags::Parity);
        }
    } else if (shape == PinnedReadShape::FullAddAlias) {
        auto narrow = block->GetHostGPR(HostRegIndex(29), Imm{0u})
                              .SetType(ValueType::U8);
        auto extended = block->ZeroExtend32(narrow).SetType(ValueType::U32);
        auto published = block->ZeroExtend32To64(extended)
                                 .SetType(ValueType::U64);
        block->SetHostGPR(published, HostRegIndex(29), Imm{0u});
        auto alias = block->BitCast(published).SetType(ValueType::U64);
        auto left = block->GetHostGPR(HostRegIndex(0), Imm{0u})
                            .SetType(ValueType::U64);
        auto result = block->Add(left, Operand{alias}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{64, ValueType::U64}, result);
    } else if (shape == PinnedReadShape::Or) {
        auto right = block->GetHostGPR(HostRegIndex(29), Imm{0u})
                             .SetType(type);
        auto result = block->Or(value, Operand{right}).SetType(type);
        block->SaveFlags(result, Flags::Negate | Flags::Zero | Flags::Parity);
    } else if (shape == PinnedReadShape::TransferredCopy) {
        auto published = block->ZeroExtend32To64(value).SetType(ValueType::U64);
        block->SetHostGPR(published, HostRegIndex(22), Imm{0u});
        auto replacement = block->LoadImm(Imm{swift::u64{0x1234}})
                                   .SetType(ValueType::U64);
        block->SetHostGPR(replacement, HostRegIndex(source_index), Imm{0u});
        auto right = block->GetHostGPR(HostRegIndex(29), Imm{0u})
                             .SetType(ValueType::U32);
        auto result = block->Xor(value, Operand{right}).SetType(ValueType::U32);
        block->StoreUniform(Uniform{64, ValueType::U32}, result);
    } else if (shape == PinnedReadShape::SelfWrite ||
               shape == PinnedReadShape::SelfWriteAddress ||
                shape == PinnedReadShape::Copy ||
               shape == PinnedReadShape::CopyU16 ||
               shape == PinnedReadShape::LoadU16) {
        auto extended = shape == PinnedReadShape::CopyU16 ||
                                shape == PinnedReadShape::LoadU16
                ? block->ZeroExtend32(value).SetType(ValueType::U32)
                : value;
        auto result = block->ZeroExtend32To64(extended).SetType(ValueType::U64);
        block->SetHostGPR(result, HostRegIndex(22), Imm{0u});
        auto address = block->BitCast(result).SetType(ValueType::U64);
        if (shape == PinnedReadShape::SelfWriteAddress) {
            auto base = block->GetHostGPR(HostRegIndex(19), Imm{0u})
                                .SetType(ValueType::U64);
            address = block->GetOperand(
                                   Operand{base, address,
                                           {OperandOp::PlusExt, 2}})
                              .SetType(ValueType::U64);
        }
        auto loaded = block->LoadMemory(Operand{address, Imm{8u}})
                              .SetType(ValueType::U8);
        block->StoreUniform(Uniform{64, ValueType::U8}, loaded);
    } else if (overwritten_write) {
        auto copied = block->ZeroExtend32To64(value).SetType(ValueType::U64);
        block->SetHostGPR(copied, HostRegIndex(23), Imm{0u});
        block->AdvancePC(Imm{2u});
        if (shape == PinnedReadShape::OverwrittenWriteFault) {
            auto address = block->LoadImm(Imm{swift::u64{0x1000}})
                                   .SetType(ValueType::U64);
            auto loaded = block->LoadMemory(Operand{address}).SetType(ValueType::U8);
            block->StoreUniform(Uniform{64, ValueType::U8}, loaded);
        }
        auto low = block->BitExtract(copied, Imm{0u}, Imm{32u})
                           .SetType(ValueType::U32);
        auto masked = block->And(low, Operand{Imm{swift::u32{0x70}}})
                              .SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(masked).SetType(ValueType::U64);
        block->SetHostGPR(result, HostRegIndex(23), Imm{0u});
    } else if (shape == PinnedReadShape::MemoryAddress) {
        auto address = block->GetOperand(Operand{value}).SetType(ValueType::U64);
        if (reuse_read) {
            auto replacement = block->LoadImm(Imm{swift::u64{0x1000}})
                                       .SetType(ValueType::U64);
            block->SetHostGPR(replacement, HostRegIndex(22), Imm{0u});
        }
        auto loaded = block->LoadMemory(Operand{address}).SetType(ValueType::U32);
        block->StoreUniform(Uniform{64, ValueType::U32}, loaded);
    } else if (shape == PinnedReadShape::SelfAnd) {
        auto result = block->And(value, Operand{value}).SetType(ValueType::U32);
        block->SaveFlags(result, Flags::Negate | Flags::Zero | Flags::Parity);
    } else if (shape == PinnedReadShape::NarrowStoreMemory) {
        auto right = block->GetHostGPR(HostRegIndex(29), Imm{0u})
                             .SetType(type);
        auto result = block->Or(value, Operand{right}).SetType(type);
        block->SetHostGPR(result, HostRegIndex(22), Imm{0u});
        auto low = block->BitExtract(result, Imm{0u}, Imm{16u})
                           .SetType(ValueType::U16);
        auto address = block->LoadImm(Imm{swift::u64{0x1000}})
                               .SetType(ValueType::U64);
        block->StoreMemory(Operand{address}, low);
    } else if (shape == PinnedReadShape::SignExtend) {
        auto result = block->SignExtend(value).SetType(ValueType::U64);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
    } else if (shape == PinnedReadShape::Subtract ||
               shape == PinnedReadShape::CallerNarrowSubtract) {
        auto right = block->LoadImm(Imm{swift::u32{1}}).SetType(type);
        auto result = block->Sub(value, Operand{right}).SetType(type);
        block->SaveFlags(result, Flags::All);
    } else {
        auto address = block->LoadImm(Imm{swift::u64{0x1000}})
                               .SetType(ValueType::U64);
        block->StoreMemory(Operand{address}, value);
    }
    if (reuse_read && shape != PinnedReadShape::MemoryAddress) {
        block->StoreUniform(Uniform{0, ValueType::U32}, value);
    }
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    FeatureSet features{};
    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(),
                   features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);
    if (shape == PinnedReadShape::NarrowStoreMemory) {
        for (auto& inst : block->GetInstList()) {
            if (inst.GetOp() != OpCode::SetHostGPR ||
                inst.GetArg<Imm>(1).Get() != 22) {
                continue;
            }
            const auto published = inst.GetArg<Value>(0);
            alloc.MapRegister(published.Id(), HostGPR{22});
            alloc.MarkHostWriteCoalesced(inst.Id());
            break;
        }
    }
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

std::size_t Count(const std::vector<std::string>& lines, std::string_view first,
                  std::string_view second) {
    return std::ranges::count_if(lines, [&](const auto& line) {
        return line.find(first) != std::string::npos &&
               line.find(second) != std::string::npos;
    });
}

bool HasShiftPreparation(const std::vector<std::string>& lines) {
    for (std::size_t index = 0; index + 1 < lines.size(); ++index) {
        const auto& preparation = lines[index];
        const auto comma = preparation.find(',');
        if ((!preparation.starts_with("mov w") &&
             !preparation.starts_with("lsl w")) ||
            comma == std::string::npos ||
            !lines[index + 1].starts_with("subs w")) {
            continue;
        }
        const auto destination = preparation.substr(4, comma - 4);
        if (lines[index + 1].find(", " + destination + ", lsl #") !=
            std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("one pinned GPR consumer reuses its fixed W view") {
    const auto lines = EmitPinnedRead(PinnedReadShape::SelfAnd, false);
    REQUIRE(Count(lines, "ubfx ", "x22") == 0);
    REQUIRE(Count(lines, "ands w", "w22, w22") == 1);
}

TEST_CASE("pinned GPR OR reads direct W views only at exact U32 width") {
    const auto wide = EmitPinnedRead(PinnedReadShape::Or, false);
    REQUIRE(Count(wide, "ubfx ", "x22") == 0);
    REQUIRE(Count(wide, "ubfx ", "x29") == 0);
    REQUIRE(Count(wide, "orr w", "w22, w29") == 1);

    const auto narrow = EmitPinnedRead(
            PinnedReadShape::Or, false, ValueType::U16);
    REQUIRE(Count(narrow, "orr w", "w22, w29") == 0);
}

TEST_CASE("a pinned low-32 self-write clears its high half in one instruction") {
    const auto lines = EmitPinnedRead(PinnedReadShape::SelfWrite, false);
    REQUIRE(Count(lines, "ubfx ", "x22") == 0);
    REQUIRE(Count(lines, "mov w22, w22", "") == 1);
    REQUIRE(Count(lines, "mov w", "w22") == 1);
    REQUIRE(Count(lines, "ldrb ", "[x22") == 1);
}

TEST_CASE("a pinned self-write feeds a computed indexed address") {
    const auto lines = EmitPinnedRead(PinnedReadShape::SelfWriteAddress, false);
    REQUIRE(Count(lines, "ubfx ", "x22") == 0);
    REQUIRE(Count(lines, "mov w22, w22", "") == 1);
    REQUIRE(Count(lines, "add x", "x19, x22, lsl #2") == 1);
}

TEST_CASE("a pinned low-32 copy publishes directly between fixed homes") {
    const auto lines = EmitPinnedRead(PinnedReadShape::Copy, false);
    REQUIRE(Count(lines, "ubfx ", "x20") == 0);
    REQUIRE(Count(lines, "mov w22, w20", "") == 1);
    REQUIRE(Count(lines, "mov w", "w20") == 1);
    REQUIRE(Count(lines, "ldrb ", "[x22") == 1);
}

TEST_CASE("a published fixed home owns later uses after the source is overwritten") {
    const auto lines = EmitPinnedRead(PinnedReadShape::TransferredCopy, false);
    REQUIRE(Count(lines, "mov w22, w20", "") == 1);
    REQUIRE(Count(lines, "eor w", "w22, w29") == 1);
    REQUIRE(Count(lines, "eor w", "w20, w29") == 0);
}

TEST_CASE("a published narrow value feeds a full-width add from its fixed home") {
    const auto lines = EmitPinnedRead(PinnedReadShape::FullAddAlias, false);
    REQUIRE(Count(lines, "add x", ", x29") == 1);
}

TEST_CASE("a pinned narrow copy zero-extends directly between fixed homes") {
    const auto lines = EmitPinnedRead(PinnedReadShape::CopyU16, false);
    REQUIRE(Count(lines, "ubfx ", "x20") == 0);
    REQUIRE(Count(lines, "uxth w22, w20", "") == 1);
    REQUIRE(Count(lines, "mov w", "w20") == 0);
    REQUIRE(Count(lines, "ldrb ", "[x22") == 1);
}

TEST_CASE("a pinned narrow load publishes directly into its fixed home") {
    const auto lines = EmitPinnedRead(PinnedReadShape::LoadU16, false);
    REQUIRE(Count(lines, "ldrh w22", "") == 1);
    REQUIRE(Count(lines, "uxth w22", "") == 0);
    REQUIRE(Count(lines, "mov w22", "") == 0);
    REQUIRE(Count(lines, "ldrb ", "[x22") == 1);
}

TEST_CASE("a signed narrow load publishes directly into its fixed home") {
    const auto lines = EmitPinnedRead(PinnedReadShape::SignedLoadU16, false);
    REQUIRE(Count(lines, "ldrsh w22", "") == 1);
    REQUIRE(Count(lines, "sxth ", "") == 0);
    REQUIRE(Count(lines, "mov w22", "") == 0);
    REQUIRE(Count(lines, "ubfx ", "") == 0);
    REQUIRE(Count(lines, "sxtw ", "w22") == 1);
    REQUIRE(Count(lines, "mul w", "w22") == 1);
}

TEST_CASE("a pinned narrow load keeps a branch flag alias in its fixed home") {
    const auto lines = EmitPinnedRead(PinnedReadShape::LoadU8Flags, false);
    REQUIRE(Count(lines, "ldrb w23", "") == 1);
    REQUIRE(Count(lines, "mov w23", "") == 0);
    REQUIRE(Count(lines, "w23, lsl #24", "") == 1);
}

TEST_CASE("a pinned narrow load keeps a saved flag alias in its fixed home") {
    const auto lines = EmitPinnedRead(PinnedReadShape::LoadU8SavedFlags, false);
    REQUIRE(Count(lines, "ldrb w23", "") == 1);
    REQUIRE(Count(lines, "mov w23", "") == 0);
    REQUIRE(Count(lines, "uxtb ", "") == 0);
    REQUIRE(Count(lines, "w23, lsl #24", "") == 1);
}

TEST_CASE("a pinned GPR supplies a sole memory address without a copy") {
    const auto direct = EmitPinnedRead(PinnedReadShape::MemoryAddress, false,
                                       ValueType::U64);
    REQUIRE(Count(direct, "mov x", "x22") == 0);
    REQUIRE(Count(direct, "ldr w", "[x22]") == 1);

    const auto overwritten = EmitPinnedRead(PinnedReadShape::MemoryAddress, true,
                                            ValueType::U64);
    REQUIRE(Count(overwritten, "ldr w", "[x22]") == 0);
    REQUIRE(Count(overwritten, "mov x", ", x22") == 1);
}

TEST_CASE("a superseded pinned GPR publication is omitted without observers") {
    const auto direct = EmitPinnedRead(PinnedReadShape::OverwrittenWrite, false);
    REQUIRE(Count(direct, "mov w23", "w1") == 1);

    const auto faulting = EmitPinnedRead(PinnedReadShape::OverwrittenWriteFault, false);
    REQUIRE(Count(faulting, "mov w23", "w1") == 2);
}

TEST_CASE("a later pinned GPR capture use keeps the read move") {
    const auto lines = EmitPinnedRead(PinnedReadShape::SelfAnd, true);
    REQUIRE(Count(lines, "ubfx ", "x22") == 1);
}

TEST_CASE("pinned GPR sign extension reads the fixed W view directly") {
    const auto lines = EmitPinnedRead(PinnedReadShape::SignExtend, false);
    REQUIRE(Count(lines, "ubfx ", "x22") == 0);
    REQUIRE(Count(lines, "sxtw ", "w22") == 1);
}

TEST_CASE("pinned GPR memory store reads the fixed W view directly") {
    const auto lines = EmitPinnedRead(PinnedReadShape::StoreMemory, false);
    REQUIRE(Count(lines, "ubfx ", "x22") == 0);
    REQUIRE(Count(lines, "str w22", "[") == 1);
}

TEST_CASE("a narrow store reads a published pinned value directly") {
    const auto lines = EmitPinnedRead(PinnedReadShape::NarrowStoreMemory, false,
                                      ValueType::U32);
    REQUIRE(Count(lines, "strh w22", "[") == 1);
    REQUIRE(Count(lines, "uxth ", "") == 0);
}

TEST_CASE("callee-saved pinned GPR subtraction reads the fixed W view directly") {
    for (auto type : {ValueType::U8, ValueType::U16, ValueType::U32}) {
        CAPTURE(type);
        const auto lines = EmitPinnedRead(PinnedReadShape::Subtract, false, type);
        REQUIRE(Count(lines, "ubfx ", "x22") == 0);
        REQUIRE(std::ranges::any_of(lines, [](const auto& line) {
            return line.find("w22") != std::string::npos;
        }));
        if (type != ValueType::U32) {
            REQUIRE_FALSE(HasShiftPreparation(lines));
        }
    }
}

TEST_CASE("caller-saved pinned narrow flags read the fixed W view directly") {
    for (auto type : {ValueType::U8, ValueType::U16}) {
        CAPTURE(type);
        const auto lines = EmitPinnedRead(
                PinnedReadShape::CallerNarrowSubtract, false, type);
        REQUIRE(Count(lines, "ubfx ", "x7") == 0);
        REQUIRE(std::ranges::any_of(lines, [](const auto& line) {
            return line.find("w7") != std::string::npos;
        }));
    }
}

TEST_CASE("a published narrow load supplies subtraction from its fixed home") {
    for (auto type : {ValueType::U8, ValueType::U16}) {
        CAPTURE(type);
        const auto lines =
                EmitPinnedRead(PinnedReadShape::LoadNarrowSubtract, false, type);
        const auto load = type == ValueType::U8 ? "ldrb w22" : "ldrh w22";
        REQUIRE(Count(lines, load, "") == 1);
        REQUIRE(Count(lines, "mov w22", "") == 0);
        REQUIRE(Count(lines, "subs w", "w22") == 1);
    }
}

TEST_CASE("a caller-saved narrow load supplies subtraction from its fixed home") {
    for (auto type : {ValueType::U8, ValueType::U16}) {
        CAPTURE(type);
        const auto lines = EmitPinnedRead(
                PinnedReadShape::LoadCallerNarrowSubtract, false, type);
        const auto load = type == ValueType::U8 ? "ldrb w2" : "ldrh w2";
        REQUIRE(Count(lines, load, "") == 1);
        REQUIRE(Count(lines, "mov w2", "") == 0);
        REQUIRE(Count(lines, "subs w", "w2") == 1);
    }
}

TEST_CASE("published narrow compare flags execute from the fixed home") {
#if defined(__aarch64__)
    const std::array<swift::u8, 28> code{
            0x48, 0x8b, 0x50, 0x08, 0x0f, 0xb6, 0x12,
            0x66, 0x41, 0x39, 0xd6, 0x41, 0x0f, 0x94, 0xc0,
            0x41, 0x0f, 0x92, 0xc1, 0x41, 0x0f, 0x9c, 0xc2,
            0x41, 0x0f, 0x9a, 0xc3, 0xf4,
    };
    void* guest_code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    std::memcpy(guest_code, code.data(), code.size());

    swift::u8 memory_value{};
    std::array<swift::u64, 2> node{
            0, reinterpret_cast<swift::u64>(&memory_value)};
    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();
    bool matched = true;
    for (swift::u32 index = 0; index < 256 && matched; ++index) {
        memory_value = static_cast<swift::u8>(index * 73u + 19u);
        const auto left = static_cast<swift::u16>(index * 997u + 113u);
        auto* core = swift::translator::x86::X86Core::Make(instance);
        auto& context = core->GetContext();
        context.rip.qword = reinterpret_cast<swift::VAddr>(guest_code);
        context.rax.qword = reinterpret_cast<swift::u64>(node.data());
        context.r14.qword = left;
        core->Run();
        const auto difference = static_cast<swift::u8>(left - memory_value);
        matched = context.rdx.qword == memory_value &&
                static_cast<swift::u8>(context.r8.qword) ==
                        static_cast<swift::u8>(left == memory_value) &&
                static_cast<swift::u8>(context.r9.qword) ==
                        static_cast<swift::u8>(left < memory_value) &&
                static_cast<swift::u8>(context.r10.qword) ==
                        static_cast<swift::u8>(
                                static_cast<swift::s16>(left) <
                                static_cast<swift::s16>(memory_value)) &&
                static_cast<swift::u8>(context.r11.qword) ==
                        static_cast<swift::u8>(std::popcount(difference) % 2 == 0);
        swift::translator::x86::X86Core::Destroy(core);
    }
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(guest_code, 4096);
    REQUIRE(matched);
#else
    SUCCEED("fixed-home execution coverage requires an AArch64 host");
#endif
}

TEST_CASE("a reused pinned GPR memory value keeps the read move") {
    const auto lines = EmitPinnedRead(PinnedReadShape::StoreMemory, true);
    REQUIRE(Count(lines, "ubfx ", "x22") == 1);
}
