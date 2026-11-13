#include "runtime/arch/arm64/numeric.h"
#include "runtime/ir/builder.h"
#include "runtime/ir/optimize.h"
#include "tests/support/check.h"
#include "tests/support/guest_memory.h"
#include "tests/support/native_code.h"
#include "tests/support/test_mode.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace {
using swift::tests::Check;
using swift::tests::NativeMode;
using namespace swift::runtime::ir;
using swift::runtime::ir::Optimize;

Value Emit(Builder& builder,
           OpCode opcode,
           Type type,
           std::initializer_list<Operand> cells,
           OperationMetadata metadata = {}) {
    const auto emitted = builder.Append(opcode, type, cells, metadata);
    CHECK(emitted);
    return emitted.GetValue();
}

Value Literal(Builder& builder, std::uint64_t bits, Type type = Type::U64) {
    return Emit(builder, OpCode::LoadImm, type, {Operand::Immediate(bits, type)});
}

void Store(Builder& builder, std::uint32_t offset, Value value, Type type = Type::U64) {
    Emit(builder,
         OpCode::StoreUniform,
         Type::Void,
         {Operand::Uniform(offset, type), Operand::ValueOf(value, type)});
}

Program Submit(Builder& builder) {
    auto submitted = builder.Submit();
    CHECK(submitted);
    return std::move(submitted.program);
}

std::uint64_t Immediate(const Program& program, Value value, Type type) {
    CHECK(program.Operations()[value.node.value].opcode == OpCode::LoadImm);
    CHECK(program.Operations()[value.node.value].type == type);
    const auto range = program.Operands(value.node);
    auto at = range.begin();
    CHECK(at != range.end());
    const auto cell = *at++;
    CHECK(at == range.end() && cell.Kind() == OperandKind::Immediate && cell.TypeView() == type);
    return cell.Payload();
}

void ArmFPStatusEffects() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    REQUIRE(block);
    REQUIRE(builder.BeginBlock(block.block) == Error::None);
    const auto input =
            Emit(builder,
                 OpCode::VecLoadConst,
                 Type::V128,
                 {Operand::Immediate(0x3f800000, Type::U64), Operand::Immediate(0, Type::U64)});
    const auto before = Literal(builder, 0x08000000, Type::U32);
    Store(builder, 32, before, Type::U32);
    Emit(builder,
         OpCode::ArmFPBinary,
         Type::V128,
         {Operand::ValueOf(input),
          Operand::ValueOf(input),
          Operand::Immediate(28, Type::U32),
          Operand::Immediate(32, Type::U32),
          Operand::Immediate(swift::runtime::arm64::ArmFPBinaryMode(0, 32, 1), Type::U32)});
    const auto status =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(32, Type::U32)});
    Store(builder, 40, status, Type::U32);
    REQUIRE(builder.EndBlock(Terminal::Return()) == Error::None);
    auto optimized = Optimize(Submit(builder));
    REQUIRE(optimized);
    CHECK(std::count_if(optimized.program.Operations().begin(),
                        optimized.program.Operations().end(),
                        [](const auto& op) { return op.opcode == OpCode::ArmFPBinary; }) == 1);
    CHECK(optimized.program.Operations()[status.node.value].opcode == OpCode::LoadUniform);
    CHECK(std::count_if(optimized.program.Operations().begin(),
                        optimized.program.Operations().end(),
                        [](const auto& op) { return op.opcode == OpCode::StoreUniform; }) == 2);
}

void LowByteMaskWriteback() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block);
    CHECK(builder.BeginBlock(block.block) == Error::None);
    const auto parent =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(0, Type::U32)});
    const auto other =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(4, Type::U32)});
    const auto extract = [&](Type view) {
        return Emit(builder,
                    OpCode::BitExtract,
                    Type::U8,
                    {Operand::ValueOf(parent, view),
                     Operand::Immediate(0, Type::U8),
                     Operand::Immediate(8, Type::U8)});
    };
    const auto low = extract(Type::Void), narrow_view = extract(Type::U8);
    const auto masked = Emit(builder,
                             OpCode::And,
                             Type::U8,
                             {Operand::ValueOf(low), Operand::Immediate(0xdf, Type::U8)});
    const auto other_mask = Emit(builder,
                                 OpCode::And,
                                 Type::U8,
                                 {Operand::ValueOf(low), Operand::Immediate(0x7f, Type::U8)});
    const auto narrowed_mask =
            Emit(builder,
                 OpCode::And,
                 Type::U8,
                 {Operand::ValueOf(narrow_view), Operand::Immediate(0xdf, Type::U8)});
    const auto insert = [&](Value whole, Type view, Value byte, unsigned first) {
        return Emit(builder,
                    OpCode::BitInsert,
                    Type::U32,
                    {Operand::ValueOf(whole, view),
                     Operand::ValueOf(byte),
                     Operand::Immediate(first, Type::U8),
                     Operand::Immediate(8, Type::U8)});
    };
    const auto combined = insert(parent, Type::Void, masked, 0);
    const auto combined_other_mask = insert(parent, Type::Void, other_mask, 0);
    const auto different_parent = insert(other, Type::Void, masked, 0);
    const auto partial_parent_view = insert(parent, Type::U8, narrowed_mask, 0);
    const auto high_field = insert(parent, Type::Void, masked, 8);
    Store(builder, 64, masked);  // A real second reader must survive the merge rewrite.
    for (const auto value :
         {combined, combined_other_mask, different_parent, partial_parent_view, high_field})
        Store(builder, static_cast<std::uint32_t>(72 + value.node.value * 8), value);
    CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
    auto input = Submit(builder);
    const auto unit = input.UnitId();
    const auto words = input.OperandArena().size(), nodes = input.Operations().size();
    auto result = Optimize(std::move(input));
    CHECK(result && result.program.UnitId() == unit && result.program.Operations().size() == nodes);
    for (const auto value : {combined, combined_other_mask}) {
        CHECK(result.program.Operations()[value.node.value].opcode == OpCode::And);
        CHECK(result.program.Operations()[value.node.value].type == Type::U32);
        const auto inputs = result.program.Operands(value.node);
        auto at = inputs.begin();
        const auto whole = *at++;
        CHECK(whole.Payload() == parent.node.value && whole.TypeView() == Type::Void);
        const auto mask = *at++;
        CHECK(mask.Kind() == OperandKind::Immediate && mask.TypeView() == Type::U32 &&
              mask.Payload() == (value == combined ? 0xffffffdfULL : 0xffffff7fULL));
        CHECK(at == inputs.end());
    }
    CHECK(result.program.Operations()[masked.node.value].opcode == OpCode::And);
    CHECK(result.program.Operations()[other_mask.node.value].opcode == OpCode::Nop);
    for (const auto value : {different_parent, partial_parent_view, high_field})
        CHECK(result.program.Operations()[value.node.value].opcode == OpCode::BitInsert);
    CHECK(result.stats.words_after <= words + 2 * nodes);
}

void FoldAndDeadCascade() {
    const TargetInfo target{TargetMode::EarlyOff, 0xa64, UINT64_C(0x8000000000000001)};
    Builder builder{Scope::Block, target};
    const auto block = builder.CreateBlock();
    CHECK(block);
    CHECK(builder.BeginBlock(block.block) == Error::None);
    const auto five = Literal(builder, 5), seven = Literal(builder, 7);
    const auto add = Emit(
            builder, OpCode::Add, Type::U64, {Operand::ValueOf(five), Operand::ValueOf(seven)});
    const auto product = Emit(builder,
                              OpCode::Mul,
                              Type::U64,
                              {Operand::ValueOf(add), Operand::Immediate(3, Type::U64)});
    const auto dead = Literal(builder, 99);
    Emit(builder,
         OpCode::Add,
         Type::U64,
         {Operand::ValueOf(dead), Operand::Immediate(1, Type::U64)});
    Store(builder, 0, product);
    CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
    auto input = Submit(builder);
    const auto unit = input.UnitId();
    const auto words = input.OperandArena().size();
    auto result = Optimize(std::move(input));
    CHECK(result && !input && result.program.UnitId() == unit);
    CHECK(result.program.Target() == target && result.program.ScopeKind() == Scope::Block);
    CHECK(result.program.Operations().size() == 7 && result.stats.nodes == 7);
    CHECK(result.stats.folded == 2 && result.stats.erased == 5);
    CHECK(result.stats.dense_bytes == 7 * sizeof(std::uint64_t));
    CHECK(result.stats.block_flags_bytes == 0);
    CHECK(result.stats.words_before == words && result.stats.words_after <= words + 14);
    CHECK(Immediate(result.program, product, Type::U64) == 36);
    for (const auto id : {0u, 1u, 2u, 4u, 5u}) {
        CHECK(result.program.Operations()[id].opcode == OpCode::Nop);
        CHECK(result.program.Operations()[id].type == Type::Void);
        CHECK(result.program.Operands(NodeId{id}).empty());
    }
    CHECK(result.program.Operations()[6].opcode == OpCode::StoreUniform);
    auto again = Optimize(std::move(result.program));
    CHECK(again && again.stats.erased == 0 && again.stats.folded == 0);
    CHECK(again.stats.final_walk_skipped == 1);
}

void CopyAndTerminalRoots() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block);
    CHECK(builder.BeginBlock(block.block) == Error::None);
    const auto source = Emit(builder,
                             OpCode::GetHostGPR,
                             Type::U64,
                             {Operand::Immediate(0, Type::U8), Operand::Immediate(0, Type::U8)});
    const auto first = Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(source)});
    const auto second = Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(first)});
    Store(builder, 0, second);
    CHECK(builder.EndBlock(Terminal::Return(source)) == Error::None);
    auto result = Optimize(Submit(builder));
    CHECK(result && result.stats.copy_forwarded == 2);
    CHECK(result.stats.final_walk_skipped == 0);  // Forwarding removes both copies' last users.
    CHECK(result.program.Operations()[first.node.value].opcode == OpCode::Nop);
    CHECK(result.program.Operations()[second.node.value].opcode == OpCode::Nop);
    CHECK(result.program.Operations()[source.node.value].opcode == OpCode::GetHostGPR);
    const auto cells = result.program.Operands(NodeId{3});
    auto at = cells.begin();
    ++at;
    CHECK((*at).Kind() == OperandKind::Value && (*at).Payload() == source.node.value);
    CHECK(result.program.Blocks()[0].terminal.value == source.node);
    CHECK(result.program.Blocks()[0].terminal.HasValue());
}

