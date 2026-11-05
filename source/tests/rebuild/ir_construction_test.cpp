#include "runtime/ir/builder.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace {
thread_local bool count_allocations{};
thread_local std::size_t failure_countdown{};
thread_local std::size_t allocation_count{};

void BeforeAllocation() {
    if (count_allocations) ++allocation_count;
    if (failure_countdown && --failure_countdown == 0) throw std::bad_alloc{};
}

void* Allocate(std::size_t size) {
    BeforeAllocation();
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc{};
}

void* AllocateAligned(std::size_t size, std::size_t alignment) {
    BeforeAllocation();
    size = size ? size : 1;
    if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1))
        throw std::bad_alloc{};
    size = (size + alignment - 1) & ~(alignment - 1);
    if (void* pointer = std::aligned_alloc(alignment, size)) return pointer;
    throw std::bad_alloc{};
}

class AllocationCount {
public:
    AllocationCount() { allocation_count = 0; count_allocations = true; }
    ~AllocationCount() { count_allocations = false; }
    std::size_t Stop() { count_allocations = false; return allocation_count; }
};

void Check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "IR construction contract failed at line %d: %s\n", line, expression);
    throw std::runtime_error("IR construction contract failed");
}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

using namespace swift::runtime::ir;

std::size_t LiveCellCount(const Program& program) {
    std::size_t count{};
    for (std::size_t id = 0; id < program.Operations().size(); ++id) {
        const auto cells = program.Operands(NodeId{static_cast<std::uint32_t>(id)});
        count += static_cast<std::size_t>(std::distance(cells.begin(), cells.end()));
    }
    return count;
}

std::size_t CellCount(CellRange cells) {
    return static_cast<std::size_t>(std::distance(cells.begin(), cells.end()));
}
// Explicitly linear, used only for small expected rows in this test binary.
CellView CellAt(CellRange cells, std::size_t index) {
    auto at = cells.begin();
    while (index && at != cells.end()) { --index; ++at; }
    CHECK(index == 0 && at != cells.end());
    return *at;
}

static_assert(sizeof(Operation) == 24);
static_assert(sizeof(BlockRecord) == 24);
static_assert(sizeof(StoredTerminal) == 16);
static_assert(sizeof(Cell) == 8);
static_assert(FlagBits(Flag::All) == 95 && FlagBits(Flag::NZCV) == 15);
static_assert(FlagBits(Flag::Carry | Flag::AuxiliaryCarry) == 65);
static_assert((Flag::All & Flag::NZCV) == Flag::NZCV);
static_assert(Operand::Flags(Flag::Carry | Flag::Parity).payload == 17);
static_assert(Operand::Flags(UINT64_MAX).payload == UINT64_MAX);
static_assert(Operand::Flags(static_cast<Flag>(UINT64_C(1) << 63)).payload == (UINT64_C(1) << 63));
static_assert(Operand::Condition(Condition::LT).payload == 11);
static_assert(Operand::Condition(15u).payload == 15);
static_assert([] {
    constexpr std::array conditions{Condition::EQ, Condition::NE, Condition::CS, Condition::CC,
        Condition::MI, Condition::PL, Condition::VS, Condition::VC, Condition::HI, Condition::LS,
        Condition::GE, Condition::LT, Condition::GT, Condition::LE, Condition::AL, Condition::NV};
    for (std::size_t i = 0; i < conditions.size(); ++i)
        if (static_cast<unsigned>(conditions[i]) != i) return false;
    return true;
}());
static_assert(noexcept(std::declval<Builder&>().SetMetadata(NodeId{}, OperationMetadata{})));
static_assert(noexcept(std::declval<const Program&>().EntryBlock()));
static_assert(noexcept(std::declval<const Builder&>().EntryBlock()));
static_assert(noexcept(std::declval<Builder&>().SetEntry(BlockRef{})));
static_assert(IsPure(EffectsFor(OpCode::Add)));
static_assert(HasAnyEffect(EffectsFor(OpCode::Adc), Effect::ReadFlags));
static_assert(HasAnyEffect(EffectsFor(OpCode::Sbb), Effect::ReadFlags));
static_assert(HasAnyEffect(EffectsFor(OpCode::LoadMemoryTSO), Effect::ReadMemory));
static_assert(HasAnyEffect(EffectsFor(OpCode::LoadMemoryTSO), Effect::MayFault));
static_assert(HasAnyEffect(EffectsFor(OpCode::LoadMemoryTSO), Effect::Ordered));
static_assert(EffectsFor(OpCode::CallLambda) == static_cast<Effect>(UINT16_MAX));
static_assert(EffectsFor(OpCode::CallLocation) == static_cast<Effect>(UINT16_MAX));
static_assert(HasAnyEffect(EffectsFor(OpCode::AddPhi), Effect::Control));
static_assert(HasAnyEffect(EffectsFor(OpCode::Add, {UINT64_C(1) << 63}), Effect::WriteFlags));
static_assert(HasAnyEffect(EffectsFor(OpCode::And,
        {FlagBits(Flag::Zero), FlagProfile::LogicalZeroCVA, false, true}), Effect::WriteFlags));
static_assert(OperationMetadata{FlagBits(Flag::Zero), FlagProfile::LogicalZeroCVA} !=
        OperationMetadata{FlagBits(Flag::Zero), FlagProfile::LogicalZeroCVA, false, true});
static_assert(TargetInfo{TargetMode::Known, 0, 1}.Has(TargetFeature::ShiftedAddress));
static_assert(!TargetInfo{TargetMode::EarlyOff, 0, 1}.Has(TargetFeature::ShiftedAddress));
static_assert(!TargetInfo{TargetMode::Unknown, 0, 1}.Has(TargetFeature::ShiftedAddress));


SubmitResult SubmitWithoutAllocation(Builder& builder) {
    AllocationCount counter;
    auto submitted = std::move(builder).Submit();
    const auto allocations = counter.Stop();
    CHECK(allocations == 0);
    CHECK(submitted);
    return submitted;
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }

