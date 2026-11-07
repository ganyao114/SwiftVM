#include "build_ir.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

#include "instruction.h"
#include "runtime/arch/x86/x87.h"
#include "semantics.h"

namespace swift::x86 {
// Internal byte-decoder helper defined in decode.cpp; not a new IR API.
std::uint8_t DisabledCetHintSize(std::span<const std::uint8_t> bytes) noexcept;

namespace {
using namespace runtime::ir;
using u64 = std::uint64_t;
using u32 = std::uint32_t;

class Emitter;

class Emitter final : public Builder {
public:
    using Value = runtime::ir::Value;
    using NodeId = runtime::ir::NodeId;
    using Producer = NodeId;

    Producer ProducerOf(Value value) const { return value.node; }

    using Operand = runtime::ir::Operand;
    using Result = AppendResult;

    Type NaturalType(Value value) const { return Operations()[value.node.value].type; }

    OperationMetadata Metadata(NodeId node) const { return Operations()[node.value].Metadata(); }

    template <OpCode op> Result Apply(Type type,
                                      std::initializer_list<Operand> inputs,
                                      OperationMetadata metadata = {}) {
        return Emit<op>(type, inputs, metadata);
    }

#define INST(name, ...)                                                                            \
    Result name(                                                                                   \
            Type type, std::initializer_list<Operand> inputs, OperationMetadata metadata = {}) {   \
        return Apply<OpCode::name>(type, inputs, metadata);                                        \
    }
#include "runtime/ir/opcodes.inc"
#undef INST

    template <class Body> void IfNonZero(Value count, Body&& body) {
        const auto skip = Emit<OpCode::NotGoto>(Type::U64, {Operand::ValueOf(count)});
        if (!skip) throw detail::Rejected{BuildError::IRRejected, skip.error};
        body();
        const auto bind = Emit<OpCode::BindLabel>(Type::Void, {Operand::ValueOf(skip.GetValue())});
        if (!bind) throw detail::Rejected{BuildError::IRRejected, bind.error};
    }

    Emitter(Scope scope, TargetInfo target, StateLayout layout, Program reusable)
            : Builder(scope, target, std::move(reusable))
            , layout(layout)
            , private_gprs(target.Has(TargetFeature::PrivateGPRState))
            , private_vectors(target.Has(TargetFeature::PrivateVectorState)) {}

    Error BeginBlock(BlockRef block) {
        const auto error = Builder::BeginBlock(block);
        if (error == Error::None) {
            valid_gprs = 0;
            whole_gprs = 0;
            pending_gprs = 0;
            valid_vectors = 0;
            last_address = {};
            x87_state = {};
            x87_zero = {};
        }
        return error;
    }

    Error EndBlock(Terminal terminal) {
        PublishRegisters();
        return Builder::EndBlock(terminal);
    }

    void PublishGPRs(std::uint16_t slots = UINT16_MAX);
    void PublishVectors();

    void PublishRegisters() {
        PublishGPRs();
        PublishVectors();
    }

    void StateMemory(unsigned, Value, Value, u64 pc, u64) {
        if (!layout.runtime_state_services) throw detail::Rejected{BuildError::Unsupported};
        PublishRegisters();
        const auto at = Emit<OpCode::LoadImm>(Type::U64, {Operand::Immediate(pc, Type::U64)});
        if (!at) throw detail::Rejected{BuildError::IRRejected, at.error};
        const auto error = Emit<OpCode::StoreUniform>(
                Type::Void,
                {Operand::Uniform(layout.pc_offset, Type::U64), Operand::ValueOf(at.GetValue())});
        if (!error) throw detail::Rejected{BuildError::IRRejected, error.error};
        FinishDispatch(5);
    }

private:
    Value ReadGpr(u32 offset, unsigned slot, unsigned bits);
    void WriteGpr(u32 offset, unsigned slot, unsigned bits, Value);
    Value ReadAddressGpr(unsigned slot);
    Value ReadXmm(u32 offset, unsigned slot);
    void WriteXmm(u32 offset, unsigned slot, Value);
    void InvalidateXmm(unsigned slot);
    Value X87Zero();
    Value X87Address(u32 offset);
    std::pair<runtime::x86::X87Command, unsigned> X87Window(
            runtime::x86::X87Command command,
            unsigned index,
            const InstructionData& instruction,
            InstructionRange following,
            std::span<const std::uint8_t> available) const;
    void FinishDispatch(u64 reason);
    Value AddressExpression(Type type, std::initializer_list<Operand> inputs);
    Value ShiftedAddress(TargetInfo target, Type type, Value base, Value index, std::uint8_t shift);
    void PrepareFault(StateLayout layout, u64 pc, TargetInfo target, bool external_helper);
    void MaterializeGPR(unsigned slot);

    struct CachedGPR {
        NodeId value, view;
        std::uint8_t view_first{};
        Type field_type{Type::Void};
    };