void MetadataSensitiveAndObservable() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block);
    CHECK(builder.BeginBlock(block.block) == Error::None);
    const auto value = Literal(builder, 8);
    const auto flags_source = Emit(builder,
                                   OpCode::Add,
                                   Type::U64,
                                   {Operand::ValueOf(value), Operand::Immediate(1, Type::U64)});
    Emit(builder,
         OpCode::SaveFlags,
         Type::Void,
         {Operand::ValueOf(flags_source), Operand::Flags(7)});
    const auto flags_read = Emit(builder,
                                 OpCode::GetFlags,
                                 Type::U64,
                                 {Operand::ValueOf(flags_source), Operand::Flags(7)});
    const OperationMetadata profile{0, FlagProfile::ArithmeticRaw, false};
    const auto profile_only = Emit(builder,
                                   OpCode::Add,
                                   Type::U64,
                                   {Operand::ValueOf(value), Operand::Immediate(2, Type::U64)},
                                   profile);
    const OperationMetadata branch{0, FlagProfile::None, true};
    const auto branch_only = Emit(builder,
                                  OpCode::Sub,
                                  Type::U64,
                                  {Operand::ValueOf(value), Operand::Immediate(3, Type::U64)},
                                  branch);
    const auto address = Literal(builder, 0x1000);
    const auto fault = Emit(builder, OpCode::LoadMemory, Type::U64, {Operand::ValueOf(address)});
    Emit(builder, OpCode::MemoryBarrierTSO, Type::Void, {});
    Emit(builder,
         OpCode::CallLambda,
         Type::Void,
         {Operand::Lambda(), Operand::Immediate(0x1234, Type::U64)});
    const auto location = builder.Append(
            OpCode::FaultLocation, Type::Void, {Operand::Immediate(0x5000, Type::U64)});
    CHECK(location);
    CHECK(builder.EndBlock(Terminal::Return(flags_read)) == Error::None);
    auto result = Optimize(Submit(builder));
    CHECK(result);
    CHECK(result.stats.final_walk_skipped == 1);
    CHECK(result.program.Operations()[flags_source.node.value].opcode == OpCode::Add);
    CHECK(result.program.Operations()[profile_only.node.value].opcode == OpCode::Add);
    CHECK(result.program.Operations()[profile_only.node.value].Metadata() == profile);
    CHECK(result.program.Operations()[branch_only.node.value].opcode == OpCode::Sub);
    CHECK(result.program.Operations()[branch_only.node.value].Metadata() == branch);
    CHECK(result.program.Operations()[fault.node.value].opcode == OpCode::LoadMemory);
    CHECK(result.program.Operations()[address.node.value].opcode == OpCode::LoadImm);
    CHECK(result.program.Operations()[8].opcode == OpCode::MemoryBarrierTSO);
    CHECK(result.program.Operations()[9].opcode == OpCode::CallLambda);
    CHECK(result.program.Operations()[location.node.value].opcode == OpCode::FaultLocation);
    CHECK(result.program.Operands(location.node).front().Payload() == 0x5000);

    // A metadata Nop is an opaque observation in the first walk but is
    // skipped by the final walk. The earlier flag-only producer still dies.
    Builder nop_graph{Scope::Block};
    const auto nop_block = nop_graph.CreateBlock();
    CHECK(nop_block && nop_graph.BeginBlock(nop_block.block) == Error::None);
    const auto input =
            Emit(nop_graph, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    const OperationMetadata arithmetic{FlagBits(Flag::All), FlagProfile::ArithmeticRaw};
    const auto old_flags = Emit(nop_graph,
                                OpCode::Add,
                                Type::U64,
                                {Operand::ValueOf(input), Operand::Immediate(1, Type::U64)},
                                arithmetic);
    const OperationMetadata opaque{FlagBits(Flag::All)};
    const auto nop = nop_graph.Append(OpCode::Nop, Type::Void, {}, opaque);
    CHECK(nop);
    const auto last = Emit(nop_graph,
                           OpCode::Sub,
                           Type::U64,
                           {Operand::ValueOf(input), Operand::Immediate(1, Type::U64)},
                           arithmetic);
    CHECK(nop_graph.EndBlock(Terminal::Return(last)) == Error::None);
    auto cleaned = Optimize(Submit(nop_graph));
    CHECK(cleaned && cleaned.stats.final_walk_skipped == 0);
    CHECK(cleaned.program.Operations()[old_flags.node.value].opcode == OpCode::Nop);
    CHECK(cleaned.program.Operations()[nop.node.value].Metadata() == opaque);
}

void UniformWindows() {
    Builder builder{Scope::Function};
    const auto first = builder.CreateBlock(), second = builder.CreateBlock();
    CHECK(first && second);
    CHECK(builder.BeginBlock(first.block) == Error::None);
    const auto word = Literal(builder, 0x11223344, Type::U32);
    Store(builder, 16, word, Type::U32);
    const auto forwarded =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(16, Type::U32)});
    Store(builder, 32, forwarded, Type::U32);
    const auto byte = Literal(builder, 7, Type::U8);
    Store(builder, 17, byte, Type::U8);  // Overlaps the cached four-byte range.
    const auto overlap =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(16, Type::U32)});
    Store(builder, 36, overlap, Type::U32);
    Emit(builder, OpCode::UniformBarrier, Type::Void, {});
    const auto barrier =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(32, Type::U32)});
    Store(builder, 40, barrier, Type::U32);
    const auto read =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(60, Type::U32)});
    Store(builder, 64, read, Type::U32);
    const auto repeated =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(60, Type::U32)});
    Store(builder, 44, repeated, Type::U32);
    CHECK(builder.EndBlock(Terminal::Jump(second.block)) == Error::None);
    CHECK(builder.BeginBlock(second.block) == Error::None);
    const auto next_block =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(40, Type::U32)});
    Store(builder, 48, next_block, Type::U32);
    CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
    auto result = Optimize(Submit(builder));
    CHECK(result);
    CHECK(result.stats.uniform_forwarded >= 2);
    CHECK(Immediate(result.program, forwarded, Type::U32) == 0x11223344);
    CHECK(result.program.Operations()[overlap.node.value].opcode == OpCode::LoadUniform);
    CHECK(result.program.Operations()[barrier.node.value].opcode == OpCode::LoadUniform);
    CHECK(result.program.Operations()[read.node.value].opcode == OpCode::LoadUniform);
    CHECK(result.program.Operations()[next_block.node.value].opcode == OpCode::LoadUniform);
    CHECK(result.program.Blocks()[0].terminal.first == second.block.id);
}

void ViewWidthAndShiftEdges() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block);
    CHECK(builder.BeginBlock(block.block) == Error::None);
    const auto wide = Literal(builder, UINT64_C(0x8000000000000000));
    const auto zero_shift = Emit(builder,
                                 OpCode::AsrImm,
                                 Type::U64,
                                 {Operand::ValueOf(wide), Operand::Immediate(64, Type::U8)});
    const auto sign_shift = Emit(builder,
                                 OpCode::AsrImm,
                                 Type::U64,
                                 {Operand::ValueOf(wide), Operand::Immediate(63, Type::U8)});
    Store(builder, 0, zero_shift);
    Store(builder, 8, sign_shift);
    const auto bits = Literal(builder, 0x12345678);
    const auto extract = Emit(builder,
                              OpCode::BitExtract,
                              Type::U8,
                              {Operand::ValueOf(bits),
                               Operand::Immediate(8, Type::U32),
                               Operand::Immediate(16, Type::U32)});
    Store(builder, 16, extract, Type::U8);
    const auto narrow = Literal(builder, 0x80, Type::U8);
    const auto widened_view = Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(narrow)});
    Store(builder, 24, widened_view);
    const auto high = Literal(builder, UINT64_C(0x100000000));
    const auto zero_low_view =
            Emit(builder, OpCode::TestZero, Type::Bool, {Operand::ValueOf(high, Type::U32)});
    CHECK(builder.EndBlock(Terminal::Return(zero_low_view)) == Error::None);
    auto result = Optimize(Submit(builder));
    CHECK(result);
    CHECK(Immediate(result.program, zero_shift, Type::U64) == UINT64_C(0x8000000000000000));
    CHECK(Immediate(result.program, sign_shift, Type::U64) == UINT64_MAX);
    CHECK(Immediate(result.program, extract, Type::U8) == 0x56);
    CHECK(result.program.Operations()[widened_view.node.value].opcode == OpCode::BitCast);
    CHECK(result.program.Operations()[widened_view.node.value].type == Type::U64);
    const auto preserved = result.program.Operands(widened_view.node).front();
    CHECK(preserved.Kind() == OperandKind::Value && preserved.Payload() == narrow.node.value &&
          preserved.TypeView() == Type::Void);
    CHECK(Immediate(result.program, narrow, Type::U8) == 0x80);
    CHECK(Immediate(result.program, zero_low_view, Type::Bool) == 1);
}

void BudgetAndUnsupported() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block);
    CHECK(builder.BeginBlock(block.block) == Error::None);
    const auto zero = Emit(builder, OpCode::Zero, Type::U64, {});
    CHECK(builder.EndBlock(Terminal::Return(zero)) == Error::None);
    auto input = Submit(builder);
    const auto unit = input.UnitId();
    auto rejected = Optimize(std::move(input), 0);
    CHECK(!rejected && rejected.error == Error::EditBudgetExceeded && !rejected.allocation_failed);
    CHECK(rejected.program && rejected.program.UnitId() == unit);
    CHECK(rejected.program.Operations()[zero.node.value].opcode == OpCode::Zero);
    CHECK(rejected.program.Operands(zero.node).empty() && rejected.stats.folded == 0);
    auto accepted = Optimize(std::move(rejected.program));
    CHECK(accepted);
    CHECK(Immediate(accepted.program, zero, Type::U64) == 0);

    Builder other{Scope::Block};
    const auto b = other.CreateBlock();
    CHECK(b && other.BeginBlock(b.block) == Error::None);
    const auto value = Literal(other, 17);
    const auto unsupported = Emit(other, OpCode::PopCount, Type::U64, {Operand::ValueOf(value)});
    CHECK(other.EndBlock(Terminal::Return(unsupported)) == Error::None);
    auto unchanged = Optimize(Submit(other));
    CHECK(unchanged);
    CHECK(unchanged.program.Operations()[unsupported.node.value].opcode == OpCode::PopCount);
    CHECK(unchanged.program.Operands(unsupported.node).front().Payload() == value.node.value);
}