namespace {

AppendResult Literal(Builder& builder, std::uint64_t bits, Type type = Type::U64) {
    const auto result = builder.Append(OpCode::LoadImm, Type::Void,
                                       {Operand::Immediate(bits, type)});
    CHECK(result);
    CHECK(result.node);
    CHECK(result.GetValue());
    return result;
}

void BlockConstructionRoundtrip() {
    const TargetInfo target{TargetMode::Known, 0xa64, 0x8000000000000001ULL};
    Builder builder{Scope::Block, target};
    const auto block = builder.CreateBlock();
    CHECK(block);
    CHECK(builder.CreateBlock().error == Error::ScopeMismatch);
    CHECK(builder.BeginBlock(block.block) == Error::None);
    const auto literal = Literal(builder, 0xfedcba9876543210ULL);
    const auto add = builder.Append(OpCode::Add, Type::Void,
            {Operand::ValueOf(literal.GetValue()), Operand::Expression(OperandOp::Identity),
             Operand::Immediate(17, Type::U64)});
    CHECK(add);
    const auto alias = builder.Append(OpCode::BitCast, Type::Void,
                                      {Operand::ValueOf(add.GetValue())});
    CHECK(alias);
    const auto local = builder.Append(OpCode::LoadLocal, Type::Void,
                                      {Operand::Local(7, Type::U32)});
    const auto uniform = builder.Append(OpCode::LoadUniform, Type::Void,
                                        {Operand::Uniform(24, Type::U64)});
    CHECK(local);
    CHECK(uniform);
    const OperationMetadata metadata{0x80000001000008d5ULL, FlagProfile::ArithmeticRaw, true, true};
    const auto narrow = builder.Append(OpCode::Add, Type::Void,
            {Operand::ValueOf(literal.GetValue(), Type::U16),
             Operand::Expression(OperandOp::Identity), Operand::Immediate(2, Type::U16)},
            metadata);
    const auto signed_view = builder.Append(OpCode::BitCast, Type::Void,
                                            {Operand::ValueOf(literal.GetValue(), Type::S16)});
    CHECK(narrow);
    CHECK(signed_view);
    CHECK(builder.EndBlock(Terminal::Return(alias.GetValue(), Type::U16)) == Error::None);
    const auto submitted = SubmitWithoutAllocation(builder);
    const auto& program = submitted.program;
    CHECK(program.ScopeKind() == Scope::Block);
    CHECK(program.Target() == target);
    CHECK(program.UnitId() == literal.GetValue().unit);
    CHECK(program.Operations().size() == 7);
    CHECK(program.Blocks().size() == 1);
    const auto& first = program.Operations()[literal.node.value];
    CHECK(first.opcode == OpCode::LoadImm);
    CHECK(first.type == Type::U64);
    const auto cells = program.Operands(first);
    CHECK(CellCount(cells) == 1);
    CHECK(cells.WordCount() == 2);
    CHECK(CellAt(cells, 0).Kind() == OperandKind::Immediate);
    CHECK(CellAt(cells, 0).TypeView() == Type::U64);
    CHECK(CellAt(cells, 0).Payload() == 0xfedcba9876543210ULL);
    const auto expression = program.Operands(program.Operations()[add.node.value]);
    CHECK(CellCount(expression) == 2);
    CHECK(expression.WordCount() == 2);
    CHECK(CellAt(expression, 0).Kind() == OperandKind::Value);
    CHECK(CellAt(expression, 0).Payload() == literal.node.value);
    CHECK(CellAt(expression, 1).Kind() == OperandKind::Immediate);
    CHECK(CellAt(expression, 1).Payload() == 17);
    CHECK(program.Operations()[local.node.value].type == Type::U32);
    CHECK(CellAt(program.Operands(program.Operations()[local.node.value]), 0).Payload() == 7);
    CHECK(CellAt(program.Operands(program.Operations()[uniform.node.value]), 0).Payload() == 24);
    CHECK(program.Operations()[narrow.node.value].type == Type::U16);
    CHECK(CellAt(program.Operands(program.Operations()[narrow.node.value]), 0).TypeView() == Type::U16);
    CHECK(program.Operations()[narrow.node.value].Metadata() == metadata);
    CHECK(program.Operations()[signed_view.node.value].type == Type::S16);
    CHECK(CellAt(program.Operands(program.Operations()[signed_view.node.value]), 0).TypeView() == Type::S16);
    CHECK(program.Operations()[literal.node.value].type == Type::U64);
    CHECK(program.Blocks()[0].State() == BlockState::Closed);
    CHECK(program.Blocks()[0].first_node == 0);
    CHECK(program.Blocks()[0].node_count == program.Operations().size());
    CHECK(program.Blocks()[0].terminal.kind == TerminalKind::Return);
    CHECK(program.Blocks()[0].terminal.HasValue());
    CHECK(program.Blocks()[0].terminal.value == alias.node);
    CHECK(program.Blocks()[0].terminal.value_type == Type::U16);
    CHECK(program.Contains(literal.GetValue()));
    CHECK(builder.CreateBlock().error == Error::Consumed);
    CHECK(builder.BeginBlock(block.block) == Error::Consumed);
    CHECK(builder.Append(OpCode::Nop, Type::Void, {}).error == Error::Consumed);
    CHECK(builder.EndBlock(Terminal::Return()) == Error::Consumed);
    CHECK(builder.Submit().error == Error::Consumed);
}

void FunctionConstructionAndIncompleteSubmit() {
    Builder builder{Scope::Function};
    CHECK(builder.Submit().error == Error::IncompleteScope);
    const auto entry = builder.CreateBlock();
    const auto yes = builder.CreateBlock();
    const auto no = builder.CreateBlock();
    CHECK(entry && yes && no);
    CHECK(builder.BeginBlock(entry.block) == Error::None);
    const auto value = Literal(builder, 27);
    const auto condition = Literal(builder, 1, Type::Bool);
    CHECK(builder.EndBlock(Terminal::Branch(condition.GetValue(), yes.block, no.block)) == Error::None);
    CHECK(builder.BeginBlock(yes.block) == Error::None);
    const auto sum = builder.Append(OpCode::Add, Type::Void,
            {Operand::ValueOf(value.GetValue()), Operand::Expression(OperandOp::Identity),
             Operand::Immediate(3, Type::U64)});
    CHECK(sum);
    CHECK(builder.EndBlock(Terminal::Jump(no.block)) == Error::None);
    {
        AllocationCount counter;
        auto incomplete = std::move(builder).Submit();
        const auto allocations = counter.Stop();
        CHECK(allocations == 0);
        CHECK(incomplete.error == Error::IncompleteScope);
        CHECK(!incomplete.program);
    }
    CHECK(builder.BeginBlock(no.block) == Error::None);
    CHECK(builder.EndBlock(Terminal::Return(value.GetValue())) == Error::None);
    const auto submitted = SubmitWithoutAllocation(builder);
    const auto& program = submitted.program;
    CHECK(program.ScopeKind() == Scope::Function);
    CHECK(program.Blocks().size() == 3);
    CHECK(program.Operations().size() == 3);
    CHECK(program.Operations()[sum.node.value].block == yes.block.id);
    CHECK(CellAt(program.Operands(program.Operations()[sum.node.value]), 0).Payload() == value.node.value);
    CHECK(program.Blocks()[entry.block.id.value].terminal.value == condition.node);
    CHECK(program.Blocks()[entry.block.id.value].terminal.HasValue());
    CHECK(program.Blocks()[entry.block.id.value].terminal.first == yes.block.id);
    CHECK(program.Blocks()[entry.block.id.value].terminal.second == no.block.id);
    CHECK(program.Blocks()[yes.block.id.value].terminal.first == no.block.id);
    CHECK(!program.Blocks()[yes.block.id.value].terminal.HasValue());
    CHECK(program.Blocks()[no.block.id.value].terminal.value == value.node);
    CHECK(value.node.value == 0 && program.Blocks()[no.block.id.value].terminal.HasValue());
    CHECK(program.Contains(sum.GetValue()));
}

void ReusedCapacityHasFreshIdentity() {
    Builder first{Scope::Block};
    CHECK(first.Reserve(32, 64, 8) == Error::None);
    const auto old_block = first.CreateBlock(); CHECK(old_block);
    CHECK(first.BeginBlock(old_block.block) == Error::None);
    const auto old_value = Literal(first, 7).GetValue();
    CHECK(first.EndBlock(Terminal::Return(old_value)) == Error::None);
    auto submitted = first.Submit(); CHECK(submitted);
    auto owner = std::move(submitted.program);
    const auto capacity = owner.ReservedBytes();
    const TargetInfo known{TargetMode::Known, 0xa64, 0};
    Builder next{Scope::Function, known, std::move(owner)};
    CHECK(!owner && next.UnitId() != old_value.unit && next.Operations().empty() && next.Blocks().empty());
    const auto block = next.CreateBlock(); CHECK(block);
    CHECK(next.BeginBlock(old_block.block) == Error::ForeignUnit);
    CHECK(next.BeginBlock(block.block) == Error::None);
    CHECK(next.Emit<OpCode::BitCast>(Type::U64, {Operand::ValueOf(old_value)}).error == Error::ForeignUnit);
    const auto value = Literal(next, 11).GetValue();
    CHECK(value.node == old_value.node && next.EndBlock(Terminal::Return(value)) == Error::None);
    submitted = next.Submit(); CHECK(submitted);
    CHECK(submitted.program.ScopeKind() == Scope::Function && submitted.program.Target() == known &&
            submitted.program.ReservedBytes() == capacity && submitted.program.Contains(value) &&
            !submitted.program.Contains(old_value));
}

void RejectedInputIsAtomic() {
    Value stale_value;
    BlockRef stale_block;
    {
        Builder expired{Scope::Block};
        const auto block = expired.CreateBlock();
        CHECK(block && expired.BeginBlock(block.block) == Error::None);
        stale_block = block.block;
        stale_value = Literal(expired, 0x8000000000000001ULL).GetValue();
    }
    Builder other{Scope::Block};
    const auto foreign_block = other.CreateBlock();
    CHECK(foreign_block);
    CHECK(other.BeginBlock(foreign_block.block) == Error::None);
    const auto foreign = Literal(other, 13);

    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block);
    CHECK(builder.BeginBlock(foreign_block.block) == Error::ForeignUnit);
    CHECK(builder.BeginBlock(stale_block) == Error::ForeignUnit);
    CHECK(builder.BeginBlock(block.block) == Error::None);
    CHECK(builder.BeginBlock(block.block) == Error::BlockAlreadyOpen);
    const auto first = Literal(builder, 5);
    CHECK(first.node == stale_value.node && first.GetValue().unit != stale_value.unit);
    const auto reject = [&](OpCode op, Type type, std::initializer_list<Operand> cells, Error error) {
        const auto result = builder.Append(op, type, cells);
        CHECK(result.error == error);
        CHECK(!result.node);
        CHECK(!result.GetValue());
    };
    reject(OpCode::BitCast, Type::Void, {Operand::ValueOf(foreign.GetValue())}, Error::ForeignUnit);
    reject(OpCode::BitCast, Type::Void, {Operand::ValueOf(stale_value, Type::S16)}, Error::ForeignUnit);
    reject(OpCode::BitCast, Type::Void, {Operand::ValueOf({})}, Error::InvalidValue);
    reject(OpCode::BitCast, Type::Void,
           {Operand::ValueOf({first.GetValue().unit, NodeId{NodeId::invalid - 1}})}, Error::InvalidValue);
    reject(OpCode::LoadImm, Type::Void, {Operand::ValueOf(first.GetValue())}, Error::InvalidOperandKind);
    reject(OpCode::BitCast, Type::Void, {}, Error::MissingOperand);
    reject(OpCode::BitCast, Type::Void,
           {Operand::ValueOf(first.GetValue()), Operand::ValueOf(first.GetValue())}, Error::ExtraOperand);
    reject(OpCode::Add, Type::Void,
           {Operand::ValueOf(first.GetValue()), Operand::Expression(OperandOp::Add, 2),
            Operand::Immediate(1, Type::U64)}, Error::MissingOperand);
    reject(OpCode::Add, Type::Void,
           {Operand::ValueOf(first.GetValue()), Operand::Expression(OperandOp::Identity, 0)}, Error::InvalidRange);
    reject(OpCode::Add, Type::Void,
           {Operand::ValueOf(first.GetValue()), Operand::Expression(OperandOp::Identity),
            Operand::ValueOf(foreign.GetValue(), Type::U16)}, Error::ForeignUnit);
    CHECK(!builder.Append(OpCode::Add, Type::Void,
            {Operand::ValueOf(first.GetValue()), Operand::Expression(OperandOp::Identity),
             Operand::Immediate(1, Type::F32)}));
    reject(OpCode::Zero, Type::Void, {}, Error::InvalidResultType);
    reject(OpCode::Zero, static_cast<Type>(255), {}, Error::InvalidType);
    reject(OpCode::Count, Type::Void, {}, Error::InvalidOpcode);
    CHECK(builder.EndBlock({}) == Error::IncompleteTerminal);
    CHECK(builder.EndBlock(Terminal::Jump(foreign_block.block)) == Error::ForeignUnit);
    CHECK(builder.EndBlock(Terminal::Return(foreign.GetValue())) == Error::ForeignUnit);
    CHECK(builder.EndBlock(Terminal::Jump(stale_block)) == Error::ForeignUnit);
    CHECK(builder.EndBlock(Terminal::Return(stale_value, Type::S16)) == Error::ForeignUnit);
    const auto second = Literal(builder, 9);
    CHECK(second.node.value == first.node.value + 1);
    CHECK(builder.EndBlock(Terminal::Return(second.GetValue())) == Error::None);
    CHECK(builder.BeginBlock(block.block) == Error::BlockAlreadyEnded);
    CHECK(builder.Append(OpCode::Nop, Type::Void, {}).error == Error::NoActiveBlock);
    const auto submitted = SubmitWithoutAllocation(builder);
    CHECK(submitted.program.Operations().size() == 2);
    CHECK(LiveCellCount(submitted.program) == 2);
    CHECK(!submitted.program.Contains(foreign.GetValue()));
    CHECK(!submitted.program.Contains(stale_value));
    CHECK(CellAt(submitted.program.Operands(submitted.program.Operations()[0]), 0).Payload() == 5);
    CHECK(CellAt(submitted.program.Operands(submitted.program.Operations()[1]), 0).Payload() == 9);
}