    static_assert(sizeof(CachedGPR) == 12);
    std::array<CachedGPR, 16> gprs;
    // Whole-parent coverage is independent of dirty: a U32 read lacks high32,
    // whereas an EAX write defines the full zero-high parent after publication.
    std::uint16_t valid_gprs{}, dirty_gprs{}, whole_gprs{}, pending_gprs{};
    NodeId last_address, x87_state, x87_zero;
    std::array<NodeId, 16> vectors;
    std::uint16_t valid_vectors{}, dirty_vectors{};
    const StateLayout layout;
    const bool private_gprs, private_vectors;
    template <class> friend struct detail::Semantics;
};

using Sem = detail::Semantics<Emitter>;
using detail::Rejected;

Value Require(AppendResult result) {
    if (!result) throw Rejected{BuildError::IRRejected, result.error};
    return result.GetValue();
}

void Require(Error error) {
    if (error != Error::None) throw Rejected{BuildError::IRRejected, error};
}

Value Emitter::AddressExpression(Type type, std::initializer_list<Operand> inputs) {
    const bool cache = private_gprs && inputs.size() <= 4;
    if (cache && last_address) {
        // This construction cursor is set only by the default-metadata
        // GetOperand below and cleared at BeginBlock. No editor or flags
        // producer rewrites that address; only its actual result type varies.
        const auto& previous = Operations()[last_address.value];
        if (previous.type == type) {
            const auto actual = Operands(last_address);
            auto at = actual.begin();
            bool same = true;
            for (const auto& input : inputs) {
                if (at == actual.end()) {
                    same = false;
                    break;
                }
                const auto cell = *at++;
                if (cell.Kind() != input.kind || cell.TypeView() != input.type ||
                    cell.Payload() != input.payload || cell.Count() != input.count ||
                    cell.Code() != input.code) {
                    same = false;
                    break;
                }
            }
            if (same && at == actual.end()) return {UnitId(), last_address};
        }
    }
    const auto value = Require(Emit<OpCode::GetOperand>(type, inputs));
    last_address = cache ? value.node : NodeId{};
    return value;
}

Value Emitter::ShiftedAddress(
        TargetInfo target, Type type, Value base, Value index, std::uint8_t shift) {
    if (!target.Has(TargetFeature::ShiftedAddress)) return {};
    return AddressExpression(type,
                             {Operand::Expression(OperandOp::AddShift, 2, shift),
                              Operand::ValueOf(base, type == Type::U32 ? Type::U32 : Type::Void),
                              Operand::ValueOf(index, type == Type::U32 ? Type::U32 : Type::Void)});
}

void Emitter::PrepareFault(StateLayout layout, u64 pc, TargetInfo target, bool external_helper) {
    PublishRegisters();
    if (target.Has(TargetFeature::FaultLocations)) {
        Require(FaultLocation(Type::Void, {Operand::Immediate(pc, Type::U64)}));
        if (!external_helper) return;
    }
    // External C++ helpers do not fault inside a native FaultSite span.
    Sem::StorePC(*this, layout, Sem::Constant(*this, pc));
}

void Emitter::MaterializeGPR(unsigned slot) {
    const auto bit = static_cast<std::uint16_t>(1u << slot);
    if (!(pending_gprs & bit)) return;
    auto& cached = gprs[slot];
    const auto type = Operations()[cached.value.value].type;
    const auto field_type = cached.field_type;
    const bool canonical_view = Operations()[cached.view.value].type == field_type;
    // The captured parent is whole U32/U64. U32 keeps the EAX write's
    // zero-high coverage; BitInsert cuts the original logical write field.
    const auto combined =
            Require(Emit<OpCode::BitInsert>(type,
                                            {Operand::ValueOf({UnitId(), cached.value}),
                                             Operand::ValueOf({UnitId(), cached.view}),
                                             Operand::Immediate(cached.view_first, Type::U8),
                                             Operand::Immediate(TypeBits(field_type), Type::U8)}));
    cached.value = combined.node;
    // A wide source was only the field's input, never a full-parent view.
    if (!canonical_view) cached.view = {};
    cached.field_type = Type::Void;
    pending_gprs = static_cast<std::uint16_t>(pending_gprs & ~bit);
}

void Emitter::PublishGPRs(std::uint16_t slots) {
    auto publish = static_cast<std::uint16_t>(dirty_gprs & slots);
    dirty_gprs = static_cast<std::uint16_t>(dirty_gprs & ~slots);
    while (publish) {
        const auto slot = std::countr_zero(publish);
        publish = static_cast<std::uint16_t>(publish & (publish - 1));
        MaterializeGPR(slot);
        const auto node = gprs[slot].value;
        const auto type = Operations()[node.value].type;
        Require(Emit<OpCode::StoreUniform>(
                Type::Void,
                {Operand::Uniform(layout.gprs_offset + slot * layout.stride, Type::U64),
                 Operand::ValueOf({UnitId(), node}, type)}));
    }
}

void Emitter::PublishVectors() {
    auto publish = dirty_vectors;
    dirty_vectors = 0;
    while (publish) {
        const auto slot = std::countr_zero(publish);
        publish = static_cast<std::uint16_t>(publish & (publish - 1));
        Require(Emit<OpCode::StoreUniform>(
                Type::Void,
                {Operand::Uniform(layout.fprs_offset + slot * 16, Type::V128),
                 Operand::ValueOf({UnitId(), vectors[slot]}, Type::V128)}));
    }
}

[[noreturn]] void Unsupported() { throw Rejected{BuildError::Unsupported}; }

struct DecodeFailure {
    std::size_t block{std::numeric_limits<std::size_t>::max()};
    BuildError error{BuildError::None};
    Error detail{Error::None};
};

Value Emitter::ReadGpr(u32 offset, unsigned slot, unsigned bits) {
    auto& builder = *this;

    const auto type = Sem::ScalarType(bits);
    const bool cache = builder.private_gprs;
    const auto bit = static_cast<std::uint16_t>(1u << slot);
    if (cache && (builder.valid_gprs & bit)) {
        auto& cached = builder.gprs[slot];
        const auto first = (offset - (layout.gprs_offset + slot * layout.stride)) * 8;
        if (builder.pending_gprs & bit) {
            if (cached.view_first == first && cached.field_type == type) {
                if (builder.Operations()[cached.view.value].type != type) {
                    const auto view = Require(
                            builder.BitExtract(type,
                                               {Operand::ValueOf({builder.UnitId(), cached.view}),
                                                Operand::Immediate(0, Type::U8),
                                                Operand::Immediate(bits, Type::U8)}));
                    cached.view = view.node;
                }
                return {builder.UnitId(), cached.view};
            }
            builder.MaterializeGPR(slot);
        }
        const auto node = cached.value;
        const Value value{builder.UnitId(), node};
        const auto natural = builder.Operations()[node.value].type;
        if (bits == 64) {
            if (builder.whole_gprs & bit) {
                if (natural == Type::U64) return value;
                // An EAX write describes the full, zero-high parent even
                // after it has been published. Widen this version once.
                const auto full = Require(
                        builder.ZeroExtend64(Type::U64, {Operand::ValueOf(value, Type::U32)}));
                cached.value = full.node;
                cached.view = node;
                cached.view_first = 0;
                return full;
            }
            // A mere EAX load did not capture RAX's unknown high half.
        } else {
            if (bits == 32 && natural == Type::U32) return value;
            if (cached.view && cached.view_first == first &&
                builder.Operations()[cached.view.value].type == type)
                return {builder.UnitId(), cached.view};
            // A narrow read is a derived value, never the new parent cache.
            // Retain only the last view in this same fixed slot, avoiding
            // repeated normalizers without a value-indexed side table.
            const auto view =
                    bits == 32 ? Require(builder.ZeroExtend32(Type::U32,
                                                              {Operand::ValueOf(value, Type::U32)}))
                               : Require(builder.BitExtract(type,
                                                            {Operand::ValueOf(value),
                                                             Operand::Immediate(first, Type::U8),
                                                             Operand::Immediate(bits, Type::U8)}));
            cached.view = view.node;
            cached.view_first = static_cast<std::uint8_t>(first);
            return view;
        }
    }
    // Only an uncaptured parent range needs a real state read. Generic callers
    // keep their original typed Uniform accesses and observation behavior.
    builder.PublishGPRs(bit);
    const auto value = Require(builder.LoadUniform(Type::Void, {Operand::Uniform(offset, type)}));
    if (cache && bits >= 32) {
        builder.gprs[slot] = {value.node, {}, 0};
        builder.valid_gprs |= bit;
        builder.whole_gprs =
                static_cast<std::uint16_t>((builder.whole_gprs & ~bit) | (bits == 64 ? bit : 0));
    }
    return value;
}

void Emitter::WriteGpr(u32 offset, unsigned slot, unsigned bits, Value value) {
    auto& builder = *this;

    const auto type = Sem::ScalarType(bits);
    const auto bit = static_cast<std::uint16_t>(1u << slot);
    if (builder.private_gprs) {
        const auto& producer = builder.Operations()[value.node.value];
        const bool canonical = IntegerType(producer.type) && producer.opcode != OpCode::BitCast &&
                               producer.opcode != OpCode::GetResult &&
                               producer.opcode != OpCode::GetFlags &&
                               !HasAnyEffect(EffectsFor(producer.opcode),
                                             Effect::Unknown | Effect::External | Effect::Control);
        // Same-width numeric values describe the whole register, including a
        // U32 write's implicit zero high half. A full overwrite kills an
        // unobserved partial without materializing its old parent.
        if (canonical && producer.type == type && (bits == 32 || bits == 64)) {
            builder.pending_gprs = static_cast<std::uint16_t>(builder.pending_gprs & ~bit);
            builder.gprs[slot] = {value.node, {}, 0};
            builder.valid_gprs |= bit;
            builder.dirty_gprs |= bit;
            builder.whole_gprs |= bit;
            return;
        }
        if (canonical && bits < 32 && (builder.valid_gprs & builder.whole_gprs & bit)) {
            auto& cached = builder.gprs[slot];
            const auto first = (offset - (layout.gprs_offset + slot * layout.stride)) * 8;
            if ((builder.pending_gprs & bit) &&
                (cached.view_first != first || cached.field_type != type))
                builder.MaterializeGPR(slot);
            cached.view = value.node;
            cached.view_first = static_cast<std::uint8_t>(first);
            cached.field_type = type;
            builder.pending_gprs |= bit;
            builder.dirty_gprs |= bit;
            return;
        }
    }
    // Partial writes preserve the old parent. An immediate full write instead
    // replaces it, so an unobserved pending full write can simply disappear.
    if (bits < 32)
        builder.PublishGPRs(bit);
    else
        builder.pending_gprs = static_cast<std::uint16_t>(builder.pending_gprs & ~bit);
    builder.valid_gprs = static_cast<std::uint16_t>(builder.valid_gprs & ~bit);
    builder.dirty_gprs = static_cast<std::uint16_t>(builder.dirty_gprs & ~bit);
    builder.whole_gprs = static_cast<std::uint16_t>(builder.whole_gprs & ~bit);
    builder.gprs[slot].view = {};
    builder.gprs[slot].field_type = Type::Void;
    // StoreUniform reads the value's logical view, then stores the descriptor
    // width. U64 field + U32 input therefore zeroes the architectural high half,
    // including raw aliases, without birthing an explicit zero-extend node.
    Require(builder.StoreUniform(Type::Void,
                                 {Operand::Uniform(offset, bits == 32 ? Type::U64 : type),
                                  Operand::ValueOf(value, type)}));
}

bool ValidRequest(std::size_t blocks, Scope scope, StateLayout layout, TargetInfo target) {
    if (!blocks || blocks > BlockId::invalid ||
        (scope != Scope::Block && scope != Scope::Function) ||
        (scope == Scope::Block && blocks != 1) ||
        static_cast<unsigned>(target.mode) > static_cast<unsigned>(TargetMode::Unknown) ||
        layout.stride < 8 || layout.pc_offset > std::numeric_limits<u32>::max() - 7 ||
        u64{layout.gprs_offset} + u64{15} * layout.stride > std::numeric_limits<u32>::max() - 7)
        return false;
    if (layout.fprs_offset != UINT32_MAX && (layout.fprs_offset > UINT32_MAX - 255 ||
                                             (u64{layout.fprs_offset} < u64{layout.pc_offset} + 8 &&
                                              layout.pc_offset < u64{layout.fprs_offset} + 256)))
        return false;
    for (unsigned reg = 0; reg < 16; ++reg) {
        const u64 offset = u64{layout.gprs_offset} + u64{reg} * layout.stride;
        if (offset < u64{layout.pc_offset} + 8 && layout.pc_offset < offset + 8) return false;
        if (layout.fprs_offset != UINT32_MAX && offset < u64{layout.fprs_offset} + 256 &&
            layout.fprs_offset < offset + 8)
            return false;
    }
    return true;
}

bool ValidSources(std::span<const BlockWindow> windows) {
    if (windows.empty()) return false;
    for (std::size_t i = 0; i < windows.size(); ++i)
        if (windows[i].bytes.empty() || !windows[i].bytes.data() ||
            windows[i].bytes.size() > std::numeric_limits<u64>::max() - windows[i].pc ||
            (i && windows[i - 1].pc >= windows[i].pc))
            return false;
    return true;
}

Value Emitter::ReadAddressGpr(unsigned slot) {
    if (!private_gprs || !(valid_gprs & (1u << slot))) return {};
    MaterializeGPR(slot);
    return {UnitId(), gprs[slot].value};
}

Value Emitter::ReadXmm(u32 offset, unsigned slot) {
    const auto bit = static_cast<std::uint16_t>(1u << slot);
    if (private_vectors && (valid_vectors & bit)) return {UnitId(), vectors[slot]};
    const auto value = Require(LoadUniform(Type::V128, {Operand::Uniform(offset, Type::V128)}));
    if (private_vectors) {
        vectors[slot] = value.node;
        valid_vectors |= bit;
    }
    return value;
}

void Emitter::WriteXmm(u32 offset, unsigned slot, Value value) {
    if (private_vectors) {
        const auto bit = static_cast<std::uint16_t>(1u << slot);
        vectors[slot] = value.node;
        valid_vectors |= bit;
        dirty_vectors |= bit;
        return;
    }
    Require(StoreUniform(
            Type::Void,
            {Operand::Uniform(offset, Type::V128), Operand::ValueOf(value, Type::V128)}));
}

void Emitter::InvalidateXmm(unsigned slot) {
    const auto bit = static_cast<std::uint16_t>(1u << slot);
    valid_vectors = static_cast<std::uint16_t>(valid_vectors & ~bit);
    dirty_vectors = static_cast<std::uint16_t>(dirty_vectors & ~bit);
}

Value Emitter::X87Zero() {
    if (!x87_zero) x87_zero = Sem::Constant(*this, 0).node;
    return {UnitId(), x87_zero};
}

Value Emitter::X87Address(u32 offset) {
    if (!x87_state)
        x87_state =
                Require(GetUniformAddress(Type::U64, {Operand::Immediate(offset, Type::U32)})).node;
    return {UnitId(), x87_state};
}

std::pair<runtime::x86::X87Command, unsigned> Emitter::X87Window(
        runtime::x86::X87Command command,
        unsigned index,
        const InstructionData& instruction,
        InstructionRange following,
        std::span<const std::uint8_t> available) const {
    using runtime::x86::X87Command;
    // Only these contiguous register windows have no independent failure after
    // their first operation succeeds. Preserve each original record's later
    // validation/counting and all actual helper stack/status effects.
    unsigned consumed = 1;
    const auto second_instruction =
            following.size() > 1 ? following[1].Record() : InstructionData{};
    const auto third_instruction = following.size() > 2 ? following[2].Record() : InstructionData{};
    const auto exact = [&](unsigned at, Op opcode, std::uint8_t low, std::uint8_t high) {
        if (at >= following.size() || available.size() < (at + 1) * 2) return false;
        const auto& record = at == 0   ? instruction
                             : at == 1 ? second_instruction
                                       : third_instruction;
        return record.addr == instruction.addr + at * 2 && record.size == 2 &&
               record.opcode == opcode && available[at * 2] == low && available[at * 2 + 1] == high;
    };
    const auto empty_after = [](const InstructionData& decoded, unsigned first_operand) {
        for (unsigned i = first_operand; i < 4; ++i)
            if (decoded.ops[i].type != GuestOperandKind::None) return false;
        return true;
    };
    if (command == X87Command::Exchange && exact(0, Op::FXCH, 0xd9, 0xc8 + index) &&
        exact(1, Op::FXCH, 0xd9, 0xc8 + index)) {
        const auto& other = second_instruction;
        if (empty_after(other, 1) && (other.ops[0].type == GuestOperandKind::None ||
                                      (other.ops[0].type == GuestOperandKind::Register &&
                                       other.ops[0].index == Register::ST0 + index))) {
            command = X87Command::ExchangeTwice;
            consumed = 2;
        }
    } else if (command == X87Command::PushOne && exact(0, Op::FLD1, 0xd9, 0xe8) &&
               exact(1, Op::FSQRT, 0xd9, 0xfa) && exact(2, Op::FSTP, 0xdd, 0xd8) &&
               empty_after(second_instruction, 0) && empty_after(third_instruction, 1) &&
               third_instruction.ops[0].type == GuestOperandKind::Register &&
               third_instruction.ops[0].index == Register::ST0) {
        command = X87Command::PushOnePop;
        consumed = 3;
    }
    return {command, consumed};
}

void Emitter::FinishDispatch(u64 reason) {
    Require(EndBlock(reason ? Terminal::Return(Sem::Constant(*this, reason))
                            : Terminal::Dispatch()));
}

BuildResult EmitDecoded(InstructionRange instructions,
                        std::span<const DecodedBlock> blocks,
                        std::span<const BlockWindow> sources,
                        Scope scope,
                        StateLayout layout,
                        TargetInfo target,
                        WorkCounts* collected,
                        DecodeFailure failure = {},
                        u32 entry_block = 0,
                        bool external_edges = false,
                        Program* reusable = nullptr,
                        bool formed_function = false) {
    if (collected) *collected = {};
    if (entry_block >= blocks.size() ||
        (formed_function
                 ? blocks.size() > BlockId::invalid
                 : !ValidRequest(blocks.size(), scope, layout, target) || !ValidSources(sources)))
        return {BuildError::InvalidInput, 0, 0, 0, Error::None, {}};
    if (!formed_function)
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const auto& block = blocks[i];
            const std::size_t first = block.first_instruction;
            if (first > instructions.size() ||
                block.instruction_count > instructions.size() - first ||
                (i && blocks[i - 1].pc >= block.pc))
                return {BuildError::InvalidInput, block.pc, 0, 0, Error::None, {}};
        }
    std::size_t decoded{}, instruction_count{};
    u64 pc = blocks.front().pc;
    Emitter builder{scope, target, layout, reusable ? std::move(*reusable) : Program{}};
    try {
        // A formed region already knows its real record count. This modest
        // hint avoids the first IR arena migrations without sizing to a budget.
        if (formed_function && instructions.size() >= 16) Require(builder.Reserve(64, 192, 0));
        const auto internal = [&](u64 address) {
            if (external_edges) return BlockId{};
            const auto found = std::lower_bound(
                    blocks.begin(),
                    blocks.end(),
                    address,
                    [](const DecodedBlock& block, u64 value) { return block.pc < value; });
            return found != blocks.end() && found->pc == address
                           ? BlockId{static_cast<u32>(found - blocks.begin())}
                           : BlockId{};
        };
        const auto external_choice = [&](u64 taken, u64 next) {
            return target.Has(TargetFeature::ConditionalUniformStore) && !internal(taken) &&
                   !internal(next);
        };
        std::vector<u64> exits;
        if (scope == Scope::Function) {
            // Only real conditional outgoing edges need extra IR bodies. This
            // reads terminal records, not a second whole-instruction plan.
            for (const auto& block : blocks) {
                if (!block.instruction_count) continue;
                const auto& last = instructions[std::size_t{block.first_instruction} +
                                                block.instruction_count - 1]
                                           .Record();
                if ((!Sem::Conditional(last.opcode) && !Sem::CounterConditional(last.opcode)) ||
                    !Sem::Direct(last))
                    continue;
                const u64 next = last.addr + last.size, taken = next + last.immediate;
                const bool taken_internal = bool(internal(taken)),
                           next_internal = bool(internal(next));
                // Two outgoing PCs already fit one existing conditional
                // Uniform store. Keep CFG bodies only for real internal edges.
                if (target.Has(TargetFeature::ConditionalUniformStore) && !taken_internal &&
                    !next_internal)
                    continue;
                if (!taken_internal) exits.push_back(taken);
                if (!next_internal) exits.push_back(next);
            }
            std::sort(exits.begin(), exits.end());
            exits.erase(std::unique(exits.begin(), exits.end()), exits.end());
            if (exits.size() > BlockId::invalid - blocks.size())
                throw Rejected{BuildError::IRRejected, Error::CapacityOverflow};
        }
        u64 unit{};
        for (std::size_t i = 0; i < blocks.size() + exits.size(); ++i) {
            const auto created = builder.CreateBlock();
            if (!created) throw Rejected{BuildError::IRRejected, created.error};
            unit = created.block.unit;
        }
        Require(builder.SetEntry({unit, BlockId{entry_block}}));
        Sem::PendingFlags pending;
        const auto finish = [&](u64 destination) {
            pending.Flush(builder);
            builder.PublishRegisters();
            const auto found = internal(destination);
            // Internal edges stay in native code. An installed fault-location
            // consumer recovers PC at the real fault, and actual exits still
            // publish their destination below.
            if (!found || !target.Has(TargetFeature::FaultLocations))
                Sem::StorePC(builder, layout, Sem::Constant(builder, destination));
            if (found)
                Require(builder.EndBlock(Terminal::Jump({unit, found})));
            else
                Sem::EndDispatch(builder);
        };
        const auto target_block = [&](u64 destination) {
            if (const auto found = internal(destination)) return BlockRef{unit, found};
            const auto found = std::lower_bound(exits.begin(), exits.end(), destination);
            if (found == exits.end() || *found != destination)
                throw Rejected{BuildError::InvalidInput};
            return BlockRef{unit,
                            BlockId{static_cast<u32>(blocks.size() + (found - exits.begin()))}};
        };
        const auto branch = [&](Value condition, u64 taken, u64 next_pc) {
            if (scope == Scope::Block || external_choice(taken, next_pc)) {
                if (target.Has(TargetFeature::ConditionalUniformStore))
                    Require(builder.Emit<OpCode::StoreUniformSelectImm>(
                            Type::Void,
                            {Operand::Uniform(layout.pc_offset, Type::U64),
                             Operand::ValueOf(condition),
                             Operand::Immediate(taken, Type::U64),
                             Operand::Immediate(next_pc, Type::U64)}));
                else {
                    const auto yes = Sem::Constant(builder, taken),
                               no = Sem::Constant(builder, next_pc);
                    const auto destination =
                            Require(builder.Emit<OpCode::Select>(Type::U64,
                                                                 {Operand::ValueOf(condition),
                                                                  Operand::ValueOf(yes),
                                                                  Operand::ValueOf(no)}));
                    Sem::StorePC(builder, layout, destination);
                }
                Sem::EndDispatch(builder);
            } else
                Require(builder.EndBlock(
                        Terminal::Branch(condition, target_block(taken), target_block(next_pc))));
        };

        struct Flow {
            decltype(finish)& finish;
            decltype(branch)& branch;
        } cfg_flow{finish, branch};

        std::size_t source{};
        for (std::size_t block = 0; block < blocks.size(); ++block) {
            const auto& slice = blocks[block];
            pc = slice.pc;
            // DecodeFunction already bounds each slice to one readable window.
            if (formed_function)
                while (source + 1 < sources.size() && sources[source + 1].pc <= pc) ++source;
            Require(builder.BeginBlock({unit, BlockId{static_cast<u32>(block)}}));
            // Empty budget frontiers dispatch to their own PC so a later
            // request can decode it. They must never become cached self entries.
            if (scope == Scope::Function && target.Has(TargetFeature::ExternalEntries) &&
                block != entry_block && slice.instruction_count)
                Require(builder.Emit<OpCode::EntryPoint>(Type::Void,
                                                         {Operand::Immediate(pc, Type::U64)}));
            bool ended{};
            unsigned x87_following{};
            for (std::size_t i = 0; i < slice.instruction_count; ++i) {
                const auto& instruction = instructions[slice.first_instruction + i].Record();
                if (!formed_function) {
                    if (instruction.addr != pc) throw Rejected{BuildError::InvalidInput};
                    if (instruction.flags == InstructionFlag::Invalid || !instruction.size ||
                        instruction.size > 15)
                        throw Rejected{BuildError::InvalidEncoding};
                    while (source + 1 < sources.size() && sources[source + 1].pc <= pc) ++source;
                }
                const auto& window = sources[source];
                if (!formed_function) {
                    if (pc < window.pc || pc - window.pc > window.bytes.size() ||
                        instruction.size >
                                window.bytes.size() - static_cast<std::size_t>(pc - window.pc))
                        throw Rejected{BuildError::Truncated};
                    if (block + 1 < blocks.size() && (pc >= blocks[block + 1].pc ||
                                                      instruction.size > blocks[block + 1].pc - pc))
                        throw Rejected{BuildError::InvalidInput};
                }
                const auto bytes = window.bytes.subspan(static_cast<std::size_t>(pc - window.pc),
                                                        instruction.size);
                if (!Sem::AllowedPrefixes(instruction, bytes)) Unsupported();
                const u64 next_pc = pc + instruction.size;
                if (!formed_function && instruction.unit_boundary &&
                    i + 1 != slice.instruction_count)
                    throw Rejected{BuildError::InvalidInput};
                // Every original x87 record retains prefix/form admission and
                // instruction counts; lookahead remains bounded by this slice.
                if (x87_following)
                    --x87_following;
                else
                    x87_following = Sem::Execute(builder,
                                                 layout,
                                                 instruction,
                                                 bytes,
                                                 pending,
                                                 pc,
                                                 next_pc,
                                                 target,
                                                 cfg_flow,
                                                 ended,
                                                 instructions.subspan(slice.first_instruction + i,
                                                                      slice.instruction_count - i),
                                                 window.bytes.subspan(static_cast<std::size_t>(
                                                         pc - window.pc))) -
                                    1;
                decoded += instruction.size;
                ++instruction_count;
                pc = next_pc;
            }
            if (failure.block == block) throw Rejected{failure.error, failure.detail};
            if (!ended) {
                if (slice.instruction_count)
                    finish(pc);
                else {
                    Sem::StorePC(builder, layout, Sem::Constant(builder, pc));
                    Sem::EndDispatch(builder);
                }
            }
        }
        for (std::size_t exit = 0; exit < exits.size(); ++exit) {
            Require(builder.BeginBlock({unit, BlockId{static_cast<u32>(blocks.size() + exit)}}));
            Sem::StorePC(builder, layout, Sem::Constant(builder, exits[exit]));
            Sem::EndDispatch(builder);
        }
        if (collected) *collected = builder.CountWork();
        auto submitted = builder.Submit();
        if (!submitted)
            return {BuildError::IRRejected, pc, decoded, instruction_count, submitted.error, {}};
        return {BuildError::None,
                0,
                decoded,
                instruction_count,
                Error::None,
                std::move(submitted.program)};
    } catch (const Rejected& rejected) {
        if (collected) *collected = builder.CountWork();
        return {rejected.error, pc, decoded, instruction_count, rejected.detail, {}};
    }
}
}  // namespace