void RawAliasesAndNormalizedUniforms() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block);
    CHECK(builder.BeginBlock(block.block) == Error::None);
    const auto raw = Literal(builder, 0x1234);
    const auto alias = Emit(builder, OpCode::BitCast, Type::U8, {Operand::ValueOf(raw, Type::U8)});
    const auto wide_alias =
            Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(alias, Type::U64)});
    Store(builder, 0, wide_alias);
    // This matching-view chain may be shortened, while its raw source still
    // carries 0x1234 for the final U64 view. No alias becomes literal 0x34.
    const auto chain = Emit(builder, OpCode::BitCast, Type::U8, {Operand::ValueOf(alias)});
    const auto chain_wide =
            Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(chain, Type::U64)});
    Store(builder, 8, chain_wide);

    const auto host = Emit(builder,
                           OpCode::GetHostGPR,
                           Type::U64,
                           {Operand::Immediate(0, Type::U8), Operand::Immediate(0, Type::U8)});
    Emit(builder,
         OpCode::StoreUniform,
         Type::Void,
         {Operand::Uniform(32, Type::U32), Operand::ValueOf(host, Type::U32)});
    const auto load =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(32, Type::U32)});
    const auto load_wide =
            Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(load, Type::U64)});
    Store(builder, 40, load_wide);
    const auto constant = Literal(builder, UINT64_C(0x100000001));
    Emit(builder,
         OpCode::StoreUniform,
         Type::Void,
         {Operand::Uniform(48, Type::U32), Operand::ValueOf(constant, Type::U32)});
    const auto constant_load =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(48, Type::U32)});
    const auto constant_wide =
            Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(constant_load, Type::U64)});
    CHECK(builder.EndBlock(Terminal::Return(constant_wide)) == Error::None);
    auto result = Optimize(Submit(builder));
    CHECK(result);
    CHECK(Immediate(result.program, raw, Type::U64) == 0x1234);
    CHECK(result.program.Operations()[alias.node.value].opcode == OpCode::BitCast);
    CHECK(result.program.Operations()[alias.node.value].type == Type::U8);
    CHECK(result.program.Operands(alias.node).front().TypeView() == Type::U8);
    CHECK(result.program.Operations()[chain.node.value].opcode == OpCode::BitCast);
    CHECK(result.program.Operands(chain.node).front().Payload() == raw.node.value);
    CHECK(result.program.Operations()[load.node.value].opcode == OpCode::ZeroExtend32);
    CHECK(result.program.Operands(load.node).front().Payload() == host.node.value);
    CHECK(result.program.Operands(load.node).front().TypeView() == Type::U32);
    // Safe wrappers can die. Check the actual live widened readers instead.
    const auto stored = [&](std::uint32_t node, Value source) {
        const auto args = result.program.Operands(NodeId{node});
        auto at = args.begin();
        ++at;
        CHECK((*at).Kind() == OperandKind::Value && (*at).Payload() == source.node.value);
        CHECK((*at).TypeView() == Type::U64);
    };
    stored(3, alias);
    stored(6, chain);
    stored(11, load);
    CHECK(Immediate(result.program, constant_load, Type::U32) == 1);
    CHECK(result.program.Operands(constant_wide.node).front().TypeView() == Type::U64);
}

void LabelIdentityIsNotADataCopy() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block);
    CHECK(builder.BeginBlock(block.block) == Error::None);
    const auto condition = Literal(builder, 1);
    const auto jump = Emit(builder, OpCode::Goto, Type::U64, {Operand::ValueOf(condition)});
    const auto label = Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(jump)});
    const auto bind = builder.Append(OpCode::BindLabel, Type::Void, {Operand::ValueOf(label)});
    CHECK(bind);
    CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
    auto result = Optimize(Submit(builder));
    CHECK(result);
    CHECK(result.program.Operations()[label.node.value].opcode == OpCode::BitCast);
    CHECK(result.program.Operands(bind.node).front().Payload() == label.node.value);
    CHECK(result.program.Operands(bind.node).front().TypeView() == Type::Void);
}

void UniformReadsObserveFlagWrites() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block && builder.BeginBlock(block.block) == Error::None);
    // CompileOptions can place the packed flags word at any Uniform slot.
    // Every sampled value has a real, non-overlapping observable Store user.
    const auto before =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(8, Type::U64)});
    Store(builder, 128, before);
    const auto input = Literal(builder, 3);
    Emit(builder,
         OpCode::Add,
         Type::U64,
         {Operand::ValueOf(input), Operand::Immediate(1, Type::U64)},
         {FlagBits(Flag::All), FlagProfile::ArithmeticCanonicalCarry});
    const auto metadata =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(8, Type::U64)});
    Store(builder, 136, metadata);
    Emit(builder, OpCode::ClearFlags, Type::Void, {Operand::Flags(Flag::Carry | Flag::Zero)});
    const auto cleared =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(8, Type::U64)});
    Store(builder, 144, cleared);
    Emit(builder, OpCode::SetCarry, Type::Void, {Operand::ValueOf(input)});
    const auto carry =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(8, Type::U64)});
    Store(builder, 152, carry);
    // A read alone does not mutate the cached Uniform value.
    const auto flag_read =
            Emit(builder, OpCode::TestFlags, Type::Bool, {Operand::Flags(Flag::Carry)});
    Store(builder, 160, flag_read, Type::U8);
    const auto read_only =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(8, Type::U64)});
    Store(builder, 168, read_only);
    CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
    auto result = Optimize(Submit(builder));
    CHECK(result);
    for (const auto value : {before, metadata, cleared, carry})
        CHECK(result.program.Operations()[value.node.value].opcode == OpCode::LoadUniform);
    auto published = result.program.Operands(NodeId{read_only.node.value + 1}).begin();
    ++published;
    CHECK((*published).Kind() == OperandKind::Value && (*published).Payload() == carry.node.value);
    CHECK(result.stats.uniform_forwarded == 1);
}

void StreamingParamsRemainComplete() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block);
    CHECK(builder.BeginBlock(block.block) == Error::None);
    std::array<Operand, 23> cells;
    cells[0] = Operand::Lambda();
    cells[1] = Operand::Immediate(0x1234, Type::U64);
    cells[2] = Operand::Arguments(20);
    for (std::size_t i = 0; i < 20; ++i) cells[i + 3] = Operand::ValueOf(Literal(builder, i));
    const auto call = builder.Append(OpCode::CallLocation, Type::Void, cells);
    CHECK(call);
    CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
    auto result = Optimize(Submit(builder));
    CHECK(result);
    CHECK(result.stats.value_uses == 20 && result.stats.erased == 0);
    CHECK(result.program.Operations()[call.node.value].opcode == OpCode::CallLocation);
    const auto args = result.program.Operands(call.node);
    CHECK(std::distance(args.begin(), args.end()) == 23);
}

void UniformReadForwardingKeepsFaultsAndAliases() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block && builder.BeginBlock(block.block) == Error::None);
    const auto gpr =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    const auto flags =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(16, Type::U64)});
    Store(builder, 64, flags);
    Emit(builder, OpCode::SetCarry, Type::Void, {Operand::ValueOf(gpr)});
    const auto address = Literal(builder, 0x1000);
    const auto fault = Emit(builder, OpCode::LoadMemoryTSO, Type::U64, {Operand::ValueOf(address)});
    const auto kept =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    Store(builder, 72, kept);
    const auto changed_flags =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(16, Type::U64)});
    Store(builder, 80, changed_flags);
    Emit(builder,
         OpCode::StoreMemoryTSO,
         Type::Void,
         {Operand::ValueOf(address), Operand::ValueOf(gpr)});
    const auto after_store =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    CHECK(builder.EndBlock(Terminal::Return(after_store)) == Error::None);
    auto result =
            Optimize(Submit(builder), SIZE_MAX, {.flags = swift::runtime::ir::UniformRange{16, 8}});
    CHECK(result);
    auto published = result.program.Operands(NodeId{kept.node.value + 1}).begin();
    ++published;
    CHECK((*published).Kind() == OperandKind::Value && (*published).Payload() == gpr.node.value);
    CHECK(result.program.Operations()[changed_flags.node.value].opcode == OpCode::LoadUniform);
    CHECK(result.program.Operations()[after_store.node.value].opcode == OpCode::LoadUniform);
    CHECK(result.program.Operations()[fault.node.value].opcode == OpCode::LoadMemoryTSO);
}

void CoveredMultiplyFlagsReleaseTheirInputs() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block && builder.BeginBlock(block.block) == Error::None);
    const OperationMetadata arithmetic{
            FlagBits(Flag::All), FlagProfile::ArithmeticCanonicalCarry, false, true};
    const auto a = Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(0, Type::U32)});
    const auto b = Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(8, Type::U32)});
    const auto add = Emit(builder,
                          OpCode::Add,
                          Type::U32,
                          {Operand::ValueOf(a), Operand::Immediate(1, Type::U32)},
                          arithmetic);
    const auto low =
            Emit(builder, OpCode::Mul, Type::U32, {Operand::ValueOf(add), Operand::ValueOf(b)});
    const auto wide_a = Emit(builder, OpCode::SignExtend, Type::U64, {Operand::ValueOf(add)});
    const auto wide_b = Emit(builder, OpCode::SignExtend, Type::U64, {Operand::ValueOf(b)});
    const auto full = Emit(
            builder, OpCode::Mul, Type::U64, {Operand::ValueOf(wide_a), Operand::ValueOf(wide_b)});
    const auto expected = Emit(builder, OpCode::SignExtend, Type::U64, {Operand::ValueOf(low)});
    const auto different = Emit(
            builder, OpCode::Xor, Type::U64, {Operand::ValueOf(full), Operand::ValueOf(expected)});
    const auto overflow =
            Emit(builder, OpCode::TestNotZero, Type::Bool, {Operand::ValueOf(different)});
    const auto carry = builder.Append(OpCode::SetCarry, Type::Void, {Operand::ValueOf(overflow)});
    CHECK(carry);
    const auto sign = builder.Append(OpCode::SetOverflow, Type::Void, {Operand::ValueOf(overflow)});
    CHECK(sign);
    const auto snapshot = Emit(builder,
                               OpCode::GetFlags,
                               Type::U64,
                               {Operand::ValueOf(low), Operand::Flags(Flag::All)});
    const auto restore = builder.Append(
            OpCode::RestoreFlags,
            Type::Void,
            {Operand::ValueOf(snapshot), Operand::Flags(Flag::Carry | Flag::Overflow)});
    CHECK(restore);
    const auto shift = Emit(builder,
                            OpCode::LsrImm,
                            Type::U32,
                            {Operand::ValueOf(low), Operand::Immediate(2, Type::U8)});
    const auto last = Emit(builder,
                           OpCode::And,
                           Type::U32,
                           {Operand::ValueOf(shift), Operand::Immediate(0x7f, Type::U32)},
                           {FlagBits(Flag::All), FlagProfile::LogicalZeroCVA});
    CHECK(builder.EndBlock(Terminal::Return(last)) == Error::None);
    auto result = Optimize(
            Submit(builder), SIZE_MAX, {.flags = swift::runtime::ir::UniformRange{128, 8}});
    CHECK(result);
    CHECK(result.program.Operations()[add.node.value].opcode == OpCode::Add);
    CHECK(result.program.Operations()[add.node.value].Metadata() == OperationMetadata{});
    CHECK(result.program.Operations()[low.node.value].opcode == OpCode::Mul);
    CHECK(result.program.Operations()[shift.node.value].opcode == OpCode::LsrImm);
    CHECK(result.program.Operations()[last.node.value].flag_outputs == FlagBits(Flag::All));
    for (const auto value : {wide_a, wide_b, full, expected, different, overflow, snapshot})
        CHECK(result.program.Operations()[value.node.value].opcode == OpCode::Nop);
    for (const auto id : {carry.node, sign.node, restore.node})
        CHECK(result.program.Operations()[id.value].opcode == OpCode::Nop);
}