void WidePayloadsAndCompoundBoundaries() {
    constexpr std::uint64_t traits = 0xfedcba9876543210ULL;
    constexpr std::uint64_t address = 0x12345678abcdef01ULL;
    constexpr std::uint64_t negative = 0xfffffffffffffff0ULL;
    constexpr auto mask = std::numeric_limits<std::uint64_t>::max();
    Builder builder{Scope::Block};
    const auto block = builder.CreateBlock();
    CHECK(block && builder.BeginBlock(block.block) == Error::None);
    const auto seed = Literal(builder, 0x8000000000000001ULL);
    const auto flags = builder.Emit<OpCode::SaveFlags>(Type::Void,
            {Operand::ValueOf(seed.GetValue()), Operand::Flags(mask)});
    const auto expression = builder.Append(OpCode::GetOperand, Type::U64,
            {Operand::Expression(OperandOp::Add, 2), Operand::ValueOf(seed.GetValue()),
             Operand::Immediate(negative, Type::S64)});
    const auto call = builder.Emit<OpCode::CallLocation>(Type::U64,
            {Operand::Lambda(traits), Operand::Immediate(address, Type::U64), Operand::Arguments(3),
             Operand::ValueOf(seed.GetValue(), Type::S16), Operand::Immediate(address, Type::U64),
             Operand::Immediate(negative, Type::S64)});
    const auto optional = builder.Append(OpCode::CallLambda, Type::U64,
            {Operand::Lambda(mask), Operand::Immediate(address, Type::U64),
             Operand::Immediate(mask, Type::S64)});
    CHECK(flags && expression && call && optional);
    const auto reject = [&](OpCode op, std::initializer_list<Operand> cells, Error error) {
        const auto result = builder.Append(op, Type::U64, cells);
        CHECK(result.error == error && !result.node && !result.GetValue());
    };
    reject(OpCode::CallLocation,
            {Operand::Lambda(traits), Operand::Immediate(address, Type::U64), Operand::Arguments(2),
             Operand::Immediate(negative, Type::S64)}, Error::MissingOperand);
    reject(OpCode::CallLocation,
            {Operand::Lambda(traits), Operand::Immediate(address, Type::U64), Operand::Arguments(1),
             Operand::Expression(OperandOp::Identity), Operand::ValueOf(seed.GetValue())}, Error::InvalidOperandKind);
    reject(OpCode::CallLocation,
            {Operand::Lambda(traits), Operand::Immediate(address, Type::U64), Operand::Arguments(1),
             Operand::Arguments(0)}, Error::InvalidOperandKind);
    reject(OpCode::GetOperand,
            {Operand::Expression(OperandOp::Add, 2), Operand::ValueOf(seed.GetValue()),
             Operand::Lambda(traits), Operand::Immediate(address, Type::U64)}, Error::InvalidOperandKind);
    reject(OpCode::CallLambda, {Operand::Lambda(traits)}, Error::MissingOperand);
    const auto tail = Literal(builder, 37);
    CHECK(tail.node.value == optional.node.value + 1);
    CHECK(builder.EndBlock(Terminal::Return(optional.GetValue())) == Error::None);
    const auto ready = SubmitWithoutAllocation(builder);
    const auto& program = ready.program;
    CHECK(program.Operations().size() == 6);
    CHECK(program.OperandArena().size() == 26);
    CHECK(LiveCellCount(program) == 16);
    const auto flag_cells = program.Operands(program.Operations()[flags.node.value]);
    CHECK(CellCount(flag_cells) == 2 && flag_cells.WordCount() == 3);
    CHECK(CellAt(flag_cells, 1).Kind() == OperandKind::Flags);
    CHECK(CellAt(flag_cells, 1).Payload() == mask);
    const auto expression_cells = program.Operands(program.Operations()[expression.node.value]);
    CHECK(CellCount(expression_cells) == 3 && expression_cells.WordCount() == 4);
    CHECK(expression_cells.front().Kind() == OperandKind::Expression);
    CHECK(expression_cells.front().Count() == 2);
    CHECK(expression_cells.front().Code() == static_cast<std::uint16_t>(OperandOp::Add));
    CHECK(CellAt(expression_cells, 1).TypeView() == Type::Void);
    CHECK(CellAt(expression_cells, 2).Payload() == negative);
    CHECK(CellAt(expression_cells, 2).TypeView() == Type::S64);
    const auto call_cells = program.Operands(program.Operations()[call.node.value]);
    CHECK(CellCount(call_cells) == 6 && call_cells.WordCount() == 10);
    auto at = call_cells.begin();
    CHECK((*at).Kind() == OperandKind::Lambda && (*at).Payload() == traits); ++at;
    CHECK((*at).Kind() == OperandKind::Immediate && (*at).Payload() == address); ++at;
    CHECK((*at).Kind() == OperandKind::Arguments && (*at).Count() == 3); ++at;
    CHECK((*at).Kind() == OperandKind::Value && (*at).Payload() == seed.node.value);
    CHECK((*at).TypeView() == Type::S16); ++at;
    CHECK((*at).Payload() == address); ++at;
    CHECK((*at).Payload() == negative && (*at).TypeView() == Type::S64); ++at;
    CHECK(at == call_cells.end());
    const auto optional_cells = program.Operands(program.Operations()[optional.node.value]);
    CHECK(CellCount(optional_cells) == 3 && optional_cells.WordCount() == 6);
    CHECK(optional_cells.front().Kind() == OperandKind::Lambda && optional_cells.front().Payload() == mask);
    CHECK(CellAt(optional_cells, 2).Payload() == mask);
    CHECK(CellAt(program.Operands(program.Operations()[tail.node.value]), 0).Payload() == 37);
    // A copied/forged record cannot borrow an arbitrary word interval.
    auto truncated = program.Operations()[seed.node.value];
    truncated.operand_count = 1;
    const auto short_range = program.Operands(truncated);
    CHECK(short_range.empty());
    CHECK(program.Operands(seed.node).WordCount() == 2);
}

void AllocationFailureIsAtomic() {
    const auto check_growth = [](bool word_growth) {
        Builder builder{Scope::Block};
        // Isolate the actual allocation domain. In the word case an initial
        // narrow cell leaves odd occupancy before the following wide pairs.
        CHECK(builder.Reserve(word_growth ? 4096 : 0, word_growth ? 0 : 8192, 1) == Error::None);
        const auto block = builder.CreateBlock();
        CHECK(block);
        CHECK(builder.BeginBlock(block.block) == Error::None);
        (void)Literal(builder, 7);
        constexpr std::uint64_t wide = 0x8123456789abcdefULL;
        const auto append_wide = [&](std::uint64_t bits) {
            return word_growth
                    ? builder.Emit<OpCode::LoadImm>(Type::Void, {Operand::Immediate(bits, Type::U64)})
                    : builder.Append(OpCode::LoadImm, Type::Void, {Operand::Immediate(bits, Type::U64)});
        };
        std::size_t successful = 1;
        bool failed{};
        for (unsigned attempt = 0; attempt < 4094 && !failed; ++attempt) {
            failure_countdown = 1;
            try {
                const auto result = append_wide(wide);
                failure_countdown = 0;
                CHECK(result && result.node.value == successful);
                ++successful;
            } catch (const std::bad_alloc&) {
                failure_countdown = 0;
                failed = true;
            }
        }
        failure_countdown = 0;
        CHECK(failed);
        const auto recovery = append_wide(~wide);
        CHECK(recovery && recovery.node.value == successful);
        CHECK(builder.EndBlock(Terminal::Return(recovery.GetValue())) == Error::None);
        const auto ready = SubmitWithoutAllocation(builder);
        CHECK(ready.program.Operations().size() == successful + 1);
        CHECK(ready.program.Blocks()[0].node_count == successful + 1);
        CHECK(ready.program.OperandArena().size() == successful * 2 + 1);
        std::size_t logical{};
        for (std::size_t id = 0; id < ready.program.Operations().size(); ++id) {
            const auto cells = ready.program.Operands(NodeId{static_cast<std::uint32_t>(id)});
            CHECK(CellCount(cells) == 1);
            const auto cell = cells.front();
            CHECK(cell.Kind() == OperandKind::Immediate && cell.TypeView() == Type::U64);
            CHECK(cell.Payload() == (logical == 0 ? 7 : logical == successful ? ~wide : wide));
            ++logical;
        }
        CHECK(logical == successful + 1);
        CHECK(CellAt(ready.program.Operands(ready.program.Operations().back()), 0).Payload() == ~wide);
    };
    check_growth(false); // Generic: wide cells rollback when operations grow.
    check_growth(true);  // Static: a wide payload requires more operand words.

    Builder function{Scope::Function};
    std::array<BlockRef, 256> blocks{};
    std::size_t declared{};
    bool block_failure{};
    const auto initial_block = function.CreateBlock();
    CHECK(initial_block && initial_block.block.id.value == 0);
    blocks[declared++] = initial_block.block;
    for (; declared < blocks.size() && !block_failure;) {
        failure_countdown = 1;
        try {
            const auto result = function.CreateBlock();
            failure_countdown = 0;
            CHECK(result);
            CHECK(result.block.id.value == declared);
            blocks[declared++] = result.block;
        } catch (const std::bad_alloc&) {
            failure_countdown = 0;
            block_failure = true;
        }
    }
    failure_countdown = 0;
    CHECK(block_failure);
    const auto recovered_block = function.CreateBlock();
    CHECK(recovered_block);
    CHECK(recovered_block.block.id.value == declared);
    CHECK(declared < blocks.size());
    blocks[declared++] = recovered_block.block;
    for (std::size_t i = 0; i < declared; ++i) {
        CHECK(function.BeginBlock(blocks[i]) == Error::None);
        CHECK(function.EndBlock(Terminal::Return()) == Error::None);
    }
    const auto ready = SubmitWithoutAllocation(function);
    CHECK(ready.program.Blocks().size() == declared);
    CHECK(ready.program.Operations().empty());
}

