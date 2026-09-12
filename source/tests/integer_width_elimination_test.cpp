#include <catch2/catch_test_macros.hpp>

#include "runtime/ir/opts/deadcode_elimination_pass.h"
#include "runtime/ir/opts/integer_width_elimination_pass.h"

namespace swift::runtime::ir {

TEST_CASE("integer direct extracts cancel local width round trips") {
    Block block{0, Location{0x1000}};
    const auto source = block.LoadUniform(Uniform{0, ValueType::U32});
    const auto extended = block.ZeroExtend32To64(source);
    block.LoadImm(Imm{7u});
    const auto extracted = block.BitExtract(extended, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    const auto left = block.LoadUniform(Uniform{8, ValueType::U32});
    const auto result = block.Add(left, Operand{extracted}).SetType(ValueType::U32);
    block.StoreUniform(Uniform{16, ValueType::U32}, result);
    const auto direct_source = block.LoadUniform(Uniform{24, ValueType::U32});
    const auto direct = block.BitExtract(direct_source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    const auto direct_result = block.Xor(left, Operand{direct}).SetType(ValueType::U32);
    block.StoreUniform(Uniform{32, ValueType::U32}, direct_result);
    const auto signed_source = block.LoadUniform(Uniform{40, ValueType::S32});
    const auto signed_extract =
            block.BitExtract(signed_source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    block.StoreUniform(Uniform{48, ValueType::U32}, signed_extract);
    const auto conversion_source = block.LoadUniform(Uniform{56, ValueType::U32});
    const auto conversion_extract =
            block.BitExtract(conversion_source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    const auto converted =
            block.VecFCvtIntToFloat(conversion_extract, Imm{32u}, Imm{64u}).SetType(ValueType::U64);
    block.StoreUniform(Uniform{64, ValueType::U64}, converted);
    const auto sign_source = block.LoadUniform(Uniform{72, ValueType::U32});
    const auto sign_extended = block.SignExtend(sign_source).SetType(ValueType::U64);
    const auto sign_extract_left =
            block.BitExtract(sign_extended, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    const auto sign_extract_right =
            block.BitExtract(sign_extended, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    const auto sign_result =
            block.And(sign_extract_left, Operand{sign_extract_right}).SetType(ValueType::U32);
    block.StoreUniform(Uniform{80, ValueType::U32}, sign_result);

    IntegerWidthEliminationPass::Run(&block);

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

    DeadCodeEliminationPass::Run(&block);
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
    const auto narrow = block.BitExtract(extended, Imm{0u}, Imm{16u}).SetType(ValueType::U16);
    block.StoreUniform(Uniform{8, ValueType::U16}, narrow);
    const auto direct = block.BitExtract(source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    block.SaveFlags(direct, Flags::All);
    const auto flagged = block.BitExtract(extended, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    block.SaveFlags(flagged, Flags::All);
    const auto opaque = block.BitExtract(source, Imm{0u}, Imm{32u}).SetType(ValueType::U32);
    const auto padding = block.LoadImm(Imm{u64{0}}).SetType(ValueType::U64);
    const auto call =
            block.CallLambda(Lambda{Imm{u64{0}}}, opaque, padding, padding).SetType(ValueType::U64);
    (void)call;

    IntegerWidthEliminationPass::Run(&block);

    REQUIRE(narrow.Def()->GetUses(false) == 1);
    REQUIRE(direct.Def()->GetUses(false) == 1);
    REQUIRE(flagged.Def()->GetUses(false) == 1);
    REQUIRE(flagged.Def()->GetPseudoOperations(OpCode::SaveFlags).size() == 1);
    REQUIRE(opaque.Def()->GetUses(false) == 1);
}

}  // namespace swift::runtime::ir
