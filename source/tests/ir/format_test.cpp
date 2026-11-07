#include "runtime/ir/format.h"
#include "tests/support/ir_builder.h"

#include <string>

namespace {
using namespace swift::runtime::ir;
using namespace swift::tests::ir_builder;

Operand Imm(std::uint64_t value, Type type = Type::U64) { return Operand::Immediate(value, type); }
}  // namespace

TEST_CASE("IR formatting preserves read views and complete metadata", "[compile][ir][format]") {
    Builder builder{Scope::Block, {TargetMode::Known, 0x64, 0}};
    Need(builder.BeginBlock(Block(builder)));
    const auto input = Constant(builder, 0xfedcba9876543210ULL);
    const auto alias =
            Need(builder.Emit<OpCode::BitCast>(Type::U8, {Operand::ValueOf(input, Type::U8)}));
    const auto sum = Need(builder.Emit<OpCode::Add>(
            Type::U8,
            {Operand::ValueOf(alias), Imm(1, Type::U8)},
            {0x800000000000005fULL, FlagProfile::ArithmeticCanonicalCarry, true, true}));
    for (unsigned i = 0; i < 12; ++i) (void)Constant(builder, i + 100);
    Need(builder.EndBlock(Terminal::Return(sum)));
    const auto program = Submit(builder);
    const auto text = ToString(program);
    REQUIRE(text.find("%0:u64 = LoadImm #0xfedcba9876543210:u64") != std::string::npos);
    REQUIRE(text.find("%1:u8 = BitCast %0:u8") != std::string::npos);
    REQUIRE(text.find("flags=0x800000000000005f profile=2 branch-only=true discard-omitted=true") !=
            std::string::npos);

    struct Output {
        std::string text;
        unsigned chunks{};
    } output;

    FormatTo({&output,
              [](void* context, std::string_view chunk) {
                  auto& sink = *static_cast<Output*>(context);
                  REQUIRE(chunk.size() <= 512);
                  sink.text.append(chunk);
                  ++sink.chunks;
              }},
             program);
    REQUIRE(output.text == text);
    REQUIRE(output.chunks > 1);
}

TEST_CASE("IR formatting consumes logical compound cells", "[compile][ir][format]") {
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Block(builder)));
    const auto source = Constant(builder, 17);
    const auto address =
            Need(builder.Emit<OpCode::GetOperand>(Type::U64,
                                                  {Operand::Expression(OperandOp::AddShift, 2, 3),
                                                   Operand::ValueOf(source),
                                                   Imm(0x123456789abcdef0ULL)}));
    Need(builder.Emit<OpCode::CallLocation>(Type::Void,
                                            {Operand::Lambda(0x1234),
                                             Imm(0x5678),
                                             Operand::Arguments(2),
                                             Operand::ValueOf(source, Type::U32),
                                             Imm(UINT64_MAX, Type::S64)}));
    Need(builder.EndBlock(Terminal::Return(address)));
    const auto program = Submit(builder);
    const auto text = ToString(program);
    REQUIRE(text.find("add-shift<shift=3>(%0:u64, #0x123456789abcdef0:u64)") != std::string::npos);
    REQUIRE(text.find("lambda<0x1234>(#0x5678:u64), args(%0:u32, #0xffffffffffffffff:s64)") !=
            std::string::npos);
}

TEST_CASE("IR formatting preserves actual entry and terminal structure", "[compile][ir][format]") {
    Builder builder{Scope::Function};
    const auto no = Block(builder), yes = Block(builder), entry = Block(builder),
               result = Block(builder), trap = Block(builder);
    Need(builder.SetEntry(entry));
    Need(builder.BeginBlock(entry));
    const auto condition = Constant(builder, 1);
    Need(builder.EndBlock(Terminal::Branch(condition, yes, no)));
    Need(builder.BeginBlock(yes));
    Need(builder.EndBlock(Terminal::Jump(result)));
    Need(builder.BeginBlock(no));
    Need(builder.EndBlock(Terminal::Dispatch()));
    Need(builder.BeginBlock(result));
    Need(builder.EndBlock(Terminal::Return(condition, Type::U8)));
    Need(builder.BeginBlock(trap));
    Need(builder.EndBlock(Terminal::Unreachable()));
    const auto program = Submit(builder);
    const auto text = ToString(program);
    REQUIRE(text.find("scope=function entry=b2") != std::string::npos);
    REQUIRE(text.find("b2 (entry)") != std::string::npos);
    REQUIRE(text.find("branch %0:u64 ? b1 : b0") != std::string::npos);
    REQUIRE(text.find("jump b3") != std::string::npos);
    REQUIRE(text.find("  dispatch\n") != std::string::npos);
    REQUIRE(text.find("  return %0:u8\n") != std::string::npos);
    REQUIRE(text.find("  unreachable\n") != std::string::npos);
    const Program consumed;
    REQUIRE(ToString(consumed) == "program <consumed>\n");
}