void FlagsObserveFaultsAliasesAndBlockExits() {
    Builder builder{Scope::Function};
    const auto first = builder.CreateBlock(), second = builder.CreateBlock();
    CHECK(first && second);
    CHECK(builder.BeginBlock(first.block) == Error::None);
    const auto value =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    const OperationMetadata arithmetic{FlagBits(Flag::All), FlagProfile::ArithmeticCanonicalCarry};
    const auto flags = [&] {
        return Emit(builder,
                    OpCode::Add,
                    Type::U64,
                    {Operand::ValueOf(value), Operand::Immediate(1, Type::U64)},
                    arithmetic);
    };
    const auto logical_flags = [&] {
        return Emit(builder,
                    OpCode::And,
                    Type::U64,
                    {Operand::ValueOf(value), Operand::ValueOf(value)},
                    {FlagBits(Flag::All), FlagProfile::LogicalZeroCVA});
    };
    const auto before_fault = logical_flags();
    const auto address = Literal(builder, 0x1000);
    Emit(builder, OpCode::LoadMemoryTSO, Type::U64, {Operand::ValueOf(address)});
    const auto before_read = logical_flags();
    const auto raw = Emit(builder,
                          OpCode::GetFlags,
                          Type::U64,
                          {Operand::ValueOf(value), Operand::Flags(Flag::Carry)});
    Store(builder, 64, raw);
    const auto before_alias = logical_flags();
    const auto alias =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(128, Type::U64)});
    Store(builder, 72, alias);
    const auto before_helper = logical_flags();
    Emit(builder,
         OpCode::CallLambda,
         Type::Void,
         {Operand::Lambda(), Operand::Immediate(0x1234, Type::U64)});
    const auto partial = flags();
    const auto clear = builder.Append(
            OpCode::ClearFlags, Type::Void, {Operand::Flags(Flag::Carry | Flag::Overflow)});
    CHECK(clear);
    Emit(builder, OpCode::SetCarry, Type::Void, {Operand::ValueOf(value)});
    CHECK(builder.EndBlock(Terminal::Jump(second.block)) == Error::None);
    CHECK(builder.BeginBlock(second.block) == Error::None);
    // An actual fault before the successor overwrites flags requires All
    // across this edge; an internal edge alone no longer implies that.
    Emit(builder, OpCode::LoadMemoryTSO, Type::U64, {Operand::ValueOf(address)});
    const auto next = flags();
    CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
    auto result = Optimize(
            Submit(builder),
            SIZE_MAX,
            {.flags = swift::runtime::ir::UniformRange{128, 8}, .guest_memory_disjoint = true});
    CHECK(result);
    for (const auto source : {before_fault, before_read, before_alias, before_helper, next}) {
        CHECK(result.program.Operations()[source.node.value].flag_outputs == FlagBits(Flag::All));
        CHECK(!result.program.Operations()[source.node.value].Metadata().discard_omitted_flags);
    }
    CHECK(result.program.Operations()[partial.node.value].flag_outputs ==
          (FlagBits(Flag::All) & ~FlagBits(Flag::Carry | Flag::Overflow)));
    CHECK(result.program.Operations()[clear.node.value].opcode == OpCode::ClearFlags);
    CHECK(result.program.Operands(clear.node).front().Payload() == FlagBits(Flag::Overflow));
    CHECK(result.stats.block_flags_bytes != 0);

    // One diamond with an empty loop header: only CF is read on the loop
    // edge, only ZF at the exit, and the real external exit observes All.
    // Five empty entry edges put this same graph beyond local CFG storage.
    Builder graph{Scope::Function};
    const auto entry = graph.CreateBlock(), empty = graph.CreateBlock();
    const auto body = graph.CreateBlock(), exit = graph.CreateBlock();
    CHECK(entry && empty && body && exit);
    std::array<BlockRef, 5> forwarding;
    for (auto& block : forwarding) {
        const auto created = graph.CreateBlock();
        CHECK(created);
        block = created.block;
    }
    CHECK(graph.BeginBlock(entry.block) == Error::None);
    const auto source =
            Emit(graph, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    const auto entry_flags = Emit(graph,
                                  OpCode::Add,
                                  Type::U64,
                                  {Operand::ValueOf(source), Operand::Immediate(1, Type::U64)},
                                  arithmetic);
    CHECK(graph.EndBlock(Terminal::Branch(source, forwarding.front(), exit.block)) == Error::None);
    for (std::size_t i = 0; i < forwarding.size(); ++i) {
        CHECK(graph.BeginBlock(forwarding[i]) == Error::None);
        CHECK(graph.EndBlock(Terminal::Jump(
                      i + 1 < forwarding.size() ? forwarding[i + 1] : empty.block)) == Error::None);
    }
    CHECK(graph.BeginBlock(empty.block) == Error::None);
    CHECK(graph.EndBlock(Terminal::Jump(body.block)) == Error::None);
    CHECK(graph.BeginBlock(body.block) == Error::None);
    const auto adc = Emit(graph,
                          OpCode::Adc,
                          Type::U64,
                          {Operand::ValueOf(source), Operand::Immediate(1, Type::U64)});
    Store(graph, 32, adc);
    const auto invert = graph.Append(OpCode::InvertCarry, Type::Void, {});
    CHECK(invert);
    const auto carry_read =
            Emit(graph, OpCode::TestFlags, Type::Bool, {Operand::Flags(Flag::Carry)});
    Store(graph, 48, carry_read, Type::U8);
    const auto body_flags = Emit(graph,
                                 OpCode::Add,
                                 Type::U64,
                                 {Operand::ValueOf(source), Operand::Immediate(2, Type::U64)},
                                 arithmetic);
    CHECK(graph.EndBlock(Terminal::Branch(carry_read, empty.block, exit.block)) == Error::None);
    CHECK(graph.BeginBlock(exit.block) == Error::None);
    const auto zero_read =
            Emit(graph, OpCode::CondSet, Type::Bool, {Operand::Condition(Condition::EQ)});
    Store(graph, 56, zero_read, Type::U8);
    const auto selected = Emit(graph,
                               OpCode::CondSelect,
                               Type::U64,
                               {Operand::Condition(Condition::NE),
                                Operand::ValueOf(source),
                                Operand::ValueOf(source)});
    Store(graph, 64, selected);
    const auto exit_flags = Emit(graph,
                                 OpCode::Sub,
                                 Type::U64,
                                 {Operand::ValueOf(source), Operand::Immediate(1, Type::U64)},
                                 arithmetic);
    CHECK(graph.EndBlock(Terminal::Return()) == Error::None);
    auto reduced =
            Optimize(Submit(graph), SIZE_MAX, {.flags = swift::runtime::ir::UniformRange{128, 8}});
    CHECK(reduced);
    CHECK(reduced.stats.final_walk_skipped == 0);  // The actual Function exit demands narrowed.
    for (const auto writer : {entry_flags, body_flags}) {
        CHECK(reduced.program.Operations()[writer.node.value].flag_outputs ==
              FlagBits(Flag::Carry | Flag::Zero));
        CHECK(reduced.program.Operations()[writer.node.value].Metadata().discard_omitted_flags);
    }
    CHECK(reduced.program.Operations()[exit_flags.node.value].flag_outputs == FlagBits(Flag::All));
    CHECK(reduced.program.Operations()[adc.node.value].opcode == OpCode::Adc);
    CHECK(reduced.program.Operations()[invert.node.value].opcode == OpCode::InvertCarry);
    CHECK(reduced.program.Blocks()[empty.block.id.value].node_count == 0);
    CHECK(reduced.program.Blocks().size() == 9);

    // Only a real external terminal can omit the Function graph. A single
    // self edge must retain the CFG path even though it also has one block.
    for (const bool self_loop : {false, true}) {
        Builder single{Scope::Function};
        const auto block = single.CreateBlock();
        CHECK(block);
        CHECK(single.BeginBlock(block.block) == Error::None);
        const auto source =
                Emit(single, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
        const auto writer = Emit(single,
                                 OpCode::And,
                                 Type::U64,
                                 {Operand::ValueOf(source), Operand::ValueOf(source)},
                                 {FlagBits(Flag::All), FlagProfile::LogicalZeroCVA});
        CHECK(single.EndBlock(self_loop ? Terminal::Jump(block.block) : Terminal::Return(source)) ==
              Error::None);
        auto local = Optimize(Submit(single));
        CHECK(local);
        CHECK(local.program.ScopeKind() == Scope::Function);
        CHECK((local.stats.block_flags_bytes != 0) == self_loop);
        // Unknown Uniform placement observes All, including on the self edge.
        CHECK(local.stats.final_walk_skipped == 1);
        if (self_loop) CHECK(reduced.stats.block_flags_bytes > local.stats.block_flags_bytes);
        if (!self_loop)
            CHECK(local.program.Operations()[writer.node.value].flag_outputs ==
                  FlagBits(Flag::All));
    }

    // The requested Z mask is already narrow: unchanged keep must still
    // acquire the literal permission when final successor demand is only Z.
    Builder permission{Scope::Function};
    const auto producer = permission.CreateBlock(), consumer = permission.CreateBlock();
    CHECK(producer && consumer);
    CHECK(permission.BeginBlock(producer.block) == Error::None);
    const auto byte =
            Emit(permission, OpCode::LoadUniform, Type::U8, {Operand::Uniform(0, Type::U8)});
    const auto logical_z = Emit(permission,
                                OpCode::And,
                                Type::U8,
                                {Operand::ValueOf(byte), Operand::ValueOf(byte)},
                                {FlagBits(Flag::Zero), FlagProfile::LogicalZeroCVA});
    CHECK(permission.EndBlock(Terminal::Jump(consumer.block)) == Error::None);
    CHECK(permission.BeginBlock(consumer.block) == Error::None);
    const auto nonzero =
            Emit(permission, OpCode::CondSet, Type::Bool, {Operand::Condition(Condition::NE)});
    Store(permission, 8, nonzero, Type::U8);
    const auto observer =
            Emit(permission, OpCode::LoadUniform, Type::U64, {Operand::Uniform(16, Type::U64)});
    const auto complete = Emit(permission,
                               OpCode::And,
                               Type::U8,
                               {Operand::ValueOf(byte), Operand::ValueOf(byte)},
                               {FlagBits(Flag::All), FlagProfile::LogicalZeroCVA});
    CHECK(permission.EndBlock(Terminal::Return(observer)) == Error::None);
    auto permitted = Optimize(
            Submit(permission), SIZE_MAX, {.flags = swift::runtime::ir::UniformRange{128, 8}});
    CHECK(permitted);
    const OperationMetadata specified_z{
            FlagBits(Flag::Zero), FlagProfile::LogicalZeroCVA, false, true};
    CHECK(permitted.program.Operations()[logical_z.node.value].Metadata() == specified_z);
    CHECK(!permitted.program.Operations()[complete.node.value].Metadata().discard_omitted_flags);
    auto edit = Builder::Edit(std::move(permitted.program));
    CHECK(edit.Rewrite(observer.node,
                       OpCode::GetFlags,
                       Type::U64,
                       {Operand::ValueOf(byte), Operand::Flags(Flag::All)}) == Error::None);
    auto observed =
            Optimize(Submit(edit), SIZE_MAX, {.flags = swift::runtime::ir::UniformRange{128, 8}});
    CHECK(observed);
    // An added whole-word observer reads the declared unspecified fields.
    // It neither invalidates a proof nor restores previously omitted flags.
    CHECK(observed.program.Operations()[logical_z.node.value].Metadata() == specified_z);
    CHECK(observed.program.Operations()[observer.node.value].opcode == OpCode::GetFlags);

    // No node, use or output mask changes here. Only the final walk grants
    // the partial writer its omitted-flags permission before a full overwrite.
    Builder local_permission{Scope::Block};
    const auto local_block = local_permission.CreateBlock();
    CHECK(local_block && local_permission.BeginBlock(local_block.block) == Error::None);
    const auto permission_source =
            Emit(local_permission, OpCode::LoadUniform, Type::U8, {Operand::Uniform(0, Type::U8)});
    const auto partial_z =
            Emit(local_permission,
                 OpCode::And,
                 Type::U8,
                 {Operand::ValueOf(permission_source), Operand::ValueOf(permission_source)},
                 {FlagBits(Flag::Zero), FlagProfile::LogicalZeroCVA});
    const auto predicate = Emit(
            local_permission, OpCode::CondSet, Type::Bool, {Operand::Condition(Condition::NE)});
    Store(local_permission, 8, predicate, Type::U8);
    Emit(local_permission,
         OpCode::And,
         Type::U8,
         {Operand::ValueOf(permission_source), Operand::ValueOf(permission_source)},
         {FlagBits(Flag::All), FlagProfile::LogicalZeroCVA});
    CHECK(local_permission.EndBlock(Terminal::Return(permission_source)) == Error::None);
    auto granted = Optimize(Submit(local_permission),
                            SIZE_MAX,
                            {.flags = swift::runtime::ir::UniformRange{128, 8}});
    CHECK(granted && granted.stats.final_walk_skipped == 0);
    CHECK(granted.stats.erased == 0 && granted.stats.folded == 0 &&
          granted.stats.copy_forwarded == 0 && granted.stats.uniform_forwarded == 0);
    CHECK(granted.program.Operations()[partial_z.node.value].Metadata() == specified_z);
}

void SingleNumericReadAbsorbsU32Normalizer() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block && builder.BeginBlock(block.block) == Error::None);
    const auto parent =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    const auto normalize = [&](Type view = Type::U32) {
        return Emit(builder, OpCode::ZeroExtend32, Type::U32, {Operand::ValueOf(parent, view)});
    };
    const auto single = normalize();
    const OperationMetadata flags{FlagBits(Flag::All), FlagProfile::ArithmeticCanonicalCarry};
    const auto sum = Emit(builder,
                          OpCode::Add,
                          Type::U32,
                          {Operand::ValueOf(single), Operand::Immediate(1, Type::U32)},
                          flags);
    Store(builder, 16, sum, Type::U32);
    const auto raw_input = normalize();
    const auto raw = Emit(builder, OpCode::BitCast, Type::U32, {Operand::ValueOf(raw_input)});
    const auto wide = Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(raw, Type::U64)});
    Store(builder, 24, wide);
    const auto shared = normalize();
    const auto first = Emit(builder,
                            OpCode::Add,
                            Type::U32,
                            {Operand::ValueOf(shared), Operand::Immediate(3, Type::U32)});
    const auto second = Emit(builder,
                             OpCode::Sub,
                             Type::U32,
                             {Operand::ValueOf(shared), Operand::Immediate(4, Type::U32)});
    Store(builder, 32, first, Type::U32);
    Store(builder, 36, second, Type::U32);
    const auto byte_input = normalize(Type::U8);
    const auto byte_sum = Emit(builder,
                               OpCode::Add,
                               Type::U32,
                               {Operand::ValueOf(byte_input), Operand::Immediate(6, Type::U32)});
    Store(builder, 40, byte_sum, Type::U32);
    CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
    auto result = Optimize(
            Submit(builder), SIZE_MAX, {.flags = swift::runtime::ir::UniformRange{128, 8}});
    CHECK(result);
    CHECK(result.program.Operations()[single.node.value].opcode == OpCode::Nop);
    CHECK(result.program.Operands(sum.node).front().Payload() == parent.node.value);
    CHECK(result.program.Operands(sum.node).front().TypeView() == Type::U32);
    CHECK(result.program.Operations()[sum.node.value].Metadata() == flags);
    for (const auto retained : {raw_input, shared, byte_input})
        CHECK(result.program.Operations()[retained.node.value].opcode == OpCode::ZeroExtend32);
    CHECK(result.program.Operands(raw.node).front().Payload() == raw_input.node.value);
    CHECK(result.program.Operands(first.node).front().Payload() == shared.node.value);
    CHECK(result.program.Operands(second.node).front().Payload() == shared.node.value);
    CHECK(result.program.Operands(byte_sum.node).front().Payload() == byte_input.node.value);
}