BuildResult BuildDecodedIR(InstructionRange instructions,
                           std::span<const DecodedBlock> blocks,
                           std::span<const BlockWindow> source_bytes,
                           Scope scope,
                           StateLayout layout,
                           TargetInfo target,
                           WorkCounts* collected,
                           u32 entry_block,
                           Program* reusable) {
    return EmitDecoded(instructions,
                       blocks,
                       source_bytes,
                       scope,
                       layout,
                       target,
                       collected,
                       {},
                       entry_block,
                       false,
                       reusable);
}

BuildResult BuildFunctionIR(std::span<const BlockWindow> readable,
                            u64 entry_pc,
                            StateLayout layout,
                            DecodeLimits limits,
                            TargetInfo target,
                            WorkCounts* collected,
                            DecodeTiming* timing,
                            Program* reusable,
                            ReadyEntryLookup ready,
                            runtime::x86::GuestCpuModel model) {
    if (collected) *collected = {};
    if (!ValidRequest(1, Scope::Function, layout, target))
        return {BuildError::InvalidInput, entry_pc, 0, 0, Error::None, {}};
    const bool entries = target.Has(TargetFeature::ExternalEntries);
    const auto decoded = DecodeFunction(
            readable, entry_pc, limits, timing, entries ? ready : ReadyEntryLookup{}, model);
    if (!decoded) {
        const auto error = decoded.error == DecodeError::InvalidInput ? BuildError::InvalidInput
                           : decoded.error == DecodeError::Truncated  ? BuildError::Truncated
                           : decoded.error == DecodeError::OverlappingTarget
                                   ? BuildError::FormationRejected
                           : decoded.error == DecodeError::UnsupportedGuestFeature
                                   ? BuildError::Unsupported
                                   : BuildError::InvalidEncoding;
        return {error, decoded.error_pc, 0, 0, Error::None, {}};
    }
    // Request/source/record structure was established by this DecodeFunction.
    // Public decoded inputs and byte-decoded Blocks retain all emitter checks.
    return EmitDecoded(decoded.instructions,
                       decoded.blocks,
                       readable,
                       Scope::Function,
                       layout,
                       target,
                       collected,
                       {},
                       decoded.entry_block,
                       false,
                       reusable,
                       true);
}

