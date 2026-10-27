#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>

#include "runtime/frontend/x86/decoder.h"

namespace {

class ArrayMemory final : public swift::runtime::MemoryInterface {
public:
    bool Read(void* dest, size_t addr, size_t size) override {
        return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
    }

    bool Write(void* src, size_t addr, size_t size) override {
        return std::memcpy(reinterpret_cast<void*>(addr), src, size);
    }

    void* GetPointer(void* src) override { return src; }
};

struct DoubleShiftShape {
    size_t immediate_shifts{};
    size_t variable_shifts{};
    size_t selects{};
    size_t flag_guards{};
    size_t carry_writes{};
    size_t overflow_writes{};
};

DoubleShiftShape DecodeDoubleShift(bool right, bool immediate, swift::u8 count) {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    std::array<swift::u8, 8> code{
            0x48,
            0x0f,
            static_cast<swift::u8>(right ? (immediate ? 0xac : 0xad)
                                         : (immediate ? 0xa4 : 0xa5)),
            0xd0,
    };
    code[4] = immediate ? count : 0xf4;
    code[5] = immediate ? 0xf4 : 0x90;
    const auto address = reinterpret_cast<VAddr>(code.data());
    Block block{0, Location{address}};
    swift::runtime::compile::IrBuilder assembler{&block};
    ArrayMemory memory;
    FeatureSet features{};
    features.shift_imm_fast = true;
    X64Decoder decoder{address, &memory, &assembler, true,
                       Arm64Features::None, false, false, features};
    decoder.Decode();

    DoubleShiftShape shape{};
    for (const auto& inst : block.GetInstList()) {
        shape.immediate_shifts += inst.GetOp() == OpCode::LslImm ||
                                  inst.GetOp() == OpCode::LsrImm;
        shape.variable_shifts += inst.GetOp() == OpCode::LslValue ||
                                 inst.GetOp() == OpCode::LsrValue;
        shape.selects += inst.GetOp() == OpCode::Select;
        shape.flag_guards += inst.GetOp() == OpCode::NotGoto;
        shape.carry_writes += inst.GetOp() == OpCode::SetCarry;
        shape.overflow_writes += inst.GetOp() == OpCode::SetOverflow;
    }
    return shape;
}

}  // namespace

TEST_CASE("immediate double shifts use a funnel without a runtime count guard") {
    for (bool right : {false, true}) {
        const auto immediate = DecodeDoubleShift(right, true, 32);
        REQUIRE(immediate.immediate_shifts >= 4);
        REQUIRE(immediate.variable_shifts == 0);
        REQUIRE(immediate.selects == 0);
        REQUIRE(immediate.flag_guards == 0);
        REQUIRE(immediate.carry_writes == 1);
        REQUIRE(immediate.overflow_writes == 1);

        const auto zero = DecodeDoubleShift(right, true, 64);
        REQUIRE(zero.immediate_shifts == 0);
        REQUIRE(zero.carry_writes == 0);
        REQUIRE(zero.overflow_writes == 0);

        const auto variable = DecodeDoubleShift(right, false, 0);
        REQUIRE(variable.immediate_shifts >= 2);
        REQUIRE(variable.variable_shifts >= 3);
        REQUIRE(variable.selects == 1);
        REQUIRE(variable.flag_guards == 1);
    }
}