void ExtractShiftAlgebra() {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block && builder.BeginBlock(block.block) == Error::None);
    const auto parent =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    const auto extract = [&](unsigned first = 0, Type view = Type::Void) {
        return Emit(builder,
                    OpCode::BitExtract,
                    Type::U8,
                    {Operand::ValueOf(parent, view),
                     Operand::Immediate(first, Type::U8),
                     Operand::Immediate(8, Type::U8)});
    };
    const auto low = extract();
    const auto normalizer =
            Emit(builder, OpCode::ZeroExtend32, Type::U32, {Operand::ValueOf(low, Type::U32)});
    const auto shifted = Emit(builder,
                              OpCode::LsrImm,
                              Type::U32,
                              {Operand::ValueOf(normalizer), Operand::Immediate(1, Type::U8)});
    Store(builder, 16, shifted, Type::U32);
    // BitExtract's raw carrier survives its narrow parent view; 33 reduces
    // modulo the U32 shift width, rather than the extracted U8 width.
    const auto raw = extract(40, Type::U8);
    const auto reduced = Emit(builder,
                              OpCode::LsrImm,
                              Type::U32,
                              {Operand::ValueOf(raw, Type::U32), Operand::Immediate(33, Type::U8)});
    Store(builder, 20, reduced, Type::U32);
    const auto shared = extract();
    const auto shared_shift =
            Emit(builder,
                 OpCode::LsrImm,
                 Type::U32,
                 {Operand::ValueOf(shared, Type::U32), Operand::Immediate(2, Type::U8)});
    Store(builder, 24, shared, Type::U8);
    Store(builder, 28, shared_shift, Type::U32);
    const auto flagged = extract();
    const OperationMetadata profile{0, FlagProfile::ArithmeticRaw};
    const auto flagged_shift =
            Emit(builder,
                 OpCode::LsrImm,
                 Type::U32,
                 {Operand::ValueOf(flagged, Type::U32), Operand::Immediate(1, Type::U8)},
                 profile);
    Store(builder, 32, flagged_shift, Type::U32);
    const auto narrow = extract();
    const auto narrow_shift = Emit(builder,
                                   OpCode::LsrImm,
                                   Type::U32,
                                   {Operand::ValueOf(narrow), Operand::Immediate(1, Type::U8)});
    Store(builder, 36, narrow_shift, Type::U32);
    const auto zero = extract();
    const auto zero_shift =
            Emit(builder,
                 OpCode::LsrImm,
                 Type::U32,
                 {Operand::ValueOf(zero, Type::U32), Operand::Immediate(32, Type::U8)});
    Store(builder, 40, zero_shift, Type::U32);
    const OperationMetadata logical{FlagBits(Flag::Carry | Flag::Zero),
                                    FlagProfile::LogicalZeroCVA};
    const auto bit = extract(0, Type::Bool);
    const auto masked = Emit(builder,
                             OpCode::And,
                             Type::U8,
                             {Operand::ValueOf(bit), Operand::Immediate(1, Type::U8)},
                             logical);
    const auto observed = Emit(
            builder, OpCode::TestFlags, Type::Bool, {Operand::Flags(Flag::Carry | Flag::Zero)});
    Store(builder, 44, observed, Type::U8);
    const auto wide_bit = extract();
    const auto wide_masked =
            Emit(builder,
                 OpCode::And,
                 Type::U8,
                 {Operand::ValueOf(wide_bit, Type::U32), Operand::Immediate(1, Type::U8)},
                 logical);
    const auto wide_observed = Emit(
            builder, OpCode::TestFlags, Type::Bool, {Operand::Flags(Flag::Carry | Flag::Zero)});
    Store(builder, 48, wide_observed, Type::U8);
    // Keep the real birth chain: walk2 first combines the shift, then sees
    // the resulting U32 extract through this later low8 observation.
    const auto clip_low = extract(0, Type::U8);
    const auto clip_normalizer =
            Emit(builder, OpCode::ZeroExtend32, Type::U32, {Operand::ValueOf(clip_low, Type::U32)});
    const auto clip_shift =
            Emit(builder,
                 OpCode::LsrImm,
                 Type::U32,
                 {Operand::ValueOf(clip_normalizer), Operand::Immediate(3, Type::U8)});
    const auto clipped = Emit(builder,
                              OpCode::BitExtract,
                              Type::U8,
                              {Operand::ValueOf(clip_shift),
                               Operand::Immediate(0, Type::U8),
                               Operand::Immediate(8, Type::U8)});
    Store(builder, 52, clipped, Type::U8);
    CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
    auto input = Submit(builder);
    const auto unit = input.UnitId();
    const auto words = input.OperandArena().size(), nodes = input.Operations().size();
    auto result = Optimize(std::move(input));
    CHECK(result);
    CHECK(result.program.UnitId() == unit && result.program.Operations().size() == nodes);
    CHECK(result.stats.folded == 5 && result.stats.erased == 8 && result.stats.copy_forwarded == 4);
    CHECK(result.stats.words_after <= words + 2 * nodes);
    const auto combined = [&](Value value, unsigned first, Type view) {
        const auto& op = result.program.Operations()[value.node.value];
        CHECK(op.opcode == OpCode::BitExtract && op.type == Type::U32 &&
              op.block == block.block.id);
        const auto inputs = result.program.Operands(value.node);
        auto at = inputs.begin();
        const auto source = *at++, offset = *at++, count = *at++;
        CHECK(at == inputs.end() && source.Kind() == OperandKind::Value &&
              source.Payload() == parent.node.value);
        CHECK(source.TypeView() == view && offset.Kind() == OperandKind::Immediate &&
              offset.Payload() == first);
        CHECK(count.Kind() == OperandKind::Immediate && count.Payload() == 7);
    };
    combined(shifted, 1, Type::Void);
    combined(reduced, 41, Type::U8);
    CHECK(result.program.Operations()[low.node.value].opcode == OpCode::Nop);
    CHECK(result.program.Operations()[normalizer.node.value].opcode == OpCode::Nop);
    CHECK(result.program.Operations()[raw.node.value].opcode == OpCode::Nop);
    for (const auto value : {shared, flagged, narrow, zero})
        CHECK(result.program.Operations()[value.node.value].opcode == OpCode::BitExtract);
    for (const auto value : {shared_shift, flagged_shift, narrow_shift})
        CHECK(result.program.Operations()[value.node.value].opcode == OpCode::LsrImm);
    CHECK(result.program.Operations()[zero_shift.node.value].opcode == OpCode::Nop);
    CHECK(result.program.Operations()[flagged_shift.node.value].Metadata() == profile);
    CHECK(result.program.Operations()[bit.node.value].opcode == OpCode::Nop);
    CHECK(result.program.Operations()[masked.node.value].opcode == OpCode::And);
    CHECK(result.program.Operations()[masked.node.value].type == Type::U8);
    CHECK(result.program.Operations()[masked.node.value].Metadata() == logical);
    CHECK(result.program.Operands(masked.node).front().Payload() == parent.node.value);
    CHECK(result.program.Operands(masked.node).front().TypeView() == Type::U8);
    CHECK(result.program.Operations()[wide_bit.node.value].opcode == OpCode::BitExtract);
    CHECK(result.program.Operands(wide_masked.node).front().Payload() == wide_bit.node.value);
    CHECK(result.program.Operands(wide_masked.node).front().TypeView() == Type::U32);
    CHECK(result.program.Operations()[wide_masked.node.value].Metadata() == logical);
    for (const auto value : {clip_low, clip_normalizer, clip_shift})
        CHECK(result.program.Operations()[value.node.value].opcode == OpCode::Nop);
    CHECK(result.program.Operations()[clipped.node.value].opcode == OpCode::BitExtract);
    CHECK(result.program.Operations()[clipped.node.value].type == Type::U8);
    auto clip_inputs = result.program.Operands(clipped.node).begin();
    const auto clip_parent = *clip_inputs++, clip_first = *clip_inputs++, clip_count = *clip_inputs;
    CHECK(clip_parent.Payload() == parent.node.value && clip_parent.TypeView() == Type::U8);
    CHECK(clip_first.Payload() == 3 && clip_count.Payload() == 5);
}