void MoveAndSubmittedOwnerLifetime() {
    for (const auto mode : {TargetMode::Known, TargetMode::EarlyOff, TargetMode::Unknown}) {
        Program owner;
        Value value;
        std::uint64_t unit{};
        const TargetInfo target{mode, 42, 0x123456789abcdef0ULL};
        {
            Builder source{Scope::Block, target};
            const auto block = source.CreateBlock();
            CHECK(block);
            CHECK(source.BeginBlock(block.block) == Error::None);
            value = Literal(source, 0x8000000000000001ULL).GetValue();
            unit = value.unit;
            Builder moved{std::move(source)};
            CHECK(source.CreateBlock().error == Error::Consumed);
            for (unsigned i = 0; i < 128; ++i) (void)Literal(moved, i);
            CHECK(moved.EndBlock(Terminal::Return(value)) == Error::None);
            auto submitted = SubmitWithoutAllocation(moved);
            const auto* operations = submitted.program.Operations().data();
            const auto* words = submitted.program.OperandArena().data();
            const auto* blocks = submitted.program.Blocks().data();
            const auto first_cell = submitted.program.Operands(submitted.program.Operations().front());
            owner = std::move(submitted.program);
            CHECK(!submitted.program);
            CHECK(owner.Operations().data() == operations);
            CHECK(owner.OperandArena().data() == words);
            CHECK(owner.Blocks().data() == blocks);
            CHECK(first_cell.begin() == owner.Operands(owner.Operations().front()).begin());
            CHECK(first_cell.front().Payload() == 0x8000000000000001ULL);
        }
        CHECK(owner);
        CHECK(owner.UnitId() == unit);
        CHECK(owner.Target() == target);
        CHECK(owner.Contains(value));
        CHECK(owner.Operations().size() == 129);
        CHECK(owner.OperandArena().size() == 130);
        CHECK(CellAt(owner.Operands(owner.Operations()[value.node.value]), 0).Payload() == 0x8000000000000001ULL);
        CHECK(owner.Blocks()[0].terminal.value == value.node);
        Program moved{std::move(owner)};
        CHECK(!owner);
        CHECK(moved.Contains(value));
        CHECK(moved.Blocks()[0].terminal.value == value.node);
    }
}

void StaticOpcodeWritesKeepTheConstructionContract() {
    Builder foreign{Scope::Block};
    const auto foreign_block = foreign.CreateBlock();
    CHECK(foreign.BeginBlock(foreign_block.block) == Error::None);
    const auto foreign_value = Literal(foreign, 9).GetValue();
    Builder builder{Scope::Block};
    CHECK(builder.Reserve(3, 6, 1) == Error::None);
    const auto block = builder.CreateBlock();
    CHECK(builder.BeginBlock(block.block) == Error::None);
    const auto value = builder.Emit<OpCode::LoadImm>(Type::Void,
            {Operand::Immediate(0xfedcba9876543210ULL, Type::U64)});
    CHECK(value && value.node.value == 0);
    CHECK(builder.Emit<OpCode::BitCast>(Type::Void,
            {Operand::ValueOf(foreign_value)}).error == Error::ForeignUnit);
    CHECK(builder.Emit<OpCode::LoadImm>(Type::Void, {}).error == Error::MissingOperand);
    CHECK(builder.Emit<OpCode::LoadImm>(Type::Void,
            {Operand::Immediate(256, Type::U8)}).error == Error::InvalidRange);
    CHECK(builder.Emit<OpCode::Nop>(Type::U64, {}).error == Error::InvalidResultType);
    CHECK(builder.Emit<OpCode::Add>(Type::Void,
            {Operand::ValueOf(value.GetValue()), Operand::Expression(OperandOp::Add, 2),
             Operand::Immediate(1, Type::U64)}).error == Error::MissingOperand);
    CHECK(builder.Emit<OpCode::Add>(Type::Void,
            {Operand::ValueOf(value.GetValue()), Operand::Expression(OperandOp::Identity, 2),
             Operand::Immediate(1, Type::U64), Operand::Immediate(2, Type::U64)}).error == Error::InvalidRange);
    CHECK(builder.Emit<OpCode::Add>(Type::Void,
            {Operand::ValueOf(value.GetValue()), Operand::ValueOf(foreign_value, Type::U32)}).error == Error::ForeignUnit);
    CHECK(builder.Emit<OpCode::Add>(Type::Void,
            {Operand::ValueOf(value.GetValue()), Operand::Expression(OperandOp::Identity),
             Operand::Immediate(1, Type::F32)}).error == Error::InvalidType);
    const auto sum = builder.Emit<OpCode::Add>(Type::Void,
            {Operand::ValueOf(value.GetValue(), Type::U32), Operand::Immediate(3, Type::U32)},
            {0x41, FlagProfile::ArithmeticRaw, false});
    CHECK(sum && sum.node.value == 1);
    AllocationCount compact_append;
    const auto generic = builder.Append(OpCode::Add, Type::Void,
            {Operand::ValueOf(value.GetValue(), Type::U32), Operand::Expression(OperandOp::Identity),
             Operand::Immediate(3, Type::U32)}, {0x41, FlagProfile::ArithmeticRaw, false});
    const auto compact_allocations = compact_append.Stop();
    // Two actual words fill the exact reserve; the discarded Identity
    // header must not inflate an allocation estimate to three words.
    CHECK(compact_allocations == 0);
    CHECK(generic && generic.node.value == 2);
    CHECK(builder.EndBlock(Terminal::Return(sum.GetValue())) == Error::None);
    const auto submitted = SubmitWithoutAllocation(builder);
    CHECK(submitted.program.Operations().size() == 3);
    CHECK(LiveCellCount(submitted.program) == 5);
    CHECK(submitted.program.OperandArena().size() == 6);
    CHECK(submitted.program.Operations()[1].type == Type::U32);
    CHECK(submitted.program.Operations()[1].Metadata().flag_outputs == 0x41);
    CHECK(CellAt(submitted.program.Operands(submitted.program.Operations()[0]), 0).Payload() ==
            0xfedcba9876543210ULL);
    CHECK(CellAt(submitted.program.Operands(submitted.program.Operations()[1]), 0).TypeView() == Type::U32);
    CHECK(submitted.program.Operations()[1].Metadata() == submitted.program.Operations()[2].Metadata());
    CHECK(submitted.program.Operations()[1].type == submitted.program.Operations()[2].type);
    const auto known_cells = submitted.program.Operands(submitted.program.Operations()[1]);
    const auto generic_cells = submitted.program.Operands(submitted.program.Operations()[2]);
    CHECK(known_cells.WordCount() == 2 && generic_cells.WordCount() == 2);
    auto known = known_cells.begin();
    auto dynamic = generic_cells.begin();
    for (; known != known_cells.end() && dynamic != generic_cells.end(); ++known, ++dynamic) {
        CHECK((*known).Kind() == (*dynamic).Kind() && (*known).TypeView() == (*dynamic).TypeView());
        CHECK((*known).Payload() == (*dynamic).Payload());
        CHECK((*known).Count() == (*dynamic).Count() && (*known).Code() == (*dynamic).Code());
    }
    CHECK(known == known_cells.end() && dynamic == generic_cells.end());
    CHECK(builder.Emit<OpCode::Nop>(Type::Void, {}).error == Error::Consumed);


}

