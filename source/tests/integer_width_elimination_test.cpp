#include "tests/support/rewrite_fixture.h"
using swift::runtime::test::RewriteFixture;
#include <catch2/catch_test_macros.hpp>

#include "runtime/compile/passes/passes.h"

namespace swift::runtime::ir {
TEST_CASE("integer direct extracts cancel local width round trips") {
    Block block{0, Location{0x1000}};
    const auto source = block.LoadUniform(Uniform{0, ValueType::U32});
    const auto extended = block.ZeroExtend32To64(source);
    block.LoadImm(Imm{7u});
    const auto extracted = block.BitExtract(ValueType::U32, extended, Imm{0u}, Imm{32u});
    const auto left = block.LoadUniform(Uniform{8, ValueType::U32});
    const auto result = block.Add(ValueType::U32, left, Operand{extracted});
    block.StoreUniform(Uniform{16, ValueType::U32}, result);
    const auto direct_source = block.LoadUniform(Uniform{24, ValueType::U32});
    const auto direct = block.BitExtract(ValueType::U32, direct_source, Imm{0u}, Imm{32u});
    const auto direct_result = block.Xor(ValueType::U32, left, Operand{direct});
    block.StoreUniform(Uniform{32, ValueType::U32}, direct_result);
    const auto signed_source = block.LoadUniform(Uniform{40, ValueType::S32});
    const auto signed_extract = block.BitExtract(ValueType::U32, signed_source, Imm{0u}, Imm{32u});
    block.StoreUniform(Uniform{48, ValueType::U32}, signed_extract);
    const auto conversion_source = block.LoadUniform(Uniform{56, ValueType::U32});
    const auto conversion_extract =
            block.BitExtract(ValueType::U32, conversion_source, Imm{0u}, Imm{32u});
    const auto converted =
            block.VecFCvtIntToFloat(ValueType::U64, conversion_extract, Imm{32u}, Imm{64u});
    block.StoreUniform(Uniform{64, ValueType::U64}, converted);
    const auto sign_source = block.LoadUniform(Uniform{72, ValueType::U32});
    const auto sign_extended = block.SignExtend(ValueType::U64, sign_source);
    const auto sign_extract_left =
            block.BitExtract(ValueType::U32, sign_extended, Imm{0u}, Imm{32u});
    const auto sign_extract_right =
            block.BitExtract(ValueType::U32, sign_extended, Imm{0u}, Imm{32u});
    const auto sign_result =
            block.And(ValueType::U32, sign_extract_left, Operand{sign_extract_right});
    block.StoreUniform(Uniform{80, ValueType::U32}, sign_result);

    swift::runtime::compile::NormalizeWidths(RewriteFixture{&block, false}.Get());

    REQUIRE(result.Def()->GetArg<Operand>(1).GetLeft().value.Def() == source.Def());
    REQUIRE(extracted.Def()->GetUses(false) == 0);
    REQUIRE(direct_result.Def()->GetArg<Operand>(1).GetLeft().value.Def() == direct_source.Def());
    REQUIRE(direct.Def()->GetUses(false) == 0);
    REQUIRE(signed_extract.Def()->GetUses(false) == 0);
    REQUIRE(converted.Def()->GetArg<Value>(0).Def() == conversion_source.Def());
    REQUIRE(conversion_extract.Def()->GetUses(false) == 0);
    REQUIRE(sign_result.Def()->GetArg<Value>(0).Def() == sign_source.Def());
    REQUIRE(sign_result.Def()->GetArg<Operand>(1).GetLeft().value.Def() ==
            sign_source.Def());
    REQUIRE(sign_extract_left.Def()->GetUses(false) == 0);
    REQUIRE(sign_extract_right.Def()->GetUses(false) == 0);

    RewriteFixture{&block}.Get().CollectDead();
    u32 extracts = 0;
    u32 extensions = 0;
    u32 sign_extensions = 0;
    for (auto& inst : block.GetInstList()) {
        extracts += inst.GetOp() == OpCode::BitExtract;
        extensions += inst.GetOp() == OpCode::ZeroExtend32To64;
        sign_extensions += inst.GetOp() == OpCode::SignExtend;
    }
    REQUIRE(extracts == 0);
    REQUIRE(extensions == 0);
    REQUIRE(sign_extensions == 0);
}

TEST_CASE("integer width elimination rejects non-direct extracts") {
    Block block{0, Location{0x2000}};
    const auto source = block.LoadUniform(Uniform{0, ValueType::U32});
    const auto extended = block.ZeroExtend32To64(source);
    const auto narrow = block.BitExtract(ValueType::U16, extended, Imm{0u}, Imm{16u});
    block.StoreUniform(Uniform{8, ValueType::U16}, narrow);
    const auto direct = block.BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
    block.SaveFlags(direct, Flags::All);
    const auto flagged = block.BitExtract(ValueType::U32, extended, Imm{0u}, Imm{32u});
    block.SaveFlags(flagged, Flags::All);
    const auto opaque = block.BitExtract(ValueType::U32, source, Imm{0u}, Imm{32u});
    const auto padding = block.LoadImm(ValueType::U64, Imm{u64{0}});
    const auto call =
            block.CallLambda(ValueType::U64, Lambda{Imm{u64{0}}}, opaque, padding, padding);
    (void)call;

    swift::runtime::compile::NormalizeWidths(RewriteFixture{&block, false}.Get());

    REQUIRE(narrow.Def()->GetUses(false) == 1);
    REQUIRE(direct.Def()->GetUses(false) == 1);
    REQUIRE(flagged.Def()->GetUses(false) == 1);
    REQUIRE(CollectPseudoOperations(block, flagged.Def(), OpCode::SaveFlags).size() == 1);
    REQUIRE(opaque.Def()->GetUses(false) == 1);
}

}  // namespace swift::runtime::ir