void SignedWidenMultiplyAlgebra() {
    constexpr auto capability = static_cast<std::uint64_t>(TargetFeature::VectorSignedMulWidenLow);
    const auto scene = [&](TargetInfo target, bool fuse) {
        Builder builder{Scope::Block, target};
        const auto block = builder.CreateBlock();
        CHECK(block && builder.BeginBlock(block.block) == Error::None);
        // Low signed halfwords are {-1,2,-3,-32768} and {-1,-4,5,-32768}.
        const auto left_bits = Literal(builder, UINT64_C(0x8000fffd0002ffff));
        const auto right_bits = Literal(builder, UINT64_C(0x80000005fffcffff));
        const auto left =
                Emit(builder, OpCode::VecDup64, Type::V128, {Operand::ValueOf(left_bits)});
        const auto right =
                Emit(builder, OpCode::VecDup64, Type::V128, {Operand::ValueOf(right_bits)});
        const auto extend = [&](Value parent, Type view) {
            return Emit(builder,
                        OpCode::VecExtend,
                        Type::V128,
                        {Operand::ValueOf(parent, view),
                         Operand::Immediate(16, Type::U8),
                         Operand::Immediate(32, Type::U8),
                         Operand::Immediate(0, Type::U8)});
        };
        const auto published = extend(left, Type::V128);
        Store(builder, 0, published, Type::V128);
        const auto publication = NodeId{published.node.value + 1};
        Emit(builder, OpCode::FaultLocation, Type::Void, {Operand::Immediate(0x1234, Type::U64)});
        Emit(builder, OpCode::LoadMemoryTSO, Type::U8, {Operand::ValueOf(left_bits)});
        const auto exclusive = extend(right, Type::Void);
        const auto product =
                Emit(builder,
                     OpCode::VecMadd16,
                     Type::V128,
                     {Operand::ValueOf(published), Operand::ValueOf(exclusive, Type::V128)});
        const auto accumulator =
                Emit(builder, OpCode::LoadUniform, Type::V128, {Operand::Uniform(64, Type::V128)});
        const auto sum = Emit(builder,
                              OpCode::VecAdd,
                              Type::V128,
                              {Operand::ValueOf(accumulator),
                               Operand::ValueOf(product),
                               Operand::Immediate(32, Type::U8)});
        Store(builder, 64, sum, Type::V128);
        Store(builder, 80, product, Type::V128);
        const auto snapshot = NodeId{sum.node.value + 2};
        CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
        auto input = Submit(builder);
        const auto unit = input.UnitId();
        const auto words = input.OperandArena().size(), nodes = input.Operations().size();
        auto result = Optimize(std::move(input), words + 2 * nodes);
        CHECK(result);
        CHECK(result.program.UnitId() == unit && result.program.Target() == target);
        CHECK(result.program.Operations().size() == nodes && result.stats.words_before == words);
        CHECK(result.stats.words_after <= words + 2 * nodes);
        const auto& operation = result.program.Operations()[product.node.value];
        CHECK(operation.type == Type::V128 && operation.block == block.block.id &&
              operation.Metadata() == OperationMetadata{});
        CHECK(result.program.Operations()[published.node.value].opcode == OpCode::VecExtend);
        CHECK(result.program.Operations()[publication.value].opcode == OpCode::StoreUniform);
        auto published_use = result.program.Operands(publication).begin();
        ++published_use;
        CHECK((*published_use).Payload() == published.node.value);
        auto add_use = result.program.Operands(sum.node).begin();
        ++add_use;
        CHECK((*add_use).Payload() == product.node.value);
        auto snapshot_use = result.program.Operands(snapshot).begin();
        ++snapshot_use;
        CHECK((*snapshot_use).Payload() == product.node.value);
        const auto inputs = result.program.Operands(product.node);
        auto at = inputs.begin();
        const auto first = *at++, second = *at++;
        if (fuse) {
            CHECK(operation.opcode == OpCode::VecSignedMulWidenLow && operation.operand_count == 3);
            CHECK(first.Payload() == left.node.value && first.TypeView() == Type::V128);
            CHECK(second.Payload() == right.node.value && second.TypeView() == Type::Void);
            const auto bits = *at++;
            CHECK(at == inputs.end() && bits.Kind() == OperandKind::Immediate &&
                  bits.Payload() == 16);
            CHECK(result.program.Operations()[exclusive.node.value].opcode == OpCode::Nop);
            CHECK(result.stats.folded == 1 && result.stats.erased == 1 &&
                  result.stats.words_after == words + 3);
            const auto a = Immediate(result.program, left_bits, Type::U64),
                       b = Immediate(result.program, right_bits, Type::U64);
            constexpr std::array<std::int32_t, 4> expected{1, -8, -15, 1073741824};
            for (unsigned lane = 0; lane < expected.size(); ++lane) {
                const auto signed_half = [&](std::uint64_t value) {
                    const auto raw = static_cast<std::int32_t>((value >> (16 * lane)) & 0xffff);
                    return raw < 0x8000 ? raw : raw - 0x10000;
                };
                CHECK(signed_half(a) * signed_half(b) == expected[lane]);
            }
        } else {
            CHECK(operation.opcode == OpCode::VecMadd16 && at == inputs.end());
            CHECK(first.Payload() == published.node.value &&
                  second.Payload() == exclusive.node.value);
            CHECK(result.program.Operations()[exclusive.node.value].opcode == OpCode::VecExtend);
            CHECK(result.stats.folded == 0 && result.stats.erased == 0 &&
                  result.stats.words_after == words);
        }
    };
    scene({TargetMode::Known, 0xa64, capability}, true);
    scene({TargetMode::Known, 0xa64, 0}, false);
    scene({TargetMode::Unknown, 0xa64, capability}, false);
    scene({TargetMode::EarlyOff, 0xa64, capability}, false);
}

void OrderedLoadAddresses() {
    Builder builder{Scope::Block, {TargetMode::Unknown, 0xa64, 0}};
    const auto block = builder.CreateBlock();
    CHECK(block && builder.BeginBlock(block.block) == Error::None);
    const auto base =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    const auto address =
            [&](OpCode opcode, Value parent, std::uint64_t amount, Type view = Type::Void) {
                return Emit(
                        builder,
                        opcode,
                        Type::U64,
                        {Operand::ValueOf(parent, view), Operand::Immediate(amount, Type::U64)});
            };
    const auto load = [&](Value at) {
        return Emit(builder, OpCode::LoadMemoryTSO, Type::U16, {Operand::ValueOf(at)});
    };
    const auto positive = address(OpCode::Add, base, 2, Type::U64);
    const auto first = load(positive);
    const auto negative = address(OpCode::Add, base, ~std::uint64_t{});
    const auto second = load(negative);  // A wide negative literal needs four actual edit words.
    const auto subtract = address(OpCode::Sub, base, 256);
    const auto third = load(subtract);
    const auto shared = address(OpCode::Add, base, 8);
    const auto fourth = load(shared);
    Store(builder, 64, shared);
    const auto narrow =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(8, Type::U32)});
    const auto widened = address(OpCode::Add, narrow, 2, Type::U64);
    const auto fifth = load(widened);  // A wide view does not grant a natural U64 base.
    CHECK(builder.EndBlock(Terminal::Return(first)) == Error::None);
    auto input = Submit(builder);
    const auto words = input.OperandArena().size(), nodes = input.Operations().size();
    const auto unit = input.UnitId();
    auto result = Optimize(std::move(input), words + 2 * nodes);
    CHECK(result);
    CHECK(result.program.UnitId() == unit && result.program.Target().mode == TargetMode::Unknown);
    CHECK(result.program.Operations().size() == nodes && result.stats.words_before == words);
    CHECK(result.stats.folded == 3 && result.stats.erased == 3 &&
          result.stats.words_after == words + 10);
    const auto folded =
            [&](Value loaded, Value removed, OperandOp opcode, std::uint64_t amount, Type view) {
                const auto& op = result.program.Operations()[loaded.node.value];
                CHECK(op.opcode == OpCode::LoadMemoryTSO && op.type == Type::U16 &&
                      op.Metadata() == OperationMetadata{});
                CHECK(result.program.Operations()[removed.node.value].opcode == OpCode::Nop);
                const auto inputs = result.program.Operands(loaded.node);
                auto at = inputs.begin();
                const auto expression = *at++, parent = *at++, literal = *at++;
                CHECK(at == inputs.end() && expression.Kind() == OperandKind::Expression &&
                      expression.Count() == 2);
                CHECK(expression.ExpressionOp() == opcode && !expression.ShiftAmount());
                CHECK(parent.Kind() == OperandKind::Value && parent.Payload() == base.node.value &&
                      parent.TypeView() == view);
                CHECK(literal.Kind() == OperandKind::Immediate && literal.TypeView() == Type::U64 &&
                      literal.Payload() == amount);
            };
    folded(first, positive, OperandOp::Add, 2, Type::U64);
    folded(second, negative, OperandOp::Add, ~std::uint64_t{}, Type::Void);
    folded(third, subtract, OperandOp::Sub, 256, Type::Void);
    CHECK(result.program.Operations()[shared.node.value].opcode == OpCode::Add);
    CHECK(result.program.Operands(fourth.node).front().Kind() == OperandKind::Value);
    CHECK(result.program.Operands(fourth.node).front().Payload() == shared.node.value);
    CHECK(result.program.Operations()[widened.node.value].opcode == OpCode::Add);
    CHECK(result.program.Operands(fifth.node).front().Payload() == widened.node.value);
}