void ExplicitWorkAccounting() {
    Builder builder{Scope::Function};
    CHECK((builder.CountWork() == WorkCounts{}));
    const auto first = builder.CreateBlock(), second = builder.CreateBlock();
    CHECK(first && second && builder.BeginBlock(first.block) == Error::None);
    const auto seed = Literal(builder, UINT64_MAX);
    const auto sum = builder.Emit<OpCode::Add>(Type::Void,
            {Operand::ValueOf(seed.GetValue()), Operand::ValueOf(seed.GetValue())});
    CHECK(sum);
    AllocationCount observation;
    CHECK((builder.CountWork() == WorkCounts{2, 2, 4, 2}));
    CHECK(observation.Stop() == 0);
    CHECK(builder.EndBlock(Terminal::Branch(sum.GetValue(), first.block, second.block)) == Error::None);
    CHECK((builder.CountWork() == WorkCounts{2, 3, 4, 2}));
    CHECK(builder.BeginBlock(second.block) == Error::None);
    CHECK(builder.Emit<OpCode::BitCast>(Type::Void, {Operand::ValueOf(seed.GetValue())}));
    CHECK(builder.EndBlock(Terminal::Return(seed.GetValue())) == Error::None);
    CHECK((builder.CountWork() == WorkCounts{3, 5, 5, 2}));
    auto owner = std::move(SubmitWithoutAllocation(builder).program);
    CHECK((builder.CountWork() == WorkCounts{}));
    auto editor = Builder::Edit(std::move(owner));
    CHECK((editor.CountWork() == WorkCounts{3, 5, 5, 2}));
    CHECK(editor.Rewrite(sum.node, OpCode::LoadImm, Type::Void,
            {Operand::Immediate(3, Type::U64)}) == Error::None);
    // Non-tail shrink leaves a retired word; only live Value uses are counted.
    CHECK((editor.CountWork() == WorkCounts{3, 3, 5, 2}));

    Builder shifted{Scope::Block};
    const auto block = shifted.CreateBlock();
    CHECK(block && shifted.BeginBlock(block.block) == Error::None);
    const auto base = Literal(shifted, 1);
    const auto address = shifted.Emit<OpCode::GetOperand>(Type::U64,
            {Operand::Expression(OperandOp::AddShift, 2, 3),
             Operand::ValueOf(base.GetValue()), Operand::Immediate(5, Type::U64)});
    CHECK(address);
    CHECK(shifted.Emit<OpCode::GetOperand>(Type::U64,
            {Operand::Expression(OperandOp::AddShift, 2, 64),
             Operand::ValueOf(base.GetValue()), Operand::Immediate(5, Type::U64)}).error == Error::InvalidRange);
    CHECK(shifted.Emit<OpCode::GetOperand>(Type::U64,
            {Operand::Expression(OperandOp::Add, 2, 3),
             Operand::ValueOf(base.GetValue()), Operand::Immediate(5, Type::U64)}).error == Error::InvalidRange);
    CHECK(shifted.EndBlock(Terminal::Return(address.GetValue())) == Error::None);
    auto ready = SubmitWithoutAllocation(shifted);
    const auto expression = ready.program.Operands(address.node).front();
    CHECK(expression.ExpressionOp() == OperandOp::AddShift && expression.ShiftAmount() == 3);
    CHECK(expression.Code() == (3u << 8 | static_cast<unsigned>(OperandOp::AddShift)));

    Builder widths{Scope::Block};
    const auto wb = widths.CreateBlock();
    CHECK(wb && widths.BeginBlock(wb.block) == Error::None);
    const auto wide = Literal(widths, UINT64_MAX);
    const auto narrow = widths.Emit<OpCode::BitExtract>(Type::U32,
            {Operand::ValueOf(wide.GetValue()), Operand::Immediate(8, Type::U8), Operand::Immediate(16, Type::U8)});
    const auto masked = widths.Append(OpCode::And, Type::U8,
            {Operand::ValueOf(wide.GetValue(), Type::U16), Operand::Immediate(255, Type::U16)});
    CHECK(narrow && masked);
    CHECK(widths.Emit<OpCode::And>(Type::F32,
            {Operand::ValueOf(wide.GetValue()), Operand::Immediate(1, Type::U64)}).error == Error::InvalidResultType);
    CHECK(widths.EndBlock(Terminal::Return(narrow.GetValue())) == Error::None);
    auto result = SubmitWithoutAllocation(widths);
    CHECK(result.program.Operations()[narrow.node.value].type == Type::U32);
    CHECK(result.program.Operands(narrow.node).front().TypeView() == Type::Void);
    CHECK(result.program.Operations()[masked.node.value].type == Type::U8);
    CHECK(result.program.Operands(masked.node).front().TypeView() == Type::U16);
}

void OwnerLocalRewrites() {
    const TargetInfo target{TargetMode::Known, 17, UINT64_C(0x8000000000000001)};
    Builder initial{Scope::Function, target};
    CHECK(initial.Reserve(3, 3, 1) == Error::None);
    const auto block = initial.CreateBlock();
    CHECK(block && initial.BeginBlock(block.block) == Error::None);
    const auto seed = Literal(initial, 7);
    const auto changed = initial.Emit<OpCode::BitCast>(Type::Void, {Operand::ValueOf(seed.GetValue())});
    const auto later = Literal(initial, 9);
    CHECK(changed && initial.EndBlock(Terminal::Return(later.GetValue())) == Error::None);
    auto submitted = SubmitWithoutAllocation(initial);
    Program owner = std::move(submitted.program);
    const auto unit = owner.UnitId();
    const auto* operations = owner.Operations().data();
    const auto* blocks = owner.Blocks().data();
    std::array<Cell, 3> old_words{};
    std::copy(owner.OperandArena().begin(), owner.OperandArena().end(), old_words.begin());
    const auto old_record = owner.Operations()[changed.node.value];
    CHECK(owner.Operands(old_record).empty()); // Record copies carry no ownership.
    CHECK(!owner.Operands(owner.Operations()[changed.node.value]).empty());
    CHECK(!owner.Operands(changed.node).empty());
    CHECK(owner.Operands(NodeId{}).empty() && owner.Operands(NodeId{3}).empty());

    Builder foreign_builder{Scope::Block};
    const auto fb = foreign_builder.CreateBlock();
    CHECK(foreign_builder.BeginBlock(fb.block) == Error::None);
    const auto foreign = Literal(foreign_builder, 13);
    CHECK(foreign_builder.EndBlock(Terminal::Return(foreign.GetValue())) == Error::None);
    auto foreign_owner = SubmitWithoutAllocation(foreign_builder);
    CHECK(owner.Operands(foreign_owner.program.Operations()[0]).empty());

    AllocationCount moving;
    auto editor = Builder::Edit(std::move(owner));
    const auto move_allocations = moving.Stop();
    CHECK(move_allocations == 0 && !owner);
    // These are fresh calls on the emptied source, never uses of an old borrow.
    CHECK(owner.Operands(changed.node).empty() && owner.OperandArena().empty());
    CHECK(editor.CreateBlock().error == Error::EditOnly);
    CHECK(editor.Append(OpCode::Nop, Type::Void, {}).error == Error::NoActiveBlock);
    AllocationCount rejection;
    CHECK(editor.Rewrite(changed.node, OpCode::BitCast, Type::Void,
            {Operand::ValueOf(changed.GetValue())} ) == Error::InvalidValue);
    CHECK(editor.Rewrite(changed.node, OpCode::BitCast, Type::Void,
            {Operand::ValueOf(later.GetValue())} ) == Error::InvalidValue);
    CHECK(editor.Rewrite(changed.node, OpCode::BitCast, Type::Void,
            {Operand::ValueOf(foreign.GetValue())} ) == Error::ForeignUnit);
    CHECK(editor.Rewrite(changed.node, OpCode::BitCast, Type::Void,
            {Operand::ValueOf(seed.GetValue(), Type::S32)} ) == Error::InvalidResultType);
    CHECK(editor.Rewrite(changed.node, OpCode::Nop, Type::Void, {}) == Error::InvalidResultType);
    CHECK(editor.Rewrite(NodeId{3}, OpCode::Nop, Type::Void, {}) == Error::InvalidValue);
    CHECK(editor.Rewrite(changed.node, OpCode::CallLocation, Type::U64,
            {Operand::Lambda(UINT64_MAX), Operand::Immediate(UINT64_MAX, Type::U64),
             Operand::Arguments(2), Operand::ValueOf(seed.GetValue()),
             Operand::ValueOf(changed.GetValue())}) == Error::InvalidValue);
    const auto rejection_allocations = rejection.Stop();
    CHECK(rejection_allocations == 0);

    // This non-tail replacement really needs a larger word arena. Its only
    // growth failure must leave the whole old record and old prefix intact.
    const std::array<Operand, 5> call{
        Operand::Lambda(UINT64_MAX), Operand::Immediate(UINT64_MAX, Type::U64),
        Operand::Arguments(2), Operand::ValueOf(seed.GetValue()), Operand::Immediate(UINT64_MAX, Type::U64)};
    bool failed{};
    failure_countdown = 1;
    try { (void)editor.Rewrite(changed.node, OpCode::CallLocation, Type::U64, call); }
    catch (const std::bad_alloc&) { failed = true; }
    failure_countdown = 0;
    CHECK(failed);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(owner.UnitId() == unit && owner.Target() == target);
    CHECK(owner.Operations().data() == operations && owner.Blocks().data() == blocks);
    CHECK(owner.Operations().size() == 3 && owner.Blocks()[0].node_count == 3);
    CHECK(owner.OperandArena().size() == old_words.size());
    for (std::size_t i = 0; i < old_words.size(); ++i)
        CHECK(owner.OperandArena()[i].bits == old_words[i].bits);
    const auto& kept = owner.Operations()[changed.node.value];
    CHECK(kept.opcode == old_record.opcode && kept.type == old_record.type && kept.block == old_record.block);
    CHECK(kept.Metadata() == old_record.Metadata() && kept.first_operand == old_record.first_operand);
    CHECK(kept.operand_count == old_record.operand_count);

    editor = Builder::Edit(std::move(owner));
    const OperationMetadata metadata{UINT64_MAX, FlagProfile::LogicalZeroCVA, true, true};
    AllocationCount inplace;
    CHECK(editor.Rewrite(changed.node, OpCode::LoadImm, Type::Void,
            {Operand::Immediate(42, Type::U64)}, {0, FlagProfile::None, false, true}) == Error::InvalidRange);
    CHECK(editor.Operations()[changed.node.value].Metadata() == old_record.Metadata());
    CHECK(editor.Operations()[changed.node.value].first_operand == old_record.first_operand);
    CHECK(editor.Rewrite(changed.node, OpCode::LoadImm, Type::Void,
            {Operand::Immediate(42, Type::U64)}, metadata) == Error::None);
    const auto inplace_allocations = inplace.Stop();
    CHECK(inplace_allocations == 0);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(owner.Operations()[changed.node.value].first_operand == old_record.first_operand);
    CHECK(owner.Operations()[changed.node.value].Metadata() == metadata);
    CHECK(owner.OperandArena().size() == old_words.size());
    CHECK(owner.Operands(changed.node).front().Payload() == 42);
    CHECK(owner.Contains(seed.GetValue()) && owner.Contains(changed.GetValue()) && owner.Contains(later.GetValue()));

    editor = Builder::Edit(std::move(owner));
    CHECK(editor.Rewrite(changed.node, OpCode::CallLocation, Type::U64, call) == Error::None);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(owner.Operations()[changed.node.value].first_operand == 3);
    CHECK(owner.Operations()[changed.node.value].operand_count == 8);
    CHECK(owner.OperandArena().size() == 11); // Old non-tail range is retired, not a second program.
    CHECK(LiveCellCount(owner) == 7);
    const auto retained_bytes = owner.ReservedBytes();
    editor = Builder::Edit(std::move(owner));
    AllocationCount repeat;
    for (unsigned i = 0; i < 32; ++i) {
        CHECK(editor.Rewrite(changed.node, OpCode::LoadImm, Type::Void,
                {Operand::Immediate(i, Type::U64)}) == Error::None);
        CHECK(editor.Rewrite(changed.node, OpCode::CallLocation, Type::U64, call) == Error::None);
    }
    const auto repeat_allocations = repeat.Stop();
    CHECK(repeat_allocations == 0);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(owner.UnitId() == unit && owner.Operations().size() == 3);
    CHECK(owner.Operations().data() == operations && owner.Blocks().data() == blocks);
    CHECK(owner.ReservedBytes() == retained_bytes && owner.OperandArena().size() == 11);
    CHECK(owner.Blocks()[0].terminal.value == later.node && owner.Blocks()[0].node_count == 3);
    const auto active = owner.Operands(changed.node);
    CHECK(active.WordCount() == 8 && CellCount(active) == 5);
    CHECK(active.front().Payload() == UINT64_MAX);
    CHECK(CellAt(active, 3).Payload() == seed.node.value);
    CHECK(CellAt(active, 4).Payload() == UINT64_MAX);
}

