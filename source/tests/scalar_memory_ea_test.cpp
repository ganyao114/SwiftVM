#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <vector>

#include "runtime/common/svm_config.h"
#include "runtime/frontend/x86/decoder.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::ir;

class DirectMemory final : public MemoryInterface {
public:
    bool Read(void* dest, size_t addr, size_t size) override {
        std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        return true;
    }

    bool Write(void* src, size_t addr, size_t size) override {
        std::memcpy(reinterpret_cast<void*>(addr), src, size);
        return true;
    }

    void* GetPointer(void* src) override { return src; }
};

template <std::size_t Size>
std::vector<Operand> DecodeMemoryOperands(
        const std::array<swift::u8, Size>& code, bool direct) {
    const auto address = reinterpret_cast<swift::VAddr>(code.data());
    DirectMemory memory;
    Block block{0, Location{address}};
    Assembler assembler{&block};
    swift::x86::X64Decoder decoder{address,
                                   &memory,
                                   &assembler,
                                   true,
                                   swift::x86::Arm64Features::None,
                                   false,
                                   direct,
                                   FeatureSet{}};
    decoder.Decode();

    std::vector<Operand> operands;
    for (auto& inst : block.GetInstList()) {
        if (inst.GetOp() == OpCode::LoadMemory || inst.GetOp() == OpCode::StoreMemory) {
            operands.push_back(inst.GetArg<Operand>(0));
        }
    }
    return operands;
}

std::vector<Operand> DecodeScalarMemoryOperands(bool direct) {
    return DecodeMemoryOperands(
            std::array<swift::u8, 16>{
                    0xf2, 0x0f, 0x10, 0x48, 0x08,
                    0xf2, 0x0f, 0x58, 0x48, 0x10,
                    0xf2, 0x0f, 0x11, 0x48, 0x18,
                    0xf4,
            },
            direct);
}

}  // namespace

TEST_CASE("x86 memory ordering belongs to each decoder and covers shared stack and TLS") {
    // Each row includes a read and a write followed by HLT. Padding keeps the
    // decoder's bounded fetch inside the buffer, including at the last opcode.
    const std::array<std::array<swift::u8, 32>, 10> inputs{{
            {0x8b, 0x00, 0x89, 0x00, 0xf4},                         // RAX
            {0x8b, 0x45, 0x00, 0x89, 0x45, 0x00, 0xf4},             // RBP
            {0x8b, 0x04, 0x24, 0x89, 0x04, 0x24, 0xf4},             // RSP
            {0x64, 0x8b, 0x00, 0x64, 0x89, 0x00, 0xf4},             // FS
            {0x65, 0x8b, 0x00, 0x65, 0x89, 0x00, 0xf4},             // GS
            {0xf2, 0x0f, 0x10, 0x04, 0x24,
             0xf2, 0x0f, 0x11, 0x04, 0x24, 0xf4},                  // MOVSD
            {0xc5, 0xfa, 0x10, 0x04, 0x24,
             0xc5, 0xfa, 0x11, 0x04, 0x24, 0xf4},                  // VMOVSS
            {0x50, 0x58, 0xf4},                                     // PUSH/POP
            {0xf3, 0x0f, 0x6f, 0x04, 0x24,
             0xf3, 0x0f, 0x7f, 0x04, 0x24, 0xf4},                  // MOVDQU
            {0x66, 0x0f, 0x38, 0x22, 0x04, 0x24,
             0xf3, 0x0f, 0x7f, 0x04, 0x24, 0xf4},                  // PMOVSXBQ
    }};
    for (std::size_t row = 0; row < inputs.size(); ++row) {
        if (row == 6 && !GetSvmConfig().avx) continue;
        CAPTURE(row);
        const auto address = reinterpret_cast<swift::VAddr>(inputs[row].data());
        DirectMemory memory;
        Block ordered{0, Location{address}};
        Block basic{0, Location{address}};
        Assembler ordered_ir{&ordered};
        Assembler basic_ir{&basic};
        swift::x86::X64Decoder first{
                address, &memory, &ordered_ir, true, Arm64Features::None,
                false, true, FeatureSet{}, 0, swift::x86::DecodeStopKind::Internal,
                TsoMode::AcqRel};
        swift::x86::X64Decoder second{
                address, &memory, &basic_ir, true, Arm64Features::None,
                false, true, FeatureSet{}, 0, swift::x86::DecodeStopKind::Internal,
                TsoMode::Relaxed};
        // Construct and run another mode before resuming the first decoder.
        second.Decode();
        first.Decode();
        auto counts = [](Block& block) {
            std::array<std::size_t, 2> result{};
            for (const auto& inst : block.GetInstList()) {
                if (inst.GetOp() == OpCode::LoadMemoryTSO ||
                    inst.GetOp() == OpCode::StoreMemoryTSO) ++result[0];
                if (inst.GetOp() == OpCode::LoadMemory ||
                    inst.GetOp() == OpCode::StoreMemory) ++result[1];
            }
            return result;
        };
        const auto strict = counts(ordered);
        const auto relaxed = counts(basic);
        REQUIRE(strict[0] == 2);
        REQUIRE(strict[1] == 0);
        REQUIRE(relaxed[0] == 0);
        REQUIRE(relaxed[1] == 2);
    }
}

TEST_CASE("scalar SSE memory operands remain composite in direct mode") {
    const auto direct = DecodeScalarMemoryOperands(true);
    const auto biased = DecodeScalarMemoryOperands(false);

    REQUIRE(direct.size() == 3);
    REQUIRE(biased.size() == 3);
    if (GetSvmConfig().addr_ea_tie) {
        for (std::size_t i = 0; i < direct.size(); ++i) {
            REQUIRE(direct[i].GetOp() == OperandOp::Plus);
            REQUIRE(direct[i].GetLeft().IsValue());
            REQUIRE(direct[i].GetRight().IsImm());
            REQUIRE(direct[i].GetRight().imm.Get() == (i + 1) * 8);
        }
    } else {
        for (const auto& operand : direct) {
            REQUIRE(operand.GetRight().Null());
        }
    }
    for (const auto& operand : biased) {
        REQUIRE(operand.GetRight().Null());
        REQUIRE(operand.GetLeft().IsValue());
        REQUIRE(operand.GetLeft().value.Def()->GetOp() == OpCode::GetOperand);
    }
}

TEST_CASE("indexed memory RMW shares its displacement-adjusted address") {
    const auto indexed = DecodeMemoryOperands(
            std::array<swift::u8, 6>{0x83, 0x44, 0x84, 0x10, 0x01, 0xf4},
            true);
    const auto unindexed = DecodeMemoryOperands(
            std::array<swift::u8, 6>{0x83, 0x44, 0x24, 0x1c, 0x01, 0xf4},
            true);

    REQUIRE(indexed.size() == 2);
    REQUIRE(indexed[0].GetOp() == OperandOp::PlusExt);
    REQUIRE(indexed[0].GetLeft().value.Def() ==
            indexed[1].GetLeft().value.Def());
    REQUIRE(indexed[0].GetRight().value.Def() ==
            indexed[1].GetRight().value.Def());

    REQUIRE(unindexed.size() == 2);
    REQUIRE(unindexed[0].GetLeft().value.Def() !=
            unindexed[1].GetLeft().value.Def());
}