void ReusedRowsKeepOnlyCurrentRequest() {
    std::vector<std::uint64_t> rows(256, ~std::uint64_t{});
    Program reusable;
    for (unsigned dead_count : {12u, 1u, 17u}) {
        const auto scope = dead_count == 1 ? Scope::Function : Scope::Block;
        Builder builder{scope, {}, std::move(reusable)};
        const auto block = builder.CreateBlock();
        CHECK(block);
        CHECK(builder.BeginBlock(block.block) == Error::None);
        const auto dead = Literal(builder, 99);
        for (unsigned i = 1; i < dead_count; ++i) Literal(builder, i);
        const auto left = Literal(builder, 7), right = Literal(builder, 11);
        const auto sum = Emit(
                builder, OpCode::Add, Type::U64, {Operand::ValueOf(left), Operand::ValueOf(right)});
        CHECK(builder.EndBlock(Terminal::Return(sum)) == Error::None);
        auto result = Optimize(Submit(builder), rows);
        CHECK(result);
        CHECK(result.program.ScopeKind() == scope);
        CHECK(result.program.Operations()[dead.node.value].opcode == OpCode::Nop);
        CHECK(Immediate(result.program, sum, Type::U64) == 18);
        reusable = std::move(result.program);
    }
}
}  // namespace

TEST_CASE("Unused ARM FP results retain FPSR effects", "[compile][ir-optimize]") {
    ArmFPStatusEffects();
}

TEST_CASE("ReusedRowsKeepOnlyCurrentRequest", "[compile][ir-optimize]") {
    CHECK_NOTHROW(ReusedRowsKeepOnlyCurrentRequest());
}

TEST_CASE("FoldAndDeadCascade", "[compile][ir-optimize]") { CHECK_NOTHROW(FoldAndDeadCascade()); }

TEST_CASE("LowByteMaskWriteback", "[compile][ir-optimize]") {
    CHECK_NOTHROW(LowByteMaskWriteback());
}

TEST_CASE("CopyAndTerminalRoots", "[compile][ir-optimize]") {
    CHECK_NOTHROW(CopyAndTerminalRoots());
}

TEST_CASE("MetadataSensitiveAndObservable", "[compile][ir-optimize]") {
    CHECK_NOTHROW(MetadataSensitiveAndObservable());
}

TEST_CASE("UniformWindows", "[compile][ir-optimize]") { CHECK_NOTHROW(UniformWindows()); }

TEST_CASE("ViewWidthAndShiftEdges", "[compile][ir-optimize]") {
    CHECK_NOTHROW(ViewWidthAndShiftEdges());
}

TEST_CASE("BudgetAndUnsupported", "[compile][ir-optimize]") {
    CHECK_NOTHROW(BudgetAndUnsupported());
}

TEST_CASE("RawAliasesAndNormalizedUniforms", "[compile][ir-optimize]") {
    CHECK_NOTHROW(RawAliasesAndNormalizedUniforms());
}

TEST_CASE("LabelIdentityIsNotADataCopy", "[compile][ir-optimize]") {
    CHECK_NOTHROW(LabelIdentityIsNotADataCopy());
}

TEST_CASE("UniformReadsObserveFlagWrites", "[compile][ir-optimize]") {
    CHECK_NOTHROW(UniformReadsObserveFlagWrites());
}

TEST_CASE("UniformReadForwardingKeepsFaultsAndAliases", "[compile][ir-optimize]") {
    CHECK_NOTHROW(UniformReadForwardingKeepsFaultsAndAliases());
}

TEST_CASE("StreamingParamsRemainComplete", "[compile][ir-optimize]") {
    CHECK_NOTHROW(StreamingParamsRemainComplete());
}

TEST_CASE("CoveredMultiplyFlagsReleaseTheirInputs", "[compile][ir-optimize]") {
    CHECK_NOTHROW(CoveredMultiplyFlagsReleaseTheirInputs());
}

TEST_CASE("FlagsObserveFaultsAliasesAndBlockExits", "[compile][ir-optimize]") {
    CHECK_NOTHROW(FlagsObserveFaultsAliasesAndBlockExits());
}

TEST_CASE("SingleNumericReadAbsorbsU32Normalizer", "[compile][ir-optimize]") {
    CHECK_NOTHROW(SingleNumericReadAbsorbsU32Normalizer());
}

TEST_CASE("ExtractShiftAlgebra", "[compile][ir-optimize]") { CHECK_NOTHROW(ExtractShiftAlgebra()); }

TEST_CASE("SignedWidenMultiplyAlgebra", "[compile][ir-optimize]") {
    CHECK_NOTHROW(SignedWidenMultiplyAlgebra());
}

TEST_CASE("OrderedLoadAddresses", "[compile][ir-optimize]") {
    CHECK_NOTHROW(OrderedLoadAddresses());
}

TEST_CASE("FP status invalidates only its bound Uniform field", "[compile][floating]") {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block && builder.BeginBlock(block.block) == Error::None);
    const auto status =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(16, Type::U32)});
    const auto gpr =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    Store(builder, 64, status, Type::U32);
    Store(builder, 80, gpr);
    const auto vector =
            Emit(builder, OpCode::LoadUniform, Type::V128, {Operand::Uniform(128, Type::V128)});
    const auto division = Emit(builder,
                               OpCode::VecFDivScalar32,
                               Type::V128,
                               {Operand::ValueOf(vector), Operand::ValueOf(vector)});
    const auto changed =
            Emit(builder, OpCode::LoadUniform, Type::U32, {Operand::Uniform(16, Type::U32)});
    Store(builder, 68, changed, Type::U32);
    const auto unchanged =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    CHECK(builder.EndBlock(Terminal::Return(unchanged)) == Error::None);
    auto optimized =
            Optimize(Submit(builder),
                     SIZE_MAX,
                     {.guest_memory_disjoint = true, .numeric_control = UniformRange{16, 4}});
    CHECK(optimized);
    CHECK(optimized.program.Operations()[division.node.value].opcode == OpCode::VecFDivScalar32);
    CHECK(optimized.program.Operations()[changed.node.value].opcode == OpCode::LoadUniform);
    CHECK(optimized.program.Operations()[unchanged.node.value].opcode == OpCode::ZeroExtend64);
    CHECK(optimized.program.Operands(unchanged.node).front().Payload() == gpr.node.value);
}

TEST_CASE("Guest safepoint retains predecessor flags", "[compile][ir-optimize]") {
    Builder builder{Scope::Function};
    const auto first = builder.CreateBlock(), second = builder.CreateBlock();
    REQUIRE(bool(first));
    REQUIRE(bool(second));
    CHECK(builder.BeginBlock(first.block) == Error::None);
    const auto left = Literal(builder, 7), right = Literal(builder, 9);
    const auto producer = Emit(builder,
                               OpCode::Add,
                               Type::U64,
                               {Operand::ValueOf(left), Operand::ValueOf(right)},
                               {FlagBits(Flag::All), FlagProfile::ArithmeticCanonicalCarry, false});
    CHECK(builder.EndBlock(Terminal::Jump(second.block)) == Error::None);
    CHECK(builder.BeginBlock(second.block) == Error::None);
    Emit(builder, OpCode::GuestSafepoint, Type::Void, {Operand::Immediate(0x1234, Type::U64)});
    Emit(builder, OpCode::ClearFlags, Type::Void, {Operand::Flags(Flag::All)});
    CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
    auto result = Optimize(Submit(builder));
    REQUIRE(bool(result));
    const auto& retained = result.program.Operations()[producer.node.value];
    CHECK(retained.opcode == OpCode::Add);
    CHECK(retained.flag_outputs == FlagBits(Flag::All));
    const auto& observation = result.program.Operations()[result.program.Blocks()[1].first_node];
    CHECK(observation.opcode == OpCode::GuestSafepoint);
}

TEST_CASE("Uniform overwritten stores honor observations and partial ranges",
          "[compile][ir-optimize]") {
    for (unsigned observer = 0; observer < 6; ++observer) {
        Builder builder{Scope::Block};
        const auto block = builder.CreateBlock();
        REQUIRE((block && builder.BeginBlock(block.block) == Error::None));
        const auto first = Literal(builder, 7), second = Literal(builder, 11);
        const auto old = NodeId{static_cast<std::uint32_t>(builder.Operations().size())};
        Store(builder, 8, first);
        if (observer == 1) {
            const auto read =
                    Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(8, Type::U64)});
            Store(builder, 32, read);
        } else if (observer == 2) {
            Emit(builder, OpCode::LoadMemory, Type::U64, {Operand::ValueOf(first)});
        } else if (observer == 3) {
            Emit(builder, OpCode::UniformBarrier, Type::Void, {});
        } else if (observer == 4) {
            const auto read =
                    Emit(builder, OpCode::LoadUniform, Type::U16, {Operand::Uniform(8, Type::U16)});
            Store(builder, 32, read, Type::U16);
        }
        Store(builder, observer == 5 ? 12 : 8, second, observer == 5 ? Type::U32 : Type::U64);
        REQUIRE(builder.EndBlock(Terminal::Return()) == Error::None);
        const auto result = Optimize(Submit(builder), SIZE_MAX, {.guest_memory_disjoint = true});
        REQUIRE(bool(result));
        CHECK((result.program.Operations()[old.value].opcode == OpCode::Nop) == (observer < 2));
        CHECK(result.stats.uniform_stores_erased == (observer < 2 ? 1 : 0));
    }
}

TEST_CASE("Uniform forwarding covers separated slots and partial byte writes",
          "[compile][ir-optimize]") {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        Builder builder{Scope::Block};
        const auto block = builder.CreateBlock();
        REQUIRE(bool(block));
        REQUIRE(builder.BeginBlock(block.block) == Error::None);
        const auto type = scenario == 2 ? Type::V128 : Type::U64;
        const auto offset = scenario == 2 ? 7u : 0u;
        const auto first =
                Emit(builder, OpCode::LoadUniform, type, {Operand::Uniform(offset, type)});
        Store(builder, 256, first, type);
        if (scenario < 2) {
            const unsigned count = scenario == 0 ? 16 : 1;
            for (unsigned i = 1; i <= count; ++i) {
                const auto at = scenario == 0 ? i * 8 : 1024;
                const auto next = Emit(
                        builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(at, Type::U64)});
                Store(builder, 512 + i * 8, next);
            }
        } else {
            const auto value = Literal(builder, 0, scenario == 3 ? Type::Bool : Type::U64);
            Store(builder, scenario == 3 ? 5 : 16, value, scenario == 3 ? Type::Bool : Type::U64);
        }
        const auto reread =
                Emit(builder, OpCode::LoadUniform, type, {Operand::Uniform(offset, type)});
        Store(builder, 768, reread, type);
        REQUIRE(builder.EndBlock(Terminal::Return()) == Error::None);
        const auto result = Optimize(Submit(builder));
        REQUIRE(bool(result));
        CHECK((result.program.Operations()[reread.node.value].opcode != OpCode::LoadUniform) ==
              (scenario == 0));
    }
}