void ZeroWordAnchorsSurviveTailEdits() {
    Builder initial{Scope::Block};
    CHECK(initial.Reserve(2, 1, 1) == Error::None);
    const auto block = initial.CreateBlock();
    CHECK(initial.BeginBlock(block.block) == Error::None);
    const auto first = Literal(initial, 0);
    const auto empty = initial.Emit<OpCode::Zero>(Type::U64, {});
    CHECK(empty && initial.EndBlock(Terminal::Return(empty.GetValue())) == Error::None);
    auto owner = std::move(SubmitWithoutAllocation(initial).program);
    CHECK(owner.Operations()[empty.node.value].first_operand == 1);
    auto editor = Builder::Edit(std::move(owner));
    bool failed{};
    failure_countdown = 1;
    try {
        (void)editor.Rewrite(first.node, OpCode::GetOperand, Type::U64,
                {Operand::Expression(OperandOp::Add, 2), Operand::Immediate(UINT64_MAX, Type::U64),
                 Operand::Immediate(UINT64_MAX, Type::U64)});
    } catch (const std::bad_alloc&) { failed = true; }
    failure_countdown = 0;
    CHECK(failed);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(owner.OperandArena().size() == 1 && owner.Operands(first.node).front().Payload() == 0);
    CHECK(owner.Operations()[empty.node.value].first_operand == 1 && owner.Operands(empty.node).empty());
    editor = Builder::Edit(std::move(owner));
    CHECK(editor.Rewrite(first.node, OpCode::Zero, Type::U64, {}) == Error::None);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(owner.OperandArena().empty());
    CHECK(owner.Operations()[empty.node.value].first_operand == 1 && owner.Operands(empty.node).empty());
    editor = Builder::Edit(std::move(owner));
    CHECK(editor.Rewrite(empty.node, OpCode::LoadImm, Type::Void,
            {Operand::Immediate(5, Type::U64)}) == Error::None);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(owner.OperandArena().size() == 1 && owner.Operands(empty.node).front().Payload() == 5);
    CHECK(owner.Operands(first.node).empty());
}

void EditableViewsAndUnusedErase() {
    Builder builder{Scope::Function};
    CHECK(builder.UnitId() != 0 && builder.Operations().empty() && builder.Blocks().empty());
    CHECK(builder.Operands(NodeId{}).empty());
    const auto first = builder.CreateBlock(), second = builder.CreateBlock();
    CHECK(first && second && builder.BeginBlock(first.block) == Error::None);
    const auto seed = Literal(builder, 42);
    const auto unused = builder.Emit<OpCode::Add>(Type::Void,
            {Operand::ValueOf(seed.GetValue()), Operand::Immediate(1, Type::U64)});
    const std::array reads{
        builder.Emit<OpCode::LoadUniform>(Type::Void, {Operand::Uniform(8, Type::U64)}),
        builder.Emit<OpCode::LoadLocal>(Type::Void, {Operand::Local(0, Type::U64)}),
        builder.Emit<OpCode::GetHostGPR>(Type::U64,
                {Operand::Immediate(1, Type::U16), Operand::Immediate(0, Type::U8)}),
        builder.Emit<OpCode::TestFlags>(Type::Void, {Operand::Flags(1)})};
    const std::array observable{
        builder.Emit<OpCode::LoadMemory>(Type::U64, {Operand::Immediate(4096, Type::U64)}),
        builder.Emit<OpCode::LoadMemoryTSO>(Type::U64, {Operand::Immediate(4096, Type::U64)}),
        builder.Emit<OpCode::StoreLocal>(Type::Void,
                {Operand::Local(0, Type::U64), Operand::ValueOf(seed.GetValue())}),
        builder.Emit<OpCode::AddPhi>(Type::U64, {Operand::Arguments(0)}),
        builder.Emit<OpCode::CallLambda>(Type::U64,
                {Operand::Lambda(), Operand::Immediate(4096, Type::U64)})};
    const std::array flagged{
        builder.Emit<OpCode::Zero>(Type::U64, {}, {UINT64_C(1) << 63}),
        builder.Emit<OpCode::Zero>(Type::U64, {}, {0, FlagProfile::ArithmeticRaw}),
        builder.Emit<OpCode::Zero>(Type::U64, {}, {0, FlagProfile::None, true})};
    CHECK(unused);
    for (const auto result : reads) CHECK(result);
    for (const auto result : observable) CHECK(result);
    for (const auto result : flagged) CHECK(result);
    CHECK(builder.EraseUnused(unused.node) == Error::EditOnly);
    CHECK(builder.EndBlock(Terminal::Jump(second.block)) == Error::None);
    CHECK(builder.BeginBlock(second.block) == Error::None);
    const auto later = Literal(builder, 9);
    CHECK(builder.EndBlock(Terminal::Return(later.GetValue())) == Error::None);
    auto owner = std::move(SubmitWithoutAllocation(builder).program);
    const auto unit = owner.UnitId();
    const auto* operations = owner.Operations().data();
    const auto* blocks = owner.Blocks().data();
    const auto nodes = owner.Operations().size();
    const auto old_block_count = owner.Blocks()[0].node_count;
    const auto words = owner.OperandArena().size();
    AllocationCount editing;
    auto editor = Builder::Edit(std::move(owner), words + 2 * nodes);
    CHECK(!owner && editor.UnitId() == unit);
    CHECK(editor.Operations().data() == operations && editor.Blocks().data() == blocks);
    CHECK(editor.Operations().size() == nodes && editor.Blocks().size() == 2);
    CHECK(editor.Operands(unused.node).WordCount() == 2);
    CHECK(editor.Operands(NodeId{}).empty() && editor.Operands(NodeId{UINT32_MAX - 1}).empty());
    for (const auto result : observable) {
        const auto op = editor.Operations()[result.node.value].opcode;
        CHECK(editor.EraseUnused(result.node) == Error::ObservableOperation);
        CHECK(editor.Operations()[result.node.value].opcode == op);
    }
    for (const auto result : flagged)
        CHECK(editor.EraseUnused(result.node) == Error::ObservableOperation);
    // No data/flags/terminal use points to any of these unused results.
    CHECK(editor.EraseUnused(unused.node) == Error::None);
    for (const auto result : reads) CHECK(editor.EraseUnused(result.node) == Error::None);
    CHECK(editor.EraseUnused(unused.node) == Error::None); // Stable tombstone.
    CHECK(editor.Operations().data() == operations && editor.Blocks().data() == blocks);
    const auto& erased = editor.Operations()[unused.node.value];
    CHECK(erased.opcode == OpCode::Nop && erased.type == Type::Void && erased.operand_count == 0);
    CHECK(erased.block == first.block.id && editor.Operands(unused.node).empty());
    CHECK(editor.Blocks()[0].node_count == old_block_count);
    CHECK(editor.Rewrite(later.node, OpCode::BitCast, Type::U64,
            {Operand::ValueOf(unused.GetValue())}) == Error::InvalidValue);
    CHECK(editor.EraseUnused(NodeId{}) == Error::InvalidValue);
    CHECK(editing.Stop() == 0);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(!owner.Contains(unused.GetValue()) && owner.Contains(seed.GetValue()) && owner.Contains(later.GetValue()));
    CHECK(owner.UnitId() == unit && owner.Operations().size() == nodes);
    CHECK(owner.OperandArena().size() == words);
    CHECK(editor.UnitId() == 0 && editor.Operations().empty() && editor.Blocks().empty());
    CHECK(editor.Operands(seed.node).empty() && editor.EraseUnused(seed.node) == Error::Consumed);
}

