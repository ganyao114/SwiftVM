#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/opts/register_alloc_pass.h"

namespace {

template <typename Build>
std::map<std::string, swift::u32> EmitBlock(
        Build&& build,
        std::vector<std::string>* emitted = nullptr,
        bool full_pin = false) {
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
    if (full_pin) {
        module_config.feature_overrides.Set(FeatureId::ra_fixed_class, true);
    }
    auto module = address_space.MapModule(
            LocationDescriptor{0x87a0}, LocationDescriptor{0x87c0}, module_config);

    IntrusivePtr<Block> block{new Block(0, Location{0x87a0})};
    build(*block);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    auto features = ResolveFeatureSet(module_config);
    auto gprs = address_space.GetTrampolines().GetGPRRegs();
    if (full_pin) {
        for (swift::u32 code = 0; code < 32; ++code) {
            if (kX86FixedGPRHomes & (1u << code)) {
                gprs.Mark(code);
            }
        }
    }
    RegAlloc alloc{block->MaxInstrId(),
                   gprs,
                   address_space.GetTrampolines().GetFPRRegs(), features};
    RegisterAllocPass::Run(block.get(), &alloc, false, features);

    alloc.CompleteGPRAllocation(module);
    arm64::JitContext context{module, alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    std::map<std::string, swift::u32> mnemonics;
    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<
            const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<
            const vixl::aarch64::Instruction*>();
    for (auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        std::string_view text{disassembler.GetOutput()};
        const auto begin = text.find_first_not_of(" \t");
        if (begin == std::string_view::npos) {
            continue;
        }
        text.remove_prefix(begin);
        if (emitted) {
            emitted->emplace_back(text);
        }
        const auto end = text.find_first_of(" \t");
        ++mnemonics[std::string{text.substr(0, end)}];
    }

    return mnemonics;
}

std::map<std::string, swift::u32> EmitFlagsPublication(
        swift::runtime::ir::Flags saved_flags) {
    using namespace swift::runtime::ir;
    return EmitBlock([saved_flags](Block& block) {
        auto left = block.LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{0, ValueType::U64});
        auto right = block.LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{8, ValueType::U64});
        auto result = block.Sub(left, Operand{right}).SetType(ValueType::U64);
        block.SaveFlags(result, saved_flags);
    });
}

std::map<std::string, swift::u32> EmitCarryConsumer() {
    using namespace swift::runtime::ir;
    return EmitBlock([](Block& block) {
        auto left = block.LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{0, ValueType::U64});
        auto right = block.LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{8, ValueType::U64});
        auto result = block.Adc(left, Operand{right}).SetType(ValueType::U64);
        block.StoreUniform(Uniform{16, ValueType::U64}, result);
    });
}

std::map<std::string, swift::u32> EmitFlagClear(
        swift::runtime::ir::Flags flags) {
    using namespace swift::runtime::ir;
    return EmitBlock([flags](Block& block) { block.ClearFlags(flags); });
}

void RequireBitfieldMerge(std::map<std::string, swift::u32>& mnemonics) {
    REQUIRE(mnemonics["mrs"] == 1);
    REQUIRE(mnemonics["ubfx"] == 1);
    REQUIRE(mnemonics["bfi"] == 1);
    REQUIRE(mnemonics["and"] == 0);
    REQUIRE(mnemonics["orr"] == 0);
}

bool Contains(const std::vector<std::string>& instructions,
              std::string_view text) {
    return std::ranges::any_of(instructions, [&](const auto& instruction) {
        return instruction.find(text) != std::string::npos;
    });
}

}