TEST_CASE("Wide pending uniform stores retain tail observations", "[compile][ir-optimize]") {
    for (unsigned offset : {0u, 7u}) {
        for (const auto type : {Type::U64, Type::Bool}) {
            Builder builder{Scope::Block};
            const auto block = builder.CreateBlock();
            REQUIRE(bool(block));
            REQUIRE(builder.BeginBlock(block.block) == Error::None);
            const auto first = Emit(
                    builder, OpCode::LoadUniform, Type::V256, {Operand::Uniform(64, Type::V256)});
            const auto second = Emit(
                    builder, OpCode::LoadUniform, Type::V256, {Operand::Uniform(128, Type::V256)});
            const auto old = builder.Operations().size();
            Store(builder, offset, first, Type::V256);
            const auto tail =
                    Emit(builder,
                         OpCode::LoadUniform,
                         type,
                         {Operand::Uniform(offset + (type == Type::Bool ? 31 : 24), type)});
            Store(builder, 96, tail, type);
            Store(builder, offset, second, Type::V256);
            REQUIRE(builder.EndBlock(Terminal::Return()) == Error::None);
            const auto result = Optimize(Submit(builder));
            REQUIRE(bool(result));
            CHECK(result.program.Operations()[old].opcode == OpCode::StoreUniform);
        }
    }
}

TEST_CASE("Ordered store displacement folding and separated GetOperand reuse",
          "[compile][ir-optimize]") {
    Builder builder{Scope::Block, TargetInfo{TargetMode::Known, 0xa64, UINT64_MAX}};
    const auto block = builder.CreateBlock();
    REQUIRE(bool(block));
    REQUIRE(builder.BeginBlock(block.block) == Error::None);
    const auto base =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
    const auto idx_a =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(8, Type::U64)});
    const auto idx_b =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(16, Type::U64)});
    const auto addr_a1 = Emit(builder,
                              OpCode::GetOperand,
                              Type::U64,
                              {Operand::Expression(OperandOp::AddShift, 2, 3),
                               Operand::ValueOf(base),
                               Operand::ValueOf(idx_a)});
    const auto val_a = Emit(builder, OpCode::LoadMemoryTSO, Type::U32, {Operand::ValueOf(addr_a1)});
    const auto addr_b = Emit(builder,
                             OpCode::GetOperand,
                             Type::U64,
                             {Operand::Expression(OperandOp::AddShift, 2, 3),
                              Operand::ValueOf(base),
                              Operand::ValueOf(idx_b)});
    const auto val_b = Emit(builder, OpCode::LoadMemoryTSO, Type::U32, {Operand::ValueOf(addr_b)});
    const auto sum = Emit(
            builder, OpCode::Add, Type::U32, {Operand::ValueOf(val_a), Operand::ValueOf(val_b)});
    const auto addr_a2 = Emit(builder,
                              OpCode::GetOperand,
                              Type::U64,
                              {Operand::Expression(OperandOp::AddShift, 2, 3),
                               Operand::ValueOf(base),
                               Operand::ValueOf(idx_a)});
    const auto store_addr = Emit(builder,
                                 OpCode::Sub,
                                 Type::U64,
                                 {Operand::ValueOf(addr_a2), Operand::Immediate(8, Type::U64)});
    Emit(builder,
         OpCode::StoreMemoryTSO,
         Type::Void,
         {Operand::ValueOf(store_addr), Operand::ValueOf(sum)});
    REQUIRE(builder.EndBlock(Terminal::Return()) == Error::None);
    const auto optimized = Optimize(Submit(builder));
    REQUIRE(bool(optimized));
    std::size_t get_operands{}, folded_stores{};
    for (const auto& op : optimized.program.Operations()) {
        if (op.opcode == OpCode::GetOperand) ++get_operands;
        if (op.opcode == OpCode::StoreMemoryTSO) {
            const auto cells = optimized.program.Operands(op);
            auto at = cells.begin();
            REQUIRE(at != cells.end());
            const auto expr = *at++;
            CHECK(expr.Kind() == OperandKind::Expression);
            CHECK(expr.ExpressionOp() == OperandOp::Sub);
            const auto addr_cell = *at++;
            CHECK(addr_cell.Payload() == addr_a1.node.value);
            const auto disp_cell = *at++;
            CHECK(disp_cell.Payload() == 8);
            ++folded_stores;
        }
    }
    CHECK(get_operands == 2);
    CHECK(folded_stores == 1);
}

TEST_CASE("Vector zero identities, V128 U64 lane forwarding, and VecLoadConst purity",
          "[compile][ir-optimize]") {
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    REQUIRE((block && builder.BeginBlock(block.block) == Error::None));
    const auto dead_const = Emit(builder,
                                 OpCode::VecLoadConst,
                                 Type::V128,
                                 {Operand::Immediate(0x11, Type::U64),
                                  Operand::Immediate(0x22, Type::U64)});
    const auto zero = Emit(builder, OpCode::VecSharedZero, Type::V128, {});
    const auto live_const = Emit(builder,
                                 OpCode::VecLoadConst,
                                 Type::V128,
                                 {Operand::Immediate(0x0102030405060708ULL, Type::U64),
                                  Operand::Immediate(0x1112131415161718ULL, Type::U64)});
    Store(builder, 128, live_const, Type::V128);
    // An intervening pure VecLoadConst neither prevents DSE nor blocks forwarding.
    const auto other_const = Emit(builder,
                                  OpCode::VecLoadConst,
                                  Type::V128,
                                  {Operand::Immediate(0xaa, Type::U64),
                                   Operand::Immediate(0xbb, Type::U64)});
    Store(builder, 160, other_const, Type::V128);
    const auto lane0 =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(128, Type::U64)});
    const auto lane1 =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(136, Type::U64)});
    Store(builder, 0, lane0, Type::U64);
    Store(builder, 8, lane1, Type::U64);
    Store(builder, 128, zero, Type::V128);
    const auto zlane =
            Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(136, Type::U64)});
    Store(builder, 16, zlane, Type::U64);

    const auto input_vec =
            Emit(builder, OpCode::LoadUniform, Type::V128, {Operand::Uniform(192, Type::V128)});
    const auto or_zero = Emit(
            builder, OpCode::VecOr, Type::V128, {Operand::ValueOf(input_vec), Operand::ValueOf(zero)});
    const auto xor_zero = Emit(builder,
                               OpCode::VecXor,
                               Type::V128,
                               {Operand::ValueOf(zero), Operand::ValueOf(or_zero)});
    const auto and_zero = Emit(builder,
                               OpCode::VecAnd,
                               Type::V128,
                               {Operand::ValueOf(input_vec), Operand::ValueOf(zero)});
    const auto self_xor = Emit(builder,
                               OpCode::VecXor,
                               Type::V128,
                               {Operand::ValueOf(input_vec), Operand::ValueOf(input_vec)});
    const auto xor_store = NodeId{static_cast<std::uint32_t>(builder.Operations().size())};
    Store(builder, 224, xor_zero, Type::V128);
    Store(builder, 240, and_zero, Type::V128);
    Store(builder, 256, self_xor, Type::V128);
    REQUIRE(builder.EndBlock(Terminal::Return()) == Error::None);

    const auto result = Optimize(Submit(builder), SIZE_MAX, {.guest_memory_disjoint = true});
    REQUIRE(bool(result));
    CHECK(result.program.Operations()[dead_const.node.value].opcode == OpCode::Nop);
    CHECK(Immediate(result.program, lane0, Type::U64) == 0x0102030405060708ULL);
    CHECK(Immediate(result.program, lane1, Type::U64) == 0x1112131415161718ULL);
    CHECK(Immediate(result.program, zlane, Type::U64) == 0);
    CHECK(result.program.Operations()[and_zero.node.value].opcode == OpCode::VecSharedZero);
    CHECK(result.program.Operations()[self_xor.node.value].opcode == OpCode::VecSharedZero);
    CHECK(result.program.Operations()[or_zero.node.value].opcode == OpCode::Nop);
    CHECK(result.program.Operations()[xor_zero.node.value].opcode == OpCode::Nop);
    auto stored_it = result.program.Operands(xor_store).begin();
    ++stored_it;
    CHECK((*stored_it).Payload() == input_vec.node.value);
}

TEST_CASE("Sandwiched LoadMemory eliminates vector dead stores while preserving scalar and boundary stores",
          "[compile][ir-optimize]") {
    const auto run_case = [&](bool fault_locs, bool disjoint, bool barrier_between) {
        const std::uint64_t features =
                fault_locs ? static_cast<std::uint64_t>(TargetFeature::FaultLocations) : 0;
        Builder builder{Scope::Block, TargetInfo{TargetMode::Known, 0x8664, features}};
        const auto block = builder.CreateBlock();
        REQUIRE((block && builder.BeginBlock(block.block) == Error::None));
        const auto base =
                Emit(builder, OpCode::LoadUniform, Type::U64, {Operand::Uniform(0, Type::U64)});
        const auto add_off = [&](std::uint64_t off) {
            return Emit(builder,
                        OpCode::Add,
                        Type::U64,
                        {Operand::ValueOf(base), Operand::Immediate(off, Type::U64)});
        };
        const auto m_lo = Emit(builder, OpCode::LoadMemory, Type::V128, {Operand::ValueOf(add_off(0x20))});
        const auto m_hi = Emit(builder, OpCode::LoadMemory, Type::V128, {Operand::ValueOf(add_off(0xe0))});
        const auto s1 = Literal(builder, 0x1234), s2 = Literal(builder, 0x5678);
        const auto vec_store1 = NodeId{static_cast<std::uint32_t>(builder.Operations().size())};
        Store(builder, 256, m_lo, Type::V128);
        const auto scalar_store1 = NodeId{static_cast<std::uint32_t>(builder.Operations().size())};
        Store(builder, 16, s1, Type::U64);
        if (barrier_between) Emit(builder, OpCode::MemoryBarrierTSO, Type::Void, {});
        const auto m_mid =
                Emit(builder, OpCode::LoadMemory, Type::V128, {Operand::ValueOf(add_off(0x80))});
        Store(builder, 256, m_mid, Type::V128);
        Store(builder, 16, s2, Type::U64);
        // A load outside [0x20, 0xf0) may fault and must commit prior vector stores.
        const auto vec_store2 = NodeId{static_cast<std::uint32_t>(builder.Operations().size())};
        Store(builder, 288, m_hi, Type::V128);
        const auto m_out =
                Emit(builder, OpCode::LoadMemory, Type::V128, {Operand::ValueOf(add_off(0x10))});
        Store(builder, 288, m_out, Type::V128);
        REQUIRE(builder.EndBlock(Terminal::Return()) == Error::None);
        const auto opt = Optimize(Submit(builder), SIZE_MAX, {.guest_memory_disjoint = disjoint});
        REQUIRE(bool(opt));
        const bool expect_dse = fault_locs && disjoint && !barrier_between;
        CHECK((opt.program.Operations()[vec_store1.value].opcode == OpCode::Nop) == expect_dse);
        CHECK(opt.program.Operations()[scalar_store1.value].opcode == OpCode::StoreUniform);
        CHECK(opt.program.Operations()[vec_store2.value].opcode == OpCode::StoreUniform);
    };
    run_case(true, true, false);
    run_case(false, true, false);
    run_case(true, false, false);
    run_case(true, true, true);
}