void EditWordBudgetsAreAtomic() {
    Builder builder{Scope::Block};
    CHECK(builder.Reserve(3, 3, 1) == Error::None);
    const auto block = builder.CreateBlock();
    CHECK(block && builder.BeginBlock(block.block) == Error::None);
    const auto anchor = Literal(builder, 0), first = Literal(builder, 1), second = Literal(builder, 2);
    CHECK(builder.EndBlock(Terminal::Return()) == Error::None);
    auto owner = std::move(SubmitWithoutAllocation(builder).program);
    const auto unit = owner.UnitId();
    const auto* operations = owner.Operations().data();
    const auto initial = owner.Operations()[first.node.value];
    std::array<Cell, 3> prefix{};
    std::copy(owner.OperandArena().begin(), owner.OperandArena().end(), prefix.begin());
    auto bounded = Builder::Edit(std::move(owner), 3);
    auto editor = std::move(bounded); // The actual bound moves with its owner.
    CHECK(bounded.UnitId() == 0);
    AllocationCount rejected;
    failure_countdown = 1; // Growth would allocate if reached.
    const auto error = editor.Rewrite(first.node, OpCode::Add, Type::Void,
            {Operand::ValueOf(anchor.GetValue()), Operand::Immediate(1, Type::U64)});
    failure_countdown = 0;
    CHECK(error == Error::EditBudgetExceeded && rejected.Stop() == 0);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(owner.Operations().data() == operations && owner.UnitId() == unit);
    const auto& kept = owner.Operations()[first.node.value];
    CHECK(kept.opcode == initial.opcode && kept.type == initial.type && kept.block == initial.block);
    CHECK(kept.first_operand == initial.first_operand && kept.operand_count == initial.operand_count);
    CHECK(kept.Metadata() == initial.Metadata() && owner.OperandArena().size() == prefix.size());
    for (std::size_t i = 0; i < prefix.size(); ++i) CHECK(owner.OperandArena()[i].bits == prefix[i].bits);

    const auto cap = owner.OperandArena().size() + 2 * owner.Operations().size();
    editor = Builder::Edit(std::move(owner), cap);
    const auto add = [&](NodeId node) {
        return editor.Rewrite(node, OpCode::Add, Type::Void,
                {Operand::ValueOf(anchor.GetValue()), Operand::Immediate(1, Type::U64)});
    };
    const auto literal = [&](NodeId node) {
        return editor.Rewrite(node, OpCode::LoadImm, Type::Void, {Operand::Immediate(1, Type::U64)});
    };
    CHECK(add(first.node) == Error::None && add(second.node) == Error::None);
    CHECK(literal(first.node) == Error::None && literal(second.node) == Error::None);
    CHECK(add(first.node) == Error::None);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(owner.OperandArena().size() == 8 && cap == 9);
    const auto old_record = owner.Operations()[second.node.value];
    std::array<Cell, 8> before{};
    std::copy(owner.OperandArena().begin(), owner.OperandArena().end(), before.begin());
    editor = Builder::Edit(std::move(owner), cap);
    // The next non-tail growth needs ten actual words. Reject even if an
    // existing geometric reserve already has room; live words alone would fit.
    AllocationCount overflow;
    CHECK(add(second.node) == Error::EditBudgetExceeded);
    CHECK(overflow.Stop() == 0);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    const auto& same = owner.Operations()[second.node.value];
    CHECK(same.opcode == old_record.opcode && same.type == old_record.type && same.block == old_record.block);
    CHECK(same.first_operand == old_record.first_operand && same.operand_count == old_record.operand_count);
    CHECK(same.Metadata() == old_record.Metadata() && owner.OperandArena().size() == before.size());
    for (std::size_t i = 0; i < before.size(); ++i) CHECK(owner.OperandArena()[i].bits == before[i].bits);
}

void TypedFlagAndConditionInputs() {
    Builder builder{Scope::Block};
    CHECK(builder.Reserve(3, 4, 1) == Error::None);
    const auto block = builder.CreateBlock(); CHECK(block && builder.BeginBlock(block.block) == Error::None);
    CHECK(builder.Append(OpCode::CondSet, Type::Void,
            {Operand::Condition(static_cast<Condition>(255))}).error == Error::InvalidRange);
    const auto typed = builder.Emit<OpCode::TestFlags>(Type::Void, {Operand::Flags(Flag::All)});
    const auto raw = builder.Append(OpCode::TestFlags, Type::Void, {Operand::Flags(UINT64_MAX)});
    const auto condition = builder.Emit<OpCode::CondSet>(Type::Void, {Operand::Condition(Condition::GE)});
    CHECK(typed && raw && condition);
    CHECK(builder.EndBlock(Terminal::Return(condition.GetValue())) == Error::None);
    auto submitted = SubmitWithoutAllocation(builder);
    CHECK(submitted.program.Operands(typed.node).front().Payload() == 95);
    CHECK(submitted.program.Operands(raw.node).front().Payload() == UINT64_MAX);
    CHECK(submitted.program.Operands(raw.node).WordCount() == 2);
    CHECK(submitted.program.Operands(condition.node).front().Payload() == 10);
}

void MetadataUpdatesStayLocal() {
    Builder builder{Scope::Function};
    CHECK(builder.Reserve(4, 16, 2) == Error::None);
    const auto first = builder.CreateBlock(), second = builder.CreateBlock(); CHECK(first && second);
    CHECK(builder.BeginBlock(first.block) == Error::None);
    const auto seed = Literal(builder, UINT64_MAX);
    const auto alu = builder.Emit<OpCode::Add>(Type::U64,
            {Operand::ValueOf(seed.GetValue()), Operand::Immediate(UINT64_C(1) << 63, Type::U64)},
            {FlagBits(Flag::Zero), FlagProfile::ArithmeticCanonicalCarry, false, true});
    const auto unused = builder.Emit<OpCode::Zero>(Type::U64, {}); CHECK(alu && unused);
    CHECK(builder.Operations()[alu.node.value].Metadata().discard_omitted_flags);
    CHECK(builder.Operations()[alu.node.value].properties & 8);
    const auto unit = builder.UnitId();
    const auto* operations = builder.Operations().data();
    const auto* blocks = builder.Blocks().data();
    const auto original = builder.Operations()[alu.node.value];
    const auto work = builder.CountWork();
    std::array<Cell, 5> prefix{};
    std::copy_n(builder.Operands(seed.node).data(), prefix.size(), prefix.begin());
    const auto unchanged = [&](const Builder& owner) {
        CHECK(owner.UnitId() == unit && owner.Operations().data() == operations && owner.Blocks().data() == blocks);
        const auto& op = owner.Operations()[alu.node.value];
        CHECK(op.opcode == original.opcode && op.type == original.type && op.block == original.block);
        CHECK(op.first_operand == original.first_operand && op.operand_count == original.operand_count);
        for (std::size_t i = 0; i < prefix.size(); ++i)
            CHECK(owner.Operands(seed.node).data()[i].bits == prefix[i].bits);
    };
    const OperationMetadata full{UINT64_MAX, FlagProfile::ArithmeticCanonicalCarry, true, true};
    AllocationCount updating;
    failure_countdown = 1; // Even a single hidden allocation would fail.
    CHECK(builder.SetMetadata(alu.node, full) == Error::None);
    CHECK(builder.SetMetadata(alu.node, {0, FlagProfile::None, false, true}) == Error::InvalidRange);
    CHECK(builder.Append(OpCode::Add, Type::U64,
            {Operand::ValueOf(seed.GetValue()), Operand::Immediate(1, Type::U64)},
            {0, FlagProfile::None, false, true}).error == Error::InvalidRange);
    CHECK(builder.Emit<OpCode::Add>(Type::U64,
            {Operand::ValueOf(seed.GetValue()), Operand::Immediate(1, Type::U64)},
            {0, FlagProfile::None, false, true}).error == Error::InvalidRange);
    CHECK(builder.SetMetadata(alu.node, {0, static_cast<FlagProfile>(255), false}) == Error::InvalidRange);
    CHECK(builder.SetMetadata(NodeId{}, {}) == Error::InvalidValue);
    CHECK(builder.SetMetadata(NodeId{3}, {}) == Error::InvalidValue);
    CHECK(builder.SetMetadata(NodeId{UINT32_MAX - 1}, {}) == Error::InvalidValue);
    failure_countdown = 0;
    CHECK(updating.Stop() == 0 && builder.Operations()[alu.node.value].Metadata() == full);
    unchanged(builder);
    CHECK(builder.CountWork() == work);
    CHECK(HasAnyEffect(EffectsFor(OpCode::Add, builder.Operations()[alu.node.value].Metadata()), Effect::WriteFlags));
    CHECK(builder.EndBlock(Terminal::Jump(second.block)) == Error::None);
    // No active block is required, and the earlier block remains the owner.
    CHECK(builder.SetMetadata(alu.node, {0, FlagProfile::LogicalZeroCVA}) == Error::None);
    CHECK(builder.BeginBlock(second.block) == Error::None);
    const auto later = Literal(builder, 9);
    CHECK(builder.EndBlock(Terminal::Return(later.GetValue())) == Error::None);
    CHECK(builder.SetMetadata(alu.node, full) == Error::None);
    auto owner = std::move(SubmitWithoutAllocation(builder).program);
    CHECK(builder.SetMetadata(alu.node, full) == Error::Consumed);
    const auto reserved = owner.ReservedBytes(), stored = owner.StorageBytes();
    const auto words = owner.OperandArena().size();
    std::array<Cell, 6> payload{};
    CHECK(words == payload.size());
    std::copy(owner.OperandArena().begin(), owner.OperandArena().end(), payload.begin());

    AllocationCount editing;
    // Metadata does not grow or inspect the arena, even under a zero edit cap.
    auto editor = Builder::Edit(std::move(owner), 0);
    failure_countdown = 1;
    CHECK(editor.SetMetadata(alu.node, {FlagBits(Flag::All), FlagProfile::ArithmeticRaw}) == Error::None);
    CHECK(editor.EraseUnused(alu.node) == Error::ObservableOperation);
    CHECK(editor.SetMetadata(alu.node, {0, FlagProfile::LogicalZeroCVA}) == Error::None);
    CHECK(EffectsFor(OpCode::Add, editor.Operations()[alu.node.value].Metadata()) == Effect::None);
    CHECK(editor.EraseUnused(alu.node) == Error::ObservableOperation); // Profile alone pins.
    CHECK(editor.SetMetadata(alu.node, {0, FlagProfile::None, true}) == Error::None);
    CHECK(editor.EraseUnused(alu.node) == Error::ObservableOperation); // branch_only alone pins.
    unchanged(editor);
    CHECK(editor.SetMetadata(alu.node, {}) == Error::None);
    // This unused result has no data, terminal or flag readers.
    CHECK(editor.EraseUnused(alu.node) == Error::None);
    CHECK(editor.SetMetadata(alu.node, full) == Error::None); // No opcode/backend admission at this write port.
    CHECK(editor.Operations()[alu.node.value].opcode == OpCode::Nop);
    CHECK(editor.EraseUnused(alu.node) == Error::ObservableOperation);
    CHECK(editor.SetMetadata(alu.node, {}) == Error::None);
    failure_countdown = 0;
    CHECK(editing.Stop() == 0);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(editor.SetMetadata(alu.node, {}) == Error::Consumed);
    CHECK(owner.UnitId() == unit && owner.Operations().data() == operations && owner.Blocks().data() == blocks);
    CHECK(owner.ReservedBytes() == reserved && owner.StorageBytes() == stored && owner.OperandArena().size() == words);
    CHECK(owner.Operations().size() == 4 && owner.Blocks()[0].node_count == 3 && owner.Blocks()[1].node_count == 1);
    CHECK(owner.Blocks()[0].terminal.first == second.block.id && owner.Blocks()[1].terminal.value == later.node);
    for (std::size_t i = 0; i < payload.size(); ++i) CHECK(owner.OperandArena()[i].bits == payload[i].bits);
}