BuildResult BuildIR(std::span<const BlockWindow> windows,
                    Scope scope,
                    StateLayout layout,
                    TargetInfo target,
                    WorkCounts* collected,
                    DecodeTiming* timing,
                    bool external_edges,
                    Program* reusable,
                    runtime::x86::GuestCpuModel model) {
    if (collected) *collected = {};
    if (!ValidRequest(windows.size(), scope, layout, target) || !ValidSources(windows))
        return {BuildError::InvalidInput, 0, 0, 0, Error::None, {}};
    std::array<Instruction, 16> local_instructions;
    std::vector<Instruction> overflow_instructions;
    std::span<const Instruction> instructions{local_instructions.data(), 0};
    DecodedBlock local_block;
    std::vector<DecodedBlock> multiple_blocks;
    if (windows.size() > 1) multiple_blocks.resize(windows.size());
    auto blocks = windows.size() == 1 ? std::span{&local_block, 1} : std::span{multiple_blocks};
    DecodeFailure failure;
    for (std::size_t block = 0; block < windows.size(); ++block) {
        const auto& window = windows[block];
        auto& slice = blocks[block];
        slice = {window.pc, static_cast<u32>(instructions.size()), 0};
        if (failure.error != BuildError::None) continue;
        auto bytes = window.bytes;
        if (block + 1 < windows.size() && windows[block + 1].pc - window.pc < bytes.size())
            bytes = bytes.first(static_cast<std::size_t>(windows[block + 1].pc - window.pc));
        u64 pc = window.pc;
        while (!bytes.empty()) {
            const auto parsed = DecodeOne(bytes, pc, timing, model);
            if (!parsed) {
                failure = {block,
                           parsed.error == DecodeError::Truncated ? BuildError::Truncated
                           : parsed.error == DecodeError::UnsupportedGuestFeature
                                   ? BuildError::Unsupported
                                   : BuildError::InvalidEncoding};
                break;
            }
            const auto& instruction = parsed.instruction.View().Record();
            if (instructions.size() == std::numeric_limits<u32>::max()) {
                failure = {block, BuildError::IRRejected, Error::CapacityOverflow};
                break;
            }
            if (instructions.size() < local_instructions.size()) {
                local_instructions[instructions.size()] = parsed.instruction;
                instructions = {local_instructions.data(), instructions.size() + 1};
            } else {
                if (overflow_instructions.empty()) {
                    overflow_instructions.reserve(local_instructions.size() * 2);
                    overflow_instructions.insert(overflow_instructions.end(),
                                                 local_instructions.begin(),
                                                 local_instructions.end());
                }
                overflow_instructions.push_back(parsed.instruction);
                instructions = overflow_instructions;
            }
            ++slice.instruction_count;
            pc += instruction.size;
            bytes = bytes.subspan(instruction.size);
            if (instruction.unit_boundary) {
                if (scope == Scope::Block &&
                    (instruction.flow == FlowKind::Conditional ||
                     instruction.flow == FlowKind::Jump) &&
                    Sem::Direct(instruction)) {
                    const u64 target_pc = pc + instruction.immediate;
                    if (window.pc < target_pc && target_pc < pc) {
                        const auto first = instructions.begin() + slice.first_instruction;
                        const auto leader =
                                std::lower_bound(first,
                                                 instructions.end(),
                                                 target_pc,
                                                 [](const Instruction& decoded, u64 address) {
                                                     return decoded.View().PC() < address;
                                                 });
                        // Emit only the prefix before a newly discovered loop header.
                        // The existing slice also bounds semantic/x87 lookahead.
                        if (leader != instructions.end() && leader->View().PC() == target_pc)
                            slice.instruction_count = static_cast<u32>(leader - first);
                    }
                }
                break;
            }
        }
    }
    return EmitDecoded(instructions,
                       blocks,
                       windows,
                       scope,
                       layout,
                       target,
                       collected,
                       failure,
                       0,
                       external_edges,
                       reusable);
}

}  // namespace swift::x86