TEST_CASE("flags-only arithmetic writes the result token directly") {
    using namespace swift::runtime::ir;

    std::vector<std::string> flags_only;
    const auto mnemonics = EmitBlock([](Block& block) {
        auto left = block.LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{0, ValueType::U64});
        auto right = block.LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{8, ValueType::U64});
        auto add = block.Add(left, Operand{right}).SetType(ValueType::U64);
        block.SaveFlags(add, Flags::All);
        auto sub = block.Sub(left, Operand{right}).SetType(ValueType::U64);
        block.SaveFlags(sub, Flags::All);
        auto logical = block.And(left, Operand{right}).SetType(ValueType::U64);
        block.SaveFlags(logical, Flags::Negate | Flags::Zero | Flags::Parity);
    }, &flags_only);

    REQUIRE(mnemonics.at("adds") == 1);
    REQUIRE(mnemonics.at("subs") == 1);
    REQUIRE(mnemonics.at("ands") == 1);
    if (swift::runtime::GetSvmConfig().flags_regs) {
        REQUIRE(Contains(flags_only, "adds x12"));
        REQUIRE(Contains(flags_only, "subs x12"));
        REQUIRE(Contains(flags_only, "ands x12"));
    } else {
        REQUIRE_FALSE(Contains(flags_only, "adds x12"));
        REQUIRE_FALSE(Contains(flags_only, "subs x12"));
        REQUIRE_FALSE(Contains(flags_only, "ands x12"));
        REQUIRE(mnemonics.at("mrs") == 3);
        REQUIRE(mnemonics.at("bfxil") == 3);
    }
    REQUIRE_FALSE(Contains(flags_only, "mov x12"));

    std::vector<std::string> observed;
    EmitBlock([](Block& block) {
        auto left = block.LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{0, ValueType::U64});
        auto right = block.LoadUniform<TypedValue<ValueType::U64>>(
                Uniform{8, ValueType::U64});
        auto result = block.Sub(left, Operand{right}).SetType(ValueType::U64);
        block.SaveFlags(result, Flags::All);
        block.StoreUniform(Uniform{16, ValueType::U64}, result);
    }, &observed);

    REQUIRE_FALSE(Contains(observed, "subs x12"));
    REQUIRE(Contains(observed, "mov x12") == swift::runtime::GetSvmConfig().flags_regs);
    REQUIRE(Contains(observed, "bfxil x26"));

    std::vector<std::string> pinned;
    EmitBlock([](Block& block) {
        auto left = block.GetHostGPR(HostRegIndex(21), Imm{0u})
                            .SetType(ValueType::U64);
        auto right = block.GetHostGPR(HostRegIndex(20), Imm{0u})
                             .SetType(ValueType::U64);
        auto result = block.Sub(left, Operand{right}).SetType(ValueType::U64);
        block.SaveFlags(result, Flags::All);
        block.SetHostGPR(result, HostRegIndex(21), Imm{0u});
    }, &pinned, true);

    REQUIRE(Contains(pinned, "subs x21"));
    REQUIRE(Contains(pinned, "bfxil x26, x21"));
    REQUIRE_FALSE(Contains(pinned, "mov x12, x21"));

    std::vector<std::string> pinned_u32;
    EmitBlock([](Block& block) {
        auto left = block.GetHostGPR(HostRegIndex(21), Imm{0u})
                            .SetType(ValueType::U32);
        auto right = block.GetHostGPR(HostRegIndex(20), Imm{0u})
                             .SetType(ValueType::U32);
        auto result = block.Sub(left, Operand{right}).SetType(ValueType::U32);
        block.SaveFlags(result, Flags::All);
        auto published = block.ZeroExtend32To64(result).SetType(ValueType::U64);
        block.SetHostGPR(published, HostRegIndex(21), Imm{0u});
    }, &pinned_u32, true);

    REQUIRE(Contains(pinned_u32, "subs w21"));
    REQUIRE_FALSE(Contains(pinned_u32, "mov w12, w21"));

    std::vector<std::string> overwritten;
    EmitBlock([](Block& block) {
        auto left = block.GetHostGPR(HostRegIndex(21), Imm{0u})
                            .SetType(ValueType::U64);
        auto right = block.GetHostGPR(HostRegIndex(20), Imm{0u})
                             .SetType(ValueType::U64);
        auto result = block.Sub(left, Operand{right}).SetType(ValueType::U64);
        block.SaveFlags(result, Flags::All);
        block.SetHostGPR(result, HostRegIndex(21), Imm{0u});
        auto replacement = block.Add(right, Operand{1}).SetType(ValueType::U64);
        block.SetHostGPR(replacement, HostRegIndex(21), Imm{0u});
    }, &overwritten, true);

    REQUIRE(Contains(overwritten, "mov x12, x21") == swift::runtime::GetSvmConfig().flags_regs);
    if (!swift::runtime::GetSvmConfig().flags_regs) {
        REQUIRE(Contains(overwritten, "bfxil x26, x21"));
    }
}

TEST_CASE("full NZCV publication omits the redundant source mask") {
    auto mnemonics = EmitFlagsPublication(swift::runtime::ir::Flags::NZCV);

    REQUIRE(mnemonics["mrs"] == 1);
    REQUIRE(mnemonics["and"] == 1);
    REQUIRE(mnemonics["orr"] == 1);
    REQUIRE(mnemonics["ubfx"] == 0);
    REQUIRE(mnemonics["bfi"] == 0);
}

TEST_CASE("contiguous partial NZCV publication uses a bitfield merge") {
    using swift::runtime::ir::Flags;
    auto nz_mnemonics = EmitFlagsPublication(Flags::Negate | Flags::Zero);
    auto z_mnemonics = EmitFlagsPublication(Flags::Zero);

    RequireBitfieldMerge(nz_mnemonics);
    RequireBitfieldMerge(z_mnemonics);
}

TEST_CASE("noncontiguous partial NZCV publication retains masked merge") {
    using swift::runtime::ir::Flags;
    auto mnemonics = EmitFlagsPublication(Flags::Negate | Flags::Carry);

    REQUIRE(mnemonics["mrs"] == 1);
    REQUIRE(mnemonics["and"] == 2);
    REQUIRE(mnemonics["orr"] == 1);
    REQUIRE(mnemonics["ubfx"] == 0);
    REQUIRE(mnemonics["bfi"] == 0);
}

TEST_CASE("NZCV restore writes the packed flags register directly") {
    auto mnemonics = EmitCarryConsumer();

    REQUIRE(mnemonics["msr"] == 1);
    REQUIRE(mnemonics["and"] == 0);
}

TEST_CASE("contiguous CV and AF clears share one bitfield clear") {
    using swift::runtime::ir::Flags;
    auto compact = EmitFlagClear(Flags::CV | Flags::AuxiliaryCarry);
    auto split = EmitFlagClear(Flags::Carry | Flags::AuxiliaryCarry);

    REQUIRE(compact["bfc"] == 1);
    REQUIRE(compact["and"] == 0);
    REQUIRE(split["bfc"] == 1);
    REQUIRE(split["and"] == 1);
}