void EntryBelongsToTheActualOwner() {
    Program empty;
    CHECK(!empty.EntryBlock());
    Builder single{Scope::Block};
    CHECK(!single.EntryBlock());
    CHECK(single.SetEntry({single.UnitId(), BlockId{0}}) == Error::InvalidBlock);
    AllocationCount incomplete;
    CHECK(single.Submit().error == Error::IncompleteScope);
    CHECK(incomplete.Stop() == 0 && !single.EntryBlock());
    const auto only = single.CreateBlock(); CHECK(only && only.block.id == BlockId{0});
    CHECK(single.EntryBlock() == only.block.id);
    CHECK(single.BeginBlock(only.block) == Error::None);
    CHECK(single.EndBlock(Terminal::Return()) == Error::None); // An empty body still has a real entry.
    auto default_owner = std::move(SubmitWithoutAllocation(single).program);
    CHECK(default_owner.EntryBlock() == only.block.id && default_owner.Operations().empty());
    CHECK(!single.EntryBlock() && single.SetEntry(only.block) == Error::Consumed);

    const TargetInfo target{TargetMode::Unknown, 17, UINT64_MAX};
    Builder builder{Scope::Function, target};
    CHECK(builder.Reserve(4, 6, 3) == Error::None);
    const auto first = builder.CreateBlock(), entry = builder.CreateBlock(), last = builder.CreateBlock();
    CHECK(first && entry && last && builder.EntryBlock() == first.block.id);
    Builder foreign{Scope::Block};
    const auto foreign_block = foreign.CreateBlock(); CHECK(foreign_block);
    AllocationCount selecting;
    failure_countdown = 1;
    CHECK(builder.SetEntry(entry.block) == Error::None);
    CHECK(builder.SetEntry(foreign_block.block) == Error::ForeignUnit);
    CHECK(builder.SetEntry({}) == Error::InvalidBlock);
    CHECK(builder.SetEntry({builder.UnitId(), BlockId{3}}) == Error::InvalidBlock);
    CHECK(builder.SetEntry({builder.UnitId(), BlockId{UINT32_MAX - 1}}) == Error::InvalidBlock);
    failure_countdown = 0;
    CHECK(selecting.Stop() == 0 && builder.EntryBlock() == entry.block.id);
    // Declaration, source IDs and body order stay 0,1,2; actual entry is 1.
    CHECK(builder.BeginBlock(first.block) == Error::None);
    const auto unreachable = Literal(builder, 111);
    CHECK(builder.EndBlock(Terminal::Return(unreachable.GetValue())) == Error::None);
    CHECK(builder.BeginBlock(entry.block) == Error::None);
    const auto seed = Literal(builder, 3);
    const auto alu = builder.Emit<OpCode::Add>(Type::U64,
            {Operand::ValueOf(seed.GetValue()), Operand::Immediate(4, Type::U64)}); CHECK(alu);
    CHECK(builder.SetEntry(entry.block) == Error::None); // Also valid with a current body.
    const OperationMetadata metadata{FlagBits(Flag::All), FlagProfile::ArithmeticCanonicalCarry};
    CHECK(builder.SetMetadata(alu.node, metadata) == Error::None);
    CHECK(builder.EndBlock(Terminal::Jump(last.block)) == Error::None);
    CHECK(builder.BeginBlock(last.block) == Error::None);
    const auto result = builder.Emit<OpCode::Add>(Type::U64,
            {Operand::ValueOf(alu.GetValue()), Operand::Immediate(5, Type::U64)}); CHECK(result);
    CHECK(builder.EndBlock(Terminal::Return(result.GetValue())) == Error::None);
    const auto unit = builder.UnitId();
    const auto work = builder.CountWork();
    const auto* operations = builder.Operations().data();
    const auto* blocks = builder.Blocks().data();
    AllocationCount moving;
    Builder moved = std::move(builder);
    CHECK(!builder.EntryBlock() && builder.SetEntry(entry.block) == Error::Consumed);
    CHECK(moved.EntryBlock() == entry.block.id && moved.UnitId() == unit);
    CHECK(moved.SetEntry(entry.block) == Error::None); // Completed bodies need no active-block gate.
    CHECK(moving.Stop() == 0);
    auto owner = std::move(SubmitWithoutAllocation(moved).program);
    CHECK(!moved.EntryBlock());
    const auto reserved = owner.ReservedBytes(), stored = owner.StorageBytes();
    std::array<Cell, 6> words{};
    CHECK(owner.OperandArena().size() == words.size());
    std::copy(owner.OperandArena().begin(), owner.OperandArena().end(), words.begin());
    std::array<Operation, 4> original{};
    std::copy(owner.Operations().begin(), owner.Operations().end(), original.begin());
    AllocationCount transferring;
    Program transferred = std::move(owner);
    CHECK(!owner.EntryBlock() && transferred.EntryBlock() == entry.block.id);
    Program assigned;
    assigned = std::move(transferred);
    CHECK(!transferred.EntryBlock() && assigned.EntryBlock() == entry.block.id);
    auto editor = Builder::Edit(std::move(assigned), 0);
    CHECK(!assigned.EntryBlock() && editor.EntryBlock() == entry.block.id);
    failure_countdown = 1;
    CHECK(editor.SetEntry(first.block) == Error::None);
    CHECK(editor.SetEntry(entry.block) == Error::None);
    CHECK(editor.SetEntry(foreign_block.block) == Error::ForeignUnit);
    CHECK(editor.SetEntry({unit, BlockId{3}}) == Error::InvalidBlock);
    CHECK(editor.EntryBlock() == entry.block.id);
    CHECK(editor.SetMetadata(alu.node, metadata) == Error::None);
    failure_countdown = 0;
    CHECK(transferring.Stop() == 0 && editor.CountWork() == work);
    CHECK(editor.Operations().data() == operations && editor.Blocks().data() == blocks);
    owner = std::move(SubmitWithoutAllocation(editor).program);
    CHECK(owner.EntryBlock() == entry.block.id && owner.UnitId() == unit && owner.Target() == target);
    CHECK(owner.ReservedBytes() == reserved && owner.StorageBytes() == stored);
    CHECK(owner.Operations().size() == original.size() && owner.Blocks().size() == 3);
    for (std::size_t i = 0; i < original.size(); ++i) {
        const auto& op = owner.Operations()[i]; const auto& before = original[i];
        CHECK(op.opcode == before.opcode && op.type == before.type && op.block == before.block);
        CHECK(op.first_operand == before.first_operand && op.operand_count == before.operand_count);
        CHECK(op.Metadata() == before.Metadata());
    }
    for (std::size_t i = 0; i < words.size(); ++i) CHECK(owner.OperandArena()[i].bits == words[i].bits);
    CHECK(owner.Blocks()[0].first_node == 0 && owner.Blocks()[1].first_node == 1 && owner.Blocks()[2].first_node == 3);
    CHECK(owner.Blocks()[1].terminal.first == last.block.id && owner.Blocks()[2].terminal.value == result.node);
    CHECK(!editor.EntryBlock() && editor.SetEntry(entry.block) == Error::Consumed);
}

}  // namespace

int main() {
    try {
        BlockConstructionRoundtrip();
        FunctionConstructionAndIncompleteSubmit();
        RejectedInputIsAtomic();
        ReusedCapacityHasFreshIdentity();
        WidePayloadsAndCompoundBoundaries();
        AllocationFailureIsAtomic();
        MoveAndSubmittedOwnerLifetime();
        StaticOpcodeWritesKeepTheConstructionContract();
        ExplicitWorkAccounting();
        OwnerLocalRewrites();
        ZeroWordAnchorsSurviveTailEdits();
        EditableViewsAndUnusedErase();
        EditWordBudgetsAreAtomic();
        TypedFlagAndConditionInputs();
        MetadataUpdatesStayLocal();
        EntryBelongsToTheActualOwner();
        std::puts("IR construction contracts passed; successful Submit allocations: 0");
        return 0;
    } catch (const std::exception& error) {
        count_allocations = false;
        failure_countdown = 0;
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
